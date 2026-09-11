#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "common/Noncopyable.h"

namespace mrpc {

class EventLoop;

// 一个 fd 的事件分发单元：持有事件掩码与各类回调
class Channel : public Noncopyable {
 public:
  using EventCallback = std::function<void()>;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  void handleEvent();

  // 让 Channel 在回调期间“绑定”住某个对象（通常是 TcpConnection 的 shared_ptr），
  // 防止回调中对象被销毁导致悬垂指针
  void tie(const std::shared_ptr<void>& obj);

  void setReadCallback(EventCallback cb) { readCallback_ = std::move(cb); }
  void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
  void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
  void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

  int fd() const { return fd_; }
  uint32_t events() const { return events_; }
  void setRevents(uint32_t revents) { revents_ = revents; }

  bool isNoneEvent() const { return events_ == kNoneEvent; }
  bool isWriting() const { return events_ & kWriteEvent; }

  void enableReading();
  void disableReading();
  void enableWriting();
  void disableWriting();
  void disableAll();
  void remove();

  static const uint32_t kNoneEvent;
  static const uint32_t kReadEvent;
  static const uint32_t kWriteEvent;

 private:
  void update();
  void handleEventWithGuard();

  EventLoop* loop_;
  const int fd_;
  uint32_t events_;
  uint32_t revents_ = 0;

  bool tied_ = false;
  std::weak_ptr<void> tie_;

  EventCallback readCallback_;
  EventCallback writeCallback_;
  EventCallback closeCallback_;
  EventCallback errorCallback_;
};

}  // namespace mrpc
