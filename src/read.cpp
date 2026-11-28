#include "read.hpp"

#include <utility>

#include "codec.hpp"

namespace sy {

std::string encode_get(const std::vector<std::string>& keys) {
  Writer w;
  w.u32(static_cast<std::uint32_t>(keys.size()));
  for (const auto& k : keys) w.str(k);
  return w.take();
}

bool answer_get(const Store& store, std::int64_t now, const std::string& req,
                std::string& resp) {
  Reader r(req);
  const std::uint32_t n = r.u32();
  if (!r.plausible(n, 4)) return false;

  Writer w;
  w.u64(static_cast<std::uint64_t>(store.horizon().current_as_of));
  w.u64(static_cast<std::uint64_t>(now));
  w.u32(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::string key = r.str();
    if (!r.ok()) return false;

    const Entry* e = store.get(key);
    const Read::Status status = e == nullptr ? Read::Status::Missing
                                : e->tombstone ? Read::Status::Deleted
                                               : Read::Status::Live;
    w.str(key);
    w.u8(static_cast<std::uint8_t>(status));
    w.str(status == Read::Status::Live ? e->value : std::string());
    w.u64(static_cast<std::uint64_t>(e ? e->version.wall : 0));
    w.u32(e ? e->version.logical : 0);
  }
  if (!r.done()) return false;
  resp = w.take();
  return true;
}

bool decode_get_resp(const std::string& resp, ReadResult& out) {
  Reader r(resp);
  out.current_as_of = static_cast<std::int64_t>(r.u64());
  out.now = static_cast<std::int64_t>(r.u64());
  const std::uint32_t n = r.u32();
  if (!r.plausible(n, 4 + 1 + 4 + 12)) return false;

  out.reads.clear();
  for (std::uint32_t i = 0; i < n; ++i) {
    Read rd;
    rd.key = r.str();
    const std::uint8_t status = r.u8();
    rd.value = r.str();
    rd.version.wall = static_cast<std::int64_t>(r.u64());
    rd.version.logical = r.u32();
    if (!r.ok() || status > 2) return false;
    rd.status = static_cast<Read::Status>(status);
    out.reads.push_back(std::move(rd));
  }
  return r.done();
}

}  // namespace sy
