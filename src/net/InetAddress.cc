#include "net/InetAddress.h"

#include <arpa/inet.h>

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

}  // namespace mrpc
