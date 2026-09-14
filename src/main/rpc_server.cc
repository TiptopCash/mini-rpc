// RPC 服务端示例：注册 EchoServiceImpl，请求经 protobuf 反射分发到具体方法。
//
// 用法: ./rpc_server [port] [threads] [heartbeatSec] [timeoutSec] [requestTimeoutMs] [nodeTag]
//   heartbeatSec <= 0 关闭心跳；requestTimeoutMs <= 0 关闭请求级超时兜底
//   nodeTag 非空时响应变成 "echo:<text>@<tag>"，用来在连接池演示里分辨是哪个节点
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
  explicit EchoServiceImpl(std::string tag) : tag_(std::move(tag)) {}

  void Echo(google::protobuf::RpcController* controller,
            const EchoRequest* request, EchoResponse* response,
            google::protobuf::Closure* done) override {
    if (request->text().empty()) {
      // 业务失败：框架看到 controller->Failed() 后回 kRpcServiceFailed
      controller->SetFailed("text must not be empty");
      done->Run();
      return;
    }
    response->set_text("echo:" + request->text() +
                       (tag_.empty() ? "" : "@" + tag_));
    done->Run();
  }

  // 反面示例：永远不调用 done->Run()。
  // 用来验证请求级超时兜底——框架会在超时后回 kRpcTimeout 并回收资源，
  // 否则 request/response/controller/MethodDone 会一起泄漏，客户端永远等下去。
  void NoReply(google::protobuf::RpcController* /*controller*/,
               const EchoRequest* /*request*/, EchoResponse* /*response*/,
               google::protobuf::Closure* /*done*/) override {}

 private:
  const std::string tag_;
};

int main(int argc, char* argv[]) {
  uint16_t port = argc > 1 ? static_cast<uint16_t>(::atoi(argv[1])) : 9300;
  int threads = argc > 2 ? ::atoi(argv[2]) : 4;
  int heartbeatSec = argc > 3 ? ::atoi(argv[3]) : 10;
  int timeoutSec = argc > 4 ? ::atoi(argv[4]) : 30;
  int64_t requestTimeoutMs = argc > 5 ? ::atoll(argv[5]) : 5000;
  const std::string nodeTag = argc > 6 ? argv[6] : "";

  EventLoop loop;
  InetAddress listenAddr(port);

  // 先于 server 声明 -> 后于 server 销毁。
  // 注册表不持有所有权，若先销毁服务对象，server 析构期间就会持有悬垂指针
  EchoServiceImpl echoService(nodeTag);
  RpcServer server(&loop, listenAddr, "RpcServer", threads);
  server.registerService(&echoService);
  server.setHeartbeat(heartbeatSec, timeoutSec);
  server.setRequestTimeout(requestTimeoutMs);

  // 优雅退出：SIGINT/SIGTERM -> loop.quit()。
  // 必须在 start() 之前构造——线程一旦创建，再屏蔽信号就晚了。
  SignalWatcher signals(&loop, {SIGINT, SIGTERM}, [&loop](int signo) {
    LOG_INFO << "RpcServer - signal " << signo << " received, shutting down";
    loop.quit();
  });
  signals.start();

  server.start();
  LOG_INFO << "RpcServer listening on port " << port << " with " << threads
           << " IO threads, request timeout " << requestTimeoutMs << " ms";
  loop.loop();
  LOG_INFO << "RpcServer - loop exited, cleaning up";
  return 0;
}
