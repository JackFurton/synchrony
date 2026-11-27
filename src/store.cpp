#include "store.hpp"

namespace sy {
namespace {

// FNV-1a with a splitmix64 finish, so the top bits that pick a leaf are as
// well mixed as the bottom ones. Not cryptographic: the nodes trust each other,
// and the cost of a collision is a difference that stays hidden until something
// else under that branch changes.
class Hasher {
 public:
  void bytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
      h_ ^= p[i];
      h_ *= 0x100000001b3ULL;
    }
  }

  void u64(std::uint64_t v) {
    std::uint8_t b[8];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<std::uint8_t>(v >> (56 - 8 * i));
    }
    bytes(b, sizeof(b));
  }

  // Length first, so ("ab", "c") and ("a", "bc") hash differently.
  void str(const std::string& s) {
    u64(s.size());
    bytes(s.data(), s.size());
  }

  std::uint64_t done() const {
    std::uint64_t x = h_;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

 private:
  std::uint64_t h_ = 0xcbf29ce484222325ULL;
};

std::uint64_t combine(const std::vector<std::uint64_t>& below,
                      std::uint32_t parent) {
  Hasher h;
  for (std::uint32_t c = parent * Store::kFanout;
       c < (parent + 1) * Store::kFanout; ++c) {
    h.u64(below[c]);
  }
  return h.done();
}

}  // namespace

std::uint32_t Store::width(int level) {
  std::uint32_t w = 1;
  for (int i = 0; i < level; ++i) w *= kFanout;
  return w;
}

std::uint32_t Store::leaf_of(const std::string& key) {
  Hasher h;
  h.str(key);
  static_assert(kLeaves == 1u << 12);
  return static_cast<std::uint32_t>(h.done() >> 52);
}

Store::Store() {
  levels_[kDepth].assign(kLeaves, Hasher().done());
  for (int level = kDepth - 1; level >= 0; --level) {
    const auto l = static_cast<std::size_t>(level);
    levels_[l].resize(width(level));
    for (std::uint32_t i = 0; i < width(level); ++i) {
      levels_[l][i] = combine(levels_[l + 1], i);
    }
  }
}

bool Store::apply(const std::string& key, const Entry& e) {
  const std::uint32_t leaf = leaf_of(key);
  auto [it, inserted] = entries_.try_emplace({leaf, key}, e);
  if (!inserted) {
    if (!(e.version > it->second.version)) return false;
    if (it->second.tombstone) --tombstones_;
    it->second = e;
  }
  if (e.tombstone) ++tombstones_;
  rehash_from(leaf);
  return true;
}

std::size_t Store::collect(std::int64_t t) {
  if (tombstones_ == 0) return 0;

  std::size_t n = 0;
  std::vector<std::uint32_t> touched;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.tombstone && it->second.applied.wall < t) {
      const std::uint32_t leaf = it->first.first;
      if (touched.empty() || touched.back() != leaf) touched.push_back(leaf);
      it = entries_.erase(it);
      --tombstones_;
      ++n;
    } else {
      ++it;
    }
  }
  for (const std::uint32_t leaf : touched) rehash_from(leaf);
  return n;
}

void Store::clear() {
  const Horizon h = horizon_;
  *this = Store();
  horizon_ = h;
}

const Entry* Store::get(const std::string& key) const {
  const auto it = entries_.find({leaf_of(key), key});
  return it == entries_.end() ? nullptr : &it->second;
}

std::uint64_t Store::hash(int level, std::uint32_t index) const {
  return levels_[static_cast<std::size_t>(level)][index];
}

std::vector<std::pair<std::string, Entry>> Store::leaf(
    std::uint32_t index) const {
  std::vector<std::pair<std::string, Entry>> out;
  for (auto it = entries_.lower_bound({index, std::string()});
       it != entries_.end() && it->first.first == index; ++it) {
    out.emplace_back(it->first.second, it->second);
  }
  return out;
}

void Store::rehash_from(std::uint32_t leaf) {
  Hasher h;
  for (const auto& [key, e] : this->leaf(leaf)) {
    h.str(key);
    h.str(e.value);
    h.u64(static_cast<std::uint64_t>(e.version.wall));
    h.u64(e.version.logical);
    h.u64(static_cast<std::uint64_t>(e.applied.wall));
    h.u64(e.applied.logical);
    h.u64(e.tombstone ? 1 : 0);
  }
  levels_[kDepth][leaf] = h.done();

  std::uint32_t index = leaf;
  for (int level = kDepth - 1; level >= 0; --level) {
    index /= kFanout;
    const auto l = static_cast<std::size_t>(level);
    levels_[l][index] = combine(levels_[l + 1], index);
  }
}

}  // namespace sy
