#include "protocol/RpcCodec.h"

#include <string>

#include "common/Logger.h"
#include "net/Buffer.h"

namespace mrpc {

bool RpcCodec::encode(const RpcMessage& msg, uint8_t type, Buffer* out) {
  std::string body;
  if (!msg.SerializeToString(&body)) {
    LOG_ERROR << "RpcCodec::encode: SerializeToString failed";
    return false;
  }
  if (body.size() > kRpcMaxBodyLen) {
    LOG_ERROR << "RpcCodec::encode: body too large (" << body.size() << ")";
    return false;
  }

  char header[kRpcHeaderLen];
  writeBE16(header, kRpcMagic);
  header[2] = static_cast<char>(kRpcVersion);
  header[3] = static_cast<char>(type);
  writeBE32(header + 4, static_cast<uint32_t>(body.size()));

  out->append(header, kRpcHeaderLen);
  out->append(body);
  return true;
}

RpcCodec::ParseResult RpcCodec::parse(Buffer* in, uint8_t* type,
                                      RpcMessage* msg) {
  // 1. 头都不完整
  if (in->readableBytes() < kRpcHeaderLen) {
    return kIncomplete;
  }

  const char* p = in->peek();

  // 2. 校验固定头
  if (readBE16(p) != kRpcMagic) {
    LOG_ERROR << "RpcCodec::parse: bad magic";
    return kError;
  }
  const uint8_t version = static_cast<uint8_t>(p[2]);
  if (version != kRpcVersion) {
    LOG_ERROR << "RpcCodec::parse: unsupported version " << int(version);
    return kError;
  }
  const uint8_t frameType = static_cast<uint8_t>(p[3]);
  const uint32_t bodyLen = readBE32(p + 4);
  if (bodyLen > kRpcMaxBodyLen) {
    LOG_ERROR << "RpcCodec::parse: body length " << bodyLen << " exceeds limit";
    return kError;
  }

  // 3. body 还没收全（半包）
  if (in->readableBytes() < kRpcHeaderLen + bodyLen) {
    return kIncomplete;
  }

  // 4. 反序列化；ParseFromArray 会先清空 msg，可安全复用同一对象
  if (!msg->ParseFromArray(p + kRpcHeaderLen, static_cast<int>(bodyLen))) {
    LOG_ERROR << "RpcCodec::parse: body deserialization failed";
    return kError;
  }

  *type = frameType;
  in->retrieve(kRpcHeaderLen + bodyLen);
  return kOk;
}

}  // namespace mrpc
