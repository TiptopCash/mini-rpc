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

WSLg 下可能看到 `libEGL warning` / `MESA: error: ZINK` 之类的输出，这是 WSLg
探测 GPU 失败后回退软件渲染，不影响窗口和功能。要消掉这些行：

```bash
LIBGL_ALWAYS_SOFTWARE=1 ./build/mini_rpc_qt_client
```

如果 WSLg 以 `COPY MODE` 运行、窗口在桌面上看不到（进程和 X 窗口都在，只是不显示），
直接用下面的 Windows 原生构建，客户端通过 `127.0.0.1` 连 WSL 里的服务端即可。

## Windows 原生构建（MSYS2 / MinGW）

在 [MSYS2](https://www.msys2.org/) 的 MINGW64 终端里装依赖：

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base mingw-w64-x86_64-protobuf
```

CMake 生成 protoc 命令时经过 `cmd.exe`，项目路径里不能有中文（会被按 GBK 解释成
不存在的目录）。路径不满足时，先建一个纯 ASCII 的目录联接再构建：

```bat
mklink /J C:\Users\<你>\mrpc <项目真实路径>
```

然后构建、自检：

```bash
cd /c/Users/<你>/mrpc
cmake -S qt-client -B qt-client/build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build qt-client/build-win -j

./qt-client/build-win/mini_rpc_qt_client.exe --selftest 127.0.0.1 9300 win
```

打包成可双击运行的分发包（Qt 插件 + 全部 MinGW 运行时 DLL）：

```bash
cd qt-client
rm -rf dist && mkdir dist
cp build-win/mini_rpc_qt_client.exe dist/
windeployqt --release --compiler-runtime --no-translations dist/mini_rpc_qt_client.exe

# objdump 扫依赖闭包，把 /mingw64/bin 里的非系统 DLL 拷进 dist；
# 多扫几轮，直到新拷进来的 DLL 的依赖也被带进来
for i in 1 2 3 4 5; do
  find dist \( -name '*.exe' -o -name '*.dll' \) -print0 |
    xargs -0 -n1 objdump -p 2>/dev/null |
    awk '/DLL Name:/ {print $3}' | sort -u |
    while read -r d; do
      [ -f "dist/$d" ] || cp "/mingw64/bin/$d" dist/ 2>/dev/null || true
    done
done
```

`dist/mini_rpc_qt_client.exe` 可以脱离 MSYS2 终端直接双击运行。

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
