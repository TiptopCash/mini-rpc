#!/usr/bin/env bash
# 心跳与空闲检测验证
#   场景 1：客户端回复 ack -> 空闲但健康，应一直存活
#   场景 2：客户端忽略 ping -> 服务端应在 timeout 后强制断开
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9300
HB=2
TO=5
DURATION=8
cd "$ROOT" || exit 1

cmake --build build -j >/tmp/hb_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "构建失败："
  tail -30 /tmp/hb_build.log
  exit 1
fi

rm -f /tmp/hb_srv.log
./build/src/proto_echo_server $PORT 4 $HB $TO >/tmp/hb_srv.log 2>&1 &
SRV=$!
sleep 1

# RC 累积本轮所有检查的失败。脚本必须以 exit $RC 收尾 ——
# 否则最后一条 echo 会让退出码恒为 0，这条 CI 步骤永远不会失败。
RC=0

echo "=== 场景 1: 客户端回复 ack（心跳 ${HB}s / 超时 ${TO}s，运行 ${DURATION}s）==="
OUT1=$(./build/src/idle_client 127.0.0.1 $PORT 1 $DURATION)
echo "$OUT1"
if echo "$OUT1" | grep -q "收到 ping: 0 次"; then
  echo "场景 1 失败：一次 ping 都没收到，心跳根本没发出去"
  RC=1
fi
if echo "$OUT1" | grep -q "仍然存活"; then
  echo "场景 1 通过：按时回 ack 的连接未被断开"
else
  echo "场景 1 失败：回了 ack 的连接不应被断开"
  RC=1
fi

echo
echo "=== 场景 2: 客户端忽略 ping（应约 ${TO}s 后被断开）==="
OUT2=$(./build/src/idle_client 127.0.0.1 $PORT 0 $DURATION)
echo "$OUT2"
if echo "$OUT2" | grep -q "已被服务端断开"; then
  echo "场景 2 通过：忽略 ping 的连接被服务端强制断开"
else
  echo "场景 2 失败：忽略 ping 的连接应在 ${TO}s 超时后被断开"
  RC=1
fi

echo
echo "=== 服务端心跳 / 断开日志 ==="
grep -E "HeartbeatMonitor|force close" /tmp/hb_srv.log | head -25

echo
echo "=== 服务端是否存活 ==="
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
  RC=1
fi

if grep -q "force close" /tmp/hb_srv.log; then
  echo "服务端日志确认执行了 force close"
else
  echo "服务端日志缺少 force close"
  RC=1
fi

kill $SRV 2>/dev/null

echo
if [ "$RC" -eq 0 ]; then
  echo "########## 完成：全部通过 ##########"
else
  echo "########## 完成：存在失败 ##########"
fi
exit $RC
