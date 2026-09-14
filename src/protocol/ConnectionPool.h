#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/Noncopyable.h"
#include "net/InetAddress.h"
#include "protocol/LoadBalancer.h"

namespace mrpc {

class EventLoop;
class EventLoopThread;
class RpcChannel;

// 后端节点连接池：管一组 RpcChannel，按负载均衡策略挑一个给调用方。
//
// 为什么一个节点只保留一条连接：
//   协议按 seq 匹配响应，同一条连接上可以同时挂很多在途请求，
//   所以「并发上限」是 seq 空间而不是连接数。多开连接只会多占 fd 和内存，
//   不增加吞吐——真正需要多条连接的是「单连接被队头阻塞」的场景，
//   在本项目的一问一答模型里不存在。
//
// 自持一个 EventLoopThread：channel 必须活在该 loop 上，自己起线程最省事，
// 调用方不需要关心 loop 从哪来。析构时先让 loop 线程里把 channel 全部
// 释放掉，再停 loop——顺序反了就会在 loop 已停的情况下析构 channel。
//
// getChannel() 只做「连接不上就换下一个」的朴素失败转移，
// 不做健康检查、熔断、权重——那些是独立的课题，这里明确不覆盖。
class ConnectionPool : public Noncopyable {
 public:
  explicit ConnectionPool(int64_t connectTimeoutMs = 3000);
  ~ConnectionPool();

  // 设置后端节点列表；连接已建立的节点会被保留，新增的等用到时才连
  void setNodes(const std::vector<InetAddress>& nodes);
  // 换负载均衡策略；key 为空时轮询，非空时只有一致性哈希会用它
  void setLoadBalancer(std::unique_ptr<LoadBalancer> lb);
  // 后续新建 channel 的单次调用超时
  void setTimeoutMs(int64_t timeoutMs);

  // 按 key 挑一个可用 channel；全部不可用返回 nullptr。
  // key 为空表示不要求粘性。
  std::shared_ptr<RpcChannel> getChannel(const std::string& key = "");

  // 幂等：析构会调用它，想提前释放也可以手动调
  void shutdown();

 private:
  struct Node {
    InetAddress addr;
    std::shared_ptr<RpcChannel> channel;
    // 连不上之后的冷却截止（steady_clock 毫秒）。没有它的话，
    // 一个挂掉的节点会让每次 getChannel 都白等满 connectTimeoutMs
    int64_t downUntilMs = 0;
  };

  // 在锁内调用：拿到 node 对应的 channel，没有就建一个并 start()
  std::shared_ptr<RpcChannel> channelFor(const std::shared_ptr<Node>& node);

  std::unique_ptr<EventLoopThread> loopThread_;
  EventLoop* loop_ = nullptr;

  std::mutex mutex_;
  std::vector<std::shared_ptr<Node>> nodes_;
  std::unique_ptr<LoadBalancer> lb_;
  int64_t connectTimeoutMs_;
  int64_t timeoutMs_ = 5000;
};

}  // namespace mrpc
