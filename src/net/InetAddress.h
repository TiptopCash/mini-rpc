#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string>

namespace mrpc {

class InetAddress {
 public:
  explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);
  InetAddress(const std::string& ip, uint16_t port);
  explicit InetAddress(const struct sockaddr_in& addr) : addr_(addr) {}

  std::string toIp() const;
  std::string toIpPort() const;
  uint16_t port() const;

  const struct sockaddr_in& getSockAddr() const { return addr_; }
  void setSockAddr(const struct sockaddr_in& addr) { addr_ = addr; }

 private:
  struct sockaddr_in addr_;
};

}  // namespace mrpc
