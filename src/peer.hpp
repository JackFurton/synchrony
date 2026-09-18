#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sy {

// Outbound bytes are buffered per peer so that one stalled link cannot stop the
// poll loop serving the others. The cap is a placeholder: issue #7 replaces it
// with a queue that survives the link being down.
constexpr std::size_t kMaxOutbound = 1u << 20;

class Peer {
 public:
  explicit Peer(int fd) : fd_(fd) {}

  int fd() const { return fd_; }
  bool wants_write() const { return out_pos_ < out_.size(); }
  std::vector<std::uint8_t>& inbound() { return in_; }
  std::size_t dropped() const { return dropped_; }

  // False means the buffer is full and these bytes were dropped. Dropping the
  // newest keeps the older, closer-to-delivery messages rather than the ones
  // most likely to be resent.
  bool queue(const std::vector<std::uint8_t>& bytes);

  bool flush();  // false once the peer is gone
  bool fill();   // false on EOF or a read error

 private:
  int fd_;
  std::vector<std::uint8_t> in_;
  std::vector<std::uint8_t> out_;
  std::size_t out_pos_ = 0;  // how much of out_ has already reached the socket
  std::size_t dropped_ = 0;
};

}  // namespace sy
