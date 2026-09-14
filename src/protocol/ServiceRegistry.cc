#include "protocol/ServiceRegistry.h"

#include "common/Logger.h"

namespace mrpc {

bool ServiceRegistry::addService(google::protobuf::Service* service) {
  if (service == nullptr) {
    LOG_ERROR << "ServiceRegistry::addService: null service";
    return false;
  }

  const google::protobuf::ServiceDescriptor* sd = service->GetDescriptor();
  const std::string serviceName = sd->full_name();

  if (services_.find(serviceName) != services_.end()) {
    LOG_ERROR << "ServiceRegistry::addService: duplicate service "
              << serviceName;
    return false;
  }
  services_.emplace(serviceName, service);

  // 注册时一次性展开方法表，运行期查找 O(1)，避免每次请求遍历 descriptor
  for (int i = 0; i < sd->method_count(); ++i) {
    const google::protobuf::MethodDescriptor* md = sd->method(i);
    methods_.emplace(serviceName + "." + md->name(),
                     MethodEntry{service, md});
  }

  LOG_INFO << "ServiceRegistry - registered " << serviceName << " with "
           << sd->method_count() << " method(s)";
  return true;
}

ServiceRegistry::LookupResult ServiceRegistry::findMethod(
    const std::string& serviceName, const std::string& methodName,
    google::protobuf::Service** service,
    const google::protobuf::MethodDescriptor** method) const {
  const auto sit = services_.find(serviceName);
  if (sit == services_.end()) {
    return LookupResult::kUnknownService;
  }

  const auto mit = methods_.find(serviceName + "." + methodName);
  if (mit == methods_.end()) {
    return LookupResult::kUnknownMethod;
  }

  *service = mit->second.service;
  *method = mit->second.method;
  return LookupResult::kFound;
}

}  // namespace mrpc
