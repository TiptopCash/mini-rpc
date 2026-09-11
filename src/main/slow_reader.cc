// 慢读客户端：先灌入大量数据、然后暂停读取，逼服务端进入
// sendInLoop 部分写 -> outputBuffer_ 缓存 -> EPOLLOUT 续传 的分支。
//
// 服务端 socket 是非阻塞的：若部分写逻辑有缺陷，超出内核缓冲的数据会直接丢失，
// 因此“完整收回且字节一致”即证明该路径工作正常。
//
// 用法: ./slow_reader [ip] [port] [total_MB] [stall_ms]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kChunk = 64 * 1024;

char patternAt(size_t offset) { return static_cast<char>(offset % 251); }

void fillChunk(std::string& buf, size_t startOffset) {
  for (size_t i = 0; i < buf.size(); ++i) {
    buf[i] = patternAt(startOffset + i);
  }
}

bool writeAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool readAll(int fd, char* data, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::read(fd, data + got, len - got);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 8888;
  size_t totalMB = argc > 3 ? static_cast<size_t>(::atoi(argv[3])) : 16;
  int stallMs = argc > 4 ? ::atoi(argv[4]) : 2000;

  const size_t total = totalMB * 1024 * 1024;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    perror("inet_pton");
    return 1;
  }
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    perror("connect");
    return 1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  std::string chunk(kChunk, 0);
  size_t sent = 0;
  while (sent < total) {
    size_t n = std::min(kChunk, total - sent);
    chunk.resize(n);
    fillChunk(chunk, sent);
    if (!writeAll(fd, chunk.data(), n)) {
      perror("write");
      return 1;
    }
    sent += n;
  }
  printf("已发送 %zu MB；接下来 %d ms 不读取，让服务端输出缓冲堆积...\n", totalMB,
         stallMs);
  fflush(stdout);

  ::usleep(static_cast<useconds_t>(stallMs) * 1000);

  std::vector<char> recvBuf(total);
  auto t0 = std::chrono::steady_clock::now();
  if (!readAll(fd, recvBuf.data(), total)) {
    perror("read");
    return 1;
  }
  auto t1 = std::chrono::steady_clock::now();

  size_t mismatch = 0;
  size_t firstBad = 0;
  for (size_t i = 0; i < total; ++i) {
    if (recvBuf[i] != patternAt(i)) {
      if (mismatch == 0) firstBad = i;
      ++mismatch;
    }
  }

  double sec = std::chrono::duration<double>(t1 - t0).count();
  printf("接收完成: %.2fs, %.1f MB/s\n", sec, totalMB / sec);
  if (mismatch == 0) {
    printf("内容校验: 全部一致（%zu 字节）\n", total);
  } else {
    printf("内容校验: 失败，mismatch=%zu，首个错误位置=%zu\n", mismatch,
           firstBad);
  }

  ::close(fd);
  return mismatch == 0 ? 0 : 1;
}
