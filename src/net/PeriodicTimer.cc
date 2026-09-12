#include "net/PeriodicTimer.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>
#include <errno.h>

#include "common/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace mrpc {

PeriodicTimer::PeriodicTimer(EventLoop* loop, int64_t intervalMs, Callback cb)
    : loop_(loop),
      timerfd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      channel_(nullptr),
      intervalMs_(intervalMs),
      callback_(std::move(cb)),
      running_(false) {
  if (timerfd_ < 0) {
    LOG_FATAL << "PeriodicTimer: timerfd_create failed: " << strerror(errno);
  }
  channel_.reset(new Channel(loop, timerfd_));
  channel_->setReadCallback([this] { handleRead(); });
}

PeriodicTimer::~PeriodicTimer() {
  stop();
  channel_->disableAll();
  channel_->remove();
  ::close(timerfd_);
}

void PeriodicTimer::start() {
  if (running_) return;

  struct itimerspec spec;
  memset(&spec, 0, sizeof(spec));
  spec.it_interval.tv_sec = intervalMs_ / 1000;
  spec.it_interval.tv_nsec = (intervalMs_ % 1000) * 1000000L;
  spec.it_value = spec.it_interval;  // 首次到期与周期相同

  if (::timerfd_settime(timerfd_, 0, &spec, nullptr) < 0) {
    LOG_FATAL << "PeriodicTimer: timerfd_settime failed: " << strerror(errno);
  }
  channel_->enableReading();
  running_ = true;
}

void PeriodicTimer::stop() {
  if (!running_) return;

  struct itimerspec spec;  // 全 0 表示解除定时
  memset(&spec, 0, sizeof(spec));
  ::timerfd_settime(timerfd_, 0, &spec, nullptr);
  channel_->disableAll();
  running_ = false;
}

void PeriodicTimer::handleRead() {
  uint64_t expirations = 0;
  ssize_t n = ::read(timerfd_, &expirations, sizeof(expirations));
  if (n != static_cast<ssize_t>(sizeof(expirations))) {
    LOG_ERROR << "PeriodicTimer: read failed: " << strerror(errno);
    return;
  }

  // 若事件循环被阻塞过久，可能一次性堆积多次到期。
  // 对心跳扫描而言补跑多次没有意义（当前状态扫描一次即可），只记录告警。
  if (expirations > 1) {
    LOG_WARN << "PeriodicTimer: " << expirations
             << " ticks coalesced (event loop was busy)";
  }
  if (callback_) callback_();
}

}  // namespace mrpc
