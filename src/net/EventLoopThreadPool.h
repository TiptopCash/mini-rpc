#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/Noncopyable.h"

namespace mrpc {

class EventLoop;
class EventLoopThread;

// 一组 IO 线程（sub reactor），新连接按 round-robin 分配
class EventLoopThreadPool : public Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;

  EventLoopThreadPool(EventLoop* baseLoop, std::string nameArg);
  ~EventLoopThreadPool();

  void setThreadNum(int numThreads) { numThreads_ = numThreads; }
  void start(const ThreadInitCallback& cb = ThreadInitCallback());

  EventLoop* getNextLoop();
  EventLoop* getLoopForHash(size_t hashCode);
  std::vector<EventLoop*> getAllLoops();

  bool started() const { return started_; }
  const std::string& name() const { return name_; }

 private:
  EventLoop* baseLoop_;
  std::string name_;
  bool started_;
  int numThreads_;
  int next_;
  std::vector<std::unique_ptr<EventLoopThread>> threads_;
  std::vector<EventLoop*> loops_;
};

}  // namespace mrpc
