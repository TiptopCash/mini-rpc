#include "protocol/LoadBalancer.h"

#include <algorithm>

namespace mrpc {

namespace {

// FNV-1a 32 位：实现短、分布够均匀，不需要为选节点引入加密哈希
uint32_t fnv1a(const std::string& s) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : s) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace

size_t RoundRobinLoadBalancer::select(const std::vector<InetAddress>& nodes,
                                      const std::string& /*key*/) {
  if (nodes.empty()) {
    return kInvalidIndex;
  }
  // 先取号再取模：取号是原子的，多线程调用也不会拿到同一个下标
  return next_.fetch_add(1) % nodes.size();
}

ConsistentHashLoadBalancer::ConsistentHashLoadBalancer(int virtualNodesPerNode)
    : virtualNodesPerNode_(virtualNodesPerNode < 1 ? 1 : virtualNodesPerNode) {}

bool ConsistentHashLoadBalancer::nodesChanged(
    const std::vector<InetAddress>& nodes) const {
  if (nodes.size() != cachedNodes_.size()) {
    return true;
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].toIpPort() != cachedNodes_[i]) {
      return true;
    }
  }
  return false;
}

void ConsistentHashLoadBalancer::rebuild(
    const std::vector<InetAddress>& nodes) {
  ring_.clear();
  cachedNodes_.clear();
  cachedNodes_.reserve(nodes.size());

  for (size_t i = 0; i < nodes.size(); ++i) {
    const std::string addr = nodes[i].toIpPort();
    cachedNodes_.push_back(addr);
    for (int v = 0; v < virtualNodesPerNode_; ++v) {
      // 虚拟节点名带上序号，才能在同一节点上产生多个分散的环上位置
      ring_[fnv1a(addr + "#" + std::to_string(v))] = i;
    }
  }
}

size_t ConsistentHashLoadBalancer::select(const std::vector<InetAddress>& nodes,
                                          const std::string& key) {
  if (nodes.empty()) {
    return kInvalidIndex;
  }
  if (nodesChanged(nodes)) {
    rebuild(nodes);
  }

  // 不带 key 的请求用一个自增序号打散；否则所有无 key 请求都会落到同一个节点
  const std::string hashKey =
      key.empty() ? std::to_string(counter_.fetch_add(1)) : key;

  // 顺时针找到第一个 >= hashKey 的虚拟节点，越过环尾则回到环首
  auto it = ring_.lower_bound(fnv1a(hashKey));
  if (it == ring_.end()) {
    it = ring_.begin();
  }
  return it->second;
}

}  // namespace mrpc
