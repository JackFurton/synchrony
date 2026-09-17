#include "wire.hpp"

#include <stdexcept>

namespace sy {

std::vector<std::uint8_t> encode_frame(const std::string& payload) {
  if (payload.size() > kMaxFrame) throw std::length_error("frame too large");
  const auto n = static_cast<std::uint32_t>(payload.size());
  std::vector<std::uint8_t> out;
  out.reserve(4 + payload.size());
  out.push_back(static_cast<std::uint8_t>(n >> 24));
  out.push_back(static_cast<std::uint8_t>(n >> 16));
  out.push_back(static_cast<std::uint8_t>(n >> 8));
  out.push_back(static_cast<std::uint8_t>(n));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool decode_frame(std::vector<std::uint8_t>& buf, std::string& out) {
  if (buf.size() < 4) return false;
  const std::uint32_t n = (static_cast<std::uint32_t>(buf[0]) << 24) |
                          (static_cast<std::uint32_t>(buf[1]) << 16) |
                          (static_cast<std::uint32_t>(buf[2]) << 8) |
                          static_cast<std::uint32_t>(buf[3]);
  if (n > kMaxFrame) throw std::length_error("frame too large");
  if (buf.size() < 4 + n) return false;
  out.assign(reinterpret_cast<const char*>(buf.data() + 4), n);
  buf.erase(buf.begin(), buf.begin() + 4 + n);
  return true;
}

}  // namespace sy
