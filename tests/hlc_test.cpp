#include "hlc.hpp"

#include <algorithm>
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

// Only here to compare against. It orders causal events correctly and that is
// all it does.
struct Lamport {
  std::uint64_t t = 0;
  std::uint64_t tick() { return ++t; }
  std::uint64_t merge(std::uint64_t remote) {
    t = std::max(t, remote) + 1;
    return t;
  }
};

void tick_follows_physical_time() {
  sy::HlcClock c;
  CHECK((c.tick(10 * ms) == sy::Hlc{10 * ms, 0}));
  CHECK((c.tick(20 * ms) == sy::Hlc{20 * ms, 0}));
}

void tick_holds_when_the_clock_stalls() {
  sy::HlcClock c;
  const auto a = c.tick(10 * ms);
  const auto b = c.tick(10 * ms);
  CHECK(b > a);
  CHECK(b.wall == 10 * ms);
  CHECK(b.logical == 1);
}

// The case that motivated this: Cristian's correction steps the clock back
// every time the offset estimate goes down. Physical time repeats itself, the
// HLC does not.
void tick_holds_when_the_clock_steps_backwards() {
  sy::HlcClock c;
  const auto before = c.tick(100 * ms);
  const auto after = c.tick(80 * ms);
  CHECK(after > before);
  CHECK(after.wall == 100 * ms);

  // And it lets go as soon as physical time catches up.
  CHECK((c.tick(101 * ms) == sy::Hlc{101 * ms, 0}));
}

void merge_cases() {
  {
    sy::HlcClock c;
    c.tick(10 * ms);
    CHECK((c.merge({50 * ms, 3}, 20 * ms) == sy::Hlc{50 * ms, 4}));
  }
  {
    sy::HlcClock c;
    c.tick(50 * ms);
    CHECK((c.merge({10 * ms, 7}, 20 * ms) == sy::Hlc{50 * ms, 1}));
  }
  {
    sy::HlcClock c;
    c.tick(10 * ms);
    CHECK((c.merge({20 * ms, 9}, 90 * ms) == sy::Hlc{90 * ms, 0}));
  }
  {
    sy::HlcClock c;
    c.tick(50 * ms);
    c.tick(50 * ms);  // logical is now 1
    CHECK((c.merge({50 * ms, 4}, 50 * ms) == sy::Hlc{50 * ms, 5}));
  }
}

// Node A's clock runs 100ms fast. A does something and tells B, and 5ms later
// B does something because of it.
void three_clocks_one_causal_pair() {
  const std::int64_t skew = 100 * ms;
  const std::int64_t a_pt = 0 + skew;  // true time 0, read on A's fast clock
  const std::int64_t b_pt = 5 * ms;    // true time 5ms, read on B's clock

  // Wall clock puts the effect 95ms before its cause.
  CHECK(b_pt < a_pt);

  // Lamport gets the order right...
  Lamport la, lb, lc;
  const auto l_a = la.tick();
  const auto l_b = lb.merge(l_a);
  CHECK(l_b > l_a);
  // ...but an unrelated event three hours later on some other node sorts
  // before both. The numbers carry no time at all.
  const auto l_c = lc.tick();
  CHECK(l_c < l_b);

  sy::HlcClock ha, hb, hc;
  const auto h_a = ha.tick(a_pt);
  const auto h_b = hb.merge(h_a, b_pt);
  CHECK(h_b > h_a);
  // Still readable as a time: off from B's clock by no more than the skew.
  CHECK(h_b.wall - b_pt <= skew);

  const auto h_c = hc.tick(3LL * 3600 * 1000 * ms);
  CHECK(h_c > h_b);
}

}  // namespace

int main() {
  tick_follows_physical_time();
  tick_holds_when_the_clock_stalls();
  tick_holds_when_the_clock_steps_backwards();
  merge_cases();
  three_clocks_one_causal_pair();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
