#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/Noncopyable.h"
#include "net/InetAddress.h"
#include "net/TimerQueue.h"

namespace mrpc {

class Channel;
class EventLoop;
class Socket;

// 主动连接器：非阻塞 connect + 可选退避重试。
//
// 与 Acceptor 对称：Acceptor 处理「被动到来的连接」，Connector 处理「主动发起的连接」。
// 非阻塞 connect 立即返回 EINPROGRESS，真正的成败要等 socket 可写后
// 用 getsockopt(SO_ERROR) 读取——EPOLLOUT 只表示「有结果了」，不区分成功失败。
//
// 成功时通过回调把 fd 的所有权交出去（见 Socket::release）。
// 本类不做 shared_ptr 管理：所有回调与定时器都只在 loop 线程触发，
// 因此只要在 loop 线程销毁（析构里 assert），捕获 this 就是安全的。
class Connector : public Noncopyable {
 public:
  using NewConnectionCallback = std::function<void(int sockfd)>;

  Connector(EventLoop* loop, const InetAddress& serverAddr,
            const std::string& name);
  ~Connector();

  void setNewConnectionCallback(NewConnectionCallback cb) {
    newConnectionCallback_ = std::move(cb);
  }

  // 开启失败重试：首次等待 initialDelayMs，之后每次翻倍，上限 maxDelayMs
  void enableRetry(int64_t initialDelayMs = 500, int64_t maxDelayMs = 30000);

  // 以下三者在任意线程均可调用，内部投递到 loop 线程
  void start();
  void stop();
  // 断开后重新发起连接（重试开启时才有效）
  void restart();

  bool connected() const { return state_ == kConnected; }

 private:
  enum StateE { kDisconnected, kConnecting, kConnected };

  void startInLoop();
  void stopInLoop();
  void connect();
  void connecting(int sockfd);
  void handleWrite();
  void handleError();
  void scheduleRetry();
  // 断开当前 socket/channel；必须在非回调上下文调用（见实现里的说明）
  void resetConnection();

  EventLoop* loop_;
  const InetAddress serverAddr_;
  const std::string name_;
  StateE state_;

  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;
  NewConnectionCallback newConnectionCallback_;

  bool started_;
  bool retry_;
  int64_t retryInitialMs_;
  int64_t retryMaxMs_;
  int64_t retryDelayMs_;  // 下一次重试的等待时间，连接成功后重置
  TimerQueue::TimerId retryTimerId_;
};

}  // namespace mrpc
