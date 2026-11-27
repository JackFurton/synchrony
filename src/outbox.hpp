#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "store.hpp"

namespace sy {

// Writes this node accepted from a client and has not yet seen come back.
//
// A write leaves with its version already fixed by this node's HLC, and source
// applies it at that version. So the write has landed once anti-entropy brings
// this node the same version or a newer one, and the store itself is the ack.
// No intermediate node ever has to be trusted with a write: if node2 dies
// holding one, it is still here, and gets sent again.
//
// Only the latest write to each key is kept, since an older one would lose to
// it at source anyway. That bounds the outbox by how many keys were touched
// during an outage, not by how many writes were made.
class Outbox {
 public:
  explicit Outbox(std::size_t max_keys) : max_keys_(max_keys) {}

  // False if this is a new key and the outbox is already at max_keys. The new
  // key is refused rather than an old one evicted: either way a write is lost,
  // but this way it is the one being offered now, which the caller can log
  // against the client that sent it, not one accepted minutes ago.
  bool add(const std::string& key, const Entry& e);

  // Removes every write the store now holds at the same version or newer, and
  // returns their keys.
  std::vector<std::string> settle(const Store& store);

  // Removes writes made before t, which source would refuse, and returns
  // their keys.
  std::vector<std::string> expire(std::int64_t t);

  // Writes not sent within the last interval, oldest version first, marked as
  // sent at now. An interval of 0 returns everything.
  std::vector<std::pair<std::string, Entry>> due(std::int64_t now,
                                                 std::int64_t interval);

  std::size_t size() const { return queued_.size(); }

 private:
  struct Queued {
    Entry entry;
    std::int64_t sent_at = 0;
    bool sent = false;
  };

  std::size_t max_keys_;
  std::map<std::string, Queued> queued_;
};

}  // namespace sy
