#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "common/Noncopyable.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/service.h"

namespace mrpc {

// 服务注册表：把「服务名 + 方法名」映射到可反射调用的 Service/MethodDescriptor。
//
// 不持有 Service 所有权：注册进来的对象必须比注册表（进而比 RpcServer）活得久，
// 通常是 main 里的局部对象或 static 对象。这与 protobuf/muduo 的惯例一致。
//
// 线程安全：只在 start() 之前写入，运行期只读。因此多个 IO 线程并发查找是安全的；
// 但严禁在 start() 之后调用 addService()。
class ServiceRegistry : public Noncopyable {
 public:
  enum class LookupResult {
    kFound,
    kUnknownService,
    kUnknownMethod,
  };

  // 注册服务，同时把它声明的方法展开进方法表。
  // 返回 false 表示服务名重复（同名服务的第二个注册被拒绝）。
  bool addService(google::protobuf::Service* service);

  // 查找方法。命中时写出 service 与 method；未命中区分「服务不存在」与「方法不存在」，
  // 便于回更精确的错误码。
  LookupResult findMethod(
      const std::string& serviceName, const std::string& methodName,
      google::protobuf::Service** service,
      const google::protobuf::MethodDescriptor** method) const;

  size_t serviceCount() const { return services_.size(); }
  size_t methodCount() const { return methods_.size(); }

 private:
  struct MethodEntry {
    google::protobuf::Service* service;
    const google::protobuf::MethodDescriptor* method;
  };

  using ServiceMap = std::unordered_map<std::string, google::protobuf::Service*>;
  using MethodMap = std::unordered_map<std::string, MethodEntry>;

  ServiceMap services_;  // full_name -> Service*
  MethodMap methods_;    // "full_name.method_name" -> MethodEntry
};

}  // namespace mrpc
