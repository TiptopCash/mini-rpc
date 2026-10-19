cmake_minimum_required(VERSION 3.16)
project(mini_rpc_qt_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${REPO_ROOT}/proto/rpc.proto")

# 客户端复用仓库里的线协议实现，服务端代码一行不改：
#   RpcCodec  固定头 + protobuf 编解码
#   Buffer    半包/粘包缓冲（这里只用作内存缓冲，不调用 readFd）
add_executable(mini_rpc_qt_client
  main.cpp
  MainWindow.cpp
  MainWindow.h
  RpcClient.cpp
  RpcClient.h
  "${REPO_ROOT}/src/common/Logger.cc"
  "${REPO_ROOT}/src/net/Buffer.cc"
  "${REPO_ROOT}/src/protocol/RpcCodec.cc"
  ${PROTO_SRCS}
)

target_include_directories(mini_rpc_qt_client PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${REPO_ROOT}/src"
  "${CMAKE_CURRENT_BINARY_DIR}"   # 生成的 rpc.pb.h
)

target_link_libraries(mini_rpc_qt_client PRIVATE
  Qt6::Widgets
  Qt6::Network
  protobuf::libprotobuf
  Threads::Threads
)
