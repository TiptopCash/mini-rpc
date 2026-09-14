#include "protocol/ConnectionPool.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "common/Logger.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "protocol/RpcChannel.h"

namespace mrpc {

namespace {

// 连不上之后的冷却时长：足够短，不至于错过对端重启；足够长，避免每次调用都白等
constexpr int64_t kNodeCooldownMs = 1000;

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

ConnectionPool::ConnectionPool(int64_t connectTimeoutMs)
    : loopThread_(std::make_unique<EventLoopThread>()),
      loop_(loopThread_->startLoop()),
      lb_(std::make_unique<RoundRobinLoadBalancer>()),
      connectTimeoutMs_(connectTimeoutMs) {}

ConnectionPool::~ConnectionPool() { shutdown(); }

void ConnectionPool::setNodes(const std::vector<InetAddress>& nodes) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<Node>> next;
  next.reserve(nodes.size());
  for (const InetAddress& addr : nodes) {
    // 地址没变就复用旧 Node，已经建好的连接不会因为重设列表被丢掉
    std::shared_ptr<Node> node;
    for (const auto& old : nodes_) {
      if (old->addr.toIpPort() == addr.toIpPort()) {
        node = old;
        break;
      }
    }
    if (!node) {
      node = std::make_shared<Node>();
    }
    node->addr = addr;
    next.push_back(node);
  }
  nodes_.swap(next);
}

void ConnectionPool::setLoadBalancer(std::unique_ptr<LoadBalancer> lb) {
  std::lock_guard<std::mutex> lock(mutex_);
  lb_ = std::move(lb);
  if (!lb_) {
    lb_ = std::make_unique<RoundRobinLoadBalancer>();
  }
}

void ConnectionPool::setTimeoutMs(int64_t timeoutMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  timeoutMs_ = timeoutMs;
}

std::shared_ptr<RpcChannel> ConnectionPool::channelFor(
    const std::shared_ptr<Node>& node) {
  if (!loop_) {  // 已 shutdown
    return nullptr;
  }
  if (!node->channel) {
    auto channel = std::make_shared<RpcChannel>(loop_, node->addr,
                                                node->addr.toIpPort());
    channel->setTimeoutMs(timeoutMs_);
    channel->start();
    node->channel = channel;
  }
  return node->channel;
}

std::shared_ptr<RpcChannel> ConnectionPool::getChannel(const std::string& key) {
  std::vector<InetAddress> addrs;
  std::vector<std::shared_ptr<Node>> candidates;
  const int64_t now = nowMs();
  {
    // LoadBalancer::select 不保证线程安全（一致性哈希要重建环），
    // 所以选节点必须在锁内做完
    std::lock_guard<std::mutex> lock(mutex_);
    if (nodes_.empty()) {
      return nullptr;
    }
    addrs.reserve(nodes_.size());
    for (const auto& node : nodes_) {
      addrs.push_back(node->addr);
    }
    const size_t idx = lb_->select(addrs, key);
    if (idx == LoadBalancer::kInvalidIndex) {
      return nullptr;
    }
    // 把策略选中的节点转到最前面，失败转移就顺着它往后找
    std::rotate(nodes_.begin(), nodes_.begin() + idx, nodes_.end());
    // 跳过冷却中的节点；全都在冷却就原样试一遍（宁可慢也不要假死）
    for (const auto& node : nodes_) {
      if (node->downUntilMs <= now) {
        candidates.push_back(node);
      }
    }
    if (candidates.empty()) {
      candidates = nodes_;
    }
  }

  // waitConnected 会阻塞，必须在锁外做，否则一个连不上的节点会卡住整池
  for (const auto& node : candidates) {
    std::shared_ptr<RpcChannel> channel;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      channel = channelFor(node);
    }
    if (channel && channel->waitConnected(connectTimeoutMs_)) {
      std::lock_guard<std::mutex> lock(mutex_);
      node->downUntilMs = 0;
      return channel;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      node->downUntilMs = nowMs() + kNodeCooldownMs;
    }
    LOG_WARN << "ConnectionPool: 节点 " << node->addr.toIpPort()
             << " 连接不可用，尝试下一个";
  }
  return nullptr;
}

void ConnectionPool::shutdown() {
  if (!loop_) {
    return;
  }
  // channel 必须在 loop 还活着的时候析构：它要在 loop 线程里清理在途请求与连接
  loop_->runInLoopAndWait([this] {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& node : nodes_) {
      node->channel.reset();
    }
  });
  loopThread_.reset();  // 退出循环线程
  loop_ = nullptr;
}

}  // namespace mrpc
