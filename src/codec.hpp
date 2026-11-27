#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sy {

// Payload bodies, big endian throughout. Strings are a u32 length then bytes.

class Writer {
 public:
  void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
  void u32(std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) u8(static_cast<std::uint8_t>(v >> s));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v >> 32));
    u32(static_cast<std::uint32_t>(v));
  }
  void str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    out_ += s;
  }
  std::size_t size() const { return out_.size(); }
  std::string take() { return std::move(out_); }

 private:
  std::string out_;
};

// Every read is bounds checked, and once one fails the rest do too, so callers
// can read a whole record and check ok() once at the end.
class Reader {
 public:
  explicit Reader(const std::string& in) : in_(in) {}

  std::uint8_t u8() {
    if (!need(1)) return 0;
    return static_cast<std::uint8_t>(in_[pos_++]);
  }
  std::uint32_t u32() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
    return v;
  }
  std::uint64_t u64() {
    const std::uint64_t hi = u32();
    return (hi << 32) | u32();
  }
  std::string str() {
    const std::uint32_t n = u32();
    if (!need(n)) return {};
    std::string s = in_.substr(pos_, n);
    pos_ += n;
    return s;
  }

  // A count is only believable if that many records could fit in what is
  // left, which stops a hostile n from reserving gigabytes.
  bool plausible(std::uint32_t n, std::size_t min_record) const {
    return ok_ && n <= (in_.size() - pos_) / min_record;
  }

  bool ok() const { return ok_; }
  bool done() const { return ok_ && pos_ == in_.size(); }

 private:
  bool need(std::size_t n) {
    if (!ok_ || in_.size() - pos_ < n) ok_ = false;
    return ok_;
  }

  const std::string& in_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace sy
