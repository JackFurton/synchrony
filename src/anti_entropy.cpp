#include "anti_entropy.hpp"

#include <utility>

namespace sy {
namespace {

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

bool read_indices(Reader& r, std::uint32_t limit,
                  std::vector<std::uint32_t>& out) {
  const std::uint32_t n = r.u32();
  if (!r.plausible(n, 4)) return false;
  out.reserve(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t index = r.u32();
    if (index >= limit) return false;
    out.push_back(index);
  }
  return r.done();
}

}  // namespace

bool answer_tree(const Store& store, const std::string& req,
                 std::string& resp) {
  Reader r(req);
  const int level = r.u8();
  std::vector<std::uint32_t> indices;
  if (level > Store::kDepth ||
      !read_indices(r, Store::width(level), indices)) {
    return false;
  }

  Writer w;
  w.u8(static_cast<std::uint8_t>(level));
  w.u32(static_cast<std::uint32_t>(indices.size()));
  for (const std::uint32_t i : indices) {
    w.u32(i);
    w.u64(store.hash(level, i));
  }
  resp = w.take();
  return true;
}

bool answer_leaves(const Store& store, const std::string& req,
                   std::string& resp) {
  Reader r(req);
  std::vector<std::uint32_t> leaves;
  if (!read_indices(r, Store::kLeaves, leaves)) return false;

  Writer body;
  std::uint32_t n = 0;
  for (const std::uint32_t leaf : leaves) {
    if (n > 0 && body.size() > kLeafBudget) break;
    for (const auto& [key, e] : store.leaf(leaf)) {
      body.str(key);
      body.str(e.value);
      body.u64(static_cast<std::uint64_t>(e.version.wall));
      body.u32(e.version.logical);
      ++n;
    }
  }

  Writer w;
  w.u32(n);
  resp = w.take() + body.take();
  return true;
}

Walk::Request Walk::tree_req(int level,
                             const std::vector<std::uint32_t>& indices) {
  level_ = level;
  requested_ = indices;

  Writer w;
  w.u8(static_cast<std::uint8_t>(level));
  w.u32(static_cast<std::uint32_t>(indices.size()));
  for (const std::uint32_t i : indices) w.u32(i);

  Request req{MsgType::TreeReq, w.take()};
  stats_.bytes += req.payload.size();
  return req;
}

Walk::Request Walk::start() { return tree_req(0, {0}); }

Walk::Step Walk::on_tree(const Store& local, const std::string& resp,
                         Request& next) {
  ++stats_.round_trips;
  stats_.bytes += resp.size();

  Reader r(resp);
  if (r.u8() != level_ || r.u32() != requested_.size()) return Step::Malformed;

  std::vector<std::uint32_t> differ;
  for (const std::uint32_t want : requested_) {
    const std::uint32_t index = r.u32();
    const std::uint64_t hash = r.u64();
    if (!r.ok() || index != want) return Step::Malformed;
    ++stats_.hashes;
    if (hash != local.hash(level_, index)) differ.push_back(index);
  }
  if (!r.done()) return Step::Malformed;

  if (differ.empty()) return Step::Done;

  if (level_ == Store::kDepth) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(differ.size()));
    for (const std::uint32_t leaf : differ) w.u32(leaf);
    next = {MsgType::LeafReq, w.take()};
    stats_.bytes += next.payload.size();
    return Step::Next;
  }

  std::vector<std::uint32_t> children;
  children.reserve(differ.size() * Store::kFanout);
  for (const std::uint32_t parent : differ) {
    for (std::uint32_t c = 0; c < Store::kFanout; ++c) {
      children.push_back(parent * Store::kFanout + c);
    }
  }
  next = tree_req(level_ + 1, children);
  return Step::Next;
}

Walk::Step Walk::on_leaves(Store& local, const std::string& resp,
                           std::vector<std::string>& applied) {
  ++stats_.round_trips;
  stats_.bytes += resp.size();

  Reader r(resp);
  const std::uint32_t n = r.u32();
  if (!r.plausible(n, 4 + 4 + 8 + 4)) return Step::Malformed;

  for (std::uint32_t i = 0; i < n; ++i) {
    std::string key = r.str();
    Entry e;
    e.value = r.str();
    e.version.wall = static_cast<std::int64_t>(r.u64());
    e.version.logical = r.u32();
    if (!r.ok()) return Step::Malformed;

    ++stats_.entries;
    if (local.apply(key, e)) {
      ++stats_.applied;
      applied.push_back(std::move(key));
    }
  }
  return r.done() ? Step::Done : Step::Malformed;
}

}  // namespace sy
