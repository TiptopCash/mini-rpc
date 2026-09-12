// 空闲连接保活 / 超时验证客户端。
//
// 用法: ./idle_client [ip] [port] [reply 0|1] [seconds]
//   reply=1: 收到心跳 ping 就回 ack，连接应一直存活
//   reply=0: 收到 ping 不回复，服务端应在 timeout 后强制断开
//
// 判定依据是「服务端是否主动断开」，即 read 返回 0。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <errno.h>

#include "net/Buffer.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

using namespace mrpc;

namespace {

bool writeAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 9200;
  const bool reply = argc > 3 ? (::atoi(argv[3]) != 0) : true;
  const int seconds = argc > 4 ? ::atoi(argv[4]) : 8;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    perror("inet_pton");
    return 1;
  }
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    perror("connect");
    return 1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // 200ms 轮询，便于观察连接何时被对端关闭
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 200000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  const auto start = std::chrono::steady_clock::now();
  Buffer in;
  char tmp[4096];
  int pings = 0;
  bool serverClosed = false;
  double elapsed = 0;

  while (true) {
    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            start)
                  .count();
    if (elapsed >= seconds) break;

    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n == 0) {
      serverClosed = true;
      break;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // 本轮无数据
      serverClosed = true;
      break;
    }
    in.append(tmp, static_cast<size_t>(n));

    uint8_t type = 0;
    RpcMessage msg;
    while (true) {
      const RpcCodec::ParseResult r = RpcCodec::parse(&in, &type, &msg);
      if (r == RpcCodec::kIncomplete) break;
      if (r == RpcCodec::kError) {
        printf("协议错误\n");
        ::close(fd);
        return 1;
      }
      if (type == kRpcHeartbeat) {
        ++pings;
        if (reply) {
          Buffer ackBuf;
          RpcMessage ack;
          ack.set_seq(msg.seq());
          RpcCodec::encode(ack, kRpcHeartbeatAck, &ackBuf);
          const std::string wire = ackBuf.retrieveAllAsString();
          writeAll(fd, wire.data(), wire.size());
        }
      }
    }
  }

  printf("模式: %s\n", reply ? "回复心跳(ack)" : "忽略心跳");
  printf("收到 ping: %d 次\n", pings);
  printf("连接状态: %s\n", serverClosed ? "已被服务端断开" : "仍然存活");
  printf("耗时: %.1fs\n", elapsed);

  ::close(fd);
  return 0;
}
