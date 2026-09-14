// RPC 客户端（手写版，用来验证第 5-6 周的服务端反射分发）。
//
// 动态代理 / 连接池是第 7-8 周的事，这里直接手工拼 RpcMessage 发请求，
// 覆盖：正常调用、未知服务、未知方法、坏 payload、服务实现主动失败。
//
// 用法: ./rpc_client [ip] [port] [count] [threads]
//   threads > 1 时额外跑多连接并发验证
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "net/Buffer.h"
#include "protocol/RpcCodec.h"
#include "protocol/RpcError.h"
#include "rpc.pb.h"

using namespace mrpc;

namespace {

int connectTo(const char* ip, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0 ||
      ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
          0) {
    ::close(fd);
    return -1;
  }

  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

bool writeAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// 同步请求-响应：发一条请求，读到序号匹配的响应为止。
// 期间若收到服务端 ping 需回 ack，否则会被当成死连接踢掉。
bool roundTrip(int fd, Buffer* in, const RpcMessage& req, RpcMessage* resp) {
  Buffer out;
  RpcCodec::encode(req, kRpcRequest, &out);
  const std::string wire = out.retrieveAllAsString();
  if (!writeAll(fd, wire.data(), wire.size())) return false;

  char tmp[65536];
  uint8_t type = 0;
  while (true) {
    while (true) {
      const RpcCodec::ParseResult r = RpcCodec::parse(in, &type, resp);
      if (r == RpcCodec::kIncomplete) break;
      if (r == RpcCodec::kError) return false;

      if (type == kRpcHeartbeat) {
        Buffer ackBuf;
        RpcMessage ack;
        ack.set_seq(resp->seq());
        RpcCodec::encode(ack, kRpcHeartbeatAck, &ackBuf);
        const std::string a = ackBuf.retrieveAllAsString();
        if (!writeAll(fd, a.data(), a.size())) return false;
        continue;
      }
      if (type == kRpcHeartbeatAck) continue;

      // 单连接顺序请求-响应，序号必然匹配
      return resp->seq() == req.seq();
    }

    const ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n <= 0) return false;
    in->append(tmp, static_cast<size_t>(n));
  }
}

RpcMessage makeEchoRequest(uint64_t seq, const std::string& text,
                           const std::string& service = "mrpc.EchoService",
                           const std::string& method = "Echo") {
  EchoRequest req;
  req.set_text(text);
  RpcMessage msg;
  msg.set_seq(seq);
  msg.set_service_name(service);
  msg.set_method_name(method);
  msg.set_payload(req.SerializeAsString());
  return msg;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 9300;
  int count = argc > 3 ? ::atoi(argv[3]) : 1000;
  int threads = argc > 4 ? ::atoi(argv[4]) : 1;

  int failures = 0;

  // ---- 阶段 1：单连接正常调用 ----
  {
    const int fd = connectTo(ip, port);
    if (fd < 0) {
      perror("connect");
      return 1;
    }
    Buffer in;
    int ok = 0, badSeq = 0, badPayload = 0;
    for (int i = 0; i < count; ++i) {
      const RpcMessage req =
          makeEchoRequest(static_cast<uint64_t>(i), "hello-" + std::to_string(i));
      RpcMessage resp;
      if (!roundTrip(fd, &in, req, &resp)) {
        printf("请求 %d 无响应\n", i);
        ++failures;
        break;
      }
      if (resp.seq() != static_cast<uint64_t>(i)) ++badSeq;
      if (resp.error_code() != kRpcOk) {
        ++failures;
        continue;
      }
      EchoResponse echoResp;
      if (!echoResp.ParseFromString(resp.payload()) ||
          echoResp.text() != "echo:hello-" + std::to_string(i)) {
        ++badPayload;
      } else {
        ++ok;
      }
    }
    printf("正常调用: 成功 %d/%d, 序号异常 %d, 内容异常 %d\n", ok, count, badSeq,
           badPayload);
    if (ok != count || badSeq != 0 || badPayload != 0) ++failures;

    // ---- 阶段 2：错误码用例 ----
    printf("错误码用例:\n");
    const std::string validPayload =
        makeEchoRequest(0, "x").payload();
    const std::string emptyTextPayload = makeEchoRequest(0, "").payload();
    const std::string malformedPayload("\xff\xff\xff", 3);

    struct Case {
      const char* name;
      std::string service;
      std::string method;
      std::string payload;
      int32_t expect;
    };
    const std::vector<Case> cases = {
        {"未知服务", "mrpc.NoSuch", "Echo", validPayload, kRpcUnknownService},
        {"未知方法", "mrpc.EchoService", "Nope", validPayload,
         kRpcUnknownMethod},
        {"坏 payload", "mrpc.EchoService", "Echo", malformedPayload,
         kRpcBadRequest},
        {"业务失败", "mrpc.EchoService", "Echo", emptyTextPayload,
         kRpcServiceFailed},
    };

    uint64_t seq = static_cast<uint64_t>(count);
    for (const auto& c : cases) {
      RpcMessage req;
      req.set_seq(seq++);
      req.set_service_name(c.service);
      req.set_method_name(c.method);
      req.set_payload(c.payload);

      RpcMessage resp;
      if (!roundTrip(fd, &in, req, &resp)) {
        printf("  %-10s -> 无响应 [FAIL]\n", c.name);
        ++failures;
        continue;
      }
      const bool pass = resp.error_code() == c.expect;
      printf("  %-10s -> error_code=%d (期望 %d) %s\n", c.name,
             resp.error_code(), c.expect, pass ? "[OK]" : "[FAIL]");
      if (!pass) ++failures;
    }

    // 连接在业务错误后仍应可用（错误不断连）
    {
      const uint64_t checkSeq = seq++;
      RpcMessage req = makeEchoRequest(checkSeq, "still-alive");
      RpcMessage resp;
      const bool alive =
          roundTrip(fd, &in, req, &resp) && resp.error_code() == kRpcOk;
      printf("错误后连接仍可用: %s\n", alive ? "[OK]" : "[FAIL]");
      if (!alive) ++failures;
    }
    ::close(fd);
  }

  // ---- 阶段 3：多连接并发（验证注册表运行期只读、多 IO 线程安全）----
  if (threads > 1) {
    std::atomic<int> totalFail{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        const int fd = connectTo(ip, port);
        if (fd < 0) {
          ++totalFail;
          return;
        }
        Buffer in;
        for (int i = 0; i < count; ++i) {
          RpcMessage req = makeEchoRequest(
              static_cast<uint64_t>(i),
              "t" + std::to_string(t) + "-" + std::to_string(i));
          RpcMessage resp;
          if (!roundTrip(fd, &in, req, &resp) || resp.error_code() != kRpcOk) {
            ++totalFail;
            break;
          }
        }
        ::close(fd);
      });
    }
    for (auto& th : pool) th.join();
    printf("并发 %d 连接 x %d 条: 失败 %d\n", threads, count, totalFail.load());
    failures += totalFail.load();
  }

  printf("%s\n", failures == 0 ? "RPC 服务端验证通过" : "RPC 服务端验证失败");
  return failures == 0 ? 0 : 1;
}
