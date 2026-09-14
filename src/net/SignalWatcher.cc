#include "net/SignalWatcher.h"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace mrpc {

SignalWatcher::SignalWatcher(EventLoop* loop, const std::vector<int>& signals,
                             SignalCallback cb)
    : loop_(loop), sigfd_(-1), callback_(std::move(cb)), running_(false) {
  sigset_t mask;
  ::sigemptyset(&mask);
  for (int signo : signals) {
    ::sigaddset(&mask, signo);
  }

  // 先屏蔽信号，再建 signalfd。
  // 屏蔽必须发生在其他线程创建之前，它们才会继承同一份掩码。
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
    LOG_FATAL << "SignalWatcher: sigprocmask failed: " << strerror(errno);
  }

  sigfd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sigfd_ < 0) {
    LOG_FATAL << "SignalWatcher: signalfd failed: " << strerror(errno);
  }

  channel_.reset(new Channel(loop, sigfd_));
  channel_->setReadCallback([this] { handleRead(); });
}

SignalWatcher::~SignalWatcher() {
  stop();
  channel_->disableAll();
  channel_->remove();
  ::close(sigfd_);
}

void SignalWatcher::start() {
  if (running_) return;
  loop_->assertInLoopThread();
  channel_->enableReading();
  running_ = true;
}

void SignalWatcher::stop() {
  if (!running_) return;
  channel_->disableAll();
  running_ = false;
}

void SignalWatcher::handleRead() {
  struct signalfd_siginfo info;
  // 一次可能有多个信号积压（比如连按两次 Ctrl-C），必须读到 EAGAIN 为止，
  // 否则 signalfd 一直是可读的，epoll 会反复唤醒（LT 模式下的忙循环）
  while (true) {
    const ssize_t n = ::read(sigfd_, &info, sizeof(info));
    if (n != static_cast<ssize_t>(sizeof(info))) {
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR << "SignalWatcher: read failed: " << strerror(errno);
      }
      return;
    }
    LOG_INFO << "SignalWatcher - received signal " << info.ssi_signo;
    if (callback_) {
      callback_(static_cast<int>(info.ssi_signo));
    }
  }
}

}  // namespace mrpc
