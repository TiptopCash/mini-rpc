#pragma once

#include <cstdint>

namespace mrpc {

// 应用层错误码。0-5 由服务端产生并经网络返回；6 两端都可能产生
// （服务端的请求超时兜底，或客户端的调用超时）；7 只在客户端本地出现，
// 不会出现在网络上。
//
// 与固定的 8 字节头无关：头部只负责分帧，错误码统一走 protobuf body。
// 约定 0 为成功；正数区间为框架保留；负数区间留给业务自定义错误。
enum RpcErrorCode : int32_t {
  kRpcOk = 0,
  kRpcUnknownService = 1,        // service_name 未注册
  kRpcUnknownMethod = 2,         // 服务存在但方法不存在
  kRpcBadRequest = 3,            // payload 反序列化失败 / 帧类型非法
  kRpcServiceFailed = 4,         // 服务实现调用 RpcController::SetFailed
  kRpcResponseEncodeFailed = 5,  // 响应序列化失败
  kRpcTimeout = 6,               // 超时：服务端漏调 done->Run()，或客户端等不到响应
  kRpcConnectionLost = 7,        // 仅客户端：连接不可用/中断，在途请求直接失败
};

}  // namespace mrpc
