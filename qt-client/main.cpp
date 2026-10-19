#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include "MainWindow.h"
#include "RpcClient.h"

// 无界面自检：连接、发一次 Echo、打印响应、退出。
// 用法：./mini_rpc_qt_client --selftest [host] [port] [text]
static int runSelfTest(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  const QString host =
      argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("127.0.0.1");
  const quint16 port =
      argc > 3 ? static_cast<quint16>(QString(argv[3]).toUShort()) : 9300;
  const QString text =
      argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("qt-selftest");

  RpcClient client;
  int exitCode = 1;

  QObject::connect(&client, &RpcClient::connected, &app, [&] {
    qInfo() << "connected, sending Echo";
    client.sendEcho(text);
  });

  QObject::connect(
      &client, &RpcClient::echoReplied, &app,
      [&](quint64 seq, const QString& reply, int errorCode,
          const QString& errorMsg, qint64 rttMs) {
        qInfo().noquote()
            << QStringLiteral("reply seq=%1 error_code=%2 rtt=%3ms payload=%4")
                   .arg(seq)
                   .arg(errorCode)
                   .arg(rttMs)
                   .arg(errorCode == 0 ? reply : errorMsg);
        exitCode = errorCode == 0 ? 0 : 1;
        app.quit();
      });

  QObject::connect(&client, &RpcClient::errorOccurred, &app,
                   [&](const QString& message) {
                     qCritical().noquote()
                         << QStringLiteral("error: %1").arg(message);
                     exitCode = 1;
                   });

  QTimer::singleShot(5000, &app, [&] {
    qCritical() << "self-test timeout (5s)";
    app.quit();
  });

  client.connectToServer(host, port);
  app.exec();
  return exitCode;
}

// 空闲自检：连接后先空闲 N 秒（服务端默认 10s 发 ping、30s 无往来则踢人），
// 再发一次 Echo。能成功说明客户端正确回了心跳 ack，而不是靠服务端关心跳。
// 用法：./mini_rpc_qt_client --idle-test [host] [port] [seconds]
static int runIdleTest(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  const QString host =
      argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("127.0.0.1");
  const quint16 port =
      argc > 3 ? static_cast<quint16>(QString(argv[3]).toUShort()) : 9300;
  const int idleSeconds = argc > 4 ? QString(argv[4]).toInt() : 35;

  RpcClient client;
  int exitCode = 1;

  QObject::connect(&client, &RpcClient::connected, &app, [&] {
    qInfo() << "connected, idling" << idleSeconds
            << "seconds to exercise heartbeat";
    QTimer::singleShot(idleSeconds * 1000, &app,
                       [&] { client.sendEcho(QStringLiteral("after-idle")); });
  });

  QObject::connect(
      &client, &RpcClient::echoReplied, &app,
      [&](quint64 seq, const QString& reply, int errorCode,
          const QString& errorMsg, qint64 rttMs) {
        qInfo().noquote()
            << QStringLiteral("reply seq=%1 error_code=%2 rtt=%3ms payload=%4")
                   .arg(seq)
                   .arg(errorCode)
                   .arg(rttMs)
                   .arg(errorCode == 0 ? reply : errorMsg);
        exitCode = errorCode == 0 ? 0 : 1;
        app.quit();
      });

  QObject::connect(&client, &RpcClient::errorOccurred, &app,
                   [&](const QString& message) {
                     qCritical().noquote()
                         << QStringLiteral("error: %1").arg(message);
                     exitCode = 1;
                   });

  QTimer::singleShot((idleSeconds + 5) * 1000, &app, [&] {
    qCritical() << "idle-test timeout";
    app.quit();
  });

  client.connectToServer(host, port);
  app.exec();
  return exitCode;
}

int main(int argc, char** argv) {
  // 离屏渲染一张窗口截图，用于确认界面布局（CI/无显示器环境也能跑）。
  // 用法：QT_QPA_PLATFORM=offscreen ./mini_rpc_qt_client --screenshot /tmp/qt-client.png
  if (argc > 1 &&
      QString::fromLocal8Bit(argv[1]) == QStringLiteral("--screenshot")) {
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2])
                                  : QStringLiteral("/tmp/qt-client.png");
    QTimer::singleShot(800, &app, [&] {
      const bool ok = window.grab().save(path);
      qInfo().noquote() << QStringLiteral("screenshot %1: %2")
                               .arg(ok ? QStringLiteral("saved")
                                       : QStringLiteral("failed"),
                                    path);
      app.quit();
    });
    return app.exec();
  }

  if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--selftest")) {
    return runSelfTest(argc, argv);
  }
  if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--idle-test")) {
    return runIdleTest(argc, argv);
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("mini-rpc Qt client"));
  QApplication::setOrganizationName(QStringLiteral("mini-rpc"));

  MainWindow window;
  window.show();
  return app.exec();
}
