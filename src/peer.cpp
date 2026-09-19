#include "peer.hpp"

#include <unistd.h>

#include <cerrno>

namespace sy {

bool Peer::queue(const std::vector<std::uint8_t>& bytes) {
  if (out_.size() - out_pos_ + bytes.size() > kMaxOutbound) {
    ++dropped_;
    return false;
  }
  out_.insert(out_.end(), bytes.begin(), bytes.end());
  return true;
}

bool Peer::flush() {
  while (out_pos_ < out_.size()) {
    const ssize_t n =
        ::write(fd_, out_.data() + out_pos_, out_.size() - out_pos_);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
    out_pos_ += static_cast<std::size_t>(n);
  }
  out_.clear();
  out_pos_ = 0;
  return true;
}

bool Peer::fill() {
  std::uint8_t chunk[4096];
  for (;;) {
    const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
    if (n > 0) {
      in_.insert(in_.end(), chunk, chunk + n);
      continue;
    }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    return false;
  }
}

}  // namespace sy
