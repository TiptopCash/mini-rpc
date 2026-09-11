#pragma once

#if !defined(__linux__)
#error \
    "This project depends on Linux epoll. Please build inside WSL2 or a Linux machine."
#endif

#include <sys/epoll.h>

#include <unordered_map>
#include <vector>

#include "common/Noncopyable.h"

namespace mrpc {

class Channel;
class EventLoop;

// 对 epoll 的极简封装
class Epoller : public Noncopyable {
 public:
  using ChannelList = std::vector<Channel*>;

  explicit Epoller(EventLoop* loop);
  ~Epoller();

  // 等待事件，将活跃 Channel 填入 activeChannels
  void poll(int timeoutMs, ChannelList* activeChannels);

  void updateChannel(Channel* channel);
  void removeChannel(Channel* channel);

 private:
  void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
  void update(int operation, Channel* channel);

  EventLoop* loop_;
  int epollfd_;
  std::vector<struct epoll_event> events_;
  std::unordered_map<int, Channel*> channels_;
};

}  // namespace mrpc
