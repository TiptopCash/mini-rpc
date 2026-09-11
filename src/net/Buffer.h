#pragma once

#include <sys/types.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace mrpc {

// 应用层读写缓冲区：解决 TCP 粘包/半包
//
//  +-------------------+------------------+------------------+
//  | prependable bytes |  readable bytes  |  writable bytes  |
//  |                   |     (CONTENT)    |                  |
//  +-------------------+------------------+------------------+
//  0      <=      readerIndex   <=   writerIndex    <=     size
class Buffer {
 public:
  static const size_t kCheapPrepend = 8;
  static const size_t kInitialSize = 1024;

  explicit Buffer(size_t initialSize = kInitialSize)
      : buffer_(kCheapPrepend + initialSize),
        readerIndex_(kCheapPrepend),
        writerIndex_(kCheapPrepend) {}

  size_t readableBytes() const { return writerIndex_ - readerIndex_; }
  size_t writableBytes() const { return buffer_.size() - writerIndex_; }
  size_t prependableBytes() const { return readerIndex_; }

  const char* peek() const { return begin() + readerIndex_; }

  const char* findCRLF() const {
    const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
    return crlf == beginWrite() ? nullptr : crlf;
  }

  void retrieve(size_t len) {
    if (len < readableBytes()) {
      readerIndex_ += len;
    } else {
      retrieveAll();
    }
  }

  void retrieveAll() {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
  }

  std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

  std::string retrieveAsString(size_t len) {
    std::string result(peek(), len);
    retrieve(len);
    return result;
  }

  void ensureWritableBytes(size_t len) {
    if (writableBytes() < len) {
      makeSpace(len);
    }
  }

  void append(const char* data, size_t len) {
    ensureWritableBytes(len);
    std::copy(data, data + len, beginWrite());
    hasWritten(len);
  }

  void append(const void* data, size_t len) {
    append(static_cast<const char*>(data), len);
  }

  void append(const std::string& str) { append(str.data(), str.size()); }

  char* beginWrite() { return begin() + writerIndex_; }
  const char* beginWrite() const { return begin() + writerIndex_; }

  void hasWritten(size_t len) { writerIndex_ += len; }

  // 从 fd 读取数据；返回实际读取字节数，出错时置 savedErrno 并返回 -1
  ssize_t readFd(int fd, int* savedErrno);

 private:
  char* begin() { return buffer_.data(); }
  const char* begin() const { return buffer_.data(); }

  void makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
      buffer_.resize(writerIndex_ + len);
    } else {
      size_t readable = readableBytes();
      std::copy(begin() + readerIndex_, begin() + writerIndex_,
                begin() + kCheapPrepend);
      readerIndex_ = kCheapPrepend;
      writerIndex_ = readerIndex_ + readable;
    }
  }

  std::vector<char> buffer_;
  size_t readerIndex_;
  size_t writerIndex_;

  static const char kCRLF[];
};

}  // namespace mrpc
