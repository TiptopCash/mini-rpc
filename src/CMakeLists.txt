find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${CMAKE_SOURCE_DIR}/proto/rpc.proto)

# ---- 网络层（不依赖 protobuf）----
add_library(mrpc_net STATIC
  common/Logger.cc
  net/InetAddress.cc
  net/Socket.cc
  net/Buffer.cc
  net/Channel.cc
  net/Epoller.cc
  net/EventLoop.cc
  net/EventLoopThread.cc
  net/EventLoopThreadPool.cc
  net/Acceptor.cc
  net/Connector.cc
  net/TcpConnection.cc
  net/TcpServer.cc
  net/TcpClient.cc
  net/PeriodicTimer.cc
  net/TimerQueue.cc
  net/SignalWatcher.cc
)
target_include_directories(mrpc_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mrpc_net PUBLIC Threads::Threads)

# ---- 协议层（固定头分帧 + protobuf 序列化 + 反射分发）----
add_library(mrpc_proto STATIC
  ${PROTO_SRCS}
  protocol/RpcCodec.cc
  protocol/HeartbeatMonitor.cc
  protocol/RpcController.cc
  protocol/ServiceRegistry.cc
  protocol/RpcServer.cc
  protocol/RpcChannel.cc
)
target_include_directories(mrpc_proto PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}   # 生成的 rpc.pb.h 所在目录
)
target_link_libraries(mrpc_proto PUBLIC mrpc_net protobuf::libprotobuf)

# ---- 示例与工具 ----
add_executable(echo_server main/echo_server.cc)
target_link_libraries(echo_server PRIVATE mrpc_net)

add_executable(echo_client main/echo_client.cc)
target_link_libraries(echo_client PRIVATE mrpc_net)

add_executable(bench_client main/bench_client.cc)
target_link_libraries(bench_client PRIVATE mrpc_net)

add_executable(slow_reader main/slow_reader.cc)
target_link_libraries(slow_reader PRIVATE mrpc_net)

add_executable(proto_echo_server main/proto_echo_server.cc)
target_link_libraries(proto_echo_server PRIVATE mrpc_proto)

add_executable(proto_echo_client main/proto_echo_client.cc)
target_link_libraries(proto_echo_client PRIVATE mrpc_proto)

add_executable(idle_client main/idle_client.cc)
target_link_libraries(idle_client PRIVATE mrpc_proto)

# ---- RPC 服务端 / 客户端（反射分发示例）----
add_executable(rpc_server main/rpc_server.cc)
target_link_libraries(rpc_server PRIVATE mrpc_proto)

add_executable(rpc_client main/rpc_client.cc)
target_link_libraries(rpc_client PRIVATE mrpc_proto)

add_executable(rpc_stub_client main/rpc_stub_client.cc)
target_link_libraries(rpc_stub_client PRIVATE mrpc_proto)

# ---- 测试 ----
add_executable(codec_test test/codec_test.cc)
target_link_libraries(codec_test PRIVATE mrpc_proto)
add_test(NAME codec_test COMMAND codec_test)

add_executable(timer_queue_test test/timer_queue_test.cc)
target_link_libraries(timer_queue_test PRIVATE mrpc_net)
add_test(NAME timer_queue_test COMMAND timer_queue_test)

add_executable(tcp_client_test test/tcp_client_test.cc)
target_link_libraries(tcp_client_test PRIVATE mrpc_net)
add_test(NAME tcp_client_test COMMAND tcp_client_test)
