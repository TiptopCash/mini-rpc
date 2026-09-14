#!/usr/bin/env bash
# 网络层验证：ASan 内存检查 + 边界测试
# 用法: bash scripts/verify.sh
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9100
cd "$ROOT" || exit 1

ASAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

echo "########## STEP 1: ASan 构建 ##########"
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="$ASAN_FLAGS" >/tmp/asan_cmake.log 2>&1
cmake --build build-asan -j >/tmp/asan_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "构建失败，日志尾部："
  tail -30 /tmp/asan_build.log
  exit 1
fi
echo "ASan 构建成功"

echo
echo "########## STEP 1b: ASan 运行时 ##########"
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=/tmp/asan_report
export UBSAN_OPTIONS=print_stacktrace=1
rm -f /tmp/asan_report.* /tmp/asan_server.log

./build-asan/src/echo_server $PORT 4 >/tmp/asan_server.log 2>&1 &
SRV=$!
sleep 1

echo "-- 正常流量 5000 条 --"
./build/src/echo_client 127.0.0.1 $PORT 5000 64

echo "-- 异常断开 x30（客户端跑到一半被 kill -9）--"
pids=""
for i in $(seq 1 30); do
  ./build/src/echo_client 127.0.0.1 $PORT 200000 64 >/dev/null 2>&1 &
  pids="$pids $!"
done
sleep 0.5
kill -9 $pids 2>/dev/null
sleep 1

echo "-- 64KB 大包 x200（走 makeSpace 扩容）--"
./build/src/echo_client 127.0.0.1 $PORT 200 65536

sleep 1
kill $SRV 2>/dev/null
sleep 0.5
kill -9 $SRV 2>/dev/null

echo "--- 服务端日志尾部 ---"
tail -15 /tmp/asan_server.log
echo
echo "--- ASan / UBSan 报告 ---"
if ls /tmp/asan_report.* >/dev/null 2>&1; then
  head -60 /tmp/asan_report.*
else
  echo "（无报告文件 = 未发现内存/UB 错误）"
fi

echo
echo "########## STEP 2: 边界测试 ##########"
./build/src/echo_server $PORT 4 >/tmp/srv2.log 2>&1 &
SRV2=$!
sleep 1

echo "-- 200 并发连接 x 200 条 --"
t0=$(date +%s.%N)
pids=""
for i in $(seq 1 200); do
  ./build/src/echo_client 127.0.0.1 $PORT 200 64 >/dev/null 2>&1 &
  pids="$pids $!"
done
fail=0
for p in $pids; do
  wait "$p" || fail=$((fail + 1))
done
t1=$(date +%s.%N)
elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
echo "完成: 200 个客户端, 失败 $fail 个, 耗时 ${elapsed}s"

echo "-- 64KB x2000（扩容 + 部分写 + EPOLLOUT）--"
./build/src/echo_client 127.0.0.1 $PORT 2000 65536

echo "-- 服务端是否存活 --"
if kill -0 $SRV2 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃！"
fi
echo "--- 服务端日志尾部 ---"
tail -20 /tmp/srv2.log

kill $SRV2 2>/dev/null

echo
echo "########## 完成 ##########"
