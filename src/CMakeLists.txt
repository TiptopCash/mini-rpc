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

find_package(Threads REQUIRED)
target_link_libraries(mrpc_net PUBLIC Threads::Threads)

add_executable(echo_server main/echo_server.cc)
target_link_libraries(echo_server PRIVATE mrpc_net)

add_executable(echo_client main/echo_client.cc)
target_link_libraries(echo_client PRIVATE mrpc_net)

add_executable(bench_client main/bench_client.cc)
target_link_libraries(bench_client PRIVATE mrpc_net)

add_executable(slow_reader main/slow_reader.cc)
target_link_libraries(slow_reader PRIVATE mrpc_net)
