#!/usr/bin/env bash
# 压测：测不同线程数 / 消息大小下的吞吐与延迟
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9100
cd "$ROOT" || exit 1

cmake --build build -j >/tmp/bench_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "构建失败："
  tail -30 /tmp/bench_build.log
  exit 1
fi

./build/src/echo_server $PORT 4 >/tmp/bench_srv.log 2>&1 &
SRV=$!
sleep 1

for cfg in "4 5 64" "8 5 64" "16 5 64" "8 5 1024" "8 5 16384" "4 5 65536"; do
  set -- $cfg
  echo "=== threads=$1 seconds=$2 msg=$3 ==="
  ./build/src/bench_client 127.0.0.1 $PORT "$1" "$2" "$3"
  echo
done

echo "--- 服务端错误检查 ---"
grep -iE "failed|error" /tmp/bench_srv.log | head -10 || echo "（无错误）"

kill $SRV 2>/dev/null
echo "########## 压测完成 ##########"
