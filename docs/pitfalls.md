# 手写 C++ RPC 框架：7 个只有实测才能发现的坑

这是一个从零写的 C++17 RPC 框架：`epoll + 主从 Reactor + 自定义二进制协议 + protobuf 反射分发 + 客户端连接池`。单机 echo 实测 31.2 万 QPS、P99 128 µs（4 IO 线程 / 16 并发连接 / 64B 消息 / WSL2 loopback，客户端与服务端同机竞争 CPU，所以这是**下限**而非上限）。

写完能跑，和写完真的对，中间隔着一堆「代码读起来完全正确、只有跑起来才知道错了」的坑。下面这 7 个，每一个都是我在实测里撞出来的，附上现象、原因、修法和实测证据。

---

## 坑 1：`EPOLLOUT` 可写，不代表连接成功

**现象**

非阻塞 `connect()` 返回 `EINPROGRESS`，于是注册 `EPOLLOUT` 等结果——这是教科书写法。但我在 `Connector::handleWrite` 里一开始只做了「可写 = 连上了」，结果对端端口没开时，连接回调照样被调用，随后第一次 `write()` 拿到 `EPIPE`。

**原因**

非阻塞 connect 的完成是「**有结果了**」，不是「成功了」。结果可能是成功，也可能是 `ECONNREFUSED` / `ETIMEDOUT` / `EHOSTUNREACH`。这些错误码不会通过 `EPOLLERR` 直接告诉你——对于 connect 来说，socket 是可写的，错误被挂在 socket 上等你去取。

**修法**

`EPOLLOUT` 之后必须 `getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)`：

```cpp
void Connector::handleWrite() {
  if (state_ != kConnecting) return;   // 已被 handleError 处理过

  const int err = getSocketError(socket_->fd());   // getsockopt(SO_ERROR)
  if (err != 0) {
    LOG_WARN << "connect refused: " << strerror(err);
    handleError();
    return;
  }
  // 到这里才是真的连上了
}
```

顺带补齐三个边角：`EINTR` 要重试，`EISCONN` 说明其实已经连上了（不要当成错误），`EAGAIN` 才是「本地资源暂时不够」。

**还有一个 UAF**：`Channel` 不能在自己的 `handleEvent` 调用栈里被析构。成功和失败两条路径统一做 `disableAll() + remove()`（**把对象留着**），由上层决定何时销毁：

```cpp
channel_->disableAll();
channel_->remove();
state_ = kConnected;
const int fd = socket_->release();
newConnectionCallback_(fd);
```

**实测**：`tcp_client_test` 里两个用例专门覆盖这条路径——「对端未就绪 → 退避重试 → 自动连上」和「对端断开 → 自动重连」。加上 200ms 起、上限 5s 的指数退避，对端重启一次不会让客户端永久失联（这是重试存在的唯一理由）。

---

## 坑 2：重复定时器的「漂移」，误差随次数线性累积

**现象**

一个 20ms 间隔的重复定时器，回调本身耗时 15ms。跑了 10 次，总耗时 215ms——但对着代码算「20ms 一次，10 次应该是 200ms」，多出来的 15ms 看着不多，可它**每多跑一次就多 15ms**。1000 次就是 15 秒的偏差。

**原因**

朴素写法是在回调**结束之后**再等一个间隔：

```cpp
// 错误写法：从回调结束时刻重新计时
void onTimer() {
  doWork();                       // 耗时 15ms
  loop->runAfter(intervalMs, onTimer);   // 从「现在」再等 20ms
}
```

每一次的周期实际是 `interval + callback`，误差按次数累加，永不收敛。

**修法**

按**原到期时间**递推下一次的到期时间，把回调耗时排除在周期之外：

```cpp
nextExpiration += interval_;   // 基于上一次的计划到期时刻，而不是 now()
```

这套 `TimerQueue` 的组织方式是「**一个 loop 一个 timerfd + 最小堆**」：timerfd 负责「最近一个定时器到期时唤醒 epoll」，堆负责在大量定时器里找最近的那个。取消用懒删除（标记 `canceled`）配合 `active_` 表做到 O(1)。

**实测**（单元测试用例 5）：

```
[5] 重复定时器不累积漂移  -> 实测总耗时 215 ms（期望约 215 ms）
```

- 朴素实现：`10 × (20 + 15) = 350 ms`
- 本实现：`10 × 20 = 200 ms` 量级，实测 215ms 吻合
- **提速约 1.6 倍，且差距随次数线性扩大**

顺带一提，定时器用的是**相对时间**而不是绝对时间，因为「`steady_clock` 的 epoch 等于 `CLOCK_MONOTONIC`」只是个假设，不是标准保证。

---

## 坑 3：超时兜底的仲裁标志，不能放在那个会被 `delete` 的对象里

**现象**

业务实现漏调 `done->Run()` 时，框架要在超时后回一个错误码，并回收 `request / response / controller`。于是我加了一条超时路径，和业务正常的 `done->Run()` 路径之间用 CAS 仲裁谁收尾。

第一版把 `std::atomic<bool> finished` 放在 `MethodDone` 里——看起来很自然。ASan 立刻抓到一个 use-after-free。

**原因**

CAS 的**赢家**收尾时会 `delete` 掉 `MethodDone`。而**输家**那一方紧接着还要：

```cpp
if (done->finished.exchange(true)) return;
// 输的一方执行到这行时，this 可能已经被赢家 delete 了
```

`exchange` 的调用本身就踩在已释放内存上。这不是「加个锁」能解决的，因为问题出在对象的生命周期，而不是可见性。

**修法**

把仲裁标志提到一个**独立的、共享所有权的对象**上，让 flag 的生命周期长于两个竞争者：

```cpp
struct RequestGate {
  std::atomic<bool> finished{false};
};

// 超时路径 / 业务回包路径，双方各自持有一个 shared_ptr<RequestGate>
if (gate->finished.exchange(true)) return;   // 输的一方直接退出，什么都不碰
```

赢家负责回收资源，输家只读一个 `atomic<bool>`，不触碰任何即将消失的对象。

**实测**：让业务实现故意不回包（`mrpc.EchoService.NoReply`），客户端超时设 150ms、服务端兜底设 400ms：

```
服务端日志: timed out in mrpc.EchoService.NoReply
客户端日志: no pending call for seq ...
LeakSanitizer 报告为空
```

三行日志分别说明：兜底真的触发了、迟到响应被正确丢弃、先前必漏的对象已被回收。

---

## 坑 4：同一份 seq 表，同步和异步的登记顺序**相反**

**现象**

客户端一条连接上按 `msg.seq()` 匹配响应，支持多个在途请求。同步调用和异步调用我用了同一套「登记 → 发送」的流程。结果同步调用偶发超时，概率不高但稳定复现。

**原因**

两条路径的时间序不同，所以要防的竞态也不同：

- **同步调用**：必须**先登记再发送**。反过来（先发再登记）的话，如果响应比我登记更快到达（同机 loopback 上完全可能），IO 线程会查不到这个 seq，当成「未知 seq」丢掉。调用方只能干等到超时——**而且响应其实早就到了**。
- **异步调用**：必须**先登记再挂定时器**。反过来，定时器可能在登记对 IO 线程可见之前就触发，那次调用永远没人收尾，回调不会执行，登记也不会被清理。

**修法**

顺序不是「统一成一种」，而是**两个方向各自按对**：

```cpp
// 同步：登记 -> 发送 -> 等条件变量
registerPending(seq, ...);
conn->send(frame);
cv.wait(lock, [&]{ return done; });

// 异步：登记 -> 挂定时器 -> 发送
registerPending(seq, ...);
timerId = loop->runAfter(timeoutMs, onTimeout);
conn->send(frame);
```

**实测**（`stub_client_test`，单进程 200 次调用）：

```
同步调用: 成功 200/200
异步调用: 成功 200/200
超时调用: [OK] (rpc call timed out after 150 ms)
超时后连接仍可用: [OK]
```

顺带解释了另一件事：**客户端的并发上限是 seq 空间，不是连接数**。一条连接就能挂大量在途请求，这也是连接池里「一个节点只用一条连接」的依据。

---

## 坑 5：连接池里挂掉的节点，会让每一次调用都白等满超时

**现象**

连接池做失败转移：连不上就换下一个节点。功能上没问题，但压测出来慢得离谱——只有一个节点挂掉，10 次调用花了 15 秒。

**原因**

第一版没有冷却。每次 `getChannel()` 选中那个挂掉的节点，都要**实打实等满 3 秒**的 connect 超时才切下一个。轮询策略下，一半的调用（5 次）各付 3 秒，`5 × 3 = 15s`——完全对得上。调用次数越多，浪费越线性增长。

**修法**

给每个节点记一个「冷却截止时间」，失败后一段时间内直接跳过，不去等它：

```cpp
struct Node {
  InetAddress addr;
  std::shared_ptr<RpcChannel> channel;
  int64_t downUntilMs = 0;   // 冷却截止，steady_clock 毫秒
};
constexpr int64_t kNodeCooldownMs = 1000;
```

冷却时长是个权衡：太短则每次调用继续白等，太长则错过对端重启。

**对照组实测**（单节点可用、10 次调用）：

| 冷却 | 耗时 | 结果 |
|---|---:|---|
| 无（冷却时间置 0） | **15.0 s** | 10/10 成功 |
| 1 s | **3.0 s** | 10/10 成功 |

**便宜 5 倍**，而且差距随调用次数扩大（按此比例 100 次调用是 150 秒 vs 3 秒）。这是我做得最值的一个对照实验——因为它一开始只是「感觉有点慢」，量化之后才发现是个 5 倍的差距。

顺带记两条连接池的坑：

- **`waitConnected()` 必须在锁外调用**。它会阻塞，在锁内调用的话，一个连不上的节点会把整个池卡住。
- **一致性哈希的空 key 会退化**。没有 key 时所有请求都落到环上同一个位置，等于把流量全打到一个节点。实现里对空 key 用一个自增序号打散；每个物理节点放 100 个虚拟节点（只放 1 个的话，环上分布严重不均）。

**实测**：

```
---- 轮询 ----
轮询: 成功 100/100  [A]=50  [B]=50
---- 一致性哈希（固定 key）----
一致性哈希: 成功 100/100  [B]=100     <- 固定 key 稳定粘在同一节点
---- 失败转移（关掉节点 B 之后）----
失败转移: 成功 100/100  [A]=100
```

---

## 坑 6：`sigprocmask` 必须在创建**任何线程之前**执行

**现象**

用 `signalfd` 把 `SIGINT/SIGTERM` 变成普通的 epoll 可读事件，好处是不用在异步信号处理函数里做任何非 async-signal-safe 的操作（`printf`/`malloc` 都不安全）。代码写完了，`Ctrl-C` 一按——进程还是被直接干掉，析构函数一行都没执行，LeakSanitizer 因此报了一堆「泄漏」。

**原因**

信号掩码是**线程级**的，而且新线程**继承创建时刻**的掩码。我的 `SignalWatcher` 构造得比 IO 线程晚，于是那 4 个 IO 线程仍持有默认掩码，SIGTERM 一到就按默认动作终止进程。主线程屏蔽得再干净也没用——**信号可以投递给进程里任何一个没屏蔽它的线程**。

**修法**

把 `sigprocmask` 移到创建任何线程之前，确保所有线程继承同一份掩码：

```cpp
// 先屏蔽信号，再建 signalfd。
// 屏蔽必须发生在其他线程创建之前，它们才会继承同一份掩码。
if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
  LOG_FATAL << "SignalWatcher: sigprocmask failed";
}
sigfd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
```

还有个小坑：`handleRead` 里要**读到 `EAGAIN` 为止**。一次可能有多个信号积压（比如连按两次 `Ctrl-C`），不读完的话 signalfd 一直是可读的，LT 模式下 epoll 会反复唤醒——变成忙循环。

**实测**（主线程 + 4 个 IO 线程）：

```
SigBlk: 0000000000004002      # bit1=SIGINT(2), bit14=SIGTERM(15)
tasks = 5                     # 5 个线程全部继承
```

```
--- 优雅退出：SIGTERM -> signalfd -> loop.quit() ---
正常退出（exit=0），所有对象按析构顺序释放
```

**这里有个连带收益**：在此之前服务端是被 `kill` 直接终止的，析构函数根本不执行，LeakSanitizer 无从判定，测试里只能设 `detect_leaks=0`——**泄漏检测等于没开**。补上优雅退出后才有资格把 `detect_leaks` 打开，也才引出下一个坑。

---

## 坑 7：「LeakSanitizer 报告为空」在没做阳性对照前，是没有意义的

**现象**

修完坑 6，测试脚本终于打印出 `报告为空：无内存错误、无未定义行为、无泄漏`。看着一片绿，但我不敢信——因为就在十分钟前，同一个脚本也是「报告为空」，而那时 `detect_leaks=0`，检测器压根没在工作。

**原因**

「没有输出」可能有两种截然不同的含义：

1. 真的没有泄漏；
2. 检测器没启动、环境变量没生效、进程没走到检查那一步、日志路径写错。

单看「为空」，这两种情况**无法区分**。这正是坑 6 能藏那么久的原因。

**修法**

加一个**阳性对照**：编译一个故意泄漏的探针，用**完全相同**的 `ASAN_OPTIONS` 跑一遍，确认它确实能产出报告。

```cpp
int main() { new int[100]; return 0; }   // 故意泄漏，不做 free
```

只有对照通过，「报告为空」才是一个有意义的结论。

**实测**：

```
阳性对照通过：LeakSanitizer 能检测到故意泄漏
报告为空：无内存错误、无未定义行为、无泄漏
```

这个思路不止适用于 LeakSanitizer。任何「负向断言」的测试都一样——**心跳超时断开**测试，如果不先确认「健康连接不会被误杀」，那「死连接被断开」可能只是因为它碰到了别的错误；**错误码**测试，不先确认正常路径返回 0，那错误码全对也可能只是因为所有请求都失败了。**测试没通过要怀疑代码，测试通过要怀疑测试。**

---

## 这几个坑的共同点

回头看，这 7 个坑有一个共同结构：

> **语义上看起来完全正确，只有测量才能发现错了。**

- `EPOLLOUT` 看起来就是「可写 = 连上了」——语义上说得通，错在 socket 的错误传递方式；
- 重复定时器「回调结束后等一个间隔」——语义上是标准的「每 20ms 执行一次」，错在周期起点；
- 仲裁标志放在被仲裁的对象里——语义上最有内聚性，错在生命周期；
- 同步/异步登记顺序相反——语义上「统一流程」更整洁，错在两条路径的时序不同；
- 无冷却的失败转移——语义上「连不上就换一个」完全正确，错的只是性能；
- `sigprocmask` 的位置——语义上「屏蔽信号」做到了，错在线程继承时机；
- 空报告——语义上是「没问题」，错在它也可能代表「没检查」。

它们都不会让代码编译失败，大部分也不会让功能立刻出错。**只有实测数字、Sanitizer 报告、`/proc` 里的信号掩码这类外部证据，才能把它们暴露出来。**

所以我现在写这个项目的顺序变成了：先能跑 → 再想办法证明它**真的**对 → 最后才敢写进文档。文档里每一个数字都有对应的脚本和日志，因为我知道面试官会问「这个 215ms 是怎么测的」。

---

## 附：项目结构

```
src/
├── net/         epoll 封装、Reactor、Buffer、定时器、信号、连接
│   EventLoop / Epoller / Channel / TimerQueue / SignalWatcher
│   Acceptor / Connector / TcpConnection / TcpServer / TcpClient
├── protocol/    协议层（依赖 protobuf）
│   RpcCodec / RpcChannel / RpcServer / ServiceRegistry / LoadBalancer / ConnectionPool
├── main/        各种 client / server / benchmark 可执行文件
└── test/        单元测试
scripts/         8 个端到端验证脚本（含 ASan/UBSan/LeakSanitizer）
```

- 仓库：<https://github.com/TiptopCash/mini-rpc>
- CI 覆盖：Release 构建 + `ctest` + 协议分帧/粘包/半包 + 心跳 + 完整 RPC 链路（含 ASan 构建）
