#pragma once

#include <cstdint>
#include <string>

namespace sy {

// What a node needs to carry its clock across a restart.
//
// offset_ns is the last Cristian estimate. Raw clocks drift by parts per
// million, so a restored offset is good for far longer than any restart takes,
// and the corrected clock is right before the first sync rather than hundreds
// of milliseconds out.
//
// hlc_floor is above every HLC wall time the previous run sent anywhere.
struct ClockState {
  std::int64_t offset_ns = 0;
  std::int64_t hlc_floor = 0;
};

// False if there is no state yet, or it is unreadable.
bool load_state(const std::string& path, ClockState& out);

// Replaces the file atomically and fsyncs it, so a crash leaves either the
// old state or the new one.
bool save_state(const std::string& path, const ClockState& s);

}  // namespace sy
