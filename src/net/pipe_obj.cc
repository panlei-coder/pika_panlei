#include "pipe_obj.h"

#include <unistd.h>

#include <cassert>

#include "event2/util.h"

namespace pikiwidb {
namespace internal {

PipeObject::PipeObject() {
  int fd[2];
  int ret = ::pipe(fd); // 创建管道
  assert(ret == 0);

  read_fd_ = fd[0];
  write_fd_ = fd[1];
  // 都设置为非阻塞模式
  evutil_make_socket_nonblocking(read_fd_);
  evutil_make_socket_nonblocking(write_fd_);
}

PipeObject::~PipeObject() {
  ::close(read_fd_);
  ::close(write_fd_);
}

// 获取读文件描述符
int PipeObject::Fd() const { return read_fd_; }

// 处理读事件
bool PipeObject::HandleReadEvent() {
  char ch;
  auto n = ::read(read_fd_, &ch, sizeof ch);
  return n == 1;
}

// 处理写事件 @todo 暂未实现？
bool PipeObject::HandleWriteEvent() {
  assert(false);
  return false;
}

void PipeObject::HandleErrorEvent() { assert(false); }

// 通知
bool PipeObject::Notify() {
  char ch = 0;
  auto n = ::write(write_fd_, &ch, sizeof ch);
  return n == 1;
}

}  // end namespace internal
}  // namespace pikiwidb
