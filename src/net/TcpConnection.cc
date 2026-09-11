#include "net/TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <errno.h>

#include "common/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

namespace mrpc {

TcpConnection::TcpConnection(EventLoop* loop, const std::string& nameArg,
                             int sockfd, const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop),
      name_(nameArg),
      state_(kConnecting),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr) {
  socket_->setTcpNoDelay(true);  // 请求-响应场景关闭 Nagle
  channel_->setReadCallback([this] { handleRead(); });
  channel_->setWriteCallback([this] { handleWrite(); });
  channel_->setCloseCallback([this] { handleClose(); });
  channel_->setErrorCallback([this] { handleError(); });
  LOG_DEBUG << "TcpConnection ctor [" << name_ << "]";
}

TcpConnection::~TcpConnection() {
  LOG_DEBUG << "TcpConnection dtor [" << name_ << "]";
}

void TcpConnection::send(const std::string& message) {
  if (state_.load() == kConnected) {
    if (loop_->isInLoopThread()) {
      sendInLoop(message);
    } else {
      auto self = shared_from_this();
      loop_->runInLoop([self, message] { self->sendInLoop(message); });
    }
  }
}

void TcpConnection::sendInLoop(const std::string& message) {
  ssize_t nwritten = 0;
  size_t remaining = message.size();

  // 输出缓冲为空时尝试直接写，减少一次拷贝
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    nwritten = ::write(channel_->fd(), message.data(), message.size());
    if (nwritten >= 0) {
      remaining = message.size() - static_cast<size_t>(nwritten);
    } else {
      nwritten = 0;
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR << "TcpConnection::sendInLoop write failed: "
                  << strerror(errno);
      }
    }
  }

  // 没写完的挂到输出缓冲，等 EPOLLOUT 继续写
  if (remaining > 0) {
    outputBuffer_.append(message.data() + nwritten, remaining);
    if (!channel_->isWriting()) {
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown() {
  if (state_.load() == kConnected) {
    setState(kDisconnecting);
    auto self = shared_from_this();
    loop_->runInLoop([self] { self->shutdownInLoop(); });
  }
}

void TcpConnection::shutdownInLoop() {
  if (!channel_->isWriting()) {
    socket_->shutdownWrite();
  }
}

void TcpConnection::connectEstablished() {
  loop_->assertInLoopThread();
  setState(kConnected);
  channel_->tie(shared_from_this());
  channel_->enableReading();

  if (connectionCallback_) {
    connectionCallback_(shared_from_this());
  }
}

void TcpConnection::connectDestroyed() {
  if (state_.load() == kConnected) {
    setState(kDisconnected);
    channel_->disableAll();
    if (connectionCallback_) {
      connectionCallback_(shared_from_this());
    }
  }
  channel_->remove();
}

void TcpConnection::handleRead() {
  int savedErrno = 0;
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0) {
    if (messageCallback_) {
      messageCallback_(shared_from_this(), &inputBuffer_);
    }
  } else if (n == 0) {
    handleClose();
  } else {
    errno = savedErrno;
    LOG_ERROR << "TcpConnection::handleRead failed: " << strerror(errno);
    handleError();
  }
}

void TcpConnection::handleWrite() {
  if (!channel_->isWriting()) {
    return;
  }
  ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(),
                      outputBuffer_.readableBytes());
  if (n > 0) {
    outputBuffer_.retrieve(static_cast<size_t>(n));
    if (outputBuffer_.readableBytes() == 0) {
      channel_->disableWriting();
      if (state_.load() == kDisconnecting) {
        shutdownInLoop();
      }
    }
  } else {
    LOG_ERROR << "TcpConnection::handleWrite failed: " << strerror(errno);
  }
}

void TcpConnection::handleClose() {
  if (state_.load() == kDisconnected) {
    return;
  }
  setState(kDisconnected);
  channel_->disableAll();

  TcpConnectionPtr guardThis(shared_from_this());
  if (connectionCallback_) {
    connectionCallback_(guardThis);
  }
  if (closeCallback_) {
    closeCallback_(guardThis);  // 交由 TcpServer 摘除并延迟销毁
  }
}

void TcpConnection::handleError() {
  int err = 0;
  socklen_t optlen = sizeof(err);
  ::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &err, &optlen);
  LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] SO_ERROR=" << err << " " << strerror(err);
}

}  // namespace mrpc
