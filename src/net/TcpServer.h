#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/Noncopyable.h"
#include "net/TcpConnection.h"

namespace mrpc {

class Acceptor;
class EventLoop;
class EventLoopThreadPool;
class InetAddress;

class TcpServer : public Noncopyable {
 public:
  using ThreadInitCallback = std::function<void(EventLoop*)>;
  enum Option { kNoReusePort, kReusePort };

  TcpServer(EventLoop* loop, const InetAddress& listenAddr,
            const std::string& nameArg, Option option = kNoReusePort);
  ~TcpServer();

  void setThreadNum(int numThreads);
  void setThreadInitCallback(const ThreadInitCallback& cb) {
    threadInitCallback_ = cb;
  }

  void start();

  void setConnectionCallback(const ConnectionCallback& cb) {
    connectionCallback_ = cb;
  }
  void setMessageCallback(const MessageCallback& cb) {
    messageCallback_ = cb;
  }

  EventLoop* getLoop() const { return loop_; }

 private:
  void newConnection(int sockfd, const InetAddress& peerAddr);
  void removeConnection(const TcpConnectionPtr& conn);
  void removeConnectionInLoop(const TcpConnectionPtr& conn);

  using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

  EventLoop* loop_;
  const std::string ipPort_;
  const std::string name_;
  std::unique_ptr<Acceptor> acceptor_;
  std::shared_ptr<EventLoopThreadPool> threadPool_;

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  ThreadInitCallback threadInitCallback_;

  std::atomic<bool> started_;
  int nextConnId_;
  ConnectionMap connections_;
};

}  // namespace mrpc
