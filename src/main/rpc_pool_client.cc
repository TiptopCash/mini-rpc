// 连接池 + 负载均衡示例：一个 ConnectionPool 对两个后端节点发请求。
//
// 验证三件事：
//   1. 轮询：同一批请求被分散到两个节点上
//   2. 一致性哈希：同一个 key 始终落到同一个节点
//   3. 失败转移：一个节点挂掉时，请求仍能由另一个节点服务
//
// 靠服务端的 nodeTag 分辨响应来自哪个节点（见 rpc_server.cc）。
// 用法: ./rpc_pool_client [ip] [portA] [portB] [count] [mode]
//   mode: all（默认，跑 1+2）/ rr / hash / failover
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "net/InetAddress.h"
#include "protocol/ConnectionPool.h"
#include "protocol/LoadBalancer.h"
#include "protocol/RpcChannel.h"
#include "protocol/RpcController.h"
#include "rpc.pb.h"

using namespace mrpc;

namespace {

// "echo:<text>@<tag>" -> tag；没有 @ 说明没配 nodeTag
std::string tagOf(const std::string& text) {
  const size_t at = text.rfind('@');
  return at == std::string::npos ? std::string() : text.substr(at + 1);
}

struct Stats {
  std::map<std::string, int> perNode;
  int ok = 0;
  int failed = 0;
};

// 发 count 个请求，key 为空表示不要求粘性
Stats runCalls(ConnectionPool& pool, int count, const std::string& key,
               const std::string& prefix) {
  Stats s;
  for (int i = 0; i < count; ++i) {
    const std::string text = prefix + std::to_string(i);
    auto channel = pool.getChannel(key);
    if (!channel) {
      ++s.failed;
      continue;
    }
    EchoRequest req;
    req.set_text(text);
    EchoResponse resp;
    RpcController ctl;
    EchoService::Stub stub(channel.get());  // 业务侧照常面对生成的 Stub
    stub.Echo(&ctl, &req, &resp, nullptr);
    const std::string expected = "echo:" + text;
    if (!ctl.Failed() && resp.text().compare(0, expected.size(), expected) == 0) {
      ++s.ok;
      ++s.perNode[tagOf(resp.text())];
    } else {
      ++s.failed;
    }
  }
  return s;
}

void printStats(const char* title, const Stats& s, int count) {
  printf("%s: 成功 %d/%d", title, s.ok, count);
  for (const auto& kv : s.perNode) {
    printf("  [%s]=%d", kv.first.empty() ? "?" : kv.first.c_str(), kv.second);
  }
  printf("\n");
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  const uint16_t portA = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 9360;
  const uint16_t portB = argc > 3 ? static_cast<uint16_t>(::atoi(argv[3])) : 9361;
  const int count = argc > 4 ? ::atoi(argv[4]) : 100;
  const std::string mode = argc > 5 ? argv[5] : "all";

  int failures = 0;

  {
    ConnectionPool pool(3000);
    pool.setNodes({InetAddress(ip, portA), InetAddress(ip, portB)});

    if (mode == "all" || mode == "rr") {
      printf("---- 轮询 ----\n");
      const Stats s = runCalls(pool, count, "", "rr-");
      printStats("轮询", s, count);
      // 两个节点都该分到流量；真轮询下差值不会超过 1
      const bool spread = s.perNode.size() >= 2;
      if (!spread || s.failed != 0) {
        printf("轮询未把请求分散到两个节点 [FAIL]\n");
        ++failures;
      }
    }

    if (mode == "all" || mode == "hash") {
      printf("---- 一致性哈希（固定 key）----\n");
      // 传空 unique_ptr 会被拒绝，这里显式指定策略
      pool.setLoadBalancer(std::make_unique<ConsistentHashLoadBalancer>());
      const std::string key = "user-42";
      const Stats s = runCalls(pool, count, key, "hs-");
      printStats("一致性哈希", s, count);
      const bool pinned = s.perNode.size() == 1;
      if (!pinned || s.failed != 0) {
        printf("同一 key 未固定到同一节点 [FAIL]\n");
        ++failures;
      }
      // 换个 key 仍应只落在一个节点上（不要求与上一个 key 不同节点）
      const Stats s2 = runCalls(pool, count, "user-7", "hs2-");
      printStats("一致性哈希(另一 key)", s2, count);
      if (s2.perNode.size() != 1 || s2.failed != 0) {
        printf("同一 key 未固定到同一节点 [FAIL]\n");
        ++failures;
      }
    }

    if (mode == "failover") {
      printf("---- 失败转移（已有一个节点不可用）----\n");
      const Stats s = runCalls(pool, count, "", "fo-");
      printStats("失败转移", s, count);
      // 不检查分布在哪个节点，只要求请求全部成功
      if (s.failed != 0 || s.ok != count) {
        printf("节点不可用时请求未全部成功 [FAIL]\n");
        ++failures;
      }
    }
  }

  printf("%s\n", failures == 0 ? "连接池验证通过" : "连接池验证失败");
  return failures == 0 ? 0 : 1;
}
