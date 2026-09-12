#include "protocol/HeartbeatMonitor.h"

#include "common/Logger.h"
#include "common/Timestamp.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/PeriodicTimer.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

namespace mrpc {

HeartbeatMonitor::HeartbeatMonitor(EventLoop* loop, TcpServer* server,
                                   HeartbeatOptions opts)
    : loop_(loop), server_(server), opts_(opts), timer_(nullptr) {
  if (opts_.timeoutSeconds <= opts_.heartbeatSeconds) {
    LOG_WARN << "HeartbeatMonitor: timeoutSeconds(" << opts_.timeoutSeconds
             << ") <= heartbeatSeconds(" << opts_.heartbeatSeconds
             << "), 连接可能在收到 ping 之前就被断开";
  }
  if (opts_.tickSeconds > 0) {
    timer_.reset(new PeriodicTimer(loop_, opts_.tickSeconds * 1000,
                                   [this] { onTick(); }));
  }
}

HeartbeatMonitor::~HeartbeatMonitor() = default;

void HeartbeatMonitor::start() {
  if (timer_) {
    timer_->start();
    LOG_INFO << "HeartbeatMonitor started - tick " << opts_.tickSeconds
             << "s, ping " << opts_.heartbeatSeconds << "s, timeout "
             << opts_.timeoutSeconds << "s";
  }
}

void HeartbeatMonitor::stop() {
  if (timer_) timer_->stop();
}

void HeartbeatMonitor::onTick() {
  const int64_t nowUs = nowMicros();

  server_->forEachConnection([&](const TcpConnectionPtr& conn) {
    if (!conn->connected()) return;

    const int64_t idleSeconds = (nowUs - conn->lastReceiveTimeUs()) / 1000000;

    if (idleSeconds >= opts_.timeoutSeconds) {
      LOG_WARN << "HeartbeatMonitor - " << conn->name() << " idle "
               << idleSeconds << "s >= timeout " << opts_.timeoutSeconds
               << "s, force close";
      conn->forceClose();
      return;
    }

    if (idleSeconds >= opts_.heartbeatSeconds) {
      RpcMessage ping;
      ping.set_seq(0);
      Buffer out;
      RpcCodec::encode(ping, kRpcHeartbeat, &out);
      conn->send(out.retrieveAllAsString());
      LOG_INFO << "HeartbeatMonitor - ping " << conn->name() << " (idle "
               << idleSeconds << "s)";
    }
  });
}

}  // namespace mrpc
