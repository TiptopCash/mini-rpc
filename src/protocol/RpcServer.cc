#include "protocol/RpcServer.h"

#include <atomic>
#include <memory>
#include <utility>

#include "common/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TimerQueue.h"
#include "protocol/HeartbeatMonitor.h"
#include "protocol/RpcCodec.h"
#include "protocol/RpcController.h"
#include "rpc.pb.h"

namespace mrpc {

namespace {

// 一次在途请求的仲裁点，由「业务回调」与「超时定时器」两条路径共享持有。
//
// 收尾（回包 + 释放资源）必须恰好执行一次，而两条路径可能在不同线程同时到达：
// 异步服务实现在自己的线程里调 done->Run()，超时定时器在连接所属的 IO 线程触发。
// 用一次 CAS 决出胜负，败者立即返回。
//
// 为什么标志不放在 MethodDone 里：胜负决出后胜者会 delete MethodDone，
// 败者若再去读它的成员就是 use-after-free。放在独立的堆对象里，
// 两条路径在 CAS 之前都只访问这个共享对象。
struct RequestGate {
  std::atomic<bool> finished{false};
  EventLoop* loop = nullptr;
  TimerQueue::TimerId timeoutId = 0;
};

// 反射调用的收尾回调，负责回包与释放请求/响应/控制器。
//
// 同步服务实现在 CallMethod 内部就调用 Run()；异步实现可以在别的线程稍后调用，
// 因此这里只通过 conn_->send() 回包——它内部会判断线程并投递回 IO 线程。
//
// 约定：服务实现必须保证 done->Run() 至多调用一次。超时兜底只保证
// 「框架不会因为漏调而泄漏」，不负责容忍重复调用。
class MethodDone : public google::protobuf::Closure {
 public:
  MethodDone(const std::shared_ptr<RequestGate>& gate,
             const TcpConnectionPtr& conn, uint64_t seq, std::string method,
             google::protobuf::Message* request,
             google::protobuf::Message* response, RpcController* controller)
      : gate_(gate),
        conn_(conn),
        seq_(seq),
        method_(std::move(method)),
        request_(request),
        response_(response),
        controller_(controller) {}

  void Run() override {
    if (gate_->finished.exchange(true)) {
      // 超时定时器先到：它已经回了 kRpcTimeout 并释放了资源，这里必须什么都不做
      LOG_WARN << "MethodDone - seq=" << seq_ << " completed after timeout ("
               << method_ << "), response discarded";
      return;
    }
    cancelTimeout();
    finish(/*timeout=*/false);
  }

  // 只由超时定时器在赢得 gate_->finished 仲裁后调用
  void onTimeout() {
    LOG_WARN << "MethodDone - seq=" << seq_ << " timed out in " << method_
             << ", service impl likely forgot done->Run()";
    finish(/*timeout=*/true);
  }

 private:
  void cancelTimeout() {
    if (gate_->timeoutId != 0) {
      gate_->loop->cancelTimer(gate_->timeoutId);
    }
  }

  void finish(bool timeout) {
    RpcMessage out;
    out.set_seq(seq_);

    if (timeout) {
      out.set_error_code(kRpcTimeout);
      out.set_error_msg("request timed out: " + method_);
    } else if (controller_->Failed()) {
      out.set_error_code(kRpcServiceFailed);
      out.set_error_msg(controller_->ErrorText());
    } else {
      std::string payload;
      if (response_->SerializeToString(&payload)) {
        out.set_payload(std::move(payload));
      } else {
        out.set_error_code(kRpcResponseEncodeFailed);
        out.set_error_msg("response serialization failed");
      }
    }

    Buffer buf;
    RpcCodec::encode(out, kRpcResponse, &buf);
    conn_->send(buf.retrieveAllAsString());

    delete request_;
    delete response_;
    delete controller_;
    delete this;
  }

  std::shared_ptr<RequestGate> gate_;
  TcpConnectionPtr conn_;
  uint64_t seq_;
  std::string method_;
  google::protobuf::Message* request_;
  google::protobuf::Message* response_;
  RpcController* controller_;
};

}  // namespace

RpcServer::RpcServer(EventLoop* loop, const InetAddress& listenAddr,
                     const std::string& name, int numThreads)
    : loop_(loop), server_(loop, listenAddr, name) {
  server_.setThreadNum(numThreads);
  server_.setConnectionCallback(
      [this](const TcpConnectionPtr& conn) { onConnection(conn); });
  server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf) {
    onMessage(conn, buf);
  });
}

RpcServer::~RpcServer() = default;

void RpcServer::registerService(google::protobuf::Service* service) {
  if (started_) {
    LOG_ERROR << "RpcServer::registerService must be called before start()";
    return;
  }
  registry_.addService(service);
}

void RpcServer::start() {
  started_ = true;
  server_.start();

  if (heartbeatSec_ > 0) {
    HeartbeatOptions opts;
    opts.tickSeconds = 1;
    opts.heartbeatSeconds = heartbeatSec_;
    opts.timeoutSeconds = timeoutSec_;
    heartbeat_.reset(new HeartbeatMonitor(loop_, &server_, opts));
    heartbeat_->start();
  } else {
    LOG_INFO << "RpcServer - heartbeat disabled";
  }
}

void RpcServer::onConnection(const TcpConnectionPtr& conn) {
  LOG_INFO << "RpcServer - " << conn->peerAddress().toIpPort() << " -> "
           << conn->localAddress().toIpPort() << " is "
           << (conn->connected() ? "UP" : "DOWN");
}

void RpcServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
  uint8_t type = 0;
  RpcMessage msg;
  // 一次读到的数据可能含多帧（粘包）或不完整（半包）：循环解析到 kIncomplete，
  // 剩余字节留在连接输入缓冲里等下次读取。
  while (true) {
    const RpcCodec::ParseResult r = RpcCodec::parse(buf, &type, &msg);
    if (r == RpcCodec::kIncomplete) {
      break;
    }
    if (r == RpcCodec::kError) {
      // 字节流已失去同步，无法恢复，只能关连接
      LOG_ERROR << "RpcServer - bad frame from " << conn->name() << ", closing";
      conn->shutdown();
      return;
    }
    dispatch(conn, type, msg);
  }
}

void RpcServer::dispatch(const TcpConnectionPtr& conn, uint8_t type,
                         RpcMessage& msg) {
  if (type == kRpcHeartbeat) {
    // 收到 ping 回 ack；对端收到 ack 不再回复，避免双方无限互回
    RpcMessage ack;
    ack.set_seq(msg.seq());
    Buffer out;
    RpcCodec::encode(ack, kRpcHeartbeatAck, &out);
    conn->send(out.retrieveAllAsString());
    return;
  }
  if (type == kRpcHeartbeatAck) {
    return;  // 网络层已自动更新 lastReceiveTime
  }
  if (type != kRpcRequest) {
    sendError(conn, msg.seq(), kRpcBadRequest, "unexpected frame type");
    return;
  }

  google::protobuf::Service* service = nullptr;
  const google::protobuf::MethodDescriptor* method = nullptr;
  switch (registry_.findMethod(msg.service_name(), msg.method_name(), &service,
                               &method)) {
    case ServiceRegistry::LookupResult::kUnknownService:
      sendError(conn, msg.seq(), kRpcUnknownService,
                "unknown service: " + msg.service_name());
      return;
    case ServiceRegistry::LookupResult::kUnknownMethod:
      sendError(conn, msg.seq(), kRpcUnknownMethod,
                "unknown method: " + msg.service_name() + "." +
                    msg.method_name());
      return;
    case ServiceRegistry::LookupResult::kFound:
      break;
  }

  // 反射的核心：框架不知道也不关心具体消息类型，
  // 用 descriptor 里的原型 New() 出正确类型的空对象，再反序列化填充。
  google::protobuf::Message* request =
      service->GetRequestPrototype(method).New();
  if (!request->ParseFromString(msg.payload())) {
    delete request;
    sendError(conn, msg.seq(), kRpcBadRequest,
              "request deserialization failed");
    return;
  }

  google::protobuf::Message* response =
      service->GetResponsePrototype(method).New();
  RpcController* controller = new RpcController();

  auto gate = std::make_shared<RequestGate>();
  // 定时器挂在连接所属的 IO 线程上：和请求的处理、回包都在同一个循环里，
  // 无需跨线程；挂到主 loop 反而会把每个连接的超时都集中到一个线程
  gate->loop = conn->getLoop();
  MethodDone* done = new MethodDone(
      gate, conn, msg.seq(),
      msg.service_name() + "." + msg.method_name(), request, response,
      controller);

  if (requestTimeoutMs_ > 0) {
    // 定时器强引用 gate（很轻），不引用 MethodDone：MethodDone 由
    // 「赢得仲裁的那条路径」delete，定时器只需在赢下 CAS 后再去碰它
    gate->timeoutId =
        gate->loop->runAfter(requestTimeoutMs_, [gate, done] {
          if (gate->finished.exchange(true)) {
            return;  // 业务已完成，这个定时器只是还没来得及被取消
          }
          done->onTimeout();
        });
  }

  // 生成的 CallMethod 是模板方法：内部按 method 索引调用用户实现的 Echo(...)。
  // 同步实现会在返回前就 done->Run()；异步实现会在别处稍后 Run()。
  service->CallMethod(method, controller, request, response, done);
}

void RpcServer::sendError(const TcpConnectionPtr& conn, uint64_t seq,
                          int32_t code, const std::string& text) {
  LOG_WARN << "RpcServer - seq=" << seq << " error " << code << ": " << text;
  RpcMessage out;
  out.set_seq(seq);
  out.set_error_code(code);
  out.set_error_msg(text);
  Buffer buf;
  RpcCodec::encode(out, kRpcResponse, &buf);
  // 业务错误只回错误码，不断开连接（字节流仍同步）
  conn->send(buf.retrieveAllAsString());
}

}  // namespace mrpc
