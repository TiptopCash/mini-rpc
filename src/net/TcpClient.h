#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "common/Noncopyable.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"

namespace mrpc {

class Connector;
class EventLoop;

// 主动连接一端的连接管理，与 TcpServer 对称：
// TcpServer 管一堆被动 accept 的连接，TcpClient 管一个主动连出去的连接。
//
// 只跑在用户传入的那一个 loop 上，不引入线程池——客户端连接数远小于服务端，
// 多连接时按「一个连接一个 loop」分散即可。
//
// 与 Connector 一样，必须在所属 loop 线程析构（析构里要摘 epoll 注册）。
class TcpClient : public Noncopyable {
 public:
  TcpClient(EventLoop* loop, const InetAddress& serverAddr,
            const std::string& name);
  ~TcpClient();

  // 建立连接；失败或断开后是否重连取决于 enableRetry
  void connect();
  // 关闭当前连接并停止重试
  void disconnect();

  void enableRetry(int64_t initialDelayMs = 500, int64_t maxDelayMs = 30000);

  void setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
  }
  void setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
  }

  // 当前连接，未连接时返回空。可在任意线程调用
  TcpConnectionPtr connection() const;
  // 不加锁的轻量判断，用于「值得为它去拿 connection() 吗」这类高频检查
  bool connected() const { return connected_.load(); }
  EventLoop* getLoop() const { return loop_; }

 private:
  void newConnection(int sockfd);
  void removeConnection(const TcpConnectionPtr& conn);

  EventLoop* loop_;
  const std::string name_;
  const InetAddress serverAddr_;
  std::unique_ptr<Connector> connector_;

  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;

  // connection_ 会被别的线程读（比如 RpcChannel 的判断），用锁保护；
  // 需要频繁判断「有没有连接」的场景走 atomic 标志，避免每次都加锁
  mutable std::mutex mutex_;
  TcpConnectionPtr connection_;
  std::atomic<bool> connected_;
  int nextConnId_;
};

}  // namespace mrpc
