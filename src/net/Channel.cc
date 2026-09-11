#include "net/Channel.h"

#include <sys/epoll.h>

#include "common/Logger.h"
#include "net/EventLoop.h"

namespace mrpc {

const uint32_t Channel::kNoneEvent = 0;
const uint32_t Channel::kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
const uint32_t Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd), events_(0) {}

Channel::~Channel() {
  // 正常流程下调用方已 disableAll + remove，这里仅兜底
  if (!isNoneEvent()) {
    remove();
  }
}

void Channel::tie(const std::shared_ptr<void>& obj) {
  tie_ = obj;
  tied_ = true;
}

void Channel::remove() { loop_->removeChannel(this); }

void Channel::update() { loop_->updateChannel(this); }

void Channel::handleEvent() {
  if (tied_) {
    std::shared_ptr<void> guard = tie_.lock();
    if (guard) {
      handleEventWithGuard();
    }
  } else {
    handleEventWithGuard();
  }
}

void Channel::handleEventWithGuard() {
  if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
    if (closeCallback_) closeCallback_();
  }
  if (revents_ & EPOLLERR) {
    if (errorCallback_) errorCallback_();
  }
  if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
    if (readCallback_) readCallback_();
  }
  if (revents_ & EPOLLOUT) {
    if (writeCallback_) writeCallback_();
  }
}

void Channel::enableReading() {
  events_ |= kReadEvent;
  update();
}

void Channel::disableReading() {
  events_ &= ~kReadEvent;
  update();
}

void Channel::enableWriting() {
  events_ |= kWriteEvent;
  update();
}

void Channel::disableWriting() {
  events_ &= ~kWriteEvent;
  update();
}

void Channel::disableAll() {
  events_ = kNoneEvent;
  update();
}

}  // namespace mrpc
