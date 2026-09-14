#!/usr/bin/env bash
# RPC 服务端验证：protobuf 反射分发 + 服务注册表 + 错误码 + 多连接并发
# 用法: bash scripts/rpc_test.sh
set -u

ROOT="/mnt/c/Users/11816/Desktop/项目/rpc"
PORT=9300
cd "$ROOT" || exit 1

echo "########## 1. 功能 / 错误码 / 并发 ##########"
./build/src/rpc_server $PORT 4 0 0 >/tmp/rpc_server.log 2>&1 &
SRV=$!
sleep 1

./build/src/rpc_client 127.0.0.1 $PORT 2000 4
RC=$?
echo "客户端退出码=$RC"

echo
echo "--- 服务端是否存活 ---"
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
fi

echo "--- 服务端是否出现坏帧（分帧层致命错误，不应出现）---"
if grep -q "bad frame" /tmp/rpc_server.log; then
  echo "发现坏帧："
  grep "bad frame" /tmp/rpc_server.log | head -5
else
  echo "（无坏帧）"
fi

echo "--- 服务注册日志 ---"
grep "registered" /tmp/rpc_server.log

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo
echo "########## 2. ASan/UBSan 内存检查 ##########"
ASAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="$ASAN_FLAGS" >/tmp/rpc_asan_cmake.log 2>&1
cmake --build build-asan --target rpc_server rpc_client -j \
  >/tmp/rpc_asan_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "ASan 构建失败，日志尾部："
  tail -30 /tmp/rpc_asan_build.log
  exit 1
fi

# detect_leaks=0：服务端暂无优雅退出（SIGINT -> loop.quit()），
# 进程被 kill 时 LeakSanitizer 无法给出有意义的结论
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=/tmp/asan_rpc_report
export UBSAN_OPTIONS=print_stacktrace=1
rm -f /tmp/asan_rpc_report.*

./build-asan/src/rpc_server $PORT 4 0 0 >/tmp/rpc_asan_server.log 2>&1 &
SRV=$!
sleep 1

./build-asan/src/rpc_client 127.0.0.1 $PORT 500 4
RC=$?

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null

echo
if compgen -G "/tmp/asan_rpc_report.*" >/dev/null; then
  echo "ASan/UBSan 报告非空（存在问题）："
  cat /tmp/asan_rpc_report.*
  exit 1
else
  echo "ASan / UBSan 报告为空（无内存错误、无未定义行为）"
fi

exit $RC
