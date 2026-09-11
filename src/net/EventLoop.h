#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Noncopyable.h"

namespace mrpc {

class Channel;
class Epoller;

// one loop per thread：事件循环
class EventLoop : public Noncopyable {
 public:
  using Functor = std::function<void()>;

  EventLoop();
  ~EventLoop();

  void loop();
  void quit();

  // 若在 loop 线程则直接执行，否则投递到 loop 线程执行
  void runInLoop(Functor cb);
  void queueInLoop(Functor cb);

  bool isInLoopThread() const;
  void assertInLoopThread();
  void wakeup();

  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);

  static EventLoop* getEventLoopOfCurrentThread();

 private:
  void handleWakeup();
  void doPendingFunctors();

  std::atomic<bool> looping_;
  std::atomic<bool> quit_;
  const std::thread::id threadId_;
  std::unique_ptr<Epoller> poller_;
  std::vector<Channel*> activeChannels_;

  int wakeupFd_;
  std::unique_ptr<Channel> wakeupChannel_;

  std::mutex mutex_;
  std::vector<Functor> pendingFunctors_;
  std::atomic<bool> callingPendingFunctors_;
};

}  // namespace mrpc
