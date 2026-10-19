#pragma once

#include <QMainWindow>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class RpcClient;

// 一个最小但完整可演示的 Qt 客户端：
// 连接/断开、发送 Echo、显示响应与 RTT、心跳 ack、错误与断线提示。
class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

 private slots:
  void onConnectClicked();
  void onConnected();
  void onDisconnected();
  void onSendClicked();
  void onEchoReplied(quint64 seq, const QString& text, int errorCode,
                     const QString& errorMsg, qint64 rttMs);
  void onError(const QString& message);
  void onHeartbeatAcked();

 private:
  void appendLog(const QString& line);
  void setConnectedUi(bool connected);

  RpcClient* client_;
  QLineEdit* hostEdit_;
  QSpinBox* portSpin_;
  QPushButton* connectButton_;
  QLabel* statusLabel_;
  QPlainTextEdit* logView_;
  QLineEdit* textEdit_;
  QPushButton* sendButton_;
  quint64 pendingSeq_ = 0;
};
