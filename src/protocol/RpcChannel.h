#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/Noncopyable.h"
#include "google/protobuf/service.h"
#include "net/InetAddress.h"
#include "net/TcpClient.h"
#include "net/TimerQueue.h"
#include "protocol/RpcError.h"

namespace mrpc {

class Buffer;
class EventLoop;
class RpcMessage;

// google::protobuf::RpcChannel 的实现：把 Service_Stub 生成的调用
// （方法描述符 + 请求 + 响应 + done）翻译成线上的 RpcMessage 帧。
//
// 有了它，业务侧只写：
//     EchoService::Stub stub(&channel);
//     stub.Echo(&controller, &request, &response, nullptr);   // 同步
//     stub.Echo(&controller, &request, &response, done);      // 异步
// 不再手写分帧、seq 匹配、心跳应答。
//
// 两种调用形态：
//   同步（done == nullptr）：调用线程阻塞在条件变量上，直到响应到达或超时
//   异步（done != nullptr）：立即返回，响应到达后在 IO 线程执行 done->Run()
//
// 生命周期：必须在所属 loop 仍运行期间析构（析构里要在 loop 线程清理在途请求
// 与连接）。同步调用不能在 IO 线程里发起——那会在等自己处理响应，直接死锁。
class RpcChannel : public google::protobuf::RpcChannel, public Noncopyable {
 public:
  RpcChannel(EventLoop* loop, const InetAddress& serverAddr,
             const std::string& name);
  ~RpcChannel() override;

  // 发起连接；失败按退避重试（连接不可用时调用会快速失败而不是干等）
  void start();
  // 阻塞等待连接就绪，同步调用前通常先等一次
  bool waitConnected(int64_t timeoutMs);
  // 单次调用超时（毫秒，<= 0 表示不限时），默认 5000
  void setTimeoutMs(int64_t timeoutMs) { timeoutMs_ = timeoutMs; }

  bool connected() const { return client_ && client_->connected(); }

  void CallMethod(const google::protobuf::MethodDescriptor* method,
                  google::protobuf::RpcController* controller,
                  const google::protobuf::Message* request,
                  google::protobuf::Message* response,
                  google::protobuf::Closure* done) override;

 private:
  // 一次在途请求。同步调用靠 ready/cv 等结果，异步调用靠 done；
  // 两条路径共用 errorCode/payload 的填写逻辑
  struct PendingCall {
    uint64_t seq = 0;
    google::protobuf::Message* response = nullptr;
    google::protobuf::RpcController* controller = nullptr;
    google::protobuf::Closure* done = nullptr;
    TimerQueue::TimerId timeoutId = 0;

    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
  };

  void onMessage(const TcpConnectionPtr& conn, Buffer* buf);
  void handleResponse(const RpcMessage& msg);
  void finishCall(const std::shared_ptr<PendingCall>& call, int32_t errorCode,
                  const std::string& errorMsg, const std::string& payload);
  void failAllPending(const std::string& reason);
  void sendRequest(const RpcMessage& msg);

  EventLoop* loop_;
  const std::string name_;
  const InetAddress serverAddr_;
  std::unique_ptr<TcpClient> client_;
  std::atomic<uint64_t> nextSeq_;
  int64_t timeoutMs_;

  // pending_ 会被 IO 线程（响应、超时定时器）与调用线程（登记、超时摘除）
  // 同时访问，用一把锁保护；不给每个请求单独加锁是因为临界区极短
  std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<PendingCall>> pending_;
  std::condition_variable connCv_;
  bool connUp_;
};

}  // namespace mrpc
