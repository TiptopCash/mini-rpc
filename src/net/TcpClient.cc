#include "net/TcpClient.h"

#include <cstdio>
#include <utility>

#include "common/Logger.h"
#include "net/Connector.h"
#include "net/EventLoop.h"

namespace mrpc {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr,
                     const std::string& name)
    : loop_(loop),
      name_(name),
      serverAddr_(serverAddr),
      connector_(new Connector(loop, serverAddr, name)),
      connected_(false),
      nextConnId_(1) {
  connector_->setNewConnectionCallback(
      [this](int sockfd) { newConnection(sockfd); });
}

TcpClient::~TcpClient() {
  // 当前连接归 TcpClient 持有，析构时也要替它把 channel 摘干净
  TcpConnectionPtr conn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    conn = connection_;
  }
  if (conn) {
    conn->getLoop()->runInLoop([conn] { conn->connectDestroyed(); });
  }
}

void TcpClient::connect() {
  LOG_INFO << "TcpClient[" << name_ << "] - connecting to "
           << serverAddr_.toIpPort();
  connector_->start();
}

void TcpClient::disconnect() {
  connected_.store(false);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_) {
      connection_->shutdown();
    }
  }
  connector_->stop();
}

void TcpClient::enableRetry(int64_t initialDelayMs, int64_t maxDelayMs) {
  connector_->enableRetry(initialDelayMs, maxDelayMs);
}

void TcpClient::newConnection(int sockfd) {
  loop_->assertInLoopThread();

  char buf[64];
  snprintf(buf, sizeof(buf), "#%d", nextConnId_);
  ++nextConnId_;
  const std::string connName = name_ + buf;

  const InetAddress localAddr(localAddressOf(sockfd));

  LOG_INFO << "TcpClient[" << name_ << "] - new connection [" << connName
           << "] local " << localAddr.toIpPort() << " -> "
           << serverAddr_.toIpPort();

  TcpConnectionPtr conn = std::make_shared<TcpConnection>(
      loop_, connName, sockfd, localAddr, serverAddr_);
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setCloseCallback(
      [this](const TcpConnectionPtr& c) { removeConnection(c); });

  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = conn;
  }
  connected_.store(true);

  conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
  loop_->assertInLoopThread();
  LOG_INFO << "TcpClient[" << name_ << "] - connection " << conn->name()
           << " closed";

  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_.reset();
  }
  connected_.store(false);

  // 延后销毁：当前正处在该连接的 close 回调里，直接析构会把调用栈上的
  // Channel 事件处理一起拆掉
  loop_->queueInLoop([conn] { conn->connectDestroyed(); });

  // 对端断开后按退避重连；disconnect() 主动关闭时 connector 已 stop，不会重连
  connector_->restart();
}

TcpConnectionPtr TcpClient::connection() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connection_;
}

}  // namespace mrpc
