#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "event2/buffer.h"
#include "event2/bufferevent.h"
#include "event_obj.h"
#include "reactor.h"

namespace pikiwidb {
class EventLoop;
class TcpObject;

// init a new tcp conn which from ::accept or ::connect
using NewTcpConnCallback = std::function<void(TcpObject*)>;
// called when got incoming data, return bytes of consumed, -1 means fatal
using TcpMessageCallback = std::function<int(TcpObject*, const char* data, int len)>;
// called when connect failed, usually retry or report error
using TcpConnFailCallback = std::function<void(EventLoop*, const char* peer_ip, int port)>;
// called when a connection being reset
using TcpDisconnectCallback = std::function<void(TcpObject*)>;
// choose a loop for load balance
using EventLoopSelector = std::function<EventLoop*()>;

class TcpObject : public EventObject {
 public:
  explicit TcpObject(EventLoop* loop);
  ~TcpObject();

  // init tcp object by result of ::accept
  // 初始化TcpObject对象，通过::accept获取连接的文件描述符、对端ip和端口
  void OnAccept(int fd, const std::string& peer_ip, int peer_port);
  // init tcp object by trying ::connect
  // 初始化TcpObject对象，通过::connect连接到指定的ip和端口
  bool Connect(const char* ip, int port);

  int Fd() const override; // 获取文件描述符
  bool SendPacket(const std::string&); // 发送数据包（字符串）
  bool SendPacket(const void*, size_t); // 发送数据包（二进制形式）
  bool SendPacket(const evbuffer_iovec* iovecs, int nvecs); // 发送数据包（iovec形式）

  void SetNewConnCallback(NewTcpConnCallback cb) { on_new_conn_ = std::move(cb); } // 设置新连接的回调函数
  void SetOnDisconnect(TcpDisconnectCallback cb) { on_disconnect_ = std::move(cb); } // 设置断开连接回调函数
  void SetMessageCallback(TcpMessageCallback cb) { on_message_ = std::move(cb); } // 设置消息回调函数
  void SetFailCallback(TcpConnFailCallback cb) { on_fail_ = std::move(cb); } // 设置连接失败回调函数

  // connection context
  // 设置和获取连接上下文
  template <typename T>
  std::shared_ptr<T> GetContext() const;
  void SetContext(std::shared_ptr<void> ctx);

  EventLoop* GetLoop() const { return loop_; } // 获取关联的EventLoop对象
  const std::string& GetPeerIp() const { return peer_ip_; } // 获取对端ip
  int GetPeerPort() const { return peer_port_; } // 获取对端端口号
  const sockaddr_in& PeerAddr() const { return peer_addr_; } // 获取对端地址结构体

  // if sync == true, wait connection closed,
  // otherwise when return, connection maybe alive for a while
  // 主动关闭连接
  void ActiveClose(bool sync = false);
  // 判断是否已连接
  bool Connected() const;

  // set idle timeout for this client
  void SetIdleTimeout(int timeout_s); // 设置空闲的连接超时时间

  // Nagle algorithm
  void SetNodelay(bool enable);  // 设置Nagle算法

 private:
  // check if idle timeout
  bool CheckIdleTimeout() const; // 检查是否达到空闲超时时间

  static void OnRecvData(struct bufferevent* bev, void* ctx); // 静态回调函数，用于处理接收到的数据
  static void OnEvent(struct bufferevent* bev, short what, void* ctx); // 静态回调函数，用于处理事件

  void HandleConnect(); // 处理连接成功事件
  void HandleConnectFailed(); // 处理连接失败事件
  void HandleDisconnect(); // 处理连接断开事件

  // 连接所处的状态
  enum class State {
    kNone,
    kConnecting,
    kConnected,
    kDisconnected,  // unrecoverable but once connected before
    kFailed,        // unrecoverable and never connected
  };

  State state_ = State::kNone; // 当前连接的状态

  EventLoop* const loop_; // 关联的EventLoop对象
  struct bufferevent* bev_ = nullptr; // 用于处理读写事件

  std::string peer_ip_; // 对端ip地址
  int peer_port_ = -1; // 对端端口号
  struct sockaddr_in peer_addr_; // 对端地址结构体

  TcpMessageCallback on_message_; // 消息回调函数
  TcpDisconnectCallback on_disconnect_; // 连接断开回调函数
  TcpConnFailCallback on_fail_; // 连接失败回调函数
  NewTcpConnCallback on_new_conn_; // 新连接回调函数

  TimerId idle_timer_ = -1; // 空闲定时器ID
  int idle_timeout_ms_ = 0; // 空闲超时时间（ms） 
  std::chrono::steady_clock::time_point last_active_; // 上次活跃时间点

  std::shared_ptr<void> context_; // 连接的上下文对象
};

// 获取连接的上下文对象
template <typename T>
inline std::shared_ptr<T> TcpObject::GetContext() const {
  return std::static_pointer_cast<T>(context_);
}

}  // namespace pikiwidb
