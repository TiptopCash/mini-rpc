#pragma once

#include <cstdint>

#include "protocol/RpcHeader.h"
#include "rpc.pb.h"

namespace mrpc {

class Buffer;

// 固定头 + protobuf body 的编解码器。
// 无状态，全部静态方法，天然线程安全。
class RpcCodec {
 public:
  enum ParseResult {
    kOk,          // 解析出一条完整消息，已从缓冲消费对应字节
    kIncomplete,  // 数据不足，等待更多字节（半包）
    kError,       // 协议错误（魔数/版本/长度非法/body 损坏），应关闭连接
  };

  // 将 msg 按 type 编码（固定头 + protobuf body）追加到 out
  static bool encode(const RpcMessage& msg, uint8_t type, Buffer* out);

  // 从 in 中尝试解析一条完整消息；kOk 时写出 type 与 msg
  static ParseResult parse(Buffer* in, uint8_t* type, RpcMessage* msg);
};

}  // namespace mrpc
