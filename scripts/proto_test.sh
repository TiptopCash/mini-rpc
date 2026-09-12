#!/usr/bin/env bash
# 协议层验证：编解码单元测试 + 端到端（粘包 / 半包）
set -u

ROOT="/mnt/c/Users/11816/Desktop/项目/rpc"
PORT=9200
cd "$ROOT" || exit 1

echo "########## 1. 编解码单元测试 ##########"
./build/src/codec_test
UT=$?
echo "单元测试退出码=$UT"

echo
echo "########## 2. 端到端（1000 条 = 500 粘包 + 500 半包）##########"
./build/src/proto_echo_server $PORT 4 >/tmp/pes.log 2>&1 &
SRV=$!
sleep 1

./build/src/proto_echo_client 127.0.0.1 $PORT 1000
RC=$?
echo "客户端退出码=$RC"

echo
echo "--- 服务端收到并处理的帧数 ---"
grep -c "seq=" /tmp/pes.log

echo "--- 服务端是否存活 ---"
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
fi

echo "--- 服务端错误检查 ---"
if grep -iqE "bad frame|error|failed" /tmp/pes.log; then
  grep -iE "bad frame|error|failed" /tmp/pes.log | head -5
else
  echo "（无错误）"
fi

kill $SRV 2>/dev/null

echo
echo "########## 完成 ##########"
