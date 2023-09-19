#include "libevent_reactor.h"

#include <cassert>

#include "event_obj.h"

namespace pikiwidb {
namespace internal {

LibeventReactor::LibeventReactor() : event_base_(event_base_new(), event_base_free) {
  auto base = event_base_.get(); // 获取事件反应器对象

  // to wakeup loop atmost every 10 ms
  // 设置唤醒事件循环的最大时间间隔为10ms
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 10 * 1000;

  // 创建唤醒事件对象，使用空函数作为回调函数
  wakeup_event_.reset(event_new(
      base, -1, EV_PERSIST, [](evutil_socket_t, int16_t, void*) {}, this));
  event_add(wakeup_event_.get(), &timeout);
}

static void OnReadable(evutil_socket_t socket, short events, void* ctx) {
  EventObject* obj = reinterpret_cast<EventObject*>(ctx);
  if (!obj->HandleReadEvent()) { // 处理读事件
    obj->HandleErrorEvent();
  }
}

static void OnWritable(evutil_socket_t socket, short events, void* ctx) {
  EventObject* obj = reinterpret_cast<EventObject*>(ctx);
  if (!obj->HandleWriteEvent()) { // 处理写事件
    obj->HandleErrorEvent();
  }
}

// 事件注册
bool LibeventReactor::Register(EventObject* evobj, int events) {
  if (!evobj) {
    return false;
  }

  if (events == 0) {
    return true;  // evobj can manage events by itself
  }

  // check if repeat
  int id = evobj->GetUniqueId();
  assert(id >= 0);
  auto it = objects_.find(id);
  if (it != objects_.end()) {
    return false;
  }

  // register events
  std::unique_ptr<Object> obj(new Object(evobj));
  if (events & kEventRead) { // 读事件
    auto base = event_base_.get();
    obj->read_event.reset(event_new(base, evobj->Fd(), EV_READ | EV_PERSIST, OnReadable, evobj));
    event_add(obj->read_event.get(), nullptr);
  }
  if (events & kEventWrite) { // 写事件
    auto base = event_base_.get();
    obj->write_event.reset(event_new(base, evobj->Fd(), EV_WRITE | EV_PERSIST, OnWritable, evobj));
    event_add(obj->write_event.get(), nullptr);
  }

  // success, keep it
  objects_.insert({id, std::move(obj)});
  return true;
}

// 注销
void LibeventReactor::Unregister(EventObject* evobj) {
  if (!evobj) {
    return;
  }

  // check if exist
  int id = evobj->GetUniqueId();
  auto it = objects_.find(id);
  if (it == objects_.end()) {
    return;
  }

  auto obj = it->second.get();
  obj->read_event.reset();
  obj->write_event.reset();
  objects_.erase(it);
}

// 事件修改
bool LibeventReactor::Modify(EventObject* evobj, int events) {
  if (!evobj) {
    return false;
  }

  int id = evobj->GetUniqueId();
  auto it = objects_.find(id);
  if (it == objects_.end()) {
    return false;
  }

  auto base = event_base_.get();
  auto obj = it->second.get();
  assert(obj->ev_obj == evobj);
  if (events & kEventRead) {
    if (!obj->read_event) {
      // event_new()函数用于创建一个新的事件对象，并将其存储在obj->read_event中
      obj->read_event.reset(event_new(base, evobj->Fd(), EV_READ | EV_PERSIST, OnReadable, evobj));
      // event_add()函数将读事件对象添加到事件反应器event_base_中
      event_add(obj->read_event.get(), nullptr);
    }
  } else {
    obj->read_event.reset();
  }

  if (events & kEventWrite) {
    if (!obj->write_event) {
      obj->write_event.reset(event_new(base, evobj->Fd(), EV_WRITE | EV_PERSIST, OnWritable, evobj));
      event_add(obj->write_event.get(), nullptr);
    }
  } else {
    obj->write_event.reset();
  }

  return true;
}

// 将事件反应器中的所有待处理事件执行一次，EVLOOP_ONCE是非阻塞模式
bool LibeventReactor::Poll() { return event_base_loop(event_base_.get(), EVLOOP_ONCE) != -1; }

// 按照指定的时间间隔重复执行
void LibeventReactor::ScheduleRepeatedly(TimerId id, int period_ms, std::function<void()> f) {
  Schedule(id, period_ms, std::move(f), true);
}

// 延迟指定的时间后执行
void LibeventReactor::ScheduleLater(TimerId id, int delay_ms, std::function<void()> f) {
  Schedule(id, delay_ms, std::move(f), false);
}

// 创建一个定时器事件，并将其添加到事件反应器中，以便在指定的时间间隔后触发回调函数。
void LibeventReactor::Schedule(TimerId id, int period_ms, std::function<void()> f, bool repeat) {
  auto timer = std::make_shared<Timer>();
  timer->id = id;
  timer->callback = std::move(f);
  timer->repeat = repeat;
  timer->ctx = this;

  int flag = (repeat ? EV_PERSIST : 0);
  struct event* ev = event_new(event_base_.get(), -1, flag, &LibeventReactor::TimerCallback, timer.get());
  timer->event = ev;

  struct timeval timeout;
  timeout.tv_sec = period_ms / 1000;
  timeout.tv_usec = 1000 * (period_ms % 1000);
  int err = event_add(ev, &timeout);
  assert(err == 0);

  timers_[id] = std::move(timer);
}

// 取消定时器
bool LibeventReactor::Cancel(TimerId id) {
  auto it = timers_.find(id);
  if (it == timers_.end()) {
    return false;
  }

  timers_.erase(it);
  return true;
}

LibeventReactor::Timer::~Timer() {
  if (event) {
    event_free(event);
  }
}

// static
// 定时器回调函数
void LibeventReactor::TimerCallback(evutil_socket_t, int16_t, void* ctx) {
  auto timer = reinterpret_cast<Timer*>(ctx)->shared_from_this();
  timer->callback();
  if (!timer->repeat) { // 不需要重复则取消定时器
    auto reactor = reinterpret_cast<LibeventReactor*>(timer->ctx);
    reactor->Cancel(timer->id);
  }
}

void* LibeventReactor::Backend() { return event_base_.get(); }

}  // namespace internal
}  // namespace pikiwidb
