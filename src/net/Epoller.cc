#include "net/Epoller.h"

#include <unistd.h>

#include <cstring>
#include <errno.h>

#include "common/Logger.h"
#include "net/Channel.h"

namespace mrpc {

Epoller::Epoller(EventLoop* loop)
    : loop_(loop), epollfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(16) {
  if (epollfd_ < 0) {
    LOG_FATAL << "epoll_create1 failed: " << strerror(errno);
  }
}

Epoller::~Epoller() {
  if (epollfd_ >= 0) {
    ::close(epollfd_);
  }
}

void Epoller::poll(int timeoutMs, ChannelList* activeChannels) {
  int numEvents = ::epoll_wait(epollfd_, events_.data(),
                               static_cast<int>(events_.size()), timeoutMs);
  int savedErrno = errno;

  if (numEvents > 0) {
    fillActiveChannels(numEvents, activeChannels);
    if (static_cast<size_t>(numEvents) == events_.size()) {
      events_.resize(events_.size() * 2);
    }
  } else if (numEvents < 0 && savedErrno != EINTR) {
    errno = savedErrno;
    LOG_ERROR << "epoll_wait failed: " << strerror(errno);
  }
}

void Epoller::fillActiveChannels(int numEvents,
                                 ChannelList* activeChannels) const {
  for (int i = 0; i < numEvents; ++i) {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
    channel->setRevents(events_[i].events);
    activeChannels->push_back(channel);
  }
}

void Epoller::updateChannel(Channel* channel) {
  const int fd = channel->fd();
  auto it = channels_.find(fd);
  if (it == channels_.end()) {
    if (!channel->isNoneEvent()) {
      channels_[fd] = channel;
      update(EPOLL_CTL_ADD, channel);
    }
  } else if (channel->isNoneEvent()) {
    update(EPOLL_CTL_DEL, channel);
    channels_.erase(it);
  } else {
    update(EPOLL_CTL_MOD, channel);
  }
}

void Epoller::removeChannel(Channel* channel) {
  auto it = channels_.find(channel->fd());
  if (it != channels_.end()) {
    channels_.erase(it);
    update(EPOLL_CTL_DEL, channel);
  }
}

void Epoller::update(int operation, Channel* channel) {
  struct epoll_event event;
  memset(&event, 0, sizeof(event));
  event.events = channel->events();
  event.data.ptr = channel;

  if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
    LOG_ERROR << "epoll_ctl op=" << operation << " fd=" << channel->fd()
              << " failed: " << strerror(errno);
  }
}

}  // namespace mrpc
