#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "event_obj.h"
#include "http_client.h"
#include "http_server.h"
#include "pipe_obj.h"
#include "reactor.h"
#include "tcp_listener_obj.h"
#include "tcp_obj.h"

namespace pikiwidb {
/// EventLoop is a wrapper for reactor and running its loop,
/// one thread should at most has one EventLoop object,
class EventLoop {
 public:
  EventLoop();
  ~EventLoop() = default;

  EventLoop(const EventLoop&) = delete;
  void operator=(const EventLoop&) = delete;

  // Run in a specific thread
  // 在指定的线程中运行事件循环
  void Run();
  // Stop loop
  // 停止事件循环
  void Stop();

  // Exec func in loop thread, it's thread-safe
  // 在事件循环线程中执行函数，线程安全的
  template <typename F, typename... Args>
  auto Execute(F&&, Args&&...) -> std::future<typename std::invoke_result<F, Args...>::type>;

  // Exec func every some time, it's thread-safe
  // 按照一定的时间间隔重复执行的函数，线程安全的
  template <typename Duration, typename F, typename... Args>
  TimerId ScheduleRepeatedly(const Duration& period, F&& f, Args&&... args);
  template <typename F, typename... Args>
  TimerId ScheduleRepeatedly(int period_ms, F&& f, Args&&... args);

  // Exec func after some time, it's thread-safe
  // 在一定的时间后执行函数，线程安全的
  template <typename Duration, typename F, typename... Args>
  TimerId ScheduleLater(const Duration& delay, F&& f, Args&&... args);
  template <typename F, typename... Args>
  TimerId ScheduleLater(int delay_ms, F&& f, Args&&... args);

  // cancel timer
  // 取消定时器
  std::future<bool> Cancel(TimerId id);

  // check if loop running in the caller's thread
  // 检查事件循环是否在调用线程中运行
  bool InThisLoop() const;

  // the backend reactor
  // 获取后端反应器
  Reactor* GetReactor() const { return reactor_.get(); }

  // TCP server
  bool Listen(const char* ip, int port, NewTcpConnCallback ccb);

  // TCP client
  std::shared_ptr<TcpObject> Connect(const char* ip, int port, NewTcpConnCallback ccb, TcpConnFailCallback fcb);

  // HTTP server
  std::shared_ptr<HttpServer> ListenHTTP(const char* ip, int port,
                                         HttpServer::OnNewClient cb = HttpServer::OnNewClient());

  // HTTP client
  std::shared_ptr<HttpClient> ConnectHTTP(const char* ip, int port);

  // 注册事件对象
  bool Register(std::shared_ptr<EventObject> src, int events);
  // 修改事件对象的监听事件
  bool Modify(std::shared_ptr<EventObject> src, int events);
  // 注销事件对象
  void Unregister(std::shared_ptr<EventObject> src);

  // 设置事件循环的名称
  void SetName(std::string name) { name_ = std::move(name); }
  // 获取事件循环的名称
  const std::string& GetName() const { return name_; }

  // 获取当前线程的事件循环对象
  static EventLoop* Self();

  // for unittest only
  void Reset();

 private:
  std::unique_ptr<Reactor> reactor_; // 后端反应器
  std::unordered_map<int, std::shared_ptr<EventObject>> objects_; // 事件对象的映射表
  std::shared_ptr<internal::PipeObject> notifier_; // 通知器对象

  std::mutex task_mutex_; // 任务互斥锁
  std::vector<std::function<void()>> tasks_; // 待执行的任务列表

  std::string name_;  // for top command 事件循环的名称
  std::atomic<bool> running_{true}; // 事件循环是否正在运行的标记

  static std::atomic<int> obj_id_generator_; // 生成事件对象ID
  static std::atomic<TimerId> timerid_generator_; // 生成定时器ID
};

template <typename F, typename... Args>
auto EventLoop::Execute(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
  using resultType = typename std::invoke_result<F, Args...>::type; // 函数调用的结果类型

  // 创建一个shared_ptr指向packaged_task对象，将传入的函数对象和参数绑定起来
  auto task =
      std::make_shared<std::packaged_task<resultType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
  std::future<resultType> fut = task->get_future(); // 获取与packaged_task关联的future

  if (InThisLoop()) { // 如果当前线程是事件循环所在的线程，直接执行
    (*task)();
  } else { // 如果当前线程不是事件循环所在的线程，将task添加到待执行的任务列表中
    std::unique_lock<std::mutex> guard(task_mutex_);
    tasks_.emplace_back([task]() { (*task)(); });
    notifier_->Notify(); // 通过通知器唤醒事件循环线程
  }

  return fut; // 返回与任务关联的future，可以用于获取任务执行结果
}

// 按照一定的时间间隔重复执行的函数，线程安全的
template <typename Duration, typename F, typename... Args>
TimerId EventLoop::ScheduleRepeatedly(const Duration& period, F&& f, Args&&... args) {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;

  // 将传入的时间间隔转换为毫秒，并确保最小间隔时间为1ms
  auto period_ms = std::max(milliseconds(1), duration_cast<milliseconds>(period));
  int ms = static_cast<int>(period_ms.count());
  return ScheduleRepeatedly(ms, std::forward<F>(f), std::forward<Args>(args)...);
}

template <typename F, typename... Args>
TimerId EventLoop::ScheduleRepeatedly(int period_ms, F&& f, Args&&... args) {
  // 创建回调函数（注意std::bind(std::forward<F>(f), std::forward<Args>(args)...)之后的对象是void()类型）
  std::function<void()> cb(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
  auto id = timerid_generator_.fetch_add(1) + 1; // 生成一个定时器ID

  if (InThisLoop()) { // 如果当前线程是事件循环所在的线程
    // 在事件循环所使用的后端反应器（Reactor）中调用ScheduleRepeatedly函数安排重复执行任务
    reactor_->ScheduleRepeatedly(id, period_ms, std::move(cb));
  } else { // 在事件循环线程中执行任务调度
    Execute([this, id, period_ms, cb]() { reactor_->ScheduleRepeatedly(id, period_ms, std::move(cb)); });
  }

  return id;
}

// 在一定的时间后执行函数，线程安全的
template <typename Duration, typename F, typename... Args>
TimerId EventLoop::ScheduleLater(const Duration& delay, F&& f, Args&&... args) {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;

  // 将传入的时间间隔转换为毫秒，并确保最小间隔时间为1ms
  auto delay_ms = std::max(milliseconds(1), duration_cast<milliseconds>(delay));
  int ms = static_cast<int>(delay_ms.count());
  return ScheduleLater(ms, std::forward<F>(f), std::forward<Args>(args)...);
}

template <typename F, typename... Args>
TimerId EventLoop::ScheduleLater(int delay_ms, F&& f, Args&&... args) {
  std::function<void()> cb(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
  auto id = timerid_generator_.fetch_add(1) + 1; // 生成一个定时器ID（fetch_add将原子变量的值增加1，并返回增加前的值）

  if (InThisLoop()) { // 如果当前线程是事件循环所在的线程
    reactor_->ScheduleLater(id, delay_ms, std::move(cb));
  } else { // 在事件循环线程中执行任务调度
    Execute([this, id, delay_ms, cb]() { reactor_->ScheduleLater(id, delay_ms, std::move(cb)); });
  }

  return id;
}

}  // namespace pikiwidb
