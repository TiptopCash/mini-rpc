#include "common/Logger.h"

#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mrpc {

const char* levelName(LogLevel level) {
  static const char* names[] = {"TRACE", "DEBUG", "INFO",
                                "WARN",  "ERROR", "FATAL"};
  if (level < TRACE || level > FATAL) return "UNKNOWN";
  return names[level];
}

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::output(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  fwrite(line.data(), 1, line.size(), stdout);
  fflush(stdout);
}

LogMessage::LogMessage(const char* file, int line, LogLevel level)
    : file_(file), line_(line), level_(level) {}

LogMessage::~LogMessage() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm_time;
  time_t sec = tv.tv_sec;
  localtime_r(&sec, &tm_time);

  char timebuf[64];
  snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d.%06ld ",
           tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
           tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, (long)tv.tv_usec);

  const char* slash = strrchr(file_, '/');
  const char* base = slash ? slash + 1 : file_;

  std::string full = std::string(timebuf) + levelName(level_) + " " + base +
                     ":" + std::to_string(line_) + " - " + stream_.str() + "\n";

  if (level_ >= Logger::instance().level()) {
    Logger::instance().output(full);
  }

  if (level_ == FATAL) {
    abort();
  }
}

}  // namespace mrpc
