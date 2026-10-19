#include "RpcClient.h"

#include <string>

#include "protocol/RpcCodec.h"
#include "protocol/RpcHeader.h"
#include "rpc.pb.h"

RpcClient::RpcClient(QObject* parent) : QObject(parent) {
  connect(&socket_, &QTcpSocket::connected, this, &RpcClient::onConnected);
  connect(&socket_, &QTcpSocket::disconnected, this,
          &RpcClient::onDisconnected);
  connect(&socket_, &QTcpSocket::readyRead, this, &RpcClient::onReadyRead);
  connect(&socket_, &QTcpSocket::errorOccurred, this,
          &RpcClient::onSocketError);
}

RpcClient::~RpcClient() {
  // QTcpSocket 析构时可能发 disconnected。此时 pending_ 等成员已经先被销毁，
  // 若槽还连着就会读已销毁的对象。先断开信号再让 socket 析构。
  socket_.disconnect(this);
  socket_.abort();
}

void RpcClient::connectToServer(const QString& host, quint16 port) {
  if (socket_.state() != QAbstractSocket::UnconnectedState) {
    socket_.abort();
  }
  recvBuffer_.retrieveAll();
  pending_.clear();
  socket_.connectToHost(host, port);
}

void RpcClient::disconnectFromServer() {
  if (socket_.state() == QAbstractSocket::UnconnectedState) return;
  socket_.disconnectFromHost();
}

bool RpcClient::isConnected() const {
  return socket_.state() == QAbstractSocket::ConnectedState;
}

quint64 RpcClient::sendEcho(const QString& text) {
  mrpc::EchoRequest request;
  request.set_text(text.toStdString());

  mrpc::RpcMessage msg;
  const quint64 seq = nextSeq_++;
  msg.set_seq(seq);
  msg.set_service_name("mrpc.EchoService");
  msg.set_method_name("Echo");
  msg.set_payload(request.SerializeAsString());

  QElapsedTimer timer;
  timer.start();
  pending_.insert(seq, timer);

  sendFrame(msg, mrpc::kRpcRequest);
  return seq;
}

void RpcClient::sendFrame(const mrpc::RpcMessage& msg, uint8_t type) {
  mrpc::Buffer out;
  if (!mrpc::RpcCodec::encode(msg, type, &out)) {
    emit errorOccurred(QStringLiteral("编码失败：type=%1").arg(type));
    return;
  }

  const std::string wire = out.retrieveAllAsString();
  const qint64 written =
      socket_.write(wire.data(), static_cast<qint64>(wire.size()));
  if (written < 0) {
    emit errorOccurred(QStringLiteral("发送失败：%1").arg(socket_.errorString()));
  }
}

void RpcClient::onConnected() { emit connected(); }

void RpcClient::onDisconnected() {
  if (!pending_.isEmpty()) {
    emit errorOccurred(
        QStringLiteral("连接断开，%1 个在途请求未收到响应").arg(pending_.size()));
    pending_.clear();
  }
  emit disconnected();
}

void RpcClient::onSocketError(QAbstractSocket::SocketError /*error*/) {
  emit errorOccurred(socket_.errorString());
}

void RpcClient::onReadyRead() {
  const QByteArray chunk = socket_.readAll();
  recvBuffer_.append(chunk.constData(), static_cast<size_t>(chunk.size()));

  // 一次 readyRead 可能收到半包，也可能收到多帧；循环切到解不出为止。
  while (recvBuffer_.readableBytes() >= mrpc::kRpcHeaderLen) {
    uint8_t type = 0;
    mrpc::RpcMessage msg;
    const mrpc::RpcCodec::ParseResult result =
        mrpc::RpcCodec::parse(&recvBuffer_, &type, &msg);

    if (result == mrpc::RpcCodec::kIncomplete) break;
    if (result == mrpc::RpcCodec::kError) {
      emit errorOccurred(QStringLiteral("协议错误，已断开连接"));
      recvBuffer_.retrieveAll();
      socket_.disconnectFromHost();
      return;
    }
    handleFrame(type, msg);
  }
}

void RpcClient::handleFrame(uint8_t type, const mrpc::RpcMessage& msg) {
  switch (type) {
    case mrpc::kRpcHeartbeat: {
      // 服务端按 lastReceiveTime 判活；不回 ack 会在 timeout 后被踢掉。
      mrpc::RpcMessage ack;
      ack.set_seq(msg.seq());
      sendFrame(ack, mrpc::kRpcHeartbeatAck);
      emit heartbeatAcked();
      return;
    }
    case mrpc::kRpcHeartbeatAck:
      return;
    case mrpc::kRpcResponse: {
      const quint64 seq = msg.seq();
      QElapsedTimer timer = pending_.take(seq);
      const qint64 rttMs = timer.isValid() ? timer.elapsed() : -1;

      if (msg.error_code() != 0) {
        emit echoReplied(seq, QString(), msg.error_code(),
                         QString::fromStdString(msg.error_msg()), rttMs);
        return;
      }

      mrpc::EchoResponse response;
      if (!response.ParseFromString(msg.payload())) {
        emit errorOccurred(QStringLiteral("响应 payload 解析失败（seq=%1）").arg(seq));
        return;
      }
      emit echoReplied(seq, QString::fromStdString(response.text()), 0, QString(),
                       rttMs);
      return;
    }
    default:
      emit errorOccurred(QStringLiteral("收到未知帧类型 %1（seq=%2）")
                             .arg(type)
                             .arg(msg.seq()));
      return;
  }
}
