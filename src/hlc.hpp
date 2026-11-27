#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace sy {

// Hybrid logical clock (Kulkarni et al., 2014).
//
// wall tracks the largest physical time this node has seen, its own or in a
// message. logical breaks ties when wall has not moved. Together they give a
// timestamp that never goes backwards, orders every causally related pair of
// events correctly, and still reads as roughly the wall-clock time, off by at
// most the clock skew between nodes.
struct Hlc {
  std::int64_t wall = 0;
  std::uint32_t logical = 0;

  auto operator<=>(const Hlc&) const = default;
};

std::string to_string(const Hlc& t);

class HlcClock {
 public:
  // A local event or a send. pt is this node's physical time.
  Hlc tick(std::int64_t pt);

  // A receive: fold the sender's timestamp in so everything after this event
  // sorts after everything that caused it.
  Hlc merge(const Hlc& remote, std::int64_t pt);

  const Hlc& last() const { return last_; }

 private:
  Hlc last_;
};

}  // namespace sy
