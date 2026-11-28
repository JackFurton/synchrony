#include "read.hpp"

#include <cstdio>
#include <string>

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

void live_deleted_and_missing() {
  sy::Store s;
  s.apply("live", {"blue", {10, 1}, {10, 1}});
  sy::Entry gone{"", {20, 0}, {20, 0}};
  gone.tombstone = true;
  s.apply("gone", gone);
  s.horizon() = {0, 900};

  std::string resp;
  CHECK(sy::answer_get(s, 1000, sy::encode_get({"live", "gone", "never"}),
                       resp));
  sy::ReadResult out;
  CHECK(sy::decode_get_resp(resp, out));
  CHECK(out.current_as_of == 900 && out.now == 1000);
  CHECK(out.reads.size() == 3);
  if (out.reads.size() != 3) return;

  CHECK(out.reads[0].key == "live");
  CHECK(out.reads[0].status == sy::Read::Status::Live);
  CHECK(out.reads[0].value == "blue");
  CHECK((out.reads[0].version == sy::Hlc{10, 1}));

  CHECK(out.reads[1].status == sy::Read::Status::Deleted);
  CHECK(out.reads[1].value.empty());
  CHECK(out.reads[1].version.wall == 20);

  CHECK(out.reads[2].key == "never");
  CHECK(out.reads[2].status == sy::Read::Status::Missing);
}

void malformed() {
  sy::Store s;
  std::string resp;
  CHECK(!sy::answer_get(s, 0, "", resp));
  CHECK(!sy::answer_get(s, 0, std::string("\xff\xff\xff\xff", 4), resp));
  CHECK(!sy::answer_get(s, 0, sy::encode_get({"k"}) + "x", resp));

  sy::ReadResult out;
  CHECK(!sy::decode_get_resp("", out));
}

}  // namespace

int main() {
  live_deleted_and_missing();
  malformed();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
