#pragma once

#include <string>

#include "google/protobuf/service.h"

namespace mrpc {

// 一次 RPC 调用的上下文，protobuf 反射分发要求传入的 RpcController 实现。
//
// 生命周期：由 RpcServer 在分发时 new 出来，交给 MethodDone Closure 持有，
// 回包后随 Closure 一起 delete。因此是「每请求一个」，不需要加锁。
//
// 取消语义：本框架暂不支持服务端主动取消，StartCancel/NotifyOnCancel 只做
// 最小可用实现，避免服务实现误以为调用会被中断。
class RpcController : public google::protobuf::RpcController {
 public:
  RpcController() = default;

  void Reset() override;
  bool Failed() const override { return failed_; }
  std::string ErrorText() const override { return errorText_; }
  void StartCancel() override;
  void SetFailed(const std::string& reason) override;
  bool IsCanceled() const override { return canceled_; }
  void NotifyOnCancel(google::protobuf::Closure* callback) override;

 private:
  bool failed_ = false;
  bool canceled_ = false;
  std::string errorText_;
  // protobuf 约定每请求最多注册一个取消回调，且不转移所有权
  google::protobuf::Closure* cancelCallback_ = nullptr;
};

}  // namespace mrpc
