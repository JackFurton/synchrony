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

// Replaces the file atomically, so a crash leaves either the old state or the
// new one.
//
// Deliberately no fsync. It runs on the event loop just before a stamped
// message goes out, and a fsync landing inside a clock sync's round trip makes
// one leg look longer than the other, which Cristian's algorithm cannot tell
// from real asymmetry: it moved node3's estimate by 6.8ms in the VM test. So
// this survives the process dying, which is what restarts are, but not the
// machine. After a reboot the restored offset still puts the clock right; the
// floor is the backstop that might be a few seconds stale.
bool save_state(const std::string& path, const ClockState& s);

}  // namespace sy
