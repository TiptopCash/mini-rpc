#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "common/Noncopyable.h"

namespace mrpc {

class EventLoop;

// 在独立线程中运行一个 EventLoop
class EventLoopThread : public Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  explicit EventLoopThread(
      const ThreadInitCallback& cb = ThreadInitCallback());
  ~EventLoopThread();

  EventLoop* startLoop();

 private:
  void threadFunc();

  EventLoop* loop_;
  bool exiting_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cond_;
  ThreadInitCallback callback_;
};

}  // namespace mrpc
