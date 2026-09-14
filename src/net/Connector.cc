#include "net/Connector.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "common/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

namespace mrpc {

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr,
                     const std::string& name)
    : loop_(loop),
      serverAddr_(serverAddr),
      name_(name),
      state_(kDisconnected),
      started_(false),
      retry_(false),
      retryInitialMs_(500),
      retryMaxMs_(30000),
      retryDelayMs_(500),
      retryTimerId_(0) {}

Connector::~Connector() {
  // 捕获 this 的定时器必须在本对象消失前撤销。要求只在 loop 线程析构：
  // 单线程下「撤销」不可能与回调执行并发，否则还需要额外加锁
  loop_->assertInLoopThread();
  if (retryTimerId_ != 0) {
    loop_->cancelTimer(retryTimerId_);
  }
  resetConnection();
}

void Connector::enableRetry(int64_t initialDelayMs, int64_t maxDelayMs) {
  retry_ = true;
  retryInitialMs_ = initialDelayMs;
  retryMaxMs_ = maxDelayMs;
  retryDelayMs_ = initialDelayMs;
}

void Connector::start() {
  started_ = true;
  loop_->runInLoop([this] { startInLoop(); });
}

void Connector::stop() {
  started_ = false;
  loop_->runInLoop([this] { stopInLoop(); });
}

void Connector::restart() {
  if (!retry_) {
    return;
  }
  loop_->runInLoop([this] {
    if (state_ == kConnecting) {
      return;  // 已经有一轮在进行，别叠加
    }
    state_ = kDisconnected;
    retryDelayMs_ = retryInitialMs_;  // 对端刚断开，立刻重连而不是先等退避
    startInLoop();
  });
}

void Connector::startInLoop() {
  loop_->assertInLoopThread();
  if (!started_ || state_ != kDisconnected) {
    return;
  }
  connect();
}

void Connector::stopInLoop() {
  loop_->assertInLoopThread();
  if (retryTimerId_ != 0) {
    loop_->cancelTimer(retryTimerId_);
    retryTimerId_ = 0;
  }
  // kConnected 时 fd 已经交给 TcpConnection，不归这里管
  state_ = kDisconnected;
  resetConnection();
}

void Connector::connect() {
  loop_->assertInLoopThread();

  // 上一轮留下的空壳 Channel/Socket 在这里销毁。此处不在任何 Channel 回调内，
  // 销毁是安全的（成功回调里不能让 Channel 析构，见 handleWrite 的说明）
  channel_.reset();
  socket_.reset();

  const int sockfd = createNonblocking();
  socket_.reset(new Socket(sockfd));

  const int r = ::connect(
      sockfd, reinterpret_cast<const struct sockaddr*>(&serverAddr_.getSockAddr()),
      sizeof(struct sockaddr_in));
  const int savedErrno = (r == 0) ? 0 : errno;

  switch (savedErrno) {
    case 0:
    case EINPROGRESS:
    case EINTR:
    case EISCONN:
      // 非阻塞 connect 的常态：立即返回 EINPROGRESS，结果等可写事件
      connecting(sockfd);
      break;
    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
      // 这类错误可能只是暂时的（对端还没起来、本机端口临时耗尽）
      LOG_WARN << "Connector[" << name_ << "] - connect to "
               << serverAddr_.toIpPort() << " failed: " << strerror(savedErrno);
      scheduleRetry();
      break;
    default:
      LOG_ERROR << "Connector[" << name_ << "] - connect to "
                << serverAddr_.toIpPort()
                << " unexpected error: " << strerror(savedErrno);
      scheduleRetry();
      break;
  }
}

void Connector::connecting(int sockfd) {
  state_ = kConnecting;
  // RPC 小包居多，关掉 Nagle 以免响应被延迟合并
  socket_->setTcpNoDelay(true);

  channel_.reset(new Channel(loop_, sockfd));
  channel_->setWriteCallback([this] { handleWrite(); });
  channel_->setErrorCallback([this] { handleError(); });
  // 连接结果就绪时 socket 可写
  channel_->enableWriting();
}

void Connector::handleWrite() {
  if (state_ != kConnecting) {
    return;  // 已被 handleError 处理过
  }

  const int err = getSocketError(socket_->fd());
  if (err != 0) {
    LOG_WARN << "Connector[" << name_ << "] - connect to "
             << serverAddr_.toIpPort() << " refused: " << strerror(err);
    handleError();
    return;
  }

  // 连接建立：先把 fd 从 epoll 摘掉，再把所有权交出去。
  // Channel 对象本身留到下一次 connect()/析构时销毁——此刻我们正处在
  // 它的 handleEvent 调用栈里，提前析构会让栈上的成员访问踩到已释放内存
  channel_->disableAll();
  channel_->remove();
  state_ = kConnected;
  retryDelayMs_ = retryInitialMs_;

  const int fd = socket_->release();
  if (newConnectionCallback_) {
    newConnectionCallback_(fd);
  } else {
    ::close(fd);
  }
}

void Connector::handleError() {
  if (state_ != kConnecting) {
    return;
  }
  const int err = getSocketError(socket_->fd());
  LOG_WARN << "Connector[" << name_ << "] - connect to "
           << serverAddr_.toIpPort() << " error: " << strerror(err);

  state_ = kDisconnected;
  // 同样不能析构 channel_：本函数由它的错误回调进入
  channel_->disableAll();
  channel_->remove();
  if (socket_) {
    ::close(socket_->release());
  }
  scheduleRetry();
}

void Connector::scheduleRetry() {
  if (!retry_ || !started_) {
    LOG_WARN << "Connector[" << name_ << "] - connect to "
             << serverAddr_.toIpPort() << " failed, giving up";
    return;
  }

  LOG_WARN << "Connector[" << name_ << "] - retry connecting to "
           << serverAddr_.toIpPort() << " in " << retryDelayMs_ << " ms";
  retryTimerId_ = loop_->runAfter(retryDelayMs_, [this] {
    retryTimerId_ = 0;
    startInLoop();
  });
  retryDelayMs_ = std::min(retryDelayMs_ * 2, retryMaxMs_);
}

void Connector::resetConnection() {
  if (channel_) {
    channel_->disableAll();
    channel_->remove();
  }
  channel_.reset();
  socket_.reset();  // Socket 析构负责 close
}

}  // namespace mrpc
