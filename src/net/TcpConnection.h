#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "common/Noncopyable.h"
#include "net/Buffer.h"
#include "net/InetAddress.h"

namespace mrpc {

class Channel;
class EventLoop;
class Socket;

class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;

// 一条 TCP 连接的完整生命周期管理
class TcpConnection : public Noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {
 public:
  TcpConnection(EventLoop* loop, const std::string& nameArg, int sockfd,
                const InetAddress& localAddr, const InetAddress& peerAddr);
  ~TcpConnection();

  EventLoop* getLoop() const { return loop_; }
  const std::string& name() const { return name_; }
  const InetAddress& localAddress() const { return localAddr_; }
  const InetAddress& peerAddress() const { return peerAddr_; }
  bool connected() const { return state_.load() == kConnected; }

  void send(const std::string& message);
  void shutdown();

  void setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
  }
  void setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
  }
  void setCloseCallback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
  }

  // 连接建立 / 销毁（由 TcpServer 调用）
  void connectEstablished();
  void connectDestroyed();

 private:
  enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

  void setState(StateE s) { state_.store(s); }
  void handleRead();
  void handleWrite();
  void handleClose();
  void handleError();
  void sendInLoop(const std::string& message);
  void shutdownInLoop();

  EventLoop* loop_;
  const std::string name_;
  std::atomic<StateE> state_;
  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;
  const InetAddress localAddr_;
  const InetAddress peerAddr_;

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  CloseCallback closeCallback_;

  Buffer inputBuffer_;
  Buffer outputBuffer_;
};

}  // namespace mrpc
