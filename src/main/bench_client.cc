// 多线程同步压测客户端：每线程一条连接，请求-响应串行
// 用法: ./bench_client [ip] [port] [threads] [seconds] [msg_size]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Us = std::chrono::duration<double, std::micro>;

int connectTo(const char* ip, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    ::close(fd);
    return -1;
  }
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(fd);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

struct Result {
  long long ok = 0;
  long long fail = 0;
  std::vector<double> latencies;  // 微秒，采样上限见 kSampleCap
};

constexpr size_t kSampleCap = 200000;

void worker(const char* ip, uint16_t port, int seconds, size_t msgSize,
            Result* result) {
  int fd = connectTo(ip, port);
  if (fd < 0) {
    result->fail = -1;
    return;
  }

  std::string msg(msgSize, 'x');
  std::string reply(msgSize, '\0');
  const auto deadline = Clock::now() + std::chrono::seconds(seconds);

  while (true) {
    // 每 64 次检查一次时间，减少时钟调用开销
    if ((result->ok & 0x3F) == 0 && Clock::now() >= deadline) {
      break;
    }

    auto t0 = Clock::now();
    ssize_t n = ::write(fd, msg.data(), msg.size());
    if (n != static_cast<ssize_t>(msg.size())) {
      ++result->fail;
      break;
    }

    size_t got = 0;
    bool broken = false;
    while (got < msg.size()) {
      ssize_t r = ::read(fd, &reply[got], msg.size() - got);
      if (r <= 0) {
        broken = true;
        break;
      }
      got += static_cast<size_t>(r);
    }
    if (broken) {
      ++result->fail;
      break;
    }

    auto t1 = Clock::now();
    ++result->ok;
    if (result->latencies.size() < kSampleCap) {
      result->latencies.push_back(Us(t1 - t0).count());
    }
  }

  ::close(fd);
}

double percentile(std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  size_t idx = static_cast<size_t>(p * (sorted.size() - 1));
  return sorted[idx];
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 8888;
  int threads = argc > 3 ? ::atoi(argv[3]) : 8;
  int seconds = argc > 4 ? ::atoi(argv[4]) : 5;
  size_t msgSize = argc > 5 ? static_cast<size_t>(::atoi(argv[5])) : 64;

  if (threads <= 0 || seconds <= 0 || msgSize == 0) {
    fprintf(stderr, "invalid args\n");
    return 1;
  }

  std::vector<Result> results(threads);
  std::vector<std::thread> workers;
  workers.reserve(threads);

  const auto start = Clock::now();
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back(worker, ip, port, seconds, msgSize, &results[i]);
  }
  for (auto& t : workers) {
    t.join();
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  long long totalOk = 0, totalFail = 0;
  std::vector<double> all;
  for (auto& r : results) {
    if (r.fail < 0) {
      fprintf(stderr, "a worker failed to connect\n");
      return 1;
    }
    totalOk += r.ok;
    totalFail += r.fail;
    all.insert(all.end(), r.latencies.begin(), r.latencies.end());
  }
  std::sort(all.begin(), all.end());

  printf(
      "threads=%d seconds=%d msg_size=%zu elapsed=%.2fs\n"
      "total_ok=%lld total_fail=%lld  QPS=%.0f\n"
      "latency(us)  P50=%.1f  P90=%.1f  P99=%.1f  P999=%.1f  max=%.1f\n",
      threads, seconds, msgSize, elapsed, totalOk, totalFail,
      elapsed > 0 ? totalOk / elapsed : 0, percentile(all, 0.50),
      percentile(all, 0.90), percentile(all, 0.99), percentile(all, 0.999),
      all.empty() ? 0 : all.back());

  return totalFail > 0 ? 1 : 0;
}
