// RPC 服务端示例：注册 EchoServiceImpl，请求经 protobuf 反射分发到具体方法。
//
// 用法: ./rpc_server [port] [threads] [heartbeatSec] [timeoutSec]
//   heartbeatSec <= 0 关闭心跳
#include <cstdlib>
#include <string>

#include "common/Logger.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "protocol/RpcServer.h"
#include "rpc.pb.h"

using namespace mrpc;

// 业务实现只需继承 protoc 生成的抽象类，完全不碰网络 / 分帧 / 编解码。
class EchoServiceImpl : public EchoService {
 public:
  void Echo(google::protobuf::RpcController* controller,
            const EchoRequest* request, EchoResponse* response,
            google::protobuf::Closure* done) override {
    if (request->text().empty()) {
      // 业务失败：框架看到 controller->Failed() 后回 kRpcServiceFailed
      controller->SetFailed("text must not be empty");
      done->Run();
      return;
    }
    response->set_text("echo:" + request->text());
    done->Run();
  }
};

int main(int argc, char* argv[]) {
  uint16_t port = argc > 1 ? static_cast<uint16_t>(::atoi(argv[1])) : 9300;
  int threads = argc > 2 ? ::atoi(argv[2]) : 4;
  int heartbeatSec = argc > 3 ? ::atoi(argv[3]) : 10;
  int timeoutSec = argc > 4 ? ::atoi(argv[4]) : 30;

  EventLoop loop;
  InetAddress listenAddr(port);
  RpcServer server(&loop, listenAddr, "RpcServer", threads);

  EchoServiceImpl echoService;  // 必须比 server 活得久（注册表不持有所有权）
  server.registerService(&echoService);
  server.setHeartbeat(heartbeatSec, timeoutSec);

  server.start();
  LOG_INFO << "RpcServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  return 0;
}
