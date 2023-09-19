#include "event_loop.h"

#if defined(__gnu_linux__)
#  include <sys/prctl.h>
#endif
#include <unistd.h>

#include "libevent_reactor.h"
#include "log.h"
#include "util.h"

namespace pikiwidb {

//  线程存储期，每一个线程都保存一个副本
static thread_local EventLoop* g_this_loop = nullptr;

std::atomic<int> EventLoop::obj_id_generator_{0};
std::atomic<TimerId> EventLoop::timerid_generator_{0};

EventLoop::EventLoop() {
  assert(!g_this_loop && "There must be only one EventLoop per thread");
  g_this_loop = this;

  reactor_.reset(new internal::LibeventReactor());
  notifier_ = std::make_shared<internal::PipeObject>();
}

// 运行事件循环监听
void EventLoop::Run() {
#if defined(__gnu_linux__)
  if (!name_.empty()) {
    prctl(PR_SET_NAME, ToValue<unsigned long>(name_.c_str()));
  }
#endif

  // 注册事件循环的通知器，监听读事件
  Register(notifier_, kEventRead);

  while (running_) { // 当事件循环处于运行状态时执行循环
    if (task_mutex_.try_lock()) { // 尝试获取任务列表的锁
      decltype(tasks_) funcs;
      funcs.swap(tasks_); // 将任务队列中的任务移动到临时容器中
      task_mutex_.unlock();

      // 遍历临时容器中的任务，并按顺序执行每一个任务
      for (const auto& f : funcs) {
        f();
      }
    }

    // 调用后端反应器的轮询函数，等待和处理事件
    if (!reactor_->Poll()) {
      ERROR("Reactor poll failed");
    }
  }

  // 遍历事件循环管理的对象
  for (auto& pair : objects_) {
    // 在后端反应器中注销对象的事件监听
    reactor_->Unregister(pair.second.get());
  }

  objects_.clear();
  reactor_.reset();
}

// 停止事件循环监听
void EventLoop::Stop() {
  running_ = false;
  notifier_->Notify(); // 通知所有对象
}

// 取消定时器
std::future<bool> EventLoop::Cancel(TimerId id) {
  if (InThisLoop()) { // 如果当前线程是事件循环所在的线程
    bool ok = reactor_ ? reactor_->Cancel(id) : false; // 在反应器中取消定时器

    std::promise<bool> prom;
    auto fut = prom.get_future(); // 生成与取消相关联的future对象
    prom.set_value(ok);
    return fut;
  } else { 
    // 不在当前线程中（添加到task_去等待执行）
    auto fut = Execute([id, this]() -> bool {
      if (!reactor_) {
        return false;
      }
      bool ok = reactor_->Cancel(id);
      INFO("cancell timer {} {}", id, ok ? "succ" : "fail");
      return ok;
    });
    return fut;
  }
}

// 判断是否是当前的线程中
bool EventLoop::InThisLoop() const { return this == g_this_loop; }
// 返回自己的线程对象EventLoop*
EventLoop* EventLoop::Self() { return g_this_loop; }

// 注册事件
bool EventLoop::Register(std::shared_ptr<EventObject> obj, int events) {
  if (!obj) return false;
  // @todo 为什么一定要在当前线程中
  assert(InThisLoop()); 
  assert(obj->GetUniqueId() == -1);

  if (!reactor_) {
    return false;
  }

  // alloc unique id
  int id = -1;
  do {
    id = obj_id_generator_.fetch_add(1) + 1; // 获取唯一ID
    if (id < 0) {
      obj_id_generator_.store(0); // 如果ID小于0，则重置2为0
    }
  } while (id < 0 || objects_.count(id) != 0); // 保证唯一性

  obj->SetUniqueId(id); // 设置对象的唯一ID
  if (reactor_->Register(obj.get(), events)) { // 在反应器中注册对象的事件监听
    objects_.insert({obj->GetUniqueId(), obj}); // 将对象添加到事件对象的映射表中
    return true;
  }

  return false;
}

// 在后端反应器中对已经注册的事件修改
bool EventLoop::Modify(std::shared_ptr<EventObject> obj, int events) {
  if (!obj) return false;

  assert(InThisLoop());
  assert(obj->GetUniqueId() >= 0);
  assert(objects_.count(obj->GetUniqueId()) == 1);

  if (!reactor_) {
    return false;
  }
  return reactor_->Modify(obj.get(), events);
}

// 注销已经注册的事件
void EventLoop::Unregister(std::shared_ptr<EventObject> obj) {
  if (!obj) return;

  int id = obj->GetUniqueId();
  assert(InThisLoop());
  assert(id >= 0);
  assert(objects_.count(id) == 1);

  if (!reactor_) {
    return;
  }
  reactor_->Unregister(obj.get());
  objects_.erase(id);
}

// 建立TCP监听
bool EventLoop::Listen(const char* ip, int port, NewTcpConnCallback ccb) {
  auto s = std::make_shared<TcpListenerObj>(this);
  s->SetNewConnCallback(ccb);

  return s->Bind(ip, port);
}

// 建立TCP连接
std::shared_ptr<TcpObject> EventLoop::Connect(const char* ip, int port, NewTcpConnCallback ccb,
                                              TcpConnFailCallback fcb) {
  auto c = std::make_shared<TcpObject>(this);
  c->SetNewConnCallback(ccb);
  c->SetFailCallback(fcb);

  if (!c->Connect(ip, port)) {
    c.reset();
  }

  return c;
}

// 建立HTTP监听
std::shared_ptr<HttpServer> EventLoop::ListenHTTP(const char* ip, int port, HttpServer::OnNewClient cb) {
  auto server = std::make_shared<HttpServer>();
  server->SetOnNewHttpContext(std::move(cb));

  // capture server to make it long live with TcpListener
  auto ncb = [server](TcpObject* conn) { server->OnNewConnection(conn); };
  Listen(ip, port, ncb);

  return server;
}

// 建立HTTP连接
std::shared_ptr<HttpClient> EventLoop::ConnectHTTP(const char* ip, int port) {
  auto client = std::make_shared<HttpClient>();

  // capture client to make it long live with TcpObject
  auto ncb = [client](TcpObject* conn) { client->OnConnect(conn); };
  auto fcb = [client](EventLoop*, const char* ip, int port) { client->OnConnectFail(ip, port); };

  client->SetLoop(this);
  Connect(ip, port, std::move(ncb), std::move(fcb));

  return client;
}

// 重置
void EventLoop::Reset() {
  for (auto& kv : objects_) {
    Unregister(kv.second);
  }
  objects_.clear();

  {
    std::unique_lock<std::mutex> guard(task_mutex_);
    tasks_.clear();
  }

  reactor_.reset(new internal::LibeventReactor());
  notifier_ = std::make_shared<internal::PipeObject>();
}

}  // namespace pikiwidb
