#pragma once

#include <functional>
#include <utility>

#include "common/Noncopyable.h"
#include "net/Channel.h"
#include "net/Socket.h"

namespace mrpc {

class EventLoop;
class InetAddress;

// 监听 socket：只负责 accept 新连接并回调上层
class Acceptor : public Noncopyable {
 public:
  using NewConnectionCallback = std::function<void(int sockfd,
                                                   const InetAddress&)>;

  Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
  ~Acceptor();

  void setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
  }

  bool listenning() const { return listenning_; }
  void listen();

 private:
  void handleRead();

  EventLoop* loop_;
  Socket acceptSocket_;
  Channel acceptChannel_;
  NewConnectionCallback newConnectionCallback_;
  bool listenning_;
};

}  // namespace mrpc
