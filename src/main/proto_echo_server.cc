// 协议层 echo 服务端：按固定头分帧、解析 protobuf、原样回包，并带应用层心跳。
//
// 用法: ./proto_echo_server [port] [threads] [heartbeatSec] [timeoutSec]
//   heartbeatSec <= 0 表示关闭心跳
#include <csignal>
#include <cstdlib>
#include <memory>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/SignalWatcher.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "protocol/HeartbeatMonitor.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

using namespace mrpc;

int main(int argc, char* argv[]) {
  uint16_t port = argc > 1 ? static_cast<uint16_t>(::atoi(argv[1])) : 9200;
  int threads = argc > 2 ? ::atoi(argv[2]) : 4;
  int heartbeatSec = argc > 3 ? ::atoi(argv[3]) : 10;
  int timeoutSec = argc > 4 ? ::atoi(argv[4]) : 30;

  EventLoop loop;
  InetAddress listenAddr(port);
  TcpServer server(&loop, listenAddr, "ProtoEchoServer");
  server.setThreadNum(threads);

  server.setConnectionCallback([](const TcpConnectionPtr& conn) {
    LOG_INFO << "ProtoEchoServer - " << conn->peerAddress().toIpPort() << " -> "
             << conn->localAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");
  });

  server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
    uint8_t type = 0;
    RpcMessage msg;
    // 一次读到的数据可能含多帧（粘包）或不完整（半包）：
    // 循环解析直到 kIncomplete，剩余字节留在 conn 的输入缓冲里等下次
    while (true) {
      const RpcCodec::ParseResult r = RpcCodec::parse(buf, &type, &msg);
      if (r == RpcCodec::kIncomplete) {
        break;
      }
      if (r == RpcCodec::kError) {
        LOG_ERROR << "ProtoEchoServer - bad frame from " << conn->name()
                  << ", closing";
        conn->shutdown();
        return;
      }

      if (type == kRpcHeartbeat) {
        // 收到 ping，回 ack。对端收到 ack 不再回复，避免双方无限互回
        Buffer out;
        RpcMessage ack;
        ack.set_seq(msg.seq());
        RpcCodec::encode(ack, kRpcHeartbeatAck, &out);
        conn->send(out.retrieveAllAsString());
        continue;
      }
      if (type == kRpcHeartbeatAck) {
        // 对端对我方 ping 的响应；lastReceiveTime 已由网络层自动更新
        continue;
      }

      LOG_INFO << "ProtoEchoServer - seq=" << msg.seq()
               << " service=" << msg.service_name()
               << " method=" << msg.method_name()
               << " payload_size=" << msg.payload().size();

      Buffer out;
      RpcCodec::encode(msg, kRpcResponse, &out);
      conn->send(out.retrieveAllAsString());
    }
  });

  // 心跳放在协议层：需要编码 RPC 帧，网络层不依赖 protobuf
  std::unique_ptr<HeartbeatMonitor> heartbeat;
  if (heartbeatSec > 0) {
    heartbeat.reset(new HeartbeatMonitor(
        &loop, &server, HeartbeatOptions{1, heartbeatSec, timeoutSec}));
  }

  // 必须在 start() 之前构造——线程一旦创建，再屏蔽信号就晚了
  SignalWatcher signals(&loop, {SIGINT, SIGTERM}, [&loop](int signo) {
    LOG_INFO << "ProtoEchoServer - signal " << signo
             << " received, shutting down";
    loop.quit();
  });
  signals.start();

  server.start();
  if (heartbeat) {
    heartbeat->start();
  } else {
    LOG_INFO << "ProtoEchoServer - heartbeat disabled";
  }

  LOG_INFO << "ProtoEchoServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  LOG_INFO << "ProtoEchoServer - loop exited, cleaning up";
  return 0;
}
