// 基于 protobuf Service::Stub 的 RPC 客户端示例（第 7-8 周）。
//
// 与 rpc_client.cc 的区别：那个是手写裸 socket 验证服务端；这个走完整客户端链路
// （RpcChannel -> TcpClient -> Connector），业务侧只面对生成的 Stub：
//   - 同步调用：done 传 nullptr，调用线程阻塞到响应或超时
//   - 异步调用：done 传自己的 Closure，响应到达后在 IO 线程回调
//   - 超时：服务端漏调 done->Run() 时客户端不会被无限挂住
//
// 用法: ./rpc_stub_client [ip] [port] [count]
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Logger.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"
#include "protocol/RpcChannel.h"
#include "protocol/RpcController.h"
#include "rpc.pb.h"

using namespace mrpc;

namespace {

// 异步调用完成计数：done->Run() 在 IO 线程触发，这里只做计数并唤醒等待方。
// 同一个对象可以给多个调用共用（每次 Run() 加一），调用方保证它活得比调用久。
class AsyncWaiter : public google::protobuf::Closure {
 public:
  explicit AsyncWaiter(int expected) : expected_(expected) {}

  void Run() override {
    {
      std::lock_guard<std::mutex> lock(m_);
      ++done_;
    }
    cv_.notify_all();
  }

  bool wait(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return done_ >= expected_; });
  }

 private:
  const int expected_;
  int done_ = 0;
  std::mutex m_;
  std::condition_variable cv_;
};

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 9300;
  const int count = argc > 3 ? ::atoi(argv[3]) : 200;

  int failures = 0;

  // channel 声明在 loopThread 之后 -> 先于它析构。
  // RpcChannel 析构时要在 loop 线程清理连接与在途请求，所以 loop 必须还活着
  EventLoopThread loopThread;
  EventLoop* loop = loopThread.startLoop();

  RpcChannel channel(loop, InetAddress(ip, port), "StubClient");
  channel.setTimeoutMs(2000);
  channel.start();
  if (!channel.waitConnected(3000)) {
    printf("连接 %s:%u 失败 [FAIL]\n", ip, port);
    return 1;
  }

  EchoService::Stub stub(&channel);  // 生成的桩：调用直接落到 RpcChannel

  // ---- 阶段 1：同步调用 ----
  {
    int ok = 0;
    for (int i = 0; i < count; ++i) {
      EchoRequest req;
      req.set_text("sync-" + std::to_string(i));
      EchoResponse resp;
      RpcController ctl;
      stub.Echo(&ctl, &req, &resp, nullptr);
      if (!ctl.Failed() && resp.text() == "echo:sync-" + std::to_string(i)) {
        ++ok;
      }
    }
    printf("同步调用: 成功 %d/%d %s\n", ok, count, ok == count ? "[OK]" : "[FAIL]");
    if (ok != count) ++failures;
  }

  // ---- 阶段 2：业务错误（服务实现 SetFailed）----
  {
    EchoRequest req;  // text 为空 -> Echo 里 SetFailed
    EchoResponse resp;
    RpcController ctl;
    stub.Echo(&ctl, &req, &resp, nullptr);
    const bool pass = ctl.Failed();
    printf("同步业务失败: %s (%s)\n", pass ? "[OK]" : "[FAIL]",
           ctl.ErrorText().c_str());
    if (!pass) ++failures;
  }

  // ---- 阶段 3：异步调用 ----
  {
    std::vector<EchoRequest> reqs(count);
    std::vector<EchoResponse> resps(count);
    std::vector<RpcController> ctls(count);
    AsyncWaiter waiter(count);

    for (int i = 0; i < count; ++i) {
      reqs[i].set_text("async-" + std::to_string(i));
      // done 非空 -> 立即返回，响应到达后在 IO 线程执行 waiter.Run()
      stub.Echo(&ctls[i], &reqs[i], &resps[i], &waiter);
    }

    const bool allDone = waiter.wait(10000);
    int ok = 0;
    if (allDone) {
      for (int i = 0; i < count; ++i) {
        if (!ctls[i].Failed() &&
            resps[i].text() == "echo:async-" + std::to_string(i)) {
          ++ok;
        }
      }
    }
    printf("异步调用: 成功 %d/%d %s\n", ok, count, ok == count ? "[OK]" : "[FAIL]");
    if (ok != count) ++failures;
  }

  // ---- 阶段 4：异步错误路径 ----
  {
    EchoRequest req;
    EchoResponse resp;
    RpcController ctl;
    AsyncWaiter waiter(1);
    stub.Echo(&ctl, &req, &resp, &waiter);
    const bool done = waiter.wait(2000);
    const bool pass = done && ctl.Failed();
    printf("异步业务失败: %s (%s)\n", pass ? "[OK]" : "[FAIL]",
           ctl.ErrorText().c_str());
    if (!pass) ++failures;
  }

  // ---- 阶段 5：客户端超时 ----
  // NoReply 永不回包，只能靠超时脱身。这里把超时调得比服务端的请求级兜底更短，
  // 于是服务端的兜底响应会在本地登记已被摘除之后到达，应被丢弃并记日志
  {
    channel.setTimeoutMs(150);
    EchoRequest req;
    req.set_text("x");
    EchoResponse resp;
    RpcController ctl;
    stub.NoReply(&ctl, &req, &resp, nullptr);
    const bool timedOut = ctl.Failed();
    printf("超时调用: %s (%s)\n", timedOut ? "[OK]" : "[FAIL]",
           ctl.ErrorText().c_str());
    if (!timedOut) ++failures;

    // 等服务端兜底的迟到响应到达（脚本把服务端 requestTimeout 配成约 400ms）
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // 超时之后连接仍应可用
    channel.setTimeoutMs(2000);
    EchoRequest req2;
    req2.set_text("after-timeout");
    EchoResponse resp2;
    RpcController ctl2;
    stub.Echo(&ctl2, &req2, &resp2, nullptr);
    const bool alive = !ctl2.Failed() && resp2.text() == "echo:after-timeout";
    printf("超时后连接仍可用: %s\n", alive ? "[OK]" : "[FAIL]");
    if (!alive) ++failures;
  }

  printf("%s\n", failures == 0 ? "Stub 客户端验证通过" : "Stub 客户端验证失败");
  return failures == 0 ? 0 : 1;
}
