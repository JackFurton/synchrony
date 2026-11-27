#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hlc.hpp"

namespace sy {

struct Entry {
  std::string value;
  Hlc version;     // HLC of the node that accepted the write
  Hlc applied;     // source's HLC when it applied it
  bool tombstone = false;

  bool operator==(const Entry&) const = default;
};

// How far this replica can be trusted about deletes, in source's time.
//
// Tombstones applied before collect_before have been thrown away. A replica
// that was last equal to its upstream before that may have held a value whose
// tombstone came and went without it seeing either, and nothing it can
// receive will ever tell it so. It has to start again.
struct Horizon {
  std::int64_t collect_before = 0;
  std::int64_t current_as_of = 0;  // last fully equal to upstream
};

// A key-value store with a fixed-shape Merkle tree over it.
//
// Keys are hashed into kLeaves buckets, and every node of the tree hashes the
// level below it, so two stores with the same root hold the same entries, and
// two that differ can find out where by walking down only the branches whose
// hashes disagree. The shape never changes with the data, which is what lets
// two nodes compare a subtree by index without agreeing on anything first.
class Store {
 public:
  static constexpr int kDepth = 3;
  static constexpr std::uint32_t kFanout = 16;
  static constexpr std::uint32_t kLeaves = 16 * 16 * 16;

  Store();

  // Keeps whichever version is newer. True if e replaced what was there.
  bool apply(const std::string& key, const Entry& e);

  const Entry* get(const std::string& key) const;
  std::size_t size() const { return entries_.size(); }
  std::size_t live() const { return entries_.size() - tombstones_; }

  // Drops tombstones source applied before t. Returns how many.
  std::size_t collect(std::int64_t t);
  void clear();

  const Horizon& horizon() const { return horizon_; }
  Horizon& horizon() { return horizon_; }

  // Level 0 is the root, level kDepth the leaves.
  static std::uint32_t width(int level);
  std::uint64_t hash(int level, std::uint32_t index) const;
  std::uint64_t root() const { return hash(0, 0); }

  static std::uint32_t leaf_of(const std::string& key);
  std::vector<std::pair<std::string, Entry>> leaf(std::uint32_t index) const;

 private:
  void rehash_from(std::uint32_t leaf);

  // Ordered by leaf first, so a leaf's entries are one contiguous range.
  std::map<std::pair<std::uint32_t, std::string>, Entry> entries_;
  std::array<std::vector<std::uint64_t>, kDepth + 1> levels_;
  std::size_t tombstones_ = 0;
  Horizon horizon_;
};

}  // namespace sy
