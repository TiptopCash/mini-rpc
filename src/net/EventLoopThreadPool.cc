#include "net/EventLoopThreadPool.h"

#include <utility>

#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

namespace mrpc {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop,
                                         std::string nameArg)
    : baseLoop_(baseLoop),
      name_(std::move(nameArg)),
      started_(false),
      numThreads_(0),
      next_(0) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
  started_ = true;

  for (int i = 0; i < numThreads_; ++i) {
    auto thread = std::make_unique<EventLoopThread>(cb);
    loops_.push_back(thread->startLoop());
    threads_.push_back(std::move(thread));
  }

  // 没有 IO 线程时，全部回落到 baseLoop
  if (numThreads_ == 0 && cb) {
    cb(baseLoop_);
  }
}

EventLoop* EventLoopThreadPool::getNextLoop() {
  EventLoop* loop = baseLoop_;
  if (!loops_.empty()) {
    loop = loops_[next_];
    next_ = (next_ + 1) % static_cast<int>(loops_.size());
  }
  return loop;
}

EventLoop* EventLoopThreadPool::getLoopForHash(size_t hashCode) {
  EventLoop* loop = baseLoop_;
  if (!loops_.empty()) {
    loop = loops_[hashCode % loops_.size()];
  }
  return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() {
  if (loops_.empty()) {
    return std::vector<EventLoop*>(1, baseLoop_);
  }
  return loops_;
}

}  // namespace mrpc
