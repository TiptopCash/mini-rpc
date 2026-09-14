#!/usr/bin/env bash
# 客户端链路验证：RpcChannel（同步/异步/超时）+ TcpClient/Connector
#                    + 服务端 stub 调用 + ASan/UBSan/LeakSanitizer
# 用法: bash scripts/stub_client_test.sh
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9350
# 服务端的请求级兜底配得比客户端超时（150ms）长，才能观察到
# 「超时后服务端的迟到响应被客户端丢弃」这条路径
REQ_TIMEOUT_MS=400
cd "$ROOT" || exit 1

echo "########## 1. Stub 客户端（同步 / 异步 / 超时 / 连接复用）##########"
./build/src/rpc_server $PORT 4 0 0 $REQ_TIMEOUT_MS >/tmp/stub_server.log 2>&1 &
SRV=$!
sleep 1

./build/src/rpc_stub_client 127.0.0.1 $PORT 200 >/tmp/stub_client.log 2>&1
RC=$?
cat /tmp/stub_client.log
echo "客户端退出码=$RC"

echo
echo "--- 服务端未崩溃 ---"
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "服务端已退出或崩溃"
  RC=1
fi

echo "--- 分帧层无坏帧 ---"
if grep -q "bad frame" /tmp/stub_server.log; then
  grep "bad frame" /tmp/stub_server.log | head -5
  RC=1
else
  echo "（无坏帧）"
fi

echo "--- 迟到响应被丢弃（客户端已超时，服务端兜底响应后到）---"
if grep -q "no pending call for seq" /tmp/stub_client.log; then
  echo "已丢弃并记录"
  grep "no pending call for seq" /tmp/stub_client.log | head -2
else
  echo "未观察到丢弃日志，超时后的迟到响应路径没有被覆盖"
  RC=1
fi

echo "--- 服务端请求级兜底确实触发过 ---"
if grep -q "timed out in mrpc.EchoService.NoReply" /tmp/stub_server.log; then
  echo "已触发"
else
  echo "未触发：检查服务端 requestTimeoutMs 配置"
  RC=1
fi

echo
echo "--- 优雅退出：SIGTERM -> signalfd -> loop.quit() ---"
kill -TERM $SRV 2>/dev/null
wait $SRV 2>/dev/null
SRV_RC=$?
if [ "$SRV_RC" -eq 0 ]; then
  echo "正常退出（exit=0）"
else
  echo "退出码=$SRV_RC（非 0，析构未执行）"
  RC=1
fi

echo
echo "########## 2. ASan/UBSan/LeakSanitizer ##########"
ASAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="$ASAN_FLAGS" >/tmp/stub_asan_cmake.log 2>&1
cmake --build build-asan --target rpc_server rpc_stub_client -j \
  >/tmp/stub_asan_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "ASan 构建失败，日志尾部："
  tail -30 /tmp/stub_asan_build.log
  exit 1
fi

export UBSAN_OPTIONS=print_stacktrace=1
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan_stub_report
rm -f /tmp/asan_stub_report.*

./build-asan/src/rpc_server $PORT 4 0 0 $REQ_TIMEOUT_MS >/tmp/stub_asan_server.log 2>&1 &
SRV=$!
sleep 1

./build-asan/src/rpc_stub_client 127.0.0.1 $PORT 100 >/tmp/stub_asan_client.log 2>&1
RC2=$?
echo "ASan 客户端退出码=$RC2"
if [ "$RC2" -ne 0 ]; then
  tail -20 /tmp/stub_asan_client.log
  RC=1
fi

kill -TERM $SRV
wait $SRV 2>/dev/null
SRV_RC=$?
if [ "$SRV_RC" -ne 0 ]; then
  echo "ASan 服务端非正常退出（exit=$SRV_RC）"
  RC=1
fi

echo
if compgen -G "/tmp/asan_stub_report.*" >/dev/null; then
  echo "ASan/UBSan/LeakSanitizer 报告非空（存在问题）："
  cat /tmp/asan_stub_report.*
  exit 1
else
  echo "报告为空：无内存错误、无未定义行为、无泄漏"
fi

if [ "$RC" -eq 0 ]; then
  echo
  echo "Stub 客户端链路验证通过"
fi
exit $RC
