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
  net/TcpConnection.cc
  net/TcpServer.cc
)
target_include_directories(mrpc_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mrpc_net PUBLIC Threads::Threads)

# ---- 协议层（固定头分帧 + protobuf 序列化）----
add_library(mrpc_proto STATIC
  ${PROTO_SRCS}
  protocol/RpcCodec.cc
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

# ---- 测试 ----
add_executable(codec_test test/codec_test.cc)
target_link_libraries(codec_test PRIVATE mrpc_proto)
add_test(NAME codec_test COMMAND codec_test)
