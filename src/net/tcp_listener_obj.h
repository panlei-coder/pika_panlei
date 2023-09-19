#pragma once

#include "event2/listener.h"
#include "event_obj.h"
#include "tcp_obj.h"

namespace pikiwidb {
class EventLoop;

class TcpListenerObj : public EventObject {
 public:
  explicit TcpListenerObj(EventLoop* loop);
  ~TcpListenerObj();

  bool Bind(const char* ip, int port);
  int Fd() const override;

  void SetNewConnCallback(NewTcpConnCallback cb) { on_new_conn_ = std::move(cb); }
  void SetEventloopSelector(EventLoopSelector cb) { loop_selector_ = std::move(cb); }
  EventLoop* SelectEventloop();

 private:
  static void OnNewConnection(struct evconnlistener*, evutil_socket_t, struct sockaddr*, int, void*);
  static void OnError(struct evconnlistener*, void*);

  EventLoop* const loop_; // 监听器所属的事件循环
  struct evconnlistener* listener_{nullptr}; // libevent的监听器对象

  NewTcpConnCallback on_new_conn_; // 新连接的回调函数

  EventLoopSelector loop_selector_; // 事件循环选择器，用于选择要处理新连接的事件循环（获取事件循环的可执行函数）
};

}  // namespace pikiwidb
