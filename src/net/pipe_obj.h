#pragma once

#include "event_obj.h"

namespace pikiwidb {
namespace internal {

/*
一个实际的场景示例是多线程或多进程之间的通信。
假设有一个主线程和多个工作线程，主线程负责接收任务并将任务分发给工作线程处理。
PipeObject 类可以用于在主线程和工作线程之间建立管道通信，主线程通过向管道
写入数据来通知工作线程有新的任务到达，而工作线程在事件循环中监听该管道的读
事件，一旦有数据到达，就会调用 HandleReadEvent() 函数进行相应的处理。
*/
class PipeObject : public EventObject {
 public:
  PipeObject();
  ~PipeObject();

  PipeObject(const PipeObject&) = delete;
  void operator=(const PipeObject&) = delete;

  int Fd() const override;
  bool HandleReadEvent() override;
  bool HandleWriteEvent() override;
  void HandleErrorEvent() override;

  bool Notify();

 private:
  int read_fd_; // 读描述符
  int write_fd_; // 写描述符
};

}  // end namespace internal
}  // namespace pikiwidb
