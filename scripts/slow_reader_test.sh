#!/usr/bin/env bash
# 强制触发 EPOLLOUT 部分写路径
#
# 严谨性依据：
#   tcp_rmem max = 32MB, tcp_wmem max = 4MB
#   内核最多吞下 36MB（服务端发送缓冲 + 客户端接收缓冲）
#   本次发送 64MB > 36MB，多余部分必然滞留在服务端 outputBuffer_
#   并在 stall 期间通过 EPOLLOUT 续传
# 直接证据：stall 期间采样服务端 RSS，应显著高于基线
set -u

# 从脚本自身位置推断仓库根目录，脚本放在任何路径下都能跑
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=9100
MB=64
STALL_MS=5000
cd "$ROOT" || exit 1

cmake --build build -j >/tmp/slow_build.log 2>&1
if [ $? -ne 0 ]; then
  echo "构建失败："
  tail -30 /tmp/slow_build.log
  exit 1
fi

./build/src/echo_server $PORT 4 >/tmp/slow_srv.log 2>&1 &
SRV=$!
sleep 1

rss_before=$(awk '/VmRSS/{print $2}' /proc/$SRV/status)
echo "服务端基线 RSS = ${rss_before} kB"

echo "=== 慢读测试：${MB}MB / 暂停 ${STALL_MS}ms 不读 ==="
./build/src/slow_reader 127.0.0.1 $PORT $MB $STALL_MS >/tmp/slow_out.log 2>&1 &
CLI=$!

# 在 stall 窗口内采样（发送很快完成，主要时间花在暂停上）
sleep 3
rss_during=$(awk '/VmRSS/{print $2}' /proc/$SRV/status)

wait $CLI
RC=$?

echo
cat /tmp/slow_out.log
echo
echo "服务端 RSS: 基线=${rss_before} kB  →  stall 期间=${rss_during} kB"
echo "增量 = $((rss_during - rss_before)) kB （即 outputBuffer_ 中滞留的数据量）"

echo
echo "=== 服务端是否存活 ==="
if kill -0 $SRV 2>/dev/null; then
  echo "存活，未崩溃"
else
  echo "已崩溃"
fi

echo "=== 服务端错误检查 ==="
if grep -iqE "failed|error" /tmp/slow_srv.log; then
  grep -iE "failed|error" /tmp/slow_srv.log | head -10
else
  echo "（无错误）"
fi

kill $SRV 2>/dev/null
echo
echo "退出码=$RC （0 = 字节全部一致）"
