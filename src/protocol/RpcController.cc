#include "protocol/RpcController.h"

namespace mrpc {

void RpcController::Reset() {
  failed_ = false;
  canceled_ = false;
  errorText_.clear();
  cancelCallback_ = nullptr;
}

void RpcController::SetFailed(const std::string& reason) {
  failed_ = true;
  errorText_ = reason;
}

void RpcController::StartCancel() {
  canceled_ = true;
  if (cancelCallback_ != nullptr) {
    google::protobuf::Closure* cb = cancelCallback_;
    cancelCallback_ = nullptr;
    cb->Run();
  }
}

void RpcController::NotifyOnCancel(google::protobuf::Closure* callback) {
  if (callback == nullptr) {
    return;
  }
  if (canceled_) {
    callback->Run();
  } else {
    cancelCallback_ = callback;
  }
}

}  // namespace mrpc
