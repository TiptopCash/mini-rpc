#include <csignal>
#include <cstdlib>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/SignalWatcher.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

using namespace mrpc;

int main(int argc, char* argv[]) {
  uint16_t port = 8888;
  int threads = 4;
  if (argc > 1) port = static_cast<uint16_t>(::atoi(argv[1]));
  if (argc > 2) threads = ::atoi(argv[2]);

  EventLoop loop;
  InetAddress listenAddr(port);
  TcpServer server(&loop, listenAddr, "EchoServer");
  server.setThreadNum(threads);

  server.setConnectionCallback([](const TcpConnectionPtr& conn) {
    LOG_INFO << "EchoServer - " << conn->peerAddress().toIpPort() << " -> "
             << conn->localAddress().toIpPort() << " is "
             << (conn->connected() ? "UP" : "DOWN");
  });

  server.setMessageCallback(
      [](const TcpConnectionPtr& conn, Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);  // echo back
      });

  // 优雅退出：SIGINT/SIGTERM 经 signalfd 变成普通的 epoll 事件，
  // 让 loop.loop() 正常返回，之后所有对象按正常顺序析构。
  // 必须在 start() 之前构造——线程一旦创建，再屏蔽信号就晚了。
  SignalWatcher signals(&loop, {SIGINT, SIGTERM}, [&loop](int signo) {
    LOG_INFO << "EchoServer - signal " << signo << " received, shutting down";
    loop.quit();
  });
  signals.start();

  server.start();
  LOG_INFO << "EchoServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  LOG_INFO << "EchoServer - loop exited, cleaning up";
  return 0;
}
