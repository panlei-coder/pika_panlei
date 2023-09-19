#pragma once

#include <cassert>
#include <functional>
#include <memory>
#include <unordered_map>

#include "event2/bufferevent.h"
#include "event2/event.h"
#include "reactor.h"

namespace pikiwidb {
class EventObject;
namespace internal {

// Libevent Reactor
class LibeventReactor : public Reactor {
 public:
  LibeventReactor();
  virtual ~LibeventReactor() {}

  bool Register(EventObject* obj, int events) override; // 注册
  void Unregister(EventObject* obj) override; // 注销
  bool Modify(EventObject* obj, int events) override; // 修改事件
  bool Poll() override; // 等待事件并处理事件

  void ScheduleRepeatedly(TimerId id, int period_ms, std::function<void()> f) override; // 重复定时任务
  void ScheduleLater(TimerId id, int delay_ms, std::function<void()> f) override; // 延迟定时调度任务

  bool Cancel(TimerId id) override; // 取消定时器
  void* Backend() override; // 获取底层的事件反应器对象

 private:
  // 事件删除器，用于释放事件对象内存
  struct EventDeleter {
    void operator()(struct event* ev) {
      if (ev) {
        event_free(ev);
      }
    }
  };

  // 定时器对象，用于管理定时任务
  struct Timer : public std::enable_shared_from_this<Timer> {
    Timer() = default;
    ~Timer();

    Timer(const Timer&) = delete;
    void operator=(const Timer&) = delete;

    TimerId id; // 定时器ID
    bool repeat; // 是否重复定时
    void* ctx = nullptr; // 上下文
    struct event* event = nullptr; // 事件对象
    std::function<void()> callback; // 定时器回调函数
  };

  // 事件对象
  struct Object {
    explicit Object(EventObject* evobj) : ev_obj(evobj) {}
    ~Object() = default;

    // 是否可读
    bool IsReadEnabled() const { return !!read_event.get(); }
    // 是否可写
    bool IsWriteEnabled() const { return !!write_event.get(); }

    std::unique_ptr<event, EventDeleter> read_event;
    std::unique_ptr<event, EventDeleter> write_event;
    EventObject* const ev_obj;
  };

  void Schedule(TimerId id, int period_ms, std::function<void()>, bool repeat);
  static void TimerCallback(evutil_socket_t, int16_t, void*);

  std::unique_ptr<event_base, decltype(event_base_free)*> event_base_; // 事件反应器对象
  std::unordered_map<int, std::unique_ptr<Object>> objects_; // 事件对象映射表，按文件描述符索引

  // use shared ptr because callback will hold timer
  std::unordered_map<TimerId, std::shared_ptr<Timer>> timers_; // 定时器映射表，按定时器ID索引

  // for wake up loop
  std::unique_ptr<event, EventDeleter> wakeup_event_; // 用于唤醒事件循环的事件对象
};

}  // end namespace internal
}  // namespace pikiwidb
