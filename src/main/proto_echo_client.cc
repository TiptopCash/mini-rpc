// 协议层端到端验证客户端。
// 刻意制造两种 TCP 边界：粘包（一次写入多帧）与半包（一帧拆多次写入），
// 验证服务端分帧正确、响应序号与内容无一错漏。
//
// 用法: ./proto_echo_client [ip] [port] [count]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

std::string makeWire(uint64_t seq) {
  RpcMessage m;
  m.set_seq(seq);
  m.set_service_name("EchoService");
  m.set_method_name("Echo");
  m.set_payload("payload-" + std::to_string(seq));
  Buffer b;
  RpcCodec::encode(m, kRpcRequest, &b);
  return b.retrieveAllAsString();
}

std::string expectedPayload(uint64_t seq) {
  return "payload-" + std::to_string(seq);
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 9200;
  int count = argc > 3 ? ::atoi(argv[3]) : 1000;

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
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  const int coalescedCount = count / 2;  // 前半：粘包
  const int fragmentedCount = count - coalescedCount;

  // ---- 阶段 1：把多条消息拼成一个字节流一次写入（制造粘包）----
  {
    std::string blob;
    for (int i = 0; i < coalescedCount; ++i) {
      blob += makeWire(static_cast<uint64_t>(i));
    }
    if (!writeAll(fd, blob.data(), blob.size())) {
      perror("write coalesced");
      return 1;
    }
  }

  // ---- 阶段 2：每条消息拆成两段写入（制造半包）----
  for (int i = 0; i < fragmentedCount; ++i) {
    const std::string wire = makeWire(static_cast<uint64_t>(coalescedCount + i));
    const size_t half = wire.size() / 2;
    if (!writeAll(fd, wire.data(), half)) {
      perror("write fragment 1");
      return 1;
    }
    ::usleep(500);  // 让第一次 write 单独到达
    if (!writeAll(fd, wire.data() + half, wire.size() - half)) {
      perror("write fragment 2");
      return 1;
    }
  }

  // ---- 读取并校验全部响应 ----
  Buffer in;
  std::vector<uint64_t> seqs;
  std::vector<std::string> payloads;
  char tmp[65536];
  bool readError = false;

  while (static_cast<int>(seqs.size()) < count) {
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n <= 0) {
      readError = true;
      break;
    }
    in.append(tmp, static_cast<size_t>(n));

    uint8_t type = 0;
    RpcMessage msg;
    while (true) {
      const RpcCodec::ParseResult r = RpcCodec::parse(&in, &type, &msg);
      if (r == RpcCodec::kIncomplete) break;
      if (r == RpcCodec::kError) {
        printf("响应解析失败\n");
        ::close(fd);
        return 1;
      }
      seqs.push_back(msg.seq());
      payloads.push_back(msg.payload());
      if (static_cast<int>(seqs.size()) == count) break;
    }
  }

  // ---- 校验：序号连续且内容逐条一致 ----
  int badSeq = 0;
  int badPayload = 0;
  for (size_t i = 0; i < seqs.size(); ++i) {
    if (seqs[i] != i) ++badSeq;
    if (payloads[i] != expectedPayload(i)) ++badPayload;
  }

  const bool pass = static_cast<int>(seqs.size()) == count && badSeq == 0 &&
                    badPayload == 0;

  printf("发出: %d 条（粘包 %d + 半包 %d）\n", count, coalescedCount,
         fragmentedCount);
  printf("收回: %zu 条\n", seqs.size());
  if (readError && static_cast<int>(seqs.size()) < count) {
    printf("读取中断（超时或连接关闭）\n");
  }
  printf("序号连续性: %s（异常 %d）\n", badSeq == 0 ? "正确" : "错误", badSeq);
  printf("内容一致性: %s（异常 %d）\n", badPayload == 0 ? "正确" : "错误",
         badPayload);
  printf("%s\n", pass ? "端到端验证通过" : "端到端验证失败");

  ::close(fd);
  return pass ? 0 : 1;
}
