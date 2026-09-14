#!/usr/bin/env bash
# RPC 服务端验证：protobuf 反射分发 + 服务注册表 + 错误码 + 多连接并发
#                  + 优雅退出 + ASan/UBSan/LeakSanitizer
# 用法: bash scripts/rpc_test.sh
set -u

ROOT="/mnt/c/Users/11816/Desktop/项目/rpc"
PORT=9300
# 请求级超时兜底配短一点：NoReply 用例要等到兜底触发才算通过
REQ_TIMEOUT_MS=300
cd "$ROOT" || exit 1

echo "########## 1. 功能 / 错误码 / 并发 ##########"
./build/src/rpc_server $PORT 4 0 0 $REQ_TIMEOUT_MS >/tmp/rpc_server.log 2>&1 &
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
  RC=1
fi

echo "--- 服务端是否出现坏帧（分帧层致命错误，不应出现）---"
if grep -q "bad frame" /tmp/rpc_server.log; then
  echo "发现坏帧："
  grep "bad frame" /tmp/rpc_server.log | head -5
  RC=1
else
  echo "（无坏帧）"
fi

echo "--- 服务注册日志 ---"
grep "registered" /tmp/rpc_server.log

echo "--- 请求级超时兜底：NoReply 漏调 done->Run() ---"
if grep -q "timed out in mrpc.EchoService.NoReply" /tmp/rpc_server.log; then
  echo "兜底已触发（客户端应收到 kRpcTimeout=6）"
  grep "timed out in" /tmp/rpc_server.log | head -3
else
  echo "未看到超时兜底日志，说明漏调 done->Run() 的请求没有被回收"
  RC=1
fi

echo
echo "--- 优雅退出：SIGTERM -> signalfd -> loop.quit() ---"
kill -TERM $SRV 2>/dev/null
wait $SRV 2>/dev/null
SRV_RC=$?
if [ "$SRV_RC" -eq 0 ]; then
  echo "正常退出（exit=0），所有对象按析构顺序释放"
else
  echo "退出码=$SRV_RC（非 0，说明被信号直接杀死，析构未执行）"
  RC=1
fi
if grep -q "loop exited, cleaning up" /tmp/rpc_server.log; then
  echo "清理日志已打印：loop 正常返回"
else
  echo "缺少清理日志，loop 未正常返回"
  RC=1
fi

echo
echo "########## 2. ASan/UBSan/LeakSanitizer ##########"
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
export UBSAN_OPTIONS=print_stacktrace=1

# 2a 阳性对照：故意泄漏一段内存，确认 LeakSanitizer 确实在工作。
# 否则「没有报告」可能只是「检测器没开」，结论不可信。
cat >/tmp/leak_probe.cc <<'EOF'
int main() { new int[100]; return 0; }
EOF
g++ -fsanitize=address -g -o /tmp/leak_probe /tmp/leak_probe.cc
rm -f /tmp/probe_report.*
ASAN_OPTIONS=detect_leaks=1:log_path=/tmp/probe_report /tmp/leak_probe \
  >/dev/null 2>&1
if compgen -G "/tmp/probe_report.*" >/dev/null; then
  echo "阳性对照通过：LeakSanitizer 能检测到故意泄漏"
else
  echo "阳性对照失败：LeakSanitizer 未生效，下面的结论不可信"
  exit 1
fi

# 2b 真实服务端：跑完整用例后优雅退出，让所有析构函数执行，泄漏才可判定
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan_rpc_report
rm -f /tmp/asan_rpc_report.*

./build-asan/src/rpc_server $PORT 4 0 0 $REQ_TIMEOUT_MS >/tmp/rpc_asan_server.log 2>&1 &
SRV=$!
sleep 1

./build-asan/src/rpc_client 127.0.0.1 $PORT 500 4
RC=$?

kill -TERM $SRV
wait $SRV 2>/dev/null
SRV_RC=$?
if [ "$SRV_RC" -ne 0 ]; then
  echo "ASan 服务端非正常退出（exit=$SRV_RC）"
  RC=1
fi

echo
if compgen -G "/tmp/asan_rpc_report.*" >/dev/null; then
  echo "ASan/UBSan/LeakSanitizer 报告非空（存在问题）："
  cat /tmp/asan_rpc_report.*
  exit 1
else
  echo "报告为空：无内存错误、无未定义行为、无泄漏"
fi

exit $RC
