// RpcCodec 单元测试：重点覆盖半包、粘包、异常帧
#include <cstdio>
#include <string>

#include "net/Buffer.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

using namespace mrpc;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    ++g_checks;                                                      \
    if (!(cond)) {                                                   \
      ++g_failures;                                                  \
      printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                \
  } while (0)

RpcMessage makeMsg(uint64_t seq, const std::string& payload) {
  RpcMessage m;
  m.set_seq(seq);
  m.set_service_name("EchoService");
  m.set_method_name("Echo");
  m.set_payload(payload);
  return m;
}

// 编码一条消息，返回线缆字节
std::string encodeToWire(const RpcMessage& m, uint8_t type) {
  Buffer b;
  RpcCodec::encode(m, type, &b);
  return b.retrieveAllAsString();
}

// ---- 1. 基本往返 ----
void testRoundTrip() {
  printf("[1] 基本往返\n");
  const RpcMessage sent = makeMsg(42, "hello");
  const std::string wire = encodeToWire(sent, kRpcRequest);

  Buffer in;
  in.append(wire);

  uint8_t type = 0;
  RpcMessage got;
  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(type == kRpcRequest);
  CHECK(got.seq() == 42);
  CHECK(got.service_name() == "EchoService");
  CHECK(got.payload() == "hello");
  CHECK(in.readableBytes() == 0);
  // 线缆长度 = 8 字节头 + protobuf body
  CHECK(wire.size() > kRpcHeaderLen);
}

// ---- 2. 半包：逐字节喂入 ----
void testFragmented() {
  printf("[2] 半包（逐字节）\n");
  const std::string wire = encodeToWire(makeMsg(7, "fragmented"), kRpcRequest);
  const size_t total = wire.size();

  Buffer in;
  uint8_t type = 0;
  RpcMessage got;
  bool ok = true;

  for (size_t i = 0; i < total; ++i) {
    in.append(wire.data() + i, 1);
    const RpcCodec::ParseResult r = RpcCodec::parse(&in, &type, &got);
    if (i + 1 < total) {
      if (r != RpcCodec::kIncomplete) ok = false;  // 未收全前必须等待
    } else {
      if (r != RpcCodec::kOk) ok = false;          // 最后一字节后必须成功
    }
  }

  CHECK(ok);
  CHECK(type == kRpcRequest);
  CHECK(got.seq() == 7);
  CHECK(got.payload() == "fragmented");
  CHECK(in.readableBytes() == 0);
}

// ---- 3. 粘包：一个缓冲里多条消息 ----
void testCoalesced() {
  printf("[3] 粘包（一次多帧）\n");
  Buffer in;
  RpcCodec::encode(makeMsg(1, "a"), kRpcRequest, &in);
  RpcCodec::encode(makeMsg(2, "bb"), kRpcResponse, &in);
  RpcCodec::encode(makeMsg(3, "ccc"), kRpcHeartbeat, &in);

  uint8_t type = 0;
  RpcMessage got;

  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(type == kRpcRequest && got.seq() == 1 && got.payload() == "a");

  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(type == kRpcResponse && got.seq() == 2 && got.payload() == "bb");

  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(type == kRpcHeartbeat && got.seq() == 3 && got.payload() == "ccc");

  // 取完之后再到 kIncomplete
  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kIncomplete);
  CHECK(in.readableBytes() == 0);
}

// ---- 4. 异常帧 ----
void testErrors() {
  printf("[4] 异常帧\n");
  uint8_t type = 0;
  RpcMessage got;

  // 4.1 魔数错误
  {
    Buffer in;
    char h[kRpcHeaderLen] = {0};
    writeBE16(h, 0xDEAD);
    h[2] = static_cast<char>(kRpcVersion);
    h[3] = static_cast<char>(kRpcRequest);
    writeBE32(h + 4, 0);
    in.append(h, kRpcHeaderLen);
    CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kError);
  }

  // 4.2 版本不支持
  {
    Buffer in;
    char h[kRpcHeaderLen] = {0};
    writeBE16(h, kRpcMagic);
    h[2] = 99;
    h[3] = static_cast<char>(kRpcRequest);
    writeBE32(h + 4, 0);
    in.append(h, kRpcHeaderLen);
    CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kError);
  }

  // 4.3 body 长度超限
  {
    Buffer in;
    char h[kRpcHeaderLen] = {0};
    writeBE16(h, kRpcMagic);
    h[2] = static_cast<char>(kRpcVersion);
    h[3] = static_cast<char>(kRpcRequest);
    writeBE32(h + 4, kRpcMaxBodyLen + 1);
    in.append(h, kRpcHeaderLen);
    CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kError);
  }

  // 4.4 头不完整
  {
    Buffer in;
    in.append("abc", 3);
    CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kIncomplete);
  }

  // 4.5 body 长度合法但内容不是合法 protobuf
  {
    Buffer in;
    char h[kRpcHeaderLen] = {0};
    writeBE16(h, kRpcMagic);
    h[2] = static_cast<char>(kRpcVersion);
    h[3] = static_cast<char>(kRpcRequest);
    writeBE32(h + 4, 4);
    in.append(h, kRpcHeaderLen);
    const char junk[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF),
                          static_cast<char>(0xFF), static_cast<char>(0xFF)};
    in.append(junk, 4);
    CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kError);
  }
}

// ---- 5. 空 body ----
void testEmptyBody() {
  printf("[5] 空 body（心跳）\n");
  RpcMessage empty;  // 所有字段取默认值
  const std::string wire = encodeToWire(empty, kRpcHeartbeat);

  Buffer in;
  in.append(wire);
  uint8_t type = 0;
  RpcMessage got;
  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(type == kRpcHeartbeat);
  CHECK(got.seq() == 0);
  CHECK(in.readableBytes() == 0);
}

// ---- 6. 大 body（跨扩容） ----
void testLargeBody() {
  printf("[6] 大 body（1MB）\n");
  const std::string big(1024 * 1024, 'Z');
  const std::string wire = encodeToWire(makeMsg(99, big), kRpcRequest);

  Buffer in;
  in.append(wire);
  uint8_t type = 0;
  RpcMessage got;
  CHECK(RpcCodec::parse(&in, &type, &got) == RpcCodec::kOk);
  CHECK(got.seq() == 99);
  CHECK(got.payload().size() == big.size());
  CHECK(got.payload() == big);
}

}  // namespace

int main() {
  testRoundTrip();
  testFragmented();
  testCoalesced();
  testErrors();
  testEmptyBody();
  testLargeBody();

  printf("\n====================\n");
  printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
  printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败");
  return g_failures == 0 ? 0 : 1;
}
