#pragma once

#include <chrono>
#include <cstdint>

namespace mrpc {

// 单调时钟微秒时间戳。
// 用 steady_clock 而非 system_clock：不受系统时间调整影响，适合计算时间间隔。
inline int64_t nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace mrpc
