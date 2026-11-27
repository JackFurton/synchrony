#include "outbox.hpp"

#include <algorithm>

namespace sy {

bool Outbox::add(const std::string& key, const Entry& e) {
  const auto it = queued_.find(key);
  if (it == queued_.end()) {
    if (queued_.size() >= max_keys_) return false;
    queued_.emplace(key, Queued{e});
    return true;
  }
  if (e.version > it->second.entry.version) it->second = Queued{e};
  return true;
}

std::vector<std::string> Outbox::settle(const Store& store) {
  std::vector<std::string> landed;
  for (auto it = queued_.begin(); it != queued_.end();) {
    const Entry* have = store.get(it->first);
    if (have != nullptr && !(have->version < it->second.entry.version)) {
      landed.push_back(it->first);
      it = queued_.erase(it);
    } else {
      ++it;
    }
  }
  return landed;
}

std::vector<std::string> Outbox::expire(std::int64_t t) {
  std::vector<std::string> gone;
  for (auto it = queued_.begin(); it != queued_.end();) {
    if (it->second.entry.version.wall < t) {
      gone.push_back(it->first);
      it = queued_.erase(it);
    } else {
      ++it;
    }
  }
  return gone;
}

std::vector<std::pair<std::string, Entry>> Outbox::due(std::int64_t now,
                                                       std::int64_t interval) {
  std::vector<std::pair<std::string, Entry>> out;
  for (auto& [key, q] : queued_) {
    if (q.sent && now - q.sent_at < interval) continue;
    q.sent = true;
    q.sent_at = now;
    out.emplace_back(key, q.entry);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.second.version < b.second.version;
  });
  return out;
}

}  // namespace sy
