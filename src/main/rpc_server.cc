// RPC 服务端示例：注册 EchoServiceImpl，请求经 protobuf 反射分发到具体方法。
//
// 用法: ./rpc_server [port] [threads] [heartbeatSec] [timeoutSec]
//   heartbeatSec <= 0 关闭心跳
#include <csignal>
#include <cstdlib>
#include <string>

#include "common/Logger.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/SignalWatcher.h"
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

  // 先于 server 声明 -> 后于 server 销毁。
  // 注册表不持有所有权，若先销毁服务对象，server 析构期间就会持有悬垂指针
  EchoServiceImpl echoService;
  RpcServer server(&loop, listenAddr, "RpcServer", threads);
  server.registerService(&echoService);
  server.setHeartbeat(heartbeatSec, timeoutSec);

  // 优雅退出：SIGINT/SIGTERM -> loop.quit()。
  // 必须在 start() 之前构造——线程一旦创建，再屏蔽信号就晚了。
  SignalWatcher signals(&loop, {SIGINT, SIGTERM}, [&loop](int signo) {
    LOG_INFO << "RpcServer - signal " << signo << " received, shutting down";
    loop.quit();
  });
  signals.start();

  server.start();
  LOG_INFO << "RpcServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  LOG_INFO << "RpcServer - loop exited, cleaning up";
  return 0;
}
