#pragma once

#include <cstdint>

namespace mrpc {

// 应用层错误码，承载在 RpcMessage.error_code 中。
//
// 与固定的 8 字节头无关：头部只负责分帧，业务错误全部走 protobuf body。
// 约定 0 为成功；正数区间为框架保留；负数区间留给业务自定义错误。
enum RpcErrorCode : int32_t {
  kRpcOk = 0,
  kRpcUnknownService = 1,        // service_name 未注册
  kRpcUnknownMethod = 2,         // 服务存在但方法不存在
  kRpcBadRequest = 3,            // payload 反序列化失败 / 帧类型非法
  kRpcServiceFailed = 4,         // 服务实现调用 RpcController::SetFailed
  kRpcResponseEncodeFailed = 5,  // 响应序列化失败
  kRpcTimeout = 6,               // 服务实现超时未回调 done->Run()，由框架兜底
};

}  // namespace mrpc
