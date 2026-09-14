// Connector / TcpClient 自测：非阻塞 connect、失败退避重试、对端断开后重连。
//
// 服务端与客户端各跑在一个独立 loop 线程上，主线程只做等待与断言。
// 注意：所有持有 Channel 的对象都要在所属 loop 线程析构，测试里统一用
// destroyInLoop() 投递删除，不直接 delete。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "common/Timestamp.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

using namespace mrpc;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failed;
}

int waitForCount(const std::atomic<int>& counter, int expect, int timeoutMs) {
  const int64_t deadline = nowMicros() + static_cast<int64_t>(timeoutMs) * 1000;
  while (nowMicros() < deadline && counter.load() < expect) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return counter.load();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 析构会摘 epoll 注册，只能在所属 loop 线程做
template <typename T>
void destroyInLoop(EventLoop* loop, T* obj) {
  loop->runInLoopAndWait([obj] { delete obj; });
}

// 回显服务端：收到什么原样发回
TcpServer* makeEchoServer(EventLoop* loop, uint16_t port) {
  TcpServer* server = new TcpServer(loop, InetAddress(port), "EchoServer");
  server->setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
    conn->send(buf->retrieveAllAsString());
  });
  // start() 会注册 accept channel，同样只能在 loop 线程里做
  loop->runInLoopAndWait([server] { server->start(); });
  return server;
}

struct ClientState {
  std::atomic<int> echoed{0};
  std::atomic<int> up{0};
};

// 连上就发一句话，收到回显则计数
TcpClient* makeClient(EventLoop* loop, uint16_t port, const std::string& name,
                      const std::shared_ptr<ClientState>& st,
                      const std::string& text) {
  TcpClient* client = new TcpClient(loop, InetAddress("127.0.0.1", port), name);
  client->setConnectionCallback([st, text](const TcpConnectionPtr& conn) {
    if (conn->connected()) {
      st->up.fetch_add(1);
      conn->send(text);
    }
  });
  client->setMessageCallback(
      [st, text](const TcpConnectionPtr& /*conn*/, Buffer* buf) {
        if (buf->retrieveAllAsString() == text) st->echoed.fetch_add(1);
      });
  return client;
}

// 1. 非阻塞 connect：EINPROGRESS -> EPOLLOUT -> 握手完成 -> 收发
void testConnectEcho(EventLoop* clientLoop, uint16_t port) {
  printf("[1] 非阻塞 connect + 收发\n");
  auto st = std::make_shared<ClientState>();

  TcpServer* server = makeEchoServer(clientLoop, port);
  TcpClient* client = makeClient(clientLoop, port, "TestClient1", st, "hello");
  client->connect();

  check(waitForCount(st->up, 1, 2000) == 1, "连接建立");
  check(waitForCount(st->echoed, 1, 2000) == 1, "收到回显");

  client->disconnect();
  sleepMs(50);
  destroyInLoop(clientLoop, client);
  destroyInLoop(clientLoop, server);
}

// 2. 服务端尚未启动：应先失败并退避重试，服务端起来后自动连上
void testRetryThenSucceed(EventLoop* clientLoop, uint16_t port) {
  printf("[2] 对端未就绪 -> 退避重试 -> 自动连上\n");
  auto st = std::make_shared<ClientState>();

  TcpClient* client = makeClient(clientLoop, port, "TestClient2", st, "retry");
  client->enableRetry(100, 800);  // 首次 100ms，之后翻倍，上限 800ms
  client->connect();

  sleepMs(300);  // 这段时间里服务端不在，连接必然失败
  check(st->up.load() == 0 && st->echoed.load() == 0,
        "服务端未启动时连不上，且不误报成功");

  TcpServer* server = makeEchoServer(clientLoop, port);  // 现在才启动
  check(waitForCount(st->echoed, 1, 4000) == 1, "重试后自动连上并收到回显");

  client->disconnect();
  sleepMs(50);
  destroyInLoop(clientLoop, client);
  destroyInLoop(clientLoop, server);
}

// 3. 对端主动断开：客户端应自动重连，重连后仍然可用
void testReconnectAfterPeerClose(EventLoop* clientLoop, uint16_t port) {
  printf("[3] 对端断开后自动重连\n");
  auto st = std::make_shared<ClientState>();

  TcpServer* server = makeEchoServer(clientLoop, port);
  TcpClient* client = makeClient(clientLoop, port, "TestClient3", st, "again");
  client->enableRetry(100, 800);
  client->connect();

  check(waitForCount(st->echoed, 1, 2000) == 1, "首轮连接可用");

  // 服务端把连接全部踢掉，模拟对端异常关闭
  clientLoop->runInLoop([server] {
    server->forEachConnection(
        [](const TcpConnectionPtr& conn) { conn->forceClose(); });
  });

  check(waitForCount(st->echoed, 2, 4000) == 2, "断开后自动重连并再次收到回显");
  check(st->up.load() >= 2, "重连建立过新的连接");

  client->disconnect();
  sleepMs(50);
  destroyInLoop(clientLoop, client);
  destroyInLoop(clientLoop, server);
}

}  // namespace

int main() {
  // 单线程 loop 足够：服务端与客户端本来就分属两个 loop，
  // 每个 loop 内部仍然是「一个线程跑事件循环」
  EventLoopThread loopThread;
  EventLoop* loop = loopThread.startLoop();

  testConnectEcho(loop, 9400);
  testRetryThenSucceed(loop, 9401);
  testReconnectAfterPeerClose(loop, 9402);

  printf("\n%s（失败 %d 项）\n", g_failed == 0 ? "全部通过" : "存在失败", g_failed);
  return g_failed == 0 ? 0 : 1;
}
