#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/Noncopyable.h"
#include "google/protobuf/service.h"
#include "net/TcpServer.h"
#include "protocol/RpcError.h"
#include "protocol/ServiceRegistry.h"
#include "rpc.pb.h"

namespace mrpc {

class EventLoop;
class InetAddress;
class HeartbeatMonitor;
class RpcController;

// RPC 服务端：把 epoll 网络层和 protobuf 反射分发接起来。
//
// 处理链路：
//   RpcCodec 分帧 -> 心跳(HeartbeatMonitor/RpcServer) -> 按名字查表(ServiceRegistry)
//   -> 用原型反射构造请求/响应 -> Service::CallMethod -> 回包
//
// 线程模型：每个连接固定在一个 IO 线程；CallMethod 就在该线程执行。
// 服务实现若把 done 异步抛给别的线程，回包仍由 conn->send() 投递回 IO 线程。
class RpcServer : public Noncopyable {
 public:
  RpcServer(EventLoop* loop, const InetAddress& listenAddr,
            const std::string& name, int numThreads = 4);
  ~RpcServer();

  // 必须在 start() 之前调用；service 生命周期须长于本 RpcServer
  void registerService(google::protobuf::Service* service);

  // heartbeatSec <= 0 表示关闭心跳
  void setHeartbeat(int heartbeatSec, int timeoutSec) {
    heartbeatSec_ = heartbeatSec;
    timeoutSec_ = timeoutSec;
  }

  // 请求级超时兜底（毫秒，<= 0 表示关闭），默认 5 秒。
  // 服务实现若是异步的却忘记调用 done->Run()，定时器到点会回 kRpcTimeout
  // 并回收这次请求占用的内存；否则这条路径只会在 ASan 报告里以泄漏的形式暴露。
  void setRequestTimeout(int64_t timeoutMs) { requestTimeoutMs_ = timeoutMs; }

  void start();

 private:
  void onConnection(const TcpConnectionPtr& conn);
  void onMessage(const TcpConnectionPtr& conn, Buffer* buf);
  void dispatch(const TcpConnectionPtr& conn, uint8_t type, RpcMessage& msg);
  void sendError(const TcpConnectionPtr& conn, uint64_t seq, int32_t code,
                 const std::string& text);

  EventLoop* loop_;
  TcpServer server_;
  ServiceRegistry registry_;
  std::unique_ptr<HeartbeatMonitor> heartbeat_;
  int heartbeatSec_ = 0;
  int timeoutSec_ = 0;
  int64_t requestTimeoutMs_ = 5000;
  bool started_ = false;
};

}  // namespace mrpc
