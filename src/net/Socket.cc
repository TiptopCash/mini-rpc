#include "net/Socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <errno.h>

#include "common/Logger.h"
#include "net/InetAddress.h"

namespace mrpc {

int createNonblocking() {
  int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        IPPROTO_TCP);
  if (sockfd < 0) {
    LOG_FATAL << "socket() failed: " << strerror(errno);
  }
  return sockfd;
}

int getSocketError(int sockfd) {
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  if (::getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
    return errno;
  }
  return optval;
}

Socket::~Socket() {
  if (sockfd_ >= 0) {
    ::close(sockfd_);
  }
}

int Socket::release() {
  const int fd = sockfd_;
  sockfd_ = -1;
  return fd;
}

void Socket::bindAddress(const InetAddress& localaddr) {
  if (::bind(sockfd_, reinterpret_cast<const struct sockaddr*>(
                          &localaddr.getSockAddr()),
             sizeof(struct sockaddr_in)) < 0) {
    LOG_FATAL << "bind() failed: " << strerror(errno);
  }
}

void Socket::listen() {
  if (::listen(sockfd_, SOMAXCONN) < 0) {
    LOG_FATAL << "listen() failed: " << strerror(errno);
  }
}

int Socket::accept(InetAddress* peeraddr) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t addrlen = sizeof(addr);
  int connfd = ::accept4(sockfd_, reinterpret_cast<struct sockaddr*>(&addr),
                         &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (connfd >= 0) {
    peeraddr->setSockAddr(addr);
  }
  return connfd;
}

void Socket::shutdownWrite() { ::shutdown(sockfd_, SHUT_WR); }

void Socket::setReuseAddr(bool on) {
  int optval = on ? 1 : 0;
  ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

void Socket::setReusePort(bool on) {
  int optval = on ? 1 : 0;
  ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

void Socket::setTcpNoDelay(bool on) {
  int optval = on ? 1 : 0;
  ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void Socket::setKeepAlive(bool on) {
  int optval = on ? 1 : 0;
  ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}

}  // namespace mrpc
