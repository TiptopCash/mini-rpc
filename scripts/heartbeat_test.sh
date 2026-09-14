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

echo "=== 场景 1: 客户端回复 ack（心跳 ${HB}s / 超时 ${TO}s，运行 ${DURATION}s）==="
./build/src/idle_client 127.0.0.1 $PORT 1 $DURATION

echo
echo "=== 场景 2: 客户端忽略 ping（应约 ${TO}s 后被断开）==="
./build/src/idle_client 127.0.0.1 $PORT 0 $DURATION

echo
echo "=== 服务端心跳 / 断开日志 ==="
grep -E "HeartbeatMonitor|force close" /tmp/hb_srv.log | head -25

echo
echo "=== 服务端是否存活 ==="
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
fi

kill $SRV 2>/dev/null

echo
echo "########## 完成 ##########"
