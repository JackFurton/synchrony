#include "state.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "hlc.hpp"

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

std::string scratch_dir() {
  const char* base = std::getenv("TMPDIR");
  std::string tmpl = std::string(base ? base : "/tmp") + "/synchrony-state-XXXXXX";
  const char* d = ::mkdtemp(tmpl.data());
  return d ? d : "";
}

void round_trip() {
  const std::string path = scratch_dir() + "/clock";
  sy::ClockState out;
  CHECK(!sy::load_state(path, out));

  CHECK(sy::save_state(path, {-483999000, 1795797261197853000}));
  CHECK(sy::load_state(path, out));
  CHECK(out.offset_ns == -483999000);
  CHECK(out.hlc_floor == 1795797261197853000);

  CHECK(sy::save_state(path, {7, 8}));
  CHECK(sy::load_state(path, out) && out.offset_ns == 7 && out.hlc_floor == 8);
  CHECK(::access((path + ".tmp").c_str(), F_OK) != 0);
}

void garbled_is_no_state() {
  const std::string path = scratch_dir() + "/clock";
  std::FILE* f = std::fopen(path.c_str(), "w");
  std::fputs("12\n", f);
  std::fclose(f);
  sy::ClockState out{1, 2};
  CHECK(!sy::load_state(path, out));
  CHECK(out.offset_ns == 1 && out.hlc_floor == 2);
}

// The previous run sent stamps up to 99ms. Restarted with its raw clock back
// at 50ms, the clock must not issue anything that sorts before them.
void restored_hlc_never_goes_back() {
  sy::HlcClock c;
  c.restore(100);
  const sy::Hlc t = c.tick(50);
  CHECK(t.wall == 100);
  CHECK((t > sy::Hlc{99, 1000}));
  CHECK((c.tick(150) == sy::Hlc{150, 0}));
}

}  // namespace

int main() {
  round_trip();
  garbled_is_no_state();
  restored_hlc_never_goes_back();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
