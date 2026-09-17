#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sy {

// Frame: 4-byte big-endian length, then that many bytes of payload.
// Length is capped so a corrupt header can't make us allocate the heap away.
constexpr std::uint32_t kMaxFrame = 1u << 20;

std::vector<std::uint8_t> encode_frame(const std::string& payload);

// Pulls one frame off the front of buf. Returns false if buf holds less than a
// whole frame; the caller reads more and tries again.
bool decode_frame(std::vector<std::uint8_t>& buf, std::string& out);

}  // namespace sy
