#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <cstring>

namespace mrpc {

InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
  memset(&addr_, 0, sizeof(addr_));
  addr_.sin_family = AF_INET;
  in_addr_t ip = loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY;
  addr_.sin_addr.s_addr = htonl(ip);
  addr_.sin_port = htons(port);
}

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
  memset(&addr_, 0, sizeof(addr_));
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
    addr_.sin_addr.s_addr = htonl(INADDR_ANY);
  }
}

std::string InetAddress::toIp() const {
  char buf[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
  return buf;
}

std::string InetAddress::toIpPort() const {
  char buf[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
  return std::string(buf) + ":" + std::to_string(ntohs(addr_.sin_port));
}

uint16_t InetAddress::port() const { return ntohs(addr_.sin_port); }

InetAddress localAddressOf(int sockfd) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
    memset(&addr, 0, sizeof(addr));
  }
  return InetAddress(addr);
}

}  // namespace mrpc
