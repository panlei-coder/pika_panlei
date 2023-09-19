#include "tcp_listener_obj.h"

#include <errno.h>
#include <unistd.h>

#include <string>

//#include "application.h"
#include "event_loop.h"
#include "log.h"
#include "util.h"

namespace pikiwidb {
TcpListenerObj::TcpListenerObj(EventLoop* loop) : loop_(loop) {}

TcpListenerObj::~TcpListenerObj() {
  if (listener_) {
    INFO("close tcp listener fd {}", Fd());
    evconnlistener_free(listener_);
  }
}

// TCP端口绑定
bool TcpListenerObj::Bind(const char* ip, int port) {
  if (listener_) {
    ERROR("repeat bind tcp socket to port {}", port);
    return false;
  }

  // 创建 sockaddr_in 结构体，表示要绑定的 IP 地址和端口号
  sockaddr_in addr = MakeSockaddr(ip, port);
  // 获取 EventLoop 对象的底层 event_base 指针
  auto base = reinterpret_cast<struct event_base*>(loop_->GetReactor()->Backend());
  // 创建 evconnlistener 对象并绑定到指定地址
  auto listener =
      evconnlistener_new_bind(base, &TcpListenerObj::OnNewConnection, this,
                              LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_DISABLED, -1,
                              (const struct sockaddr*)&addr, int(sizeof(addr)));
  if (!listener) {
    ERROR("failed listen tcp port {}", port);
    return false;
  }

  // 设置错误回调函数
  evconnlistener_set_error_cb(listener, &TcpListenerObj::OnError);
  if (!loop_->Register(shared_from_this(), 0)) {
    ERROR("add tcp listener to loop failed, socket {}", Fd());
    evconnlistener_free(listener);
    return false;
  }

  // 将监听对象注册到事件循环中
  INFO("tcp listen on port {}", port);
  listener_ = listener;
  evconnlistener_enable(listener_);
  return true;
}

// 获取监听器描述符
int TcpListenerObj::Fd() const {
  if (listener_) {
    return static_cast<int>(evconnlistener_get_fd(listener_));
  }

  return -1;
}

// 获取监听器所属的事件循环
EventLoop* TcpListenerObj::SelectEventloop() {
  if (loop_selector_) {
    return loop_selector_();
  }

  return loop_;
}

// 建立新的TCP连接
void TcpListenerObj::OnNewConnection(struct evconnlistener*, evutil_socket_t fd, struct sockaddr* peer, int,
                                     void* obj) {
  auto acceptor = reinterpret_cast<TcpListenerObj*>(obj);
  if (acceptor->on_new_conn_) { // 如果有新连接的回调函数
    // convert address
    std::string ipstr = GetSockaddrIp(peer);
    int port = GetSockaddrPort(peer);
    if (ipstr.empty() || port == -1) {
      ERROR("invalid peer address for tcp fd {}", fd);
      close(fd);
      return;
    }

    INFO("new conn fd {} from {}", fd, ipstr);

    // make new conn
    auto loop = acceptor->SelectEventloop();
    // Application::Instance().Next();
    auto on_create = acceptor->on_new_conn_;  // cpp11 doesn't support lambda
                                              // capture initializers
    auto create_conn = [loop, on_create, fd, ipstr, port]() {
      auto conn(std::make_shared<TcpObject>(loop));
      conn->SetNewConnCallback(on_create);
      conn->OnAccept(fd, ipstr, port);
      if (!loop->Register(conn, 0)) {
        ERROR("Failed to register socket {}", fd);
      }
    };
    loop->Execute(std::move(create_conn)); // 添加到待执行的任务队列或者在当前线程中执行
  } else {
    WARN("close new conn fd {}", fd);
    close(fd);
  }
}

// 错误处理
void TcpListenerObj::OnError(struct evconnlistener* listener, void* obj) {
  auto acceptor = reinterpret_cast<TcpListenerObj*>(obj);
  INFO("listener fd {} with errno {}", acceptor->Fd(), errno);

  // man 2 accept. TODO alert
  switch (errno) {
    case EAGAIN:
    case EINTR:
    case ECONNABORTED:
    case EPROTO:
      return;

    case EMFILE:
    case ENFILE:
      ERROR("not enough file descriptor, error is {}", errno);
      return;

    case ENOBUFS:
    case ENOMEM:
      ERROR("not enough memory, socket buffer limits");
      return;

    default:
      ERROR("BUG: accept with errno = {}", errno);
      assert(false);
      break;
  }
}
}  // namespace pikiwidb
