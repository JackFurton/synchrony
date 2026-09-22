#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sy {

// Frame layout, all integers big endian:
//
//   offset  size  field
//   0       4     length of everything after this field
//   4       1     version
//   5       1     type
//   6       1     hop count
//   7       1     origin length
//   8       4     sequence number
//   12      n     origin id
//   12+n    rest  payload
//
// Length is capped so a corrupt or hostile header cannot make us allocate the
// heap away before we have read a single byte of the body.
constexpr std::uint8_t kVersion = 1;
constexpr std::uint32_t kMaxFrame = 1u << 20;

// A message that has been round the chain more times than there are nodes is a
// routing loop, not a slow path.
constexpr std::uint8_t kMaxHops = 16;

enum class MsgType : std::uint8_t {
  Hello = 1,
  Data = 2,
  TimeReq = 3,
  TimeResp = 4,
};

struct Message {
  MsgType type = MsgType::Data;
  std::uint8_t hops = 0;
  std::uint32_t seq = 0;
  std::string origin;
  std::string payload;
};

std::vector<std::uint8_t> encode(const Message& msg);

enum class Decoded {
  Ok,
  Incomplete,   // read more and call again
  Malformed,    // unrecoverable: the stream is desynchronised, drop the peer
};

// On Ok the frame is consumed from buf. On Incomplete buf is left untouched.
Decoded decode(std::vector<std::uint8_t>& buf, Message& out);

const char* to_string(MsgType type);

// Payload bodies for the time messages. Kept out of the header so the header
// stays the same size for every message type.
std::string encode_i64(std::int64_t v);
bool decode_i64(const std::string& payload, std::int64_t& out);

}  // namespace sy
