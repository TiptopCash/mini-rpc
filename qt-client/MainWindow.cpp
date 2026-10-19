#include "MainWindow.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

#include "RpcClient.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      client_(new RpcClient(this)),
      hostEdit_(new QLineEdit(QStringLiteral("127.0.0.1"), this)),
      portSpin_(new QSpinBox(this)),
      connectButton_(new QPushButton(QStringLiteral("连接"), this)),
      statusLabel_(new QLabel(QStringLiteral("未连接"), this)),
      logView_(new QPlainTextEdit(this)),
      textEdit_(new QLineEdit(this)),
      sendButton_(new QPushButton(QStringLiteral("发送 Echo"), this)) {
  setWindowTitle(QStringLiteral("mini-rpc Qt 客户端"));
  resize(780, 540);

  hostEdit_->setMaximumWidth(160);
  portSpin_->setRange(1, 65535);
  portSpin_->setValue(9300);
  portSpin_->setMaximumWidth(90);

  logView_->setReadOnly(true);
  logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  logView_->setPlaceholderText(
      QStringLiteral("连接 127.0.0.1:9300 后，在下方输入文本发送 Echo 请求"));

  textEdit_->setPlaceholderText(
      QStringLiteral("输入文本，回车发送（服务端会回 echo:<文本>）"));

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);

  auto* top = new QHBoxLayout();
  top->addWidget(new QLabel(QStringLiteral("服务端"), central));
  top->addWidget(hostEdit_);
  top->addWidget(new QLabel(QStringLiteral(":"), central));
  top->addWidget(portSpin_);
  top->addWidget(connectButton_);
  top->addSpacing(12);
  top->addWidget(statusLabel_, 1);
  root->addLayout(top);

  root->addWidget(logView_, 1);

  auto* bottom = new QHBoxLayout();
  bottom->addWidget(textEdit_, 1);
  bottom->addWidget(sendButton_);
  root->addLayout(bottom);

  setCentralWidget(central);

  connect(connectButton_, &QPushButton::clicked, this,
          &MainWindow::onConnectClicked);
  connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSendClicked);
  connect(textEdit_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);

  connect(client_, &RpcClient::connected, this, &MainWindow::onConnected);
  connect(client_, &RpcClient::disconnected, this, &MainWindow::onDisconnected);
  connect(client_, &RpcClient::errorOccurred, this, &MainWindow::onError);
  connect(client_, &RpcClient::echoReplied, this, &MainWindow::onEchoReplied);
  connect(client_, &RpcClient::heartbeatAcked, this,
          &MainWindow::onHeartbeatAcked);

  setConnectedUi(false);
  appendLog(QStringLiteral("就绪。默认连接 127.0.0.1:9300"));
}

void MainWindow::onConnectClicked() {
  if (client_->isConnected()) {
    client_->disconnectFromServer();
    return;
  }

  const QString host = hostEdit_->text().trimmed();
  if (host.isEmpty()) {
    appendLog(QStringLiteral("服务端地址不能为空"));
    return;
  }

  statusLabel_->setText(QStringLiteral("连接中…"));
  connectButton_->setEnabled(false);
  appendLog(QStringLiteral("正在连接 %1:%2 …")
                .arg(host)
                .arg(portSpin_->value()));
  client_->connectToServer(host, static_cast<quint16>(portSpin_->value()));
}

void MainWindow::onConnected() {
  connectButton_->setEnabled(true);
  setConnectedUi(true);
  statusLabel_->setText(QStringLiteral("已连接 %1:%2")
                            .arg(hostEdit_->text())
                            .arg(portSpin_->value()));
  appendLog(QStringLiteral("连接成功"));
  textEdit_->setFocus();
}

void MainWindow::onDisconnected() {
  connectButton_->setEnabled(true);
  pendingSeq_ = 0;
  setConnectedUi(false);
  statusLabel_->setText(QStringLiteral("未连接"));
  appendLog(QStringLiteral("连接已断开"));
}

void MainWindow::onSendClicked() {
  if (!client_->isConnected()) {
    appendLog(QStringLiteral("未连接，不能发送"));
    return;
  }
  if (pendingSeq_ != 0) {
    appendLog(QStringLiteral("上一个请求仍在等待响应（#%1）").arg(pendingSeq_));
    return;
  }

  const QString text = textEdit_->text();
  pendingSeq_ = client_->sendEcho(text);
  sendButton_->setEnabled(false);
  appendLog(QStringLiteral("→ #%1 %2")
                .arg(pendingSeq_)
                .arg(text.isEmpty() ? QStringLiteral("(空文本)") : text));
}

void MainWindow::onEchoReplied(quint64 seq, const QString& text, int errorCode,
                               const QString& errorMsg, qint64 rttMs) {
  if (seq == pendingSeq_) {
    pendingSeq_ = 0;
    sendButton_->setEnabled(client_->isConnected());
  }

  const QString rtt =
      rttMs >= 0 ? QStringLiteral("%1 ms").arg(rttMs) : QStringLiteral("?");
  if (errorCode == 0) {
    appendLog(QStringLiteral("← #%1 RTT %2  %3").arg(seq).arg(rtt, text));
  } else {
    appendLog(QStringLiteral("← #%1 RTT %2  error_code=%3  %4")
                  .arg(seq)
                  .arg(rtt)
                  .arg(errorCode)
                  .arg(errorMsg));
  }

  textEdit_->selectAll();
}

void MainWindow::onError(const QString& message) {
  appendLog(QStringLiteral("错误：%1").arg(message));
  if (!client_->isConnected()) {
    connectButton_->setEnabled(true);
    setConnectedUi(false);
    statusLabel_->setText(QStringLiteral("未连接"));
  }
}

void MainWindow::onHeartbeatAcked() {
  appendLog(QStringLiteral("♥ 收到服务端心跳 ping，已回 ack"));
}

void MainWindow::appendLog(const QString& line) {
  const QString ts = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
  logView_->appendPlainText(QStringLiteral("[%1] %2").arg(ts, line));
}

void MainWindow::setConnectedUi(bool connected) {
  connectButton_->setText(connected ? QStringLiteral("断开")
                                    : QStringLiteral("连接"));
  hostEdit_->setEnabled(!connected);
  portSpin_->setEnabled(!connected);
  textEdit_->setEnabled(connected);
  sendButton_->setEnabled(connected && pendingSeq_ == 0);

  statusLabel_->setStyleSheet(
      connected ? QStringLiteral("color:#0a7a2f; font-weight:600;")
                : QStringLiteral("color:#b02020; font-weight:600;"));
}
