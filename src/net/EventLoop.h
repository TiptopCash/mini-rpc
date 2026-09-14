#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Noncopyable.h"
#include "net/TimerQueue.h"

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

  using TimerId = TimerQueue::TimerId;

  // 定时器。可在任意线程调用，回调保证在 loop 线程执行：
  //   runAfter(100, cb)  -> 100ms 后执行一次
  //   runEvery(1000, cb) -> 首次 1s 后，之后每秒一次
  TimerId runAfter(int64_t delayMs, TimerQueue::TimerCallback cb);
  TimerId runEvery(int64_t intervalMs, TimerQueue::TimerCallback cb);
  void cancelTimer(TimerId id);

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

  // 声明在最后 -> 最先销毁，且此时 poller_ 仍存活（~TimerQueue 要摘除自己的 fd）
  std::unique_ptr<TimerQueue> timerQueue_;
};

}  // namespace mrpc
