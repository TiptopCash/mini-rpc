#pragma once

#include "common/Noncopyable.h"

namespace mrpc {

class InetAddress;

// 创建一个非阻塞、close-on-exec 的 TCP socket
int createNonblocking();

// 读取并清除 socket 上的待处理错误（SO_ERROR）。
// 非阻塞 connect 的结果只能这样拿到：EPOLLOUT 只说明「有结果了」，不区分成败。
int getSocketError(int sockfd);

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

  // 交出 fd 的所有权（析构时不再 close）。
  // Connector 连接成功后要把 fd 移交给 TcpConnection：如果这里不放弃所有权，
  // Socket 析构会把刚建立的连接关掉。
  int release();

 private:
  int sockfd_;
};

}  // namespace mrpc
