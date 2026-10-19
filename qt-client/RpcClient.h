#pragma once

#include <cstdint>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTcpSocket>

#include "net/Buffer.h"

namespace mrpc {
class RpcMessage;
}

// RPC 客户端的 Qt 实现。
//
// 传输用 QTcpSocket（信号槽 + 非阻塞），分帧和序列化直接复用仓库里
// 的 RpcCodec / Buffer —— 客户端与服务端说的是同一套线协议：
//   8 字节固定头（magic/version/type/bodyLen，大端）+ protobuf body
//
// 所有方法都在 GUI 线程调用，不需要额外线程；响应到达由 readyRead 驱动。
class RpcClient : public QObject {
  Q_OBJECT

 public:
  explicit RpcClient(QObject* parent = nullptr);
  ~RpcClient() override;

  void connectToServer(const QString& host, quint16 port);
  void disconnectFromServer();
  bool isConnected() const;

  // 发起一次 Echo 调用，返回请求序号；响应通过 echoReplied 带回来。
  // 支持多个请求同时在途，按 seq 匹配。
  quint64 sendEcho(const QString& text);

 signals:
  void connected();
  void disconnected();
  void errorOccurred(const QString& message);
  // errorCode == 0 时 text 是服务端返回的业务内容；否则看 errorMsg
  void echoReplied(quint64 seq, const QString& text, int errorCode,
                   const QString& errorMsg, qint64 rttMs);
  // 收到服务端 ping 并已回 ack（服务端默认 10s 空闲发一次）
  void heartbeatAcked();

 private slots:
  void onConnected();
  void onDisconnected();
  void onReadyRead();
  void onSocketError(QAbstractSocket::SocketError error);

 private:
  void sendFrame(const mrpc::RpcMessage& msg, uint8_t type);
  void handleFrame(uint8_t type, const mrpc::RpcMessage& msg);

  QTcpSocket socket_;
  mrpc::Buffer recvBuffer_;
  QHash<quint64, QElapsedTimer> pending_;
  quint64 nextSeq_ = 1;
};
