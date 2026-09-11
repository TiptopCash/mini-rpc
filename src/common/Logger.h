#pragma once

#include <mutex>
#include <sstream>
#include <string>

namespace mrpc {

enum LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL };

const char* levelName(LogLevel level);

class Logger {
 public:
  static Logger& instance();

  LogLevel level() const { return level_; }
  void setLevel(LogLevel level) { level_ = level; }

  void output(const std::string& line);

 private:
  Logger() = default;
  LogLevel level_ = INFO;
  std::mutex mutex_;
};

// RAII: 析构时统一加上时间戳、级别、文件行号并输出
class LogMessage {
 public:
  LogMessage(const char* file, int line, LogLevel level);
  ~LogMessage();

  std::ostringstream& stream() { return stream_; }

 private:
  const char* file_;
  int line_;
  LogLevel level_;
  std::ostringstream stream_;
};

}  // namespace mrpc

#define LOG_TRACE mrpc::LogMessage(__FILE__, __LINE__, mrpc::TRACE).stream()
#define LOG_DEBUG mrpc::LogMessage(__FILE__, __LINE__, mrpc::DEBUG).stream()
#define LOG_INFO mrpc::LogMessage(__FILE__, __LINE__, mrpc::INFO).stream()
#define LOG_WARN mrpc::LogMessage(__FILE__, __LINE__, mrpc::WARN).stream()
#define LOG_ERROR mrpc::LogMessage(__FILE__, __LINE__, mrpc::ERROR).stream()
#define LOG_FATAL mrpc::LogMessage(__FILE__, __LINE__, mrpc::FATAL).stream()
