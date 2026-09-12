#pragma once

#include <memory>

#include "common/Noncopyable.h"

namespace mrpc {

class EventLoop;
class PeriodicTimer;
class TcpServer;
class TcpConnection;

struct HeartbeatOptions {
  int tickSeconds = 1;        // 扫描周期
  int heartbeatSeconds = 10;  // 空闲超过该值 -> 发 ping
  int timeoutSeconds = 30;    // 空闲超过该值 -> 强制断开
};

// 应用层心跳与空闲检测。
//
// 每个 tick 扫描全部连接，依据「最近接收时间」判定：
//   空闲 >= heartbeatSeconds -> 发送 ping（对端应回 ack）
//   空闲 >= timeoutSeconds   -> 强制关闭（对端已死或网络已断）
//
// 之所以能检测「半开连接」（对端断电/断网但无 FIN）：TCP 本身不会通知，
// 只能靠应用层定期探测 + 超时判定。
class HeartbeatMonitor : public Noncopyable {
 public:
  HeartbeatMonitor(EventLoop* loop, TcpServer* server, HeartbeatOptions opts);
  ~HeartbeatMonitor();

  void start();
  void stop();

 private:
  void onTick();

  EventLoop* loop_;
  TcpServer* server_;
  HeartbeatOptions opts_;
  std::unique_ptr<PeriodicTimer> timer_;
};

}  // namespace mrpc
