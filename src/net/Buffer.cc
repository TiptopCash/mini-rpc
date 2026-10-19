#include "net/Buffer.h"

#ifndef _WIN32
#include <sys/uio.h>
#endif

#include <errno.h>

namespace mrpc {

const char Buffer::kCRLF[] = "\r\n";

#ifndef _WIN32
// readv 是 POSIX 接口，MinGW 没有 sys/uio.h。Qt 客户端只用内存缓冲部分，
// 走不到这里；服务端在 Linux 下编译不受影响。
ssize_t Buffer::readFd(int fd, int* savedErrno) {
  // 栈上 64KB 临时缓冲 + readv 分散读，尽量一次系统调用读完
  char extrabuf[65536];
  struct iovec vec[2];
  const size_t writable = writableBytes();

  vec[0].iov_base = begin() + writerIndex_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof(extrabuf);

  const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);

  if (n < 0) {
    *savedErrno = errno;
  } else if (static_cast<size_t>(n) <= writable) {
    writerIndex_ += static_cast<size_t>(n);
  } else {
    writerIndex_ = buffer_.size();
    append(extrabuf, static_cast<size_t>(n) - writable);
  }
  return n;
}
#endif  // !_WIN32

}  // namespace mrpc
