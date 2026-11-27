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
  Hlc version;  // source's HLC when it applied the write

  bool operator==(const Entry&) const = default;
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
};

}  // namespace sy
