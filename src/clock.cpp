#include "clock.hpp"

#include <ctime>

namespace sy {

std::int64_t now_ns() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::int64_t cristian_offset(std::int64_t t1, std::int64_t upstream,
                             std::int64_t t4) {
  return upstream + (t4 - t1) / 2 - t4;
}

}  // namespace sy
