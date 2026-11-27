#include "anti_entropy.hpp"

#include <algorithm>
#include <utility>

#include "codec.hpp"

namespace sy {
namespace {

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
  if (level == 0) {
    w.u64(static_cast<std::uint64_t>(store.horizon().collect_before));
    w.u64(static_cast<std::uint64_t>(store.horizon().current_as_of));
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
    const auto entries = store.leaf(leaf);
    body.u32(leaf);
    body.u64(store.hash(Store::kDepth, leaf));
    body.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& [key, e] : entries) {
      body.str(key);
      body.str(e.value);
      body.u64(static_cast<std::uint64_t>(e.version.wall));
      body.u32(e.version.logical);
      body.u64(static_cast<std::uint64_t>(e.applied.wall));
      body.u32(e.applied.logical);
      body.u8(e.tombstone ? 1 : 0);
    }
    ++n;
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

Walk::Step Walk::on_tree(Store& local, const std::string& resp,
                         Request& next) {
  ++stats_.round_trips;
  stats_.bytes += resp.size();

  Reader r(resp);
  if (r.u8() != level_ || r.u32() != requested_.size()) return Step::Malformed;

  std::vector<std::pair<std::uint32_t, std::uint64_t>> theirs;
  for (const std::uint32_t want : requested_) {
    const std::uint32_t index = r.u32();
    const std::uint64_t hash = r.u64();
    if (!r.ok() || index != want) return Step::Malformed;
    theirs.emplace_back(index, hash);
  }
  if (level_ == 0) {
    upstream_.collect_before = static_cast<std::int64_t>(r.u64());
    upstream_.current_as_of = static_cast<std::int64_t>(r.u64());
  }
  if (!r.done()) return Step::Malformed;

  if (level_ == 0) {
    Horizon& h = local.horizon();
    if (upstream_.collect_before > h.current_as_of) {
      if (local.size() > 0) {
        local.clear();
        stats_.wiped = true;
      }
      // Empty now, so it can hold nothing older than upstream does.
      h.current_as_of = upstream_.current_as_of;
    }
    stats_.collected = local.collect(upstream_.collect_before);
    h.collect_before = std::max(h.collect_before, upstream_.collect_before);
  }

  std::vector<std::uint32_t> differ;
  for (const auto& [index, hash] : theirs) {
    ++stats_.hashes;
    if (hash != local.hash(level_, index)) differ.push_back(index);
  }

  if (differ.empty()) {
    if (level_ == 0) caught_up(local);
    return Step::Done;
  }

  if (level_ == Store::kDepth) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(differ.size()));
    for (const std::uint32_t leaf : differ) w.u32(leaf);
    requested_ = differ;
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
  if (n > requested_.size()) return Step::Malformed;

  // Equal to upstream as of this walk only if every leaf asked for came back
  // and now hashes the same here. Short of that, the response was cut at the
  // budget or this replica holds something upstream does not.
  bool equal = n == requested_.size();
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t leaf = r.u32();
    const std::uint64_t hash = r.u64();
    const std::uint32_t m = r.u32();
    if (!r.plausible(m, 4 + 4 + 12 + 12 + 1) || leaf != requested_[i]) {
      return Step::Malformed;
    }

    for (std::uint32_t j = 0; j < m; ++j) {
      std::string key = r.str();
      Entry e;
      e.value = r.str();
      e.version.wall = static_cast<std::int64_t>(r.u64());
      e.version.logical = r.u32();
      e.applied.wall = static_cast<std::int64_t>(r.u64());
      e.applied.logical = r.u32();
      const std::uint8_t tombstone = r.u8();
      if (!r.ok() || tombstone > 1 || Store::leaf_of(key) != leaf) {
        return Step::Malformed;
      }
      e.tombstone = tombstone == 1;

      ++stats_.entries;
      if (local.apply(key, e)) {
        ++stats_.applied;
        applied.push_back(std::move(key));
      }
    }
    equal = equal && local.hash(Store::kDepth, leaf) == hash;
  }
  if (!r.done()) return Step::Malformed;

  if (equal) caught_up(local);
  return Step::Done;
}

void Walk::caught_up(Store& local) const {
  Horizon& h = local.horizon();
  h.current_as_of = std::max(h.current_as_of, upstream_.current_as_of);
}

}  // namespace sy
