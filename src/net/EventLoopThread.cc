#include "net/EventLoopThread.h"

#include "net/EventLoop.h"

namespace mrpc {

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop_(nullptr), exiting_(false), callback_(cb) {}

EventLoopThread::~EventLoopThread() {
  exiting_ = true;
  if (loop_ != nullptr) {
    loop_->quit();
  }
  // 线程可能已自行结束（loop_ 被置空），但 thread_ 仍 joinable，必须 join
  if (thread_.joinable()) {
    thread_.join();
  }
}

EventLoop* EventLoopThread::startLoop() {
  thread_ = std::thread(&EventLoopThread::threadFunc, this);

  EventLoop* loop = nullptr;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return loop_ != nullptr; });
    loop = loop_;
  }
  return loop;
}

void EventLoopThread::threadFunc() {
  EventLoop loop;  // 栈上创建，生命周期与线程一致

  if (callback_) {
    callback_(&loop);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = &loop;
    cond_.notify_one();
  }

  loop.loop();

  loop_ = nullptr;
}

}  // namespace mrpc
