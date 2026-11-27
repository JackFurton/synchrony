#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "anti_entropy.hpp"
#include "clock.hpp"
#include "hlc.hpp"
#include "net.hpp"
#include "outbox.hpp"
#include "peer.hpp"
#include "read.hpp"
#include "store.hpp"
#include "wire.hpp"

namespace {

constexpr std::int64_t kSyncIntervalNs = 500'000'000;
constexpr std::int64_t kSyncTimeoutNs = 4'000'000'000;
constexpr std::int64_t kWalkIntervalNs = 1'000'000'000;
constexpr std::int64_t kRedialNs = 1'000'000'000;
constexpr std::int64_t kConnectTimeoutNs = 3'000'000'000;
// Long enough for a write to reach source and come back down three hops of
// once-a-second walks, so a resend means something was actually lost.
constexpr std::int64_t kResendNs = 5'000'000'000;
constexpr std::size_t kOutboxKeys = 10'000;
constexpr std::int64_t kSecondNs = 1'000'000'000;
constexpr int kPollTimeoutMs = 100;

struct Config {
  std::string id = "node";
  std::uint16_t listen_port = 9000;
  std::string upstream_host;
  std::uint16_t upstream_port = 0;
  // How long source keeps a tombstone. Also the longest a write can sit in an
  // outbox and still be accepted, and the longest a replica can go without
  // catching up before it has to start over.
  std::int64_t tombstone_ttl_s = 600;
};

void log(const Config& cfg, const std::string& msg) {
  std::printf("[%s] %s\n", cfg.id.c_str(), msg.c_str());
  std::fflush(stdout);
}

std::string describe(const sy::Message& m) {
  return std::string(sy::to_string(m.type)) + " origin=" + m.origin +
         " hops=" + std::to_string(m.hops) + " seq=" + std::to_string(m.seq) +
         " hlc=" + sy::to_string(m.hlc) + " payload=" + m.payload;
}

bool parse_args(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--id" && has_value) {
      cfg.id = argv[++i];
    } else if (arg == "--listen" && has_value) {
      cfg.listen_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--tombstone-ttl" && has_value) {
      cfg.tombstone_ttl_s = std::atoll(argv[++i]);
      if (cfg.tombstone_ttl_s <= 0) return false;
    } else if (arg == "--upstream" && has_value) {
      const std::string hostport = argv[++i];
      const auto colon = hostport.rfind(':');
      if (colon == std::string::npos) return false;
      cfg.upstream_host = hostport.substr(0, colon);
      cfg.upstream_port =
          static_cast<std::uint16_t>(std::atoi(hostport.c_str() + colon + 1));
    } else {
      return false;
    }
  }
  return true;
}

struct Pending {
  bool active = false;
  std::uint32_t seq = 0;
  std::int64_t t1 = 0;
};

struct Walking {
  bool active = false;
  std::uint32_t seq = 0;  // shared by every request in one walk
  std::int64_t started = 0;
  sy::Walk walk;
};

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    std::fprintf(stderr,
                 "usage: synchrony-node --id NAME --listen PORT "
                 "[--upstream HOST:PORT] [--tombstone-ttl SECONDS]\n");
    return 2;
  }

  const int listen_fd = sy::listen_on(cfg.listen_port);
  if (listen_fd < 0) {
    std::fprintf(stderr, "listen on %u: %s\n", cfg.listen_port,
                 std::strerror(errno));
    return 1;
  }
  log(cfg, "listening on port " + std::to_string(cfg.listen_port));

  std::map<int, sy::Peer> peers;
  sy::Clock clock;
  sy::HlcClock hlc;
  sy::Store store;
  sy::Outbox outbox(kOutboxKeys);
  Pending pending;
  Walking walking;
  // Being source is configuration, not the absence of a live upstream: a
  // node that has lost its upstream is partitioned, not promoted.
  const bool is_source = cfg.upstream_host.empty();
  int upstream_fd = -1;
  int connecting_fd = -1;
  std::int64_t connect_started = 0;
  std::int64_t next_dial = 0;
  std::vector<sy::Message> held;  // relayed while upstream was down
  std::size_t held_bytes = 0;
  std::uint32_t next_seq = 0;
  std::int64_t last_sync = 0;
  std::int64_t last_walk = 0;

  if (is_source) {
    log(cfg, "no upstream, acting as source; clock is the reference");
  }

  const auto drop = [&](int fd, const char* why) {
    log(cfg, std::string("dropping peer: ") + why);
    ::close(fd);
    peers.erase(fd);
    if (fd == upstream_fd) {
      upstream_fd = -1;
      pending.active = false;
      walking.active = false;
      next_dial = sy::now_ns() + kRedialNs;
    }
  };

  const auto send_to = [&](int fd, const sy::Message& msg) {
    const auto it = peers.find(fd);
    if (it == peers.end()) return;
    if (!it->second.queue(sy::encode(msg))) {
      log(cfg, "buffer full, dropped " + describe(msg));
      return;
    }
    if (!it->second.flush()) drop(fd, "write failed");
  };

  // False if sending lost the peer, so the caller stops reading from it.
  const auto reply = [&](int fd, sy::MsgType type, std::uint32_t seq,
                         std::string payload) {
    sy::Message resp;
    resp.type = type;
    resp.origin = cfg.id;
    resp.seq = seq;  // echoed, so the asker can match it
    resp.payload = std::move(payload);
    resp.hlc = hlc.tick(clock.now());
    send_to(fd, resp);
    return peers.find(fd) != peers.end();
  };

  // False if sending lost upstream, which is usually the link the caller is
  // in the middle of reading.
  const auto send_walk = [&](const sy::Walk::Request& r) {
    sy::Message req;
    req.type = r.type;
    req.origin = cfg.id;
    req.seq = walking.seq;
    req.payload = r.payload;
    req.hlc = hlc.tick(clock.now());
    const int fd = upstream_fd;
    send_to(fd, req);
    return peers.find(fd) != peers.end();
  };

  // The version was fixed when this node accepted the write, and goes out as
  // the header HLC so source applies it at that version, however late.
  const auto send_write = [&](const std::string& key, const sy::Entry& e) {
    sy::Message m;
    m.type = e.tombstone ? sy::MsgType::Delete : sy::MsgType::Put;
    m.origin = cfg.id;
    m.seq = next_seq++;
    m.hlc = e.version;
    m.payload = e.tombstone ? key : key + "=" + e.value;
    send_to(upstream_fd, m);
  };

  const auto flush_outbox = [&](std::int64_t interval) {
    if (upstream_fd < 0) return;
    for (const auto& [key, e] : outbox.due(sy::now_ns(), interval)) {
      send_write(key, e);
    }
  };

  const auto upstream_connected = [&](int fd) {
    upstream_fd = fd;
    peers.emplace(fd, sy::Peer(fd));
    log(cfg, "upstream connected: " + cfg.upstream_host + ":" +
                 std::to_string(cfg.upstream_port) +
                 " outbox=" + std::to_string(outbox.size()));

    sy::Message hello;
    hello.type = sy::MsgType::Hello;
    hello.origin = cfg.id;
    hello.seq = next_seq++;
    hello.payload = "hello";
    hello.hlc = hlc.tick(clock.now());
    send_to(fd, hello);

    if (!held.empty()) {
      log(cfg, "forwarding " + std::to_string(held.size()) +
                   " held while upstream was down");
    }
    for (const auto& m : held) send_to(fd, m);
    held.clear();
    held_bytes = 0;

    // Whatever was in flight on the old link may or may not have made it, and
    // resending costs nothing: source keeps the newer version, which a copy
    // of the same write is not.
    flush_outbox(0);
  };

  for (;;) {
    if (!is_source && upstream_fd < 0 && connecting_fd < 0 &&
        sy::now_ns() >= next_dial) {
      connecting_fd = sy::dial_async(cfg.upstream_host, cfg.upstream_port);
      connect_started = sy::now_ns();
      if (connecting_fd < 0) next_dial = connect_started + kRedialNs;
    }

    std::vector<pollfd> pfds;
    pfds.push_back({listen_fd, POLLIN, 0});
    for (auto& [fd, peer] : peers) {
      pfds.push_back(
          {fd,
           static_cast<short>(POLLIN | (peer.wants_write() ? POLLOUT : 0)), 0});
    }
    const std::size_t peer_end = pfds.size();
    if (connecting_fd >= 0) pfds.push_back({connecting_fd, POLLOUT, 0});

    if (::poll(pfds.data(), pfds.size(), kPollTimeoutMs) < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 1;
    }

    const std::int64_t raw_now = sy::now_ns();

    if (is_source) {
      sy::Horizon& h = store.horizon();
      h.current_as_of = clock.now();
      h.collect_before = h.current_as_of - cfg.tombstone_ttl_s * kSecondNs;
      if (const std::size_t n = store.collect(h.collect_before); n > 0) {
        log(cfg, "collected " + std::to_string(n) + " tombstones");
      }
    }

    if (connecting_fd >= 0) {
      const int fd = connecting_fd;
      if (pfds.back().revents & (POLLOUT | POLLERR | POLLHUP)) {
        connecting_fd = -1;
        const int err = sy::connect_error(fd);
        if (err == 0) {
          upstream_connected(fd);
        } else {
          log(cfg, std::string("upstream dial failed: ") + std::strerror(err));
          ::close(fd);
          next_dial = raw_now + kRedialNs;
        }
      } else if (raw_now - connect_started > kConnectTimeoutNs) {
        // A host that is down entirely never answers the SYN, and the kernel
        // would wait minutes before saying so.
        log(cfg, "upstream dial timed out");
        connecting_fd = -1;
        ::close(fd);
        next_dial = raw_now;
      }
    }

    flush_outbox(kResendNs);

    if (pending.active && raw_now - pending.t1 > kSyncTimeoutNs) {
      log(cfg, "sync timed out, will retry");
      pending.active = false;
    }

    if (upstream_fd >= 0 && !pending.active &&
        raw_now - last_sync >= kSyncIntervalNs) {
      sy::Message req;
      req.type = sy::MsgType::TimeReq;
      req.origin = cfg.id;
      req.seq = next_seq++;
      req.hlc = hlc.tick(clock.now());

      pending = {true, req.seq, sy::now_ns()};
      last_sync = raw_now;
      send_to(upstream_fd, req);
    }

    if (walking.active && raw_now - walking.started > kSyncTimeoutNs) {
      log(cfg, "anti-entropy timed out, will retry");
      walking.active = false;
    }

    if (upstream_fd >= 0 && !walking.active &&
        raw_now - last_walk >= kWalkIntervalNs) {
      walking = {true, next_seq++, raw_now, sy::Walk{}};
      last_walk = raw_now;
      send_walk(walking.walk.start());
    }

    if (pfds[0].revents & POLLIN) {
      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd >= 0) {
        sy::set_nonblocking(fd);
        peers.emplace(fd, sy::Peer(fd));
        log(cfg, "downstream connected");
      }
    }

    for (std::size_t i = 1; i < peer_end; ++i) {
      const int fd = pfds[i].fd;
      auto it = peers.find(fd);
      if (it == peers.end()) continue;

      if (pfds[i].revents & POLLOUT) {
        if (!it->second.flush()) {
          drop(fd, "write failed");
          continue;
        }
      }

      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

      // A peer that writes and then closes hands over both in the same read,
      // so whatever arrived before the close is decoded before acting on it.
      const bool open = it->second.fill();

      bool fatal = false;
      bool gone = false;
      for (;;) {
        sy::Message msg;
        const sy::Decoded status = sy::decode(it->second.inbound(), msg);
        if (status == sy::Decoded::Incomplete) break;
        if (status == sy::Decoded::Malformed) {
          fatal = true;
          break;
        }

        hlc.merge(msg.hlc, clock.now());

        if (msg.type == sy::MsgType::TimeReq) {
          if (!reply(fd, sy::MsgType::TimeResp, msg.seq,
                     sy::encode_i64(clock.now()))) {
            gone = true;
            break;
          }
          continue;
        }

        if (msg.type == sy::MsgType::Get) {
          std::string body;
          if (!sy::answer_get(store, clock.now(), msg.payload, body)) {
            fatal = true;
            break;
          }
          if (!reply(fd, sy::MsgType::GetResp, msg.seq, std::move(body))) {
            gone = true;
            break;
          }
          continue;
        }
        if (msg.type == sy::MsgType::GetResp) {
          log(cfg, "unexpected get_resp from a peer, ignoring");
          continue;
        }

        if (msg.type == sy::MsgType::TreeReq ||
            msg.type == sy::MsgType::LeafReq) {
          const bool tree = msg.type == sy::MsgType::TreeReq;
          std::string body;
          if (!(tree ? sy::answer_tree(store, msg.payload, body)
                     : sy::answer_leaves(store, msg.payload, body))) {
            fatal = true;
            break;
          }
          if (!reply(fd, tree ? sy::MsgType::TreeResp : sy::MsgType::LeafResp,
                     msg.seq, std::move(body))) {
            gone = true;
            break;
          }
          continue;
        }

        if (msg.type == sy::MsgType::TreeResp ||
            msg.type == sy::MsgType::LeafResp) {
          if (!walking.active || fd != upstream_fd ||
              msg.seq != walking.seq) {
            log(cfg, std::string("unmatched ") + sy::to_string(msg.type) +
                         " seq=" + std::to_string(msg.seq));
            continue;
          }

          sy::Walk::Request next;
          std::vector<std::string> applied;
          const sy::Walk::Step step =
              msg.type == sy::MsgType::TreeResp
                  ? walking.walk.on_tree(store, msg.payload, next)
                  : walking.walk.on_leaves(store, msg.payload, applied);

          // lag is how long the write took to get here from the node that
          // accepted it, read off this node's corrected clock against that
          // node's stamp, so it is only as good as the clock sync on both.
          for (const auto& key : applied) {
            const sy::Entry* e = store.get(key);
            log(cfg, "applied key=" + key +
                         " version=" + sy::to_string(e->version) +
                         " lag_us=" +
                         std::to_string((clock.now() - e->version.wall) / 1000) +
                         (e->tombstone ? " deleted" : ""));
          }

          if (step == sy::Walk::Step::Malformed) {
            fatal = true;
            break;
          }
          if (step == sy::Walk::Step::Next) {
            if (!send_walk(next)) {
              gone = true;
              break;
            }
            continue;
          }

          if (!applied.empty() && outbox.size() > 0) {
            for (const auto& key : outbox.settle(store)) {
              log(cfg, "landed key=" + key +
                           " outbox=" + std::to_string(outbox.size()));
            }
          }
          for (const auto& key :
               outbox.expire(store.horizon().collect_before)) {
            log(cfg, "expired key=" + key + " outbox=" +
                         std::to_string(outbox.size()));
          }

          walking.active = false;
          const auto& st = walking.walk.stats();
          if (st.wiped) {
            log(cfg, "upstream collected tombstones from before this replica "
                     "last caught up, refilling from scratch");
          }
          if (st.collected > 0) {
            log(cfg, "collected " + std::to_string(st.collected) +
                         " tombstones");
          }
          if (st.round_trips > 1) {
            log(cfg, "anti-entropy upstream=" + msg.origin +
                         " round_trips=" + std::to_string(st.round_trips) +
                         " hashes=" + std::to_string(st.hashes) +
                         " entries=" + std::to_string(st.entries) +
                         " applied=" + std::to_string(st.applied) +
                         " bytes=" + std::to_string(st.bytes) +
                         " tombstones=" +
                         std::to_string(store.size() - store.live()) +
                         " keys=" + std::to_string(store.live()));
          }
          continue;
        }

        if (msg.type == sy::MsgType::TimeResp) {
          const std::int64_t t4 = sy::now_ns();
          std::int64_t upstream_time = 0;
          if (!pending.active || msg.seq != pending.seq) {
            log(cfg, "unmatched time_resp seq=" + std::to_string(msg.seq));
            continue;
          }
          if (!sy::decode_i64(msg.payload, upstream_time)) {
            fatal = true;
            break;
          }

          const std::int64_t rtt = t4 - pending.t1;
          const std::int64_t offset =
              sy::cristian_offset(pending.t1, upstream_time, t4);
          clock.set_offset_ns(offset);
          pending.active = false;

          // A lower offset moves this node's clock backwards. Measure how far,
          // then take the next timestamp across the step to show it held.
          const std::int64_t pt = clock.now();
          const std::int64_t stepped_back =
              std::max<std::int64_t>(0, hlc.last().wall - pt);
          const sy::Hlc stamp = hlc.tick(pt);

          log(cfg, "sync upstream=" + msg.origin +
                       " rtt_us=" + std::to_string(rtt / 1000) +
                       " offset_us=" + std::to_string(offset / 1000) +
                       " hlc=" + sy::to_string(stamp) +
                       " stepped_back_us=" +
                       std::to_string(stepped_back / 1000));
          continue;
        }

        log(cfg, "recv " + describe(msg));

        if (msg.type == sy::MsgType::Put || msg.type == sy::MsgType::Delete) {
          const bool del = msg.type == sy::MsgType::Delete;
          const auto eq = msg.payload.find('=');
          if (del ? (msg.payload.empty() || eq != std::string::npos)
                  : (eq == 0 || eq == std::string::npos)) {
            log(cfg, "malformed write, dropping " + describe(msg));
            continue;
          }
          const std::string key = del ? msg.payload : msg.payload.substr(0, eq);

          // Nothing in the chain has stamped it yet, so it came straight from
          // a client and this node is the one that accepted it.
          const bool from_client = msg.hlc == sy::Hlc{};
          sy::Entry e;
          e.value = del ? std::string() : msg.payload.substr(eq + 1);
          e.version = from_client ? hlc.tick(clock.now()) : msg.hlc;
          e.tombstone = del;

          // Only source applies writes. Versions come from whichever node
          // accepted each write, so a write that sat out an outage still
          // loses to anything written after it, however late it arrives.
          if (is_source) {
            const char* verb = del ? "delete" : "put";
            // A tombstone that would have stopped this may already be gone.
            if (e.version.wall < store.horizon().collect_before) {
              log(cfg, std::string(verb) + " key=" + key +
                           " version=" + sy::to_string(e.version) +
                           " origin=" + msg.origin + " expired");
              continue;
            }
            e.applied = hlc.tick(clock.now());
            const bool kept = store.apply(key, e);
            log(cfg, std::string(verb) + " key=" + key +
                         " version=" + sy::to_string(e.version) +
                         " origin=" + msg.origin +
                         " hops=" + std::to_string(msg.hops) +
                         (kept ? "" : " stale"));
            continue;
          }

          if (from_client) {
            if (!outbox.add(key, e)) {
              log(cfg, "outbox full, refusing write key=" + key);
              continue;
            }
            log(cfg, "accepted key=" + key +
                         " version=" + sy::to_string(e.version) +
                         " outbox=" + std::to_string(outbox.size()));
            flush_outbox(kResendNs);
            continue;
          }
        }

        if (is_source) {
          log(cfg, "delivered at source: " + describe(msg));
          continue;
        }
        if (msg.hops >= sy::kMaxHops) {
          log(cfg, "hop limit reached, dropping " + describe(msg));
          continue;
        }
        ++msg.hops;

        // Best effort only. A write's origin keeps it until it lands and will
        // send it again, so dropping one here costs a resend, not the write.
        if (upstream_fd < 0) {
          const std::size_t n = msg.origin.size() + msg.payload.size();
          if (held_bytes + n > sy::kMaxOutbound) {
            log(cfg, "upstream down and hold full, dropping " + describe(msg));
          } else {
            held_bytes += n;
            held.push_back(std::move(msg));
          }
          continue;
        }
        send_to(upstream_fd, msg);
      }

      if (gone) continue;
      if (fatal) {
        drop(fd, "malformed frame");
      } else if (!open) {
        drop(fd, "closed");
      }
    }
  }
}
