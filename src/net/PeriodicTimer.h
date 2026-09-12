#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "common/Noncopyable.h"

namespace mrpc {

class Channel;
class EventLoop;

// 基于 timerfd 的周期性定时器。
// 到期时 timerfd 变为可读，由 EventLoop 的 epoll 唤醒并执行回调——
// 无需轮询、无需调整 epoll_wait 超时，天然融入 Reactor。
class PeriodicTimer : public Noncopyable {
 public:
  using Callback = std::function<void()>;

  PeriodicTimer(EventLoop* loop, int64_t intervalMs, Callback cb);
  ~PeriodicTimer();

  void start();
  void stop();

 private:
  void handleRead();

  EventLoop* loop_;
  int timerfd_;
  std::unique_ptr<Channel> channel_;
  int64_t intervalMs_;
  Callback callback_;
  bool running_;
};

}  // namespace mrpc
