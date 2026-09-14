#!/usr/bin/env bash
# 连接池 + 负载均衡验证：轮询分散、一致性哈希粘性、单节点故障转移，含 ASan/LSan
# 用法: bash scripts/pool_client_test.sh
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORTA=9360
PORTB=9361
COUNT=100
cd "$ROOT" || exit 1

# 两个后端只在 nodeTag 上不同，客户端靠响应里的 tag 分辨是哪个节点服务的
start_servers() {
  local bin="$1"
  "$bin" $PORTA 2 0 0 0 A >/tmp/pool_srv_a.log 2>&1 &
  SRV_A=$!
  "$bin" $PORTB 2 0 0 0 B >/tmp/pool_srv_b.log 2>&1 &
  SRV_B=$!
  sleep 1
}

echo "########## 1. 轮询 + 一致性哈希 ##########"
start_servers ./build/src/rpc_server

./build/src/rpc_pool_client 127.0.0.1 $PORTA $PORTB $COUNT all >/tmp/pool_client.log 2>&1
RC=$?
grep -E "^(----|轮询|一致性哈希)" /tmp/pool_client.log
echo "客户端退出码=$RC"

echo "--- 轮询把请求分散到两个节点 ---"
if grep -q "轮询: 成功 $COUNT/$COUNT" /tmp/pool_client.log &&
   grep -q "\[A\]=" /tmp/pool_client.log &&
   grep -q "\[B\]=" /tmp/pool_client.log; then
  echo "已分散"
else
  echo "未分散到两个节点"
  RC=1
fi

echo "--- 一致性哈希把同一 key 固定在单个节点 ---"
HASH_TAGS=$(grep "^一致性哈希:" /tmp/pool_client.log | grep -o '\[[A-Za-z0-9_]*\]=' | wc -l)
if grep -q "一致性哈希: 成功 $COUNT/$COUNT" /tmp/pool_client.log &&
   [ "$HASH_TAGS" -eq 1 ]; then
  echo "已固定到单节点"
else
  echo "同一 key 落到了 $HASH_TAGS 个节点上"
  RC=1
fi

echo
echo "--- 服务端未崩溃、分帧层无坏帧 ---"
for f in /tmp/pool_srv_a.log /tmp/pool_srv_b.log; do
  if grep -q "bad frame" "$f"; then
    grep "bad frame" "$f" | head -3
    RC=1
  fi
done
kill -0 $SRV_A 2>/dev/null && kill -0 $SRV_B 2>/dev/null && echo "两个节点都存活" || { echo "有节点已退出"; RC=1; }

echo
echo "########## 2. 失败转移（关掉节点 B 之后请求仍全部成功）##########"
kill -TERM $SRV_B 2>/dev/null
wait $SRV_B 2>/dev/null
echo "已停掉节点 B（tag=B）"

./build/src/rpc_pool_client 127.0.0.1 $PORTA $PORTB $COUNT failover >/tmp/pool_failover.log 2>&1
RC2=$?
grep -E "^(----|失败转移)" /tmp/pool_failover.log
grep -q "节点 .* 连接不可用，尝试下一个" /tmp/pool_failover.log && echo "已观察到失败转移日志"
echo "客户端退出码=$RC2"
[ "$RC2" -eq 0 ] || RC=1

echo
echo "--- 优雅退出节点 A ---"
kill -TERM $SRV_A 2>/dev/null
wait $SRV_A 2>/dev/null
SRV_RC=$?
if [ "$SRV_RC" -eq 0 ]; then
  echo "正常退出（exit=0）"
else
  echo "退出码=$SRV_RC（非 0，析构未执行）"
  RC=1
fi

echo
echo "########## 3. ASan/UBSan/LeakSanitizer ##########"
ASAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="$ASAN_FLAGS" >/tmp/pool_asan_cmake.log 2>&1
cmake --build build-asan --target rpc_server rpc_pool_client -j \
  >/tmp/pool_asan_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "ASan 构建失败，日志尾部："
  tail -30 /tmp/pool_asan_build.log
  exit 1
fi

export UBSAN_OPTIONS=print_stacktrace=1
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan_pool_report
rm -f /tmp/asan_pool_report.*

build-asan/src/rpc_server $PORTA 2 0 0 0 A >/tmp/pool_asan_srv_a.log 2>&1 &
SRV_A=$!
build-asan/src/rpc_server $PORTB 2 0 0 0 B >/tmp/pool_asan_srv_b.log 2>&1 &
SRV_B=$!
sleep 1

build-asan/src/rpc_pool_client 127.0.0.1 $PORTA $PORTB 50 all >/tmp/pool_asan_client.log 2>&1
RC2=$?
echo "ASan 客户端退出码=$RC2"
if [ "$RC2" -ne 0 ]; then
  tail -20 /tmp/pool_asan_client.log
  RC=1
fi

kill -TERM $SRV_A $SRV_B 2>/dev/null
wait $SRV_A 2>/dev/null
[ $? -eq 0 ] || { echo "ASan 节点 A 非正常退出"; RC=1; }
wait $SRV_B 2>/dev/null
[ $? -eq 0 ] || { echo "ASan 节点 B 非正常退出"; RC=1; }

echo
if compgen -G "/tmp/asan_pool_report.*" >/dev/null; then
  echo "ASan/UBSan/LeakSanitizer 报告非空（存在问题）："
  cat /tmp/asan_pool_report.*
  exit 1
else
  echo "报告为空：无内存错误、无未定义行为、无泄漏"
fi

if [ "$RC" -eq 0 ]; then
  echo
  echo "连接池验证通过"
fi
exit $RC
