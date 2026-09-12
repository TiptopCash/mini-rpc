#pragma once

#include <cstddef>
#include <cstdint>

namespace mrpc {

// 固定头（8 字节），只承担「分帧」职责：
// 接收方无需解析 body，就能知道这一帧有多长、是不是心跳。
//
//    0               1               2               3
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//    |   magic (2B, 大端)    |  ver  | type  |     bodyLen (4B, 大端)    |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//
// 所有业务字段（含 seq）由 protobuf RpcMessage 承载。
constexpr uint16_t kRpcMagic = 0xCAFE;
constexpr uint8_t kRpcVersion = 1;
constexpr size_t kRpcHeaderLen = 8;
// 单帧 body 上限，用于防御超大/恶意长度字段
constexpr uint32_t kRpcMaxBodyLen = 64u * 1024 * 1024;

enum RpcMessageType : uint8_t {
  kRpcRequest = 1,
  kRpcResponse = 2,
  kRpcHeartbeat = 3,
};

// ---- 网络字节序（大端）读写 ----

inline void writeBE16(char* p, uint16_t v) {
  p[0] = static_cast<char>((v >> 8) & 0xFF);
  p[1] = static_cast<char>(v & 0xFF);
}

inline void writeBE32(char* p, uint32_t v) {
  p[0] = static_cast<char>((v >> 24) & 0xFF);
  p[1] = static_cast<char>((v >> 16) & 0xFF);
  p[2] = static_cast<char>((v >> 8) & 0xFF);
  p[3] = static_cast<char>(v & 0xFF);
}

inline uint16_t readBE16(const char* p) {
  return static_cast<uint16_t>((static_cast<unsigned char>(p[0]) << 8) |
                               static_cast<unsigned char>(p[1]));
}

inline uint32_t readBE32(const char* p) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

}  // namespace mrpc
