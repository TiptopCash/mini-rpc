#!/usr/bin/env bash
# 协议层验证：编解码单元测试 + 端到端（粘包 / 半包）
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9200
cd "$ROOT" || exit 1

# RC 累积本轮所有检查的失败。脚本必须以 exit $RC 收尾 ——
# 否则最后一条 echo 会让退出码恒为 0，这条 CI 步骤永远不会失败。
RC=0

echo "########## 1. 编解码单元测试 ##########"
./build/src/codec_test
UT=$?
echo "单元测试退出码=$UT"
if [ "$UT" -ne 0 ]; then
  echo "编解码单元测试失败"
  RC=1
fi

echo
echo "########## 2. 端到端（1000 条 = 500 粘包 + 500 半包）##########"
./build/src/proto_echo_server $PORT 4 >/tmp/pes.log 2>&1 &
SRV=$!
sleep 1

./build/src/proto_echo_client 127.0.0.1 $PORT 1000
CLI_RC=$?
echo "客户端退出码=$CLI_RC"
if [ "$CLI_RC" -ne 0 ]; then
  RC=1
fi

echo
echo "--- 服务端收到并处理的帧数 ---"
FRAMES=$(grep -c "seq=" /tmp/pes.log || true)
FRAMES=${FRAMES:-0}
echo "$FRAMES"
if [ "$FRAMES" -lt 1000 ]; then
  echo "服务端处理的帧数不足（期望 1000，实际 $FRAMES）"
  RC=1
fi

echo "--- 服务端是否存活 ---"
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
  RC=1
fi

echo "--- 服务端错误检查 ---"
if grep -q "bad frame" /tmp/pes.log; then
  echo "出现坏帧（分帧层致命错误）："
  grep "bad frame" /tmp/pes.log | head -5
  RC=1
else
  echo "（无坏帧）"
fi
# 其余 error/failed 只作展示：它们不足以判定失败，不并入 RC（避免误报把 CI 弄红）
if grep -iqE "error|failed" /tmp/pes.log; then
  grep -iE "error|failed" /tmp/pes.log | head -5
fi

kill $SRV 2>/dev/null

echo
if [ "$RC" -eq 0 ]; then
  echo "########## 完成：全部通过 ##########"
else
  echo "########## 完成：存在失败 ##########"
fi
exit $RC
