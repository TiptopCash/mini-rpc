# mini-rpc Qt 客户端

mini-rpc 的 Qt Widgets 桌面客户端。服务端代码一行不改，客户端直接说仓库已有的线协议：

```
8 字节固定头（magic=0xCAFE / version / type / bodyLen，大端）+ protobuf body
```

分帧和序列化复用仓库里的 `src/protocol/RpcCodec.cc` 与 `src/net/Buffer.cc`，
没有另写一套协议。

## 功能

- 连接 / 断开 RPC 服务端（默认 `127.0.0.1:9300`）
- 发送 `mrpc.EchoService/Echo` 请求，显示响应、`error_code` 和 RTT
- 按 `seq` 匹配响应，支持多个请求同时在途
- 收到服务端心跳 ping 后回 `kRpcHeartbeatAck`（默认 10s ping / 30s 超时）
- 断线、连接失败、协议错误都会写进日志面板

## 依赖

- CMake 3.16+
- Qt 6：Widgets + Network 模块
- protobuf（版本与服务端一致）

Ubuntu / WSL：

```bash
sudo apt install qt6-base-dev libprotobuf-dev protobuf-compiler
```

## 构建

```bash
cd qt-client
cmake -B build
cmake --build build -j
```

## 运行

先启动服务端（仓库根目录）：

```bash
./build/src/rpc_server 9300 4
```

再启动客户端：

```bash
./qt-client/build/mini_rpc_qt_client
```

Windows 上用 WSLg 运行时会直接弹出窗口；如果配置了 Windows 版 Qt 和 protobuf，
同一份 CMake 工程也可以在 Windows 原生构建。

## 无界面自检

```bash
# 连接 -> 发一次 Echo -> 打印响应 -> 退出
./build/mini_rpc_qt_client --selftest 127.0.0.1 9300 hello

# 空闲 35 秒再发，验证心跳 ack（服务端 30 秒无往来会踢连接）
./build/mini_rpc_qt_client --idle-test 127.0.0.1 9300 35
```

## 线程模型

所有网络操作都在 GUI 线程：`QTcpSocket` 是非阻塞的，`readyRead` 信号驱动收包，
`connect`/`send` 只是把字节写进 Qt 的发送缓冲区。没有额外线程，也没有锁。
