#include "tcp_obj.h"

#include <netinet/tcp.h>

#include <cassert>
#include <cstdlib>
#include <memory>

#include "event2/event.h"
#include "event2/util.h"
#include "event_loop.h"
#include "log.h"
#include "util.h"

namespace pikiwidb {
TcpObject::TcpObject(EventLoop* loop) : loop_(loop) {
  memset(&peer_addr_, 0, sizeof peer_addr_);
  last_active_ = std::chrono::steady_clock::now();
}

TcpObject::~TcpObject() {
  if (idle_timer_ != -1) {
    loop_->Cancel(idle_timer_);
  }

  if (bev_) {
    INFO("close tcp fd {}", Fd());
    bufferevent_disable(bev_, EV_READ | EV_WRITE);
    bufferevent_free(bev_);
  }
}

// 接受到新连接时初始化
void TcpObject::OnAccept(int fd, const std::string& peer_ip, int peer_port) {
  assert(loop_->InThisLoop());

  peer_ip_ = peer_ip;
  peer_port_ = peer_port;
  peer_addr_ = MakeSockaddr(peer_ip.c_str(), peer_port);

  evutil_make_socket_nonblocking(fd); // 非阻塞
  evutil_make_socket_closeonexec(fd); // 将文件描述符设置为在执行exec系列函数时关闭

  // 获取关联的 EventLoop 对象的底层事件库指针
  auto base = reinterpret_cast<struct event_base*>(loop_->GetReactor()->Backend());
  // 创建一个基于socket的bufferevent对象
  bev_ = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  assert(bev_);

  HandleConnect();
}

// TCP连接操作
bool TcpObject::Connect(const char* ip, int port) {
  assert(loop_->InThisLoop()); // 确保当前线程在 EventLoop 对象所属的线程中

  if (state_ != State::kNone) { // 如果当前状态不为kNone
    ERROR("repeat connect tcp socket to {}:{}", ip, port);
    return false;
  }

  // new bufferevent then connect
  // 创建新的bufferevent对象并进行连接
  auto base = reinterpret_cast<struct event_base*>(loop_->GetReactor()->Backend());
  auto bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE); // 创建一个基于socket的bufferevent对象，文件描述符为-1，设置在释放时自动关闭
  if (!bev) { // 如果创建失败
    ERROR("can't new bufferevent");
    return false;
  }
  // 设置bufferevent的回调函数，这里没有读写事件回调函数，只设置了事件回调函数
  bufferevent_setcb(bev, nullptr, nullptr, &TcpObject::OnEvent, this);

  sockaddr_in addr = MakeSockaddr(ip, port);
  int err = bufferevent_socket_connect(bev, (struct sockaddr*)&addr, int(sizeof addr)); // 使用bufferevent对象进行连接
  if (err != 0) {
    ERROR("bufferevent_socket_connect failed to {}:{}", ip, port);
    bufferevent_free(bev);
    return false;
  }

  // success, keep this
  // 如果连接成功，将对象注册到EventLoop中
  if (!loop_->Register(shared_from_this(), 0)) {
    ERROR("add tcp obj to loop failed, fd {}", bufferevent_getfd(bev));
    bufferevent_free(bev);
    return false;
  }

  INFO("in loop {}, trying connect to {}:{}", loop_->GetName(), ip, port);
  // update state
  bev_ = bev;
  peer_ip_ = ip;
  peer_port_ = port;
  peer_addr_ = addr;
  state_ = State::kConnecting;

  return true;
}

// 返回连接描述符
int TcpObject::Fd() const {
  if (bev_) {
    return bufferevent_getfd(bev_);
  }

  return -1;
}

// 发送数据包（字符串）
bool TcpObject::SendPacket(const std::string& data) { return this->SendPacket(data.data(), data.size()); }

// 发送数据包（二进制形式）
bool TcpObject::SendPacket(const void* data, size_t size) {
  if (state_ != State::kConnected) {
    ERROR("send tcp data in wrong state {}", static_cast<int>(state_));
    return false;
  }

  if (!data || size == 0) {
    return true;
  }

  assert(loop_->InThisLoop());
  auto output = bufferevent_get_output(bev_); //获取输出缓存区
  evbuffer_add(output, data, size); // 将数据添加到输出缓存区
  return true;
}

// 处理连接成功
// @todo 为什么没有将this注册到EventLoop中
void TcpObject::HandleConnect() {
  assert(loop_->InThisLoop());
  assert(state_ == State::kNone || state_ == State::kConnecting);
  INFO("HandleConnect success with {}:{}", peer_ip_, peer_port_);

  state_ = State::kConnected;
  bufferevent_setcb(bev_, &TcpObject::OnRecvData, nullptr, &TcpObject::OnEvent, this);
  bufferevent_enable(bev_, EV_READ);

  if (on_new_conn_) {
    on_new_conn_(this);
  }
}

// 发送数据包（iovec形式，向量数据）
/*
{
    char buf1[BUF_SIZE] = { 0 };
    char buf2[BUF_SIZE] = { 0 };
    char buf3[BUF_SIZE] = { 0 };
    struct iovec iov[3];
    ssize_t nread;
 
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 8;
    iov[2].iov_base = buf3;
    iov[2].iov_len = BUF_SIZE;
 
    nread = readv(STDIN_FILENO, iov, 3);
    printf("%ld bytes read.\n", nread);
    printf("buf1: %s\n", buf1);
    printf("buf2: %s\n", buf2);
}

12345HelloWorldIamJason
24 bytes read.
buf1: 12345
buf2: HelloWor
buf3: ldIamJason
*/
bool TcpObject::SendPacket(const evbuffer_iovec* iovecs, int nvecs) {
  if (state_ != State::kConnected) {
    ERROR("send tcp data in wrong state {}", static_cast<int>(state_));
    return false;
  }

  if (!iovecs || nvecs <= 0) {
    return true;
  }

  assert(loop_->InThisLoop());
  auto output = bufferevent_get_output(bev_);
  evbuffer_add_iovec(output, const_cast<evbuffer_iovec*>(iovecs), nvecs);
  return true;
}

// 处理连接失败
void TcpObject::HandleConnectFailed() {
  assert(loop_->InThisLoop());
  assert(state_ == State::kConnecting);
  ERROR("HandleConnectFailed to {}:{}", peer_ip_, peer_port_);

  state_ = State::kFailed;
  if (on_fail_) {
    on_fail_(loop_, peer_ip_.c_str(), peer_port_);
  }

  loop_->Unregister(shared_from_this());
}

// 处理断开连接
void TcpObject::HandleDisconnect() {
  assert(loop_->InThisLoop());
  assert(state_ == State::kConnected);

  state_ = State::kDisconnected;
  if (on_disconnect_) {
    on_disconnect_(this);
  }

  loop_->Unregister(shared_from_this());
}

// 设置TCP连接的空闲超时时间，并创建一个定时器来检测是否超时
void TcpObject::SetIdleTimeout(int timeout_ms) {
  if (timeout_ms <= 0) {
    return;
  }

  idle_timeout_ms_ = timeout_ms;
  if (idle_timer_ != -1) { // 如果存在定时器，则取消定时器
    loop_->Cancel(idle_timer_);
  }

  auto w_obj(weak_from_this()); // 弱引用，不增加计数
  // Actual precision is 0.1s.
  idle_timer_ = loop_->ScheduleRepeatedly(100, [w_obj]() {
    auto c = w_obj.lock(); // 如果当前没有引用当前对象的shared_ptr，则返回该对象的共享指针
    if (!c) { // 连接丢失
      return;  // connection already lost
    }

    auto tcp_conn = std::static_pointer_cast<TcpObject>(c);
    bool timeout = tcp_conn->CheckIdleTimeout(); // 检测是否到达空闲超时时间
    if (timeout) {
      tcp_conn->ActiveClose(false); // 超时了就关闭连接，但是不触发关闭事件
    }
  });
}

// 设置是否启用Nagle算法，默认是关闭的
/*
Nagle是针对每一个TCP连接的。
它要求一个TCP连接上最多只能有一个未被确认的小分组。
在改分组的确认到达之前不能发送其他小分组。
TCP会搜集这些小的分组，然后在之前小分组的确认到达后将刚才搜集的小分组合并发送出去。
*/
void TcpObject::SetNodelay(bool enable) {
  if (bev_) {
    int fd = bufferevent_getfd(bev_);
    int nodelay = enable ? 1 : 0; // 1：打开nagle算法，0：关闭
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(int));
  }
}

// 检测是否超过了空闲超时时间
bool TcpObject::CheckIdleTimeout() const {
  using namespace std::chrono;

  int elapsed = static_cast<int>(duration_cast<milliseconds>(steady_clock::now() - last_active_).count());
  if (elapsed > idle_timeout_ms_) {
    WARN("TcpObject::Timeout: elapsed {}, idle timeout {}, peer {}:{}", elapsed, idle_timeout_ms_, peer_ip_,
         peer_port_);
    return true;
  }

  return false;
}

// 接收并处理数据
void TcpObject::OnRecvData(struct bufferevent* bev, void* obj) {
  auto me = std::static_pointer_cast<TcpObject>(reinterpret_cast<TcpObject*>(obj)->shared_from_this());

  assert(me->loop_->InThisLoop());
  assert(me->bev_ == bev);

  if (me->idle_timer_ != -1) { // 如果存在空闲定时器，更新最后活动时间
    me->last_active_ = std::chrono::steady_clock::now();
  }

  // 获取bufferevent的输入缓存区
  auto input = bufferevent_get_input(bev);
  evbuffer_pullup(input, -1); // 将输入缓存区中的数据移动到起始位置

  struct evbuffer_iovec data[1]; // 创建evbuffer_iovec（向量）数组
  int nvecs = evbuffer_peek(input, -1, nullptr, data, 1); // 从缓存区中读取数据
  if (nvecs != 1) { // 如果读取的数据向量数量不为1，则直接返回
    return;
  }

  // 获取数据向量的起始位置
  const char* start = reinterpret_cast<const char*>(data[0].iov_base);
  // 获取数据向量的长度
  const int len = static_cast<int>(data[0].iov_len);
  int total_consumed = 0;
  bool error = false;
  // 在没有错误且没有消耗完所有数据时循环处理数据
  while (!error && total_consumed < len) { 
    // 调用on_message_处理数据，返回消费的数据量（注意这里并不会将消费的数据进行移除）
    int consumed = me->on_message_(me.get(), start + total_consumed, len - total_consumed);
    if (consumed > 0) {
      total_consumed += consumed;
    } else {
      if (consumed < 0) {
        error = true;
      }

      break;
    }
  }

  // 如果消费了数据，则从输入缓存区中移除已经消费的数据
  if (total_consumed > 0) {
    evbuffer_drain(input, total_consumed);
  }

  // 如果发生了错误，则断开连接
  if (error) {
    me->HandleDisconnect();
  }
}

// 处理TCP事件
void TcpObject::OnEvent(struct bufferevent* bev, short events, void* obj) {
  auto me = std::static_pointer_cast<TcpObject>(reinterpret_cast<TcpObject*>(obj)->shared_from_this());

  assert(me->loop_->InThisLoop());

  INFO("TcpObject::OnEvent {:x}, state {}, obj {}", events, static_cast<int>(me->state_), obj);

  // 根据当前状态进行处理
  switch (me->state_) {
    case State::kConnecting:
      if (events & BEV_EVENT_CONNECTED) { // 如果连接成功
        me->HandleConnect();
      } else { // 如果连接失败
        me->HandleConnectFailed();
      }
      return;

    case State::kConnected:
      if (events & BEV_EVENT_EOF) { // 连接失败
        me->HandleDisconnect();
      }
      return;

    default:
      ERROR("TcpObject::OnEvent wrong state {}", static_cast<int>(me->state_));
      return;
  }
}

// 设置连接的上下文对象
void TcpObject::SetContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }

// 关闭TCP连接
void TcpObject::ActiveClose(bool sync) {
  // weak: don't prolong life of this
  std::weak_ptr<TcpObject> me = std::static_pointer_cast<TcpObject>(shared_from_this());
  auto destroy = [me]() {
    auto conn = me.lock(); // 只有当该对象的所有共享指针都使用完了，才能进行断开销毁
    if (conn && conn->state_ == State::kConnected) {
      conn->HandleDisconnect();
    }
  };

  if (loop_->InThisLoop()) {
    destroy();
  } else {
    auto fut = loop_->Execute([=]() { destroy(); });
    if (sync) { // 如果是同步，则会阻塞在这里
      fut.get();
    }
  }
}

// 判断TCP连接是否处于连接的状态
bool TcpObject::Connected() const { return state_ == State::kConnected; }

}  // namespace pikiwidb
