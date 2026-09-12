#include "net/TcpServer.h"

#include <sys/socket.h>

#include <cstdio>
#include <cstring>
#include <errno.h>

#include "common/Logger.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/EventLoopThreadPool.h"
#include "net/InetAddress.h"

namespace mrpc {

namespace {

InetAddress getLocalAddr(int sockfd) {
  struct sockaddr_in localaddr;
  memset(&localaddr, 0, sizeof(localaddr));
  socklen_t addrlen = sizeof(localaddr);
  if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&localaddr),
                    &addrlen) < 0) {
    LOG_ERROR << "getsockname failed: " << strerror(errno);
  }
  return InetAddress(localaddr);
}

}  // namespace

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                     const std::string& nameArg, Option option)
    : loop_(loop),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new EventLoopThreadPool(loop, name_)),
      started_(false),
      nextConnId_(1) {
  acceptor_->setNewConnectionCallback(
      [this](int sockfd, const InetAddress& peerAddr) {
        newConnection(sockfd, peerAddr);
      });
}

TcpServer::~TcpServer() {
  for (auto& item : connections_) {
    TcpConnectionPtr conn = item.second;
    item.second.reset();
    conn->getLoop()->runInLoop([conn] { conn->connectDestroyed(); });
  }
}

void TcpServer::setThreadNum(int numThreads) {
  threadPool_->setThreadNum(numThreads);
}

void TcpServer::start() {
  if (started_.exchange(true)) {
    return;
  }
  threadPool_->start(threadInitCallback_);
  acceptor_->listen();
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
  loop_->assertInLoopThread();
  EventLoop* ioLoop = threadPool_->getNextLoop();

  char buf[64];
  snprintf(buf, sizeof(buf), "-%s#%d", ipPort_.c_str(), nextConnId_);
  ++nextConnId_;
  std::string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_ << "] - new connection ["
           << connName << "] from " << peerAddr.toIpPort();

  InetAddress localAddr(getLocalAddr(sockfd));
  TcpConnectionPtr conn =
      std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr,
                                      peerAddr);
  connections_[connName] = conn;
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setCloseCallback(
      [this](const TcpConnectionPtr& c) { removeConnection(c); });

  ioLoop->runInLoop([conn] { conn->connectEstablished(); });
}

void TcpServer::forEachConnection(
    const std::function<void(const TcpConnectionPtr&)>& cb) {
  loop_->assertInLoopThread();
  for (const auto& item : connections_) {
    cb(item.second);
  }
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
  loop_->runInLoop([this, conn] { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnection [" << name_ << "] - connection "
           << conn->name();
  connections_.erase(conn->name());
  EventLoop* ioLoop = conn->getLoop();
  ioLoop->queueInLoop([conn] { conn->connectDestroyed(); });
}

}  // namespace mrpc
