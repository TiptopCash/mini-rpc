// 协议层 echo 服务端：按固定头分帧，解析 protobuf，原样回包
#include <cstdlib>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

using namespace mrpc;

int main(int argc, char* argv[]) {
  uint16_t port = argc > 1 ? static_cast<uint16_t>(::atoi(argv[1])) : 9200;
  int threads = argc > 2 ? ::atoi(argv[2]) : 4;

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
        continue;  // 心跳无需回包
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

  server.start();
  LOG_INFO << "ProtoEchoServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  return 0;
}
