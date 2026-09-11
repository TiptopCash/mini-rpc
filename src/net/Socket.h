#pragma once

#include "common/Noncopyable.h"

namespace mrpc {

class InetAddress;

// 创建一个非阻塞、close-on-exec 的 TCP socket
int createNonblocking();

class Socket : public Noncopyable {
 public:
  explicit Socket(int sockfd) : sockfd_(sockfd) {}
  ~Socket();

  int fd() const { return sockfd_; }

  void bindAddress(const InetAddress& localaddr);
  void listen();
  // 返回已连接的 fd（非阻塞），失败返回 -1
  int accept(InetAddress* peeraddr);

  void shutdownWrite();

  void setReuseAddr(bool on);
  void setReusePort(bool on);
  void setTcpNoDelay(bool on);
  void setKeepAlive(bool on);

 private:
  const int sockfd_;
};

}  // namespace mrpc
