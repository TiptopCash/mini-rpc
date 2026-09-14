#include "net/TimerQueue.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>
#include <errno.h>
#include <utility>

#include "common/Logger.h"
#include "common/Timestamp.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace mrpc {

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      channel_(nullptr),
      nextId_(1) {
  if (timerfd_ < 0) {
    LOG_FATAL << "TimerQueue: timerfd_create failed: " << strerror(errno);
  }
  channel_.reset(new Channel(loop, timerfd_));
  channel_->setReadCallback([this] { handleRead(); });
  // 常驻监听：没有定时器时 timerfd 处于解除定时状态，不会产生事件，
  // 因此无需反复 enable/disable
  channel_->enableReading();
}

TimerQueue::~TimerQueue() {
  // 析构时不能再触发回调（loop 可能已退出），堆里的定时器直接放弃
  channel_->disableAll();
  channel_->remove();
  ::close(timerfd_);
}

TimerQueue::TimerId TimerQueue::addTimer(int64_t delayMs, TimerCallback cb) {
  // 到期时刻按「调用时刻」算，而不是插入堆的时刻：
  // 跨线程投递会延后插入，若按插入时刻算，实际延迟会被拉长
  return schedule(nowMicros() + delayMs * 1000, 0, std::move(cb));
}

TimerQueue::TimerId TimerQueue::addRepeatingTimer(int64_t intervalMs,
                                                  TimerCallback cb) {
  return schedule(nowMicros() + intervalMs * 1000, intervalMs, std::move(cb));
}

TimerQueue::TimerId TimerQueue::schedule(int64_t expiration, int64_t intervalMs,
                                        TimerCallback cb) {
  auto timer = std::make_shared<Timer>();
  timer->id = nextId_.fetch_add(1);
  timer->expiration = expiration;
  timer->intervalMs = intervalMs;
  timer->cb = std::move(cb);
  timer->canceled = false;

  // 堆与 active_ 都无锁，插入必须发生在 loop 线程；若已在 loop 线程则同步执行
  loop_->runInLoop([this, timer] { insert(timer); });
  return timer->id;
}

void TimerQueue::insert(const std::shared_ptr<Timer>& timer) {
  loop_->assertInLoopThread();

  // 只有新定时器比堆顶更早才需要重新 arm timerfd；
  // 否则 timerfd 已经指向同一个（或更早的）时刻，省一次系统调用
  const bool earliest =
      heap_.empty() || timer->expiration < heap_.top()->expiration;

  heap_.push(timer);
  active_[timer->id] = timer;

  if (earliest) {
    resetTimerfd();
  }
}

void TimerQueue::cancelTimer(TimerId id) {
  loop_->runInLoop([this, id] { cancelInLoop(id); });
}

void TimerQueue::cancelInLoop(TimerId id) {
  loop_->assertInLoopThread();

  auto it = active_.find(id);
  if (it == active_.end()) {
    return;  // 已到期或已取消
  }
  it->second->canceled = true;
  active_.erase(it);

  // 最小堆做不到 O(log n) 的任意位置删除，所以取消时只打标记、等它浮到堆顶再丢弃。
  // 但堆顶的已取消项必须现在就清掉：否则 timerfd 会为了一个死定时器把我们唤醒一次。
  while (!heap_.empty() && heap_.top()->canceled) {
    heap_.pop();
  }
  resetTimerfd();
}

void TimerQueue::handleRead() {
  loop_->assertInLoopThread();

  uint64_t expirations = 0;
  // 读走计数即可。LT 模式下不读会一直可读，导致 epoll 反复唤醒
  const ssize_t n = ::read(timerfd_, &expirations, sizeof(expirations));
  if (n != static_cast<ssize_t>(sizeof(expirations)) && errno != EAGAIN) {
    LOG_ERROR << "TimerQueue: read timerfd failed: " << strerror(errno);
  }

  const int64_t now = nowMicros();
  // 每轮都重新读堆顶：回调里可能又 addTimer，堆随时会变
  while (!heap_.empty() && heap_.top()->expiration <= now) {
    std::shared_ptr<Timer> timer = heap_.top();
    heap_.pop();

    if (timer->canceled) {
      continue;
    }

    if (timer->intervalMs > 0) {
      // 重复定时器以「原定到期时刻」推进，避免回调耗时逐次累积成漂移；
      // 若循环被阻塞太久已错过多个周期，则改为以「现在」为基准重排而不补跑——
      // 补跑对超时/心跳类定时器没有意义，只会把欠账集中放大成一次突发
      const int64_t intervalUs = timer->intervalMs * 1000;
      int64_t next = timer->expiration + intervalUs;
      if (next <= now) {
        next = now + intervalUs;
      }
      timer->expiration = next;
      heap_.push(timer);
    } else {
      timer->canceled = true;  // 标记已执行，迟到的 cancelTimer 成为 no-op
      active_.erase(timer->id);
    }

    if (timer->cb) {
      timer->cb();
    }
  }

  resetTimerfd();
}

void TimerQueue::resetTimerfd() {
  struct itimerspec spec;
  memset(&spec, 0, sizeof(spec));

  if (!heap_.empty()) {
    // 用相对时间而非 TFD_TIMER_ABSTIME：不必假设 steady_clock 与 CLOCK_MONOTONIC
    // 是同一个时钟源（绝对时间戳一旦对不上就是灾难性的静默错误）
    int64_t delayUs = heap_.top()->expiration - nowMicros();
    if (delayUs < 1) {
      delayUs = 1;  // it_value 为 0 表示解除定时，必须避开
    }
    spec.it_value.tv_sec = delayUs / 1000000;
    spec.it_value.tv_nsec = (delayUs % 1000000) * 1000;
  }
  // it_value 全 0 = 解除定时

  if (::timerfd_settime(timerfd_, 0, &spec, nullptr) < 0) {
    LOG_ERROR << "TimerQueue: timerfd_settime failed: " << strerror(errno);
  }
}

size_t TimerQueue::size() const {
  loop_->assertInLoopThread();
  return active_.size();
}

}  // namespace mrpc
