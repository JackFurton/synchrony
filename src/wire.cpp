#include "wire.hpp"

#include <cstring>

namespace sy {
namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 24));
  out.push_back(static_cast<std::uint8_t>(v >> 16));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

std::uint32_t get_u32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

constexpr std::size_t kHeader = 8;  // everything after the length field

bool known_type(std::uint8_t t) {
  return t >= static_cast<std::uint8_t>(MsgType::Hello) &&
         t <= static_cast<std::uint8_t>(MsgType::TimeResp);
}

}  // namespace

std::vector<std::uint8_t> encode(const Message& msg) {
  const std::size_t body = kHeader + msg.origin.size() + msg.payload.size();

  std::vector<std::uint8_t> out;
  out.reserve(4 + body);
  put_u32(out, static_cast<std::uint32_t>(body));
  out.push_back(kVersion);
  out.push_back(static_cast<std::uint8_t>(msg.type));
  out.push_back(msg.hops);
  out.push_back(static_cast<std::uint8_t>(msg.origin.size()));
  put_u32(out, msg.seq);
  out.insert(out.end(), msg.origin.begin(), msg.origin.end());
  out.insert(out.end(), msg.payload.begin(), msg.payload.end());
  return out;
}

Decoded decode(std::vector<std::uint8_t>& buf, Message& out) {
  if (buf.size() < 4) return Decoded::Incomplete;

  const std::uint32_t body = get_u32(buf.data());
  if (body > kMaxFrame || body < kHeader) return Decoded::Malformed;
  if (buf.size() < 4 + body) return Decoded::Incomplete;

  const std::uint8_t* p = buf.data() + 4;
  if (p[0] != kVersion) return Decoded::Malformed;
  if (!known_type(p[1])) return Decoded::Malformed;

  const std::size_t origin_len = p[3];
  if (kHeader + origin_len > body) return Decoded::Malformed;

  out.type = static_cast<MsgType>(p[1]);
  out.hops = p[2];
  out.seq = get_u32(p + 4);
  out.origin.assign(reinterpret_cast<const char*>(p + kHeader), origin_len);
  out.payload.assign(
      reinterpret_cast<const char*>(p + kHeader + origin_len),
      body - kHeader - origin_len);

  buf.erase(buf.begin(), buf.begin() + 4 + body);
  return Decoded::Ok;
}

std::string encode_i64(std::int64_t v) {
  const auto u = static_cast<std::uint64_t>(v);
  std::string out(8, '\0');
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i)] =
        static_cast<char>((u >> (56 - 8 * i)) & 0xFF);
  }
  return out;
}

bool decode_i64(const std::string& payload, std::int64_t& out) {
  if (payload.size() != 8) return false;
  std::uint64_t u = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    u = (u << 8) | static_cast<std::uint8_t>(payload[i]);
  }
  out = static_cast<std::int64_t>(u);
  return true;
}

const char* to_string(MsgType type) {
  switch (type) {
    case MsgType::Hello: return "hello";
    case MsgType::Data: return "data";
    case MsgType::TimeReq: return "time_req";
    case MsgType::TimeResp: return "time_resp";
  }
  return "unknown";
}

}  // namespace sy
