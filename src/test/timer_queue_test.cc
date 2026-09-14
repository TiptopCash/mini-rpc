// TimerQueue 自测：一次/重复定时器、取消、跨线程增删、重复触发不漂移。
//
// 约定：被定时器捕获的状态一律放在堆上（shared_ptr），不捕获函数栈变量。
// 定时器可能在本函数返回之后仍然存活（取消没生效时就是如此），捕获栈引用
// 会让「逻辑没停住」退化成 use-after-return，反而掩盖真正要验证的行为。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Timestamp.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"
#include "net/TimerQueue.h"

using namespace mrpc;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failed;
}

// 轮询等待 loop 线程把计数推上去；超时返回当前值，由调用方断言
int waitForCount(const std::atomic<int>& counter, int expect, int timeoutMs) {
  const int64_t deadline = nowMicros() + static_cast<int64_t>(timeoutMs) * 1000;
  while (nowMicros() < deadline && counter.load() < expect) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return counter.load();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool approx(int64_t measuredUs, int64_t expectedUs, int64_t toleranceUs) {
  const int64_t diff =
      measuredUs > expectedUs ? measuredUs - expectedUs : expectedUs - measuredUs;
  return diff <= toleranceUs;
}

// 1. 触发顺序按到期时间，而不是加入顺序
void testOrder(EventLoop* loop) {
  printf("[1] 一次性定时器按到期时间触发\n");
  struct State {
    std::mutex m;
    std::vector<int> order;
    std::atomic<int> fired{0};
  };
  auto st = std::make_shared<State>();

  for (int delay : {30, 10, 20}) {
    loop->runAfter(delay, [st, delay] {
      {
        std::lock_guard<std::mutex> lk(st->m);
        st->order.push_back(delay);
      }
      st->fired.fetch_add(1);
    });
  }

  check(waitForCount(st->fired, 3, 1000) == 3, "三个定时器都触发了");
  std::lock_guard<std::mutex> lk(st->m);
  check(st->order == std::vector<int>({10, 20, 30}),
        "触发顺序为 10 -> 20 -> 30（乱序加入也按到期时间排序）");
}

// 2. 取消未到期的定时器：不得触发
void testCancel(EventLoop* loop) {
  printf("[2] 取消未到期的定时器\n");
  struct State {
    std::atomic<int> fired{0};
  };
  auto st = std::make_shared<State>();

  const TimerQueue::TimerId id =
      loop->runAfter(20, [st] { st->fired.fetch_add(1); });
  loop->cancelTimer(id);  // 主线程调用，内部投递到 loop 线程执行

  sleepMs(120);
  check(st->fired.load() == 0, "被取消的定时器没有触发");
}

// 3. 取消未知 / 已触发的 id：静默忽略，不崩溃、不误伤其他定时器
void testCancelUnknown(EventLoop* loop) {
  printf("[3] 取消未知 id 与重复取消\n");
  struct State {
    std::atomic<int> fired{0};
  };
  auto st = std::make_shared<State>();

  loop->cancelTimer(999999);  // 从未存在过的 id
  loop->runAfter(10, [st] { st->fired.fetch_add(1); });

  check(waitForCount(st->fired, 1, 1000) == 1, "无关定时器不受影响");
  loop->cancelTimer(999999);  // 再次取消同一个不存在的 id
  check(true, "重复取消未知 id 未崩溃");
}

// 4. 重复定时器：按时重复，回调内自取消后停止
void testRepeating(EventLoop* loop) {
  printf("[4] 重复定时器与回调内自取消\n");
  struct State {
    EventLoop* loop = nullptr;
    std::atomic<int> fired{0};
    std::atomic<int> stopped{0};  // 取消完成（回调最后一步）后置 1
    std::atomic<TimerQueue::TimerId> id{0};
  };
  auto st = std::make_shared<State>();
  st->loop = loop;

  st->id.store(loop->runEvery(10, [st] {
    const int n = st->fired.fetch_add(1) + 1;
    if (n == 5) {
      // 定时器回调里增删定时器必须安全：这是在 loop 线程内直接操作堆
      st->loop->cancelTimer(st->id.load());
      st->stopped.store(1);
    }
  }));

  check(waitForCount(st->fired, 5, 1000) == 5, "重复定时器触发了 5 次");
  sleepMs(80);  // 取消之后应再无触发
  check(st->fired.load() == 5 && st->stopped.load() == 1,
        "回调内自取消生效，之后不再触发");
}

// 5. 重复定时器以「原定到期时刻」推进，不在回调耗时上累积漂移
void testNoDrift(EventLoop* loop) {
  printf("[5] 重复定时器不累积漂移\n");
  const int kTimes = 10;
  const int kIntervalMs = 20;
  const int kCallbackWorkMs = 15;  // 必须小于间隔，否则重排逻辑会主动跳到「现在」

  struct State {
    EventLoop* loop = nullptr;
    std::atomic<int> fired{0};
    std::atomic<int> done{0};  // 取消完成（回调最后一步）后置 1
    std::atomic<int64_t> startUs{0};
    std::atomic<int64_t> elapsedUs{0};
    std::atomic<TimerQueue::TimerId> id{0};
  };
  auto st = std::make_shared<State>();
  st->loop = loop;

  // 在主线程调用、由 loop 线程执行，取到接近「注册时刻」的起点
  loop->runInLoop([st] { st->startUs.store(nowMicros()); });

  st->id.store(loop->runEvery(kIntervalMs, [st] {
    sleepMs(kCallbackWorkMs);
    if (st->fired.fetch_add(1) + 1 == kTimes) {
      st->elapsedUs.store(nowMicros() - st->startUs.load());
      st->loop->cancelTimer(st->id.load());
      st->done.store(1);
    }
  }));

  check(waitForCount(st->done, 1, 2000) == 1, "重复定时器触发满 10 次");
  const double measuredMs = static_cast<double>(st->elapsedUs.load()) / 1000.0;
  const int expectedMs = kTimes * kIntervalMs + kCallbackWorkMs;
  printf("     实测总耗时 %.0f ms（期望约 %d ms）\n", measuredMs, expectedMs);
  // 若每次都从「回调结束」重新计时，总耗时会退化成 kTimes*(间隔+回调耗时)=350ms
  check(approx(st->elapsedUs.load(), static_cast<int64_t>(expectedMs) * 1000, 60000),
        "总耗时接近「次数 x 间隔」，未在回调耗时上累积");
}

// 6. 跨线程增删：调用线程与 loop 线程不同时也必须生效
void testCrossThread(EventLoop* loop) {
  printf("[6] 跨线程 addTimer / cancelTimer\n");
  struct State {
    std::atomic<int> fired{0};
    std::atomic<int> canceledFired{0};
    std::atomic<int64_t> observedUs{0};
  };
  auto st = std::make_shared<State>();

  const int64_t addStartUs = nowMicros();
  loop->runAfter(50, [st] { st->observedUs.store(nowMicros()); });

  const TimerQueue::TimerId victim =
      loop->runAfter(30, [st] { st->canceledFired.fetch_add(1); });
  loop->cancelTimer(victim);  // 与 addTimer 同为主线程调用

  loop->runAfter(60, [st] { st->fired.fetch_add(1); });

  check(waitForCount(st->fired, 1, 1000) == 1, "跨线程注册的定时器触发了");
  check(st->canceledFired.load() == 0, "跨线程取消的定时器没有触发");
  // 到期时刻按「调用时刻 + delay」算，投递到 loop 线程的排队时间不应计入延迟
  check(approx(st->observedUs.load() - addStartUs, 50000, 30000),
        "跨线程定时器延迟准确（投递排队时间未计入）");
}

}  // namespace

int main() {
  // loop 跑在独立线程：主线程扮演「外部调用者」，才能顺带验证跨线程路径
  EventLoopThread loopThread;
  EventLoop* loop = loopThread.startLoop();

  testOrder(loop);
  testCancel(loop);
  testCancelUnknown(loop);
  testRepeating(loop);
  testNoDrift(loop);
  testCrossThread(loop);

  printf("\n%s（失败 %d 项）\n", g_failed == 0 ? "全部通过" : "存在失败", g_failed);
  return g_failed == 0 ? 0 : 1;
}
