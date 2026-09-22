#pragma once

#include <cstdint>

namespace sy {

std::int64_t now_ns();

// Cristian's algorithm: take the upstream's timestamp and assume the reply
// spent exactly half the round trip in flight.
//
// That assumption is the entire error term. If the request took a and the reply
// took b, the estimate is wrong by (a - b) / 2, and no amount of sampling fixes
// it, because nothing in the exchange can tell the two directions apart. Along
// a chain each node inherits its upstream's error and adds its own, which is
// what the VM test measures.
std::int64_t cristian_offset(std::int64_t t1, std::int64_t upstream,
                             std::int64_t t4);

// A node's own clock plus whatever correction it has most recently estimated.
class Clock {
 public:
  std::int64_t now() const { return now_ns() + offset_ns_; }
  std::int64_t offset_ns() const { return offset_ns_; }
  void set_offset_ns(std::int64_t offset) { offset_ns_ = offset; }

 private:
  std::int64_t offset_ns_ = 0;
};

}  // namespace sy
