#include "clock.hpp"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                   #cond);                                               \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

constexpr std::int64_t ms = 1'000'000;

// One exchange where the upstream clock genuinely reads upstream_ahead_by more
// than ours, with a known delay each way. t1 is arbitrary; only differences
// matter. A correct estimate recovers upstream_ahead_by exactly.
std::int64_t estimate(std::int64_t upstream_ahead_by, std::int64_t up_delay,
                      std::int64_t down_delay) {
  const std::int64_t t1 = 1'000'000'000'000;
  // The upstream stamps its reply the moment the request lands, in its frame.
  const std::int64_t upstream = (t1 + up_delay) + upstream_ahead_by;
  const std::int64_t t4 = t1 + up_delay + down_delay;
  return sy::cristian_offset(t1, upstream, t4);
}

void perfect_link_is_exact() {
  CHECK(estimate(0, 0, 0) == 0);
  CHECK(estimate(50 * ms, 0, 0) == 50 * ms);
}

void symmetric_delay_cancels() {
  CHECK(estimate(0, 20 * ms, 20 * ms) == 0);
  CHECK(estimate(-7 * ms, 100 * ms, 100 * ms) == -7 * ms);
}

// The reason the whole chain drifts: half the round trip is not half the path
// when the path is lopsided, and the estimate is out by (up - down) / 2.
void asymmetric_delay_is_off_by_half_the_difference() {
  CHECK(estimate(0, 10 * ms, 50 * ms) == -20 * ms);
  CHECK(estimate(0, 50 * ms, 10 * ms) == 20 * ms);
  CHECK(estimate(5 * ms, 10 * ms, 50 * ms) == 5 * ms - 20 * ms);
}

// Sampling harder does not help: the error is in the model, not the noise.
void more_samples_do_not_help() {
  for (int i = 1; i <= 50; ++i) {
    const std::int64_t jitter = i * 1000;
    CHECK(estimate(0, 10 * ms + jitter, 50 * ms + jitter) == -20 * ms);
  }
}

void clock_applies_its_offset() {
  sy::Clock c;
  CHECK(c.offset_ns() == 0);
  const std::int64_t before = c.now();
  c.set_offset_ns(1'000'000'000);
  CHECK(c.offset_ns() == 1'000'000'000);
  CHECK(c.now() - before >= 900'000'000);
}

}  // namespace

int main() {
  perfect_link_is_exact();
  symmetric_delay_cancels();
  asymmetric_delay_is_off_by_half_the_difference();
  more_samples_do_not_help();
  clock_applies_its_offset();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
