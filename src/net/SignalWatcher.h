#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "common/Noncopyable.h"

namespace mrpc {

class Channel;
class EventLoop;

// 基于 signalfd 的信号监听：把信号变成 fd 上的可读事件，纳入 epoll。
//
// 与 eventfd（唤醒阻塞的 epoll_wait）、timerfd（定时器）是同一套思路——
// 信号不再是异步的「信号处理函数」，而是普通的 IO 事件。因此回调里可以
// 安全地做任何事（真正的 signal handler 里只能调用 async-signal-safe 函数，
// 连 LOG_INFO 都不合规）。
//
// ⚠️ 构造函数会调用 sigprocmask 屏蔽这些信号，且**必须在创建任何 IO 线程之前**
// 构造。新线程会继承创建时刻的信号掩码；若构造晚了，其他线程仍按默认动作
// 处理 SIGINT，进程会被直接终止，优雅退出失效。
class SignalWatcher : public Noncopyable {
 public:
  using SignalCallback = std::function<void(int signo)>;

  SignalWatcher(EventLoop* loop, const std::vector<int>& signals,
                SignalCallback cb);
  ~SignalWatcher();

  void start();
  void stop();

 private:
  void handleRead();

  EventLoop* loop_;
  int sigfd_;
  std::unique_ptr<Channel> channel_;
  SignalCallback callback_;
  bool running_;
};

}  // namespace mrpc
