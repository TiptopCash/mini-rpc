// 简易阻塞式 echo 客户端，用于功能验证与粗略压测
// 用法: ./echo_client [ip] [port] [count] [msg_size]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
  const char* ip = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(::atoi(argv[2])) : 8888;
  int count = argc > 3 ? ::atoi(argv[3]) : 1000;
  size_t msgSize = argc > 4 ? static_cast<size_t>(::atoi(argv[4])) : 64;

  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
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

  if (::connect(sockfd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) < 0) {
    perror("connect");
    return 1;
  }

  std::string msg(msgSize, 'x');
  std::string reply(msgSize, '\0');

  auto start = std::chrono::steady_clock::now();
  bool ok = true;

  for (int i = 0; i < count && ok; ++i) {
    ssize_t n = ::write(sockfd, msg.data(), msg.size());
    if (n != static_cast<ssize_t>(msg.size())) {
      perror("write");
      ok = false;
      break;
    }
    size_t got = 0;
    while (got < msg.size()) {
      ssize_t r = ::read(sockfd, &reply[got], msg.size() - got);
      if (r <= 0) {
        perror("read");
        ok = false;
        break;
      }
      got += static_cast<size_t>(r);
    }
  }

  auto end = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(end - start).count();
  if (ok && sec > 0) {
    printf("count=%d size=%zu time=%.3fs QPS=%.0f\n", count, msgSize, sec,
           count / sec);
  } else {
    printf("benchmark aborted\n");
  }

  ::close(sockfd);
  return ok ? 0 : 1;
}
