#include "protocol/RpcChannel.h"

#include <chrono>
#include <utility>
#include <vector>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/TcpConnection.h"
#include "protocol/RpcCodec.h"
#include "rpc.pb.h"

namespace mrpc {

RpcChannel::RpcChannel(EventLoop* loop, const InetAddress& serverAddr,
                       const std::string& name)
    : loop_(loop),
      name_(name),
      serverAddr_(serverAddr),
      client_(nullptr),
      nextSeq_(1),
      timeoutMs_(5000),
      connUp_(false) {}

RpcChannel::~RpcChannel() {
  // 在 loop 线程里一次性做完清理：那里不可能有网络/定时器回调正在执行，
  // 因此取消掉的定时器绝不会再回来碰本对象（从别的线程取消就存在这个窗口）
  loop_->runInLoopAndWait([this] {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_.empty()) {
        LOG_WARN << "RpcChannel[" << name_ << "] - destroyed with "
                 << pending_.size() << " in-flight call(s), they will never "
                 << "complete";
      }
      for (auto& item : pending_) {
        if (item.second->timeoutId != 0) {
          loop_->cancelTimer(item.second->timeoutId);
        }
      }
      pending_.clear();
    }
    // TcpClient/Connector 同样要求在本 loop 线程析构
    client_.reset();
  });
}

void RpcChannel::start() {
  client_.reset(new TcpClient(loop_, serverAddr_, name_));
  // 客户端不重试就基本不可用：对端重启一次就永远连不上
  client_->enableRetry(200, 5000);

  client_->setConnectionCallback([this](const TcpConnectionPtr& conn) {
    const bool up = conn->connected();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connUp_ = up;
    }
    connCv_.notify_all();

    if (up) {
      LOG_INFO << "RpcChannel[" << name_ << "] - connected to "
               << serverAddr_.toIpPort();
      return;
    }
    LOG_WARN << "RpcChannel[" << name_ << "] - connection lost";
    // 不做这一步的话，每个在途请求都要各自等满超时才失败
    failAllPending("connection lost");
  });

  client_->setMessageCallback(
      [this](const TcpConnectionPtr& conn, Buffer* buf) { onMessage(conn, buf); });

  client_->connect();
}

bool RpcChannel::waitConnected(int64_t timeoutMs) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (connUp_) return true;
  return connCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return connUp_; });
}

void RpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                            google::protobuf::RpcController* controller,
                            const google::protobuf::Message* request,
                            google::protobuf::Message* response,
                            google::protobuf::Closure* done) {
  if (!client_ || !client_->connected()) {
    // 快速失败，不自动等待连接：调用方若想等，用 waitConnected()
    if (controller) {
      controller->SetFailed("no available connection to " +
                            serverAddr_.toIpPort());
    }
    if (done) done->Run();
    return;
  }

  RpcMessage msg;
  const uint64_t seq = nextSeq_.fetch_add(1);
  msg.set_seq(seq);
  msg.set_service_name(method->service()->full_name());
  msg.set_method_name(method->name());

  std::string payload;
  if (!request->SerializeToString(&payload)) {
    if (controller) controller->SetFailed("request serialization failed");
    if (done) done->Run();
    return;
  }
  msg.set_payload(std::move(payload));

  auto call = std::make_shared<PendingCall>();
  call->seq = seq;
  call->response = response;
  call->controller = controller;
  call->done = done;

  if (done == nullptr) {
    // 同步：必须先登记再发送。反过来的话，响应可能比登记先到，
    // 会被当成「未知 seq」丢弃，调用方一直等到超时
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_[seq] = call;
    }
    sendRequest(msg);

    std::unique_lock<std::mutex> lock(call->m);
    const bool ok = call->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs_),
                                      [&call] { return call->ready; });
    if (ok) return;  // 结果已由 IO 线程写进 response/controller

    lock.unlock();
    {
      std::lock_guard<std::mutex> guard(mutex_);
      pending_.erase(seq);
    }
    if (controller) {
      controller->SetFailed("rpc call timed out after " +
                            std::to_string(timeoutMs_) + " ms");
    }
    LOG_WARN << "RpcChannel[" << name_ << "] - seq " << seq
             << " timed out, late response will be dropped";
    return;
  }

  // 异步：先发布登记再挂超时定时器。
  // 反过来的话，定时器可能在登记可见之前就触发，那次调用就永远没人收尾了。
  // 先发布最多让一个「迟到」的定时器空跑一趟（找不到登记直接返回），无害
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_[seq] = call;
  }
  if (timeoutMs_ > 0) {
    const TimerQueue::TimerId id = loop_->runAfter(timeoutMs_, [this, seq] {
      std::shared_ptr<PendingCall> timedOut;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(seq);
        if (it == pending_.end()) return;  // 响应先到了
        timedOut = it->second;
        pending_.erase(it);
      }
      LOG_WARN << "RpcChannel[" << name_ << "] - seq " << seq << " timed out";
      finishCall(timedOut, kRpcTimeout, "rpc call timed out", "");
    });
    std::lock_guard<std::mutex> lock(mutex_);
    call->timeoutId = id;
  }
  sendRequest(msg);
}

void RpcChannel::sendRequest(const RpcMessage& msg) {
  const TcpConnectionPtr conn = client_->connection();
  if (!conn) {
    LOG_WARN << "RpcChannel[" << name_ << "] - connection gone before send";
    return;  // 连接刚断：由连接回调统一失败在途请求
  }
  Buffer out;
  RpcCodec::encode(msg, kRpcRequest, &out);
  conn->send(out.retrieveAllAsString());
}

void RpcChannel::onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
  uint8_t type = 0;
  RpcMessage msg;
  while (true) {
    const RpcCodec::ParseResult r = RpcCodec::parse(buf, &type, &msg);
    if (r == RpcCodec::kIncomplete) {
      break;
    }
    if (r == RpcCodec::kError) {
      LOG_ERROR << "RpcChannel[" << name_ << "] - bad frame from server, closing";
      conn->shutdown();
      return;
    }

    if (type == kRpcHeartbeat) {
      // 服务端开启心跳时必须回 ack，否则会被当成死连接踢掉
      RpcMessage ack;
      ack.set_seq(msg.seq());
      Buffer out;
      RpcCodec::encode(ack, kRpcHeartbeatAck, &out);
      conn->send(out.retrieveAllAsString());
      continue;
    }
    if (type == kRpcHeartbeatAck) {
      continue;
    }
    if (type == kRpcResponse) {
      handleResponse(msg);
      continue;
    }
    LOG_WARN << "RpcChannel[" << name_ << "] - unexpected frame type "
             << static_cast<int>(type);
  }
}

void RpcChannel::handleResponse(const RpcMessage& msg) {
  std::shared_ptr<PendingCall> call;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(msg.seq());
    if (it == pending_.end()) {
      // 同步调用超时后迟到的响应走到这里
      LOG_WARN << "RpcChannel[" << name_ << "] - no pending call for seq "
               << msg.seq();
      return;
    }
    call = it->second;
    pending_.erase(it);
  }

  if (call->timeoutId != 0) {
    loop_->cancelTimer(call->timeoutId);
  }
  finishCall(call, msg.error_code(), msg.error_msg(), msg.payload());
}

void RpcChannel::finishCall(const std::shared_ptr<PendingCall>& call,
                            int32_t errorCode, const std::string& errorMsg,
                            const std::string& payload) {
  if (call->controller != nullptr) {
    if (errorCode != kRpcOk) {
      call->controller->SetFailed(errorMsg.empty()
                                      ? "rpc error " + std::to_string(errorCode)
                                      : errorMsg);
    } else if (!call->response->ParseFromString(payload)) {
      call->controller->SetFailed("response deserialization failed");
    }
  }

  if (call->done != nullptr) {
    // 异步：占位调用方不得在 Run() 之外碰 response
    call->done->Run();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(call->m);
    call->ready = true;
  }
  call->cv.notify_one();
}

void RpcChannel::failAllPending(const std::string& reason) {
  std::vector<std::shared_ptr<PendingCall>> calls;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : pending_) {
      calls.push_back(item.second);
    }
    pending_.clear();
  }

  for (auto& call : calls) {
    if (call->timeoutId != 0) {
      loop_->cancelTimer(call->timeoutId);
    }
    finishCall(call, kRpcConnectionLost, reason, "");
  }
}

}  // namespace mrpc
