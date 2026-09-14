#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/Noncopyable.h"
#include "net/InetAddress.h"

namespace mrpc {

// 负载均衡策略：从候选节点里挑一个下标。
//
// select() 不保证线程安全（一致性哈希要重建环），调用方需串行调用——
// ConnectionPool 在自己的锁里调用它。
class LoadBalancer {
 public:
  static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

  virtual ~LoadBalancer() = default;

  // key 为空表示调用方不要求粘性；非空时只有一致性哈希会用到它
  virtual size_t select(const std::vector<InetAddress>& nodes,
                        const std::string& key) = 0;
};

// 轮询：请求均匀分散到各节点，节点数变化时不需要额外处理
class RoundRobinLoadBalancer : public LoadBalancer {
 public:
  size_t select(const std::vector<InetAddress>& nodes,
                const std::string& key) override;

 private:
  std::atomic<uint64_t> next_{0};
};

// 一致性哈希：同一 key 固定落到同一节点，节点增减时只影响环上相邻的一段，
// 适合「同一用户的请求尽量打到同一后端」这类有粘性要求的场景。
//
// 实现要点：每个物理节点在环上放 virtualNodesPerNode_ 个虚拟节点。
// 虚拟节点太少（比如 1 个）时，节点在环上的分布会很不均匀，
// 请求会明显倾斜到个别节点；100 个左右是常用的折中。
class ConsistentHashLoadBalancer : public LoadBalancer {
 public:
  explicit ConsistentHashLoadBalancer(int virtualNodesPerNode = 100);

  size_t select(const std::vector<InetAddress>& nodes,
                const std::string& key) override;

  // 仅供自测：当前哈希环上的虚拟节点数
  size_t ringSize() const { return ring_.size(); }

 private:
  // 节点列表变了才重建环，否则每次选节点都要重算上百个哈希
  bool nodesChanged(const std::vector<InetAddress>& nodes) const;
  void rebuild(const std::vector<InetAddress>& nodes);

  const int virtualNodesPerNode_;
  std::vector<std::string> cachedNodes_;
  std::map<uint32_t, size_t> ring_;  // hash -> 节点下标
  std::atomic<uint64_t> counter_{0};
};

}  // namespace mrpc
