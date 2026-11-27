#include "hlc.hpp"

#include <algorithm>

namespace sy {

std::string to_string(const Hlc& t) {
  return std::to_string(t.wall) + "." + std::to_string(t.logical);
}

Hlc HlcClock::tick(std::int64_t pt) {
  if (pt > last_.wall) {
    last_ = {pt, 0};
  } else {
    ++last_.logical;
  }
  return last_;
}

Hlc HlcClock::merge(const Hlc& remote, std::int64_t pt) {
  const std::int64_t wall = std::max({last_.wall, remote.wall, pt});

  std::uint32_t logical = 0;
  if (wall == last_.wall && wall == remote.wall) {
    logical = std::max(last_.logical, remote.logical) + 1;
  } else if (wall == last_.wall) {
    logical = last_.logical + 1;
  } else if (wall == remote.wall) {
    logical = remote.logical + 1;
  }

  last_ = {wall, logical};
  return last_;
}

}  // namespace sy
