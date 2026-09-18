#include "wire.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

sy::Message sample() {
  sy::Message m;
  m.type = sy::MsgType::Hello;
  m.hops = 2;
  m.seq = 70000;  // wider than 16 bits, so a truncated seq field shows up
  m.origin = "node3";
  m.payload = "hello";
  return m;
}

void round_trip() {
  const sy::Message in = sample();
  auto buf = sy::encode(in);

  sy::Message out;
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.type == in.type);
  CHECK(out.hops == in.hops);
  CHECK(out.seq == in.seq);
  CHECK(out.origin == in.origin);
  CHECK(out.payload == in.payload);
  CHECK(buf.empty());
}

void empty_fields() {
  sy::Message in;
  in.type = sy::MsgType::Data;
  auto buf = sy::encode(in);

  sy::Message out;
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.origin.empty());
  CHECK(out.payload.empty());
}

void max_origin() {
  sy::Message in = sample();
  in.origin = std::string(255, 'x');
  auto buf = sy::encode(in);

  sy::Message out;
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.origin.size() == 255);
  CHECK(out.payload == in.payload);
}

// The case that matters in practice: TCP hands us the frame in arbitrary
// pieces, and every prefix short of the last byte must be Incomplete without
// consuming anything.
void split_reads() {
  const auto whole = sy::encode(sample());

  std::vector<std::uint8_t> buf;
  sy::Message out;
  for (std::size_t i = 0; i + 1 < whole.size(); ++i) {
    buf.push_back(whole[i]);
    const auto before = buf.size();
    CHECK(sy::decode(buf, out) == sy::Decoded::Incomplete);
    CHECK(buf.size() == before);
  }

  buf.push_back(whole.back());
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.origin == "node3");
  CHECK(buf.empty());
}

void two_frames_in_one_read() {
  sy::Message first = sample();
  first.payload = "one";
  sy::Message second = sample();
  second.payload = "two";

  auto buf = sy::encode(first);
  const auto more = sy::encode(second);
  buf.insert(buf.end(), more.begin(), more.end());

  sy::Message out;
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.payload == "one");
  CHECK(sy::decode(buf, out) == sy::Decoded::Ok);
  CHECK(out.payload == "two");
  CHECK(buf.empty());
  CHECK(sy::decode(buf, out) == sy::Decoded::Incomplete);
}

void length_header_that_lies() {
  sy::Message out;

  // Longer than the cap: must be rejected before anything is allocated.
  std::vector<std::uint8_t> huge = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(sy::decode(huge, out) == sy::Decoded::Malformed);

  // Shorter than the fixed header.
  std::vector<std::uint8_t> tiny = {0, 0, 0, 3, 1, 1, 0, 0};
  CHECK(sy::decode(tiny, out) == sy::Decoded::Malformed);

  // Origin length runs past the end of the body.
  auto buf = sy::encode(sample());
  buf[7] = 0xFF;
  CHECK(sy::decode(buf, out) == sy::Decoded::Malformed);
}

void bad_version_and_type() {
  sy::Message out;

  auto wrong_version = sy::encode(sample());
  wrong_version[4] = sy::kVersion + 1;
  CHECK(sy::decode(wrong_version, out) == sy::Decoded::Malformed);

  auto wrong_type = sy::encode(sample());
  wrong_type[5] = 0x7F;
  CHECK(sy::decode(wrong_type, out) == sy::Decoded::Malformed);
}

void empty_buffer() {
  std::vector<std::uint8_t> buf;
  sy::Message out;
  CHECK(sy::decode(buf, out) == sy::Decoded::Incomplete);
}

}  // namespace

int main() {
  round_trip();
  empty_fields();
  max_origin();
  split_reads();
  two_frames_in_one_read();
  length_header_that_lies();
  bad_version_and_type();
  empty_buffer();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
