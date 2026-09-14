#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "common/Noncopyable.h"

namespace mrpc {

class Channel;
class EventLoop;

// 通用定时器集合：一个 EventLoop 共用一个 timerfd + 一个最小堆。
//
// 与 PeriodicTimer（每个定时器独占一个 timerfd）的分工：
//   - PeriodicTimer 只服务于「固定周期、长期存在」的心跳扫描，简单直白
//   - TimerQueue 服务于数量不定、生命周期短的定时器（RPC 超时、连接重试），
//     把「最近到期时刻」换算成 timerfd 的到期时间，堆里维护全部定时器：
//     万级定时器也只占一个 fd、一次 epoll_wait 唤醒
//
// 所有回调在 loop 线程执行，因此可以安全地在回调里增删定时器。
class TimerQueue : public Noncopyable {
 public:
  using TimerCallback = std::function<void()>;
  using TimerId = uint64_t;

  explicit TimerQueue(EventLoop* loop);
  ~TimerQueue();

  // delayMs 毫秒后执行一次
  TimerId addTimer(int64_t delayMs, TimerCallback cb);
  // 首次在 intervalMs 后、之后每隔 intervalMs 执行一次
  TimerId addRepeatingTimer(int64_t intervalMs, TimerCallback cb);

  // 取消定时器。可从任意线程调用；对已到期或已取消的 id 静默忽略
  void cancelTimer(TimerId id);

  // 存活（未取消、未到期）的定时器数量。仅供 loop 线程内断言/自测使用
  size_t size() const;

 private:
  struct Timer {
    TimerId id = 0;
    int64_t expiration = 0;   // 绝对到期时刻（微秒，steady_clock）
    int64_t intervalMs = 0;   // 0 表示一次性
    TimerCallback cb;
    bool canceled = false;
  };

  // 最小堆：std::priority_queue 默认是最大堆，这里反转比较让堆顶最早到期
  struct LaterFirst {
    bool operator()(const std::shared_ptr<Timer>& lhs,
                    const std::shared_ptr<Timer>& rhs) const {
      return lhs->expiration > rhs->expiration;
    }
  };
  using TimerHeap =
      std::priority_queue<std::shared_ptr<Timer>, std::vector<std::shared_ptr<Timer>>,
                          LaterFirst>;

  TimerId schedule(int64_t expiration, int64_t intervalMs, TimerCallback cb);

  void insert(const std::shared_ptr<Timer>& timer);
  void cancelInLoop(TimerId id);
  void handleRead();
  // 把 timerfd 重新 arm 到堆顶定时器的到期时刻；堆空则解除定时
  void resetTimerfd();

  EventLoop* loop_;
  int timerfd_;
  std::unique_ptr<Channel> channel_;
  TimerHeap heap_;
  // id -> 定时器，用于取消时 O(1) 定位；只在 loop 线程访问
  std::unordered_map<TimerId, std::shared_ptr<Timer>> active_;
  // 调用线程就分配好 id，跨线程调用时也能立刻拿到句柄
  std::atomic<TimerId> nextId_;
};

}  // namespace mrpc
