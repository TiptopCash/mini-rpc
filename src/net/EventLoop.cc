#include "net/EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstring>
#include <errno.h>
#include <utility>

#include "common/Logger.h"
#include "net/Channel.h"
#include "net/Epoller.h"

namespace mrpc {

namespace {

const int kPollTimeMs = 10000;

thread_local EventLoop* t_loopInThisThread = nullptr;

int createEventfd() {
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0) {
    LOG_FATAL << "eventfd failed: " << strerror(errno);
  }
  return evtfd;
}

}  // namespace

EventLoop* EventLoop::getEventLoopOfCurrentThread() {
  return t_loopInThisThread;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      threadId_(std::this_thread::get_id()),
      poller_(new Epoller(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      callingPendingFunctors_(false) {
  if (t_loopInThisThread) {
    LOG_FATAL << "Another EventLoop already exists in this thread";
  } else {
    t_loopInThisThread = this;
  }
  wakeupChannel_->setReadCallback([this] { handleWakeup(); });
  wakeupChannel_->enableReading();
}

EventLoop::~EventLoop() {
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = nullptr;
}

void EventLoop::loop() {
  assertInLoopThread();
  looping_ = true;
  quit_ = false;

  while (!quit_) {
    activeChannels_.clear();
    poller_->poll(kPollTimeMs, &activeChannels_);
    for (Channel* channel : activeChannels_) {
      channel->handleEvent();
    }
    doPendingFunctors();
  }

  looping_ = false;
}

void EventLoop::quit() {
  quit_ = true;
  if (!isInLoopThread()) {
    wakeup();
  }
}

void EventLoop::runInLoop(Functor cb) {
  if (isInLoopThread()) {
    cb();
  } else {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor cb) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingFunctors_.push_back(std::move(cb));
  }
  // 若非 loop 线程，需要唤醒以便尽快执行；若正在执行 pending functors，
  // 该回调可能又投递了新任务，也必须唤醒
  if (!isInLoopThread() || callingPendingFunctors_) {
    wakeup();
  }
}

bool EventLoop::isInLoopThread() const {
  return threadId_ == std::this_thread::get_id();
}

void EventLoop::assertInLoopThread() {
  if (!isInLoopThread()) {
    LOG_FATAL << "EventLoop::assertInLoopThread - not in loop thread";
  }
}

void EventLoop::wakeup() {
  uint64_t one = 1;
  ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
  if (n != sizeof(one)) {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes";
  }
}

void EventLoop::updateChannel(Channel* channel) {
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel* channel) {
  assertInLoopThread();
  poller_->removeChannel(channel);
}

void EventLoop::handleWakeup() {
  uint64_t one = 1;
  ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
  (void)n;
}

void EventLoop::doPendingFunctors() {
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    functors.swap(pendingFunctors_);
  }
  for (const Functor& functor : functors) {
    functor();
  }
  callingPendingFunctors_ = false;
}

}  // namespace mrpc
