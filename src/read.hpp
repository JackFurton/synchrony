#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "store.hpp"

namespace sy {

// Reads are answered from whichever replica is asked, so every answer says
// how far behind source that replica might be.
//
//   Get      u32 n, n x str key
//   GetResp  i64 current_as_of, i64 now, u32 n,
//            n x (str key, u8 status, str value, i64 wall, u32 logical)
//
// now is the answering node's corrected clock, so now - current_as_of is the
// longest the replica has gone without being equal to its upstream, read in
// source's time. Source answers with current_as_of == now.

struct Read {
  enum class Status : std::uint8_t { Missing = 0, Live = 1, Deleted = 2 };

  std::string key;
  Status status = Status::Missing;
  std::string value;
  Hlc version;
};

struct ReadResult {
  std::int64_t current_as_of = 0;
  std::int64_t now = 0;
  std::vector<Read> reads;
};

std::string encode_get(const std::vector<std::string>& keys);

// False if the request was malformed.
bool answer_get(const Store& store, std::int64_t now, const std::string& req,
                std::string& resp);

bool decode_get_resp(const std::string& resp, ReadResult& out);

}  // namespace sy
