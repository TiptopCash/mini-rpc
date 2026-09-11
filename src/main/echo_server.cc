#include <cstdlib>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
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

  server.start();
  LOG_INFO << "EchoServer listening on port " << port << " with " << threads
           << " IO threads";
  loop.loop();
  return 0;
}
