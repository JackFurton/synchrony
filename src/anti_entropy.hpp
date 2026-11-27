#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "store.hpp"
#include "wire.hpp"

namespace sy {

// Merkle anti-entropy against the upstream neighbour, and only that neighbour.
//
// The downstream walks the upstream's tree from the root, one level per round
// trip, asking only for the children of nodes whose hashes disagree with its
// own. At the bottom it asks for the entries in the leaves that still disagree.
// Cost is proportional to how much differs, not to how much is stored.
//
// Payloads, all integers big endian:
//
//   TreeReq   u8 level, u32 n, n x u32 index
//   TreeResp  u8 level, u32 n, n x (u32 index, u64 hash)
//   LeafReq   u32 n, n x u32 leaf
//   LeafResp  u32 n, n x (u32 len, key, u32 len, value, i64 wall, u32 logical)

// A leaf response stops adding leaves once it passes this, well under
// kMaxFrame. Whatever it left out still differs, so the next walk picks it up.
constexpr std::size_t kLeafBudget = 256 * 1024;

// Upstream side. Stateless: each request carries everything needed to answer
// it. False means the request was malformed.
bool answer_tree(const Store& store, const std::string& req, std::string& resp);
bool answer_leaves(const Store& store, const std::string& req,
                   std::string& resp);

// Downstream side: one walk from the root to whichever leaves disagree.
class Walk {
 public:
  struct Request {
    MsgType type;
    std::string payload;
  };

  enum class Step { Next, Done, Malformed };

  struct Stats {
    int round_trips = 0;
    std::size_t hashes = 0;   // compared against the local tree
    std::size_t entries = 0;  // received in leaf responses
    std::size_t applied = 0;  // of those, newer than what was here
    std::size_t bytes = 0;    // payload in both directions
  };

  Request start();

  // On Next, next holds the request to send.
  Step on_tree(const Store& local, const std::string& resp, Request& next);

  // Always the last step. Keys that were applied are appended to applied.
  Step on_leaves(Store& local, const std::string& resp,
                 std::vector<std::string>& applied);

  const Stats& stats() const { return stats_; }

 private:
  Request tree_req(int level, const std::vector<std::uint32_t>& indices);

  int level_ = 0;
  std::vector<std::uint32_t> requested_;
  Stats stats_;
};

}  // namespace sy
