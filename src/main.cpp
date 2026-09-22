#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "clock.hpp"
#include "net.hpp"
#include "peer.hpp"
#include "wire.hpp"

namespace {

constexpr std::int64_t kSyncIntervalNs = 500'000'000;
constexpr std::int64_t kSyncTimeoutNs = 4'000'000'000;
constexpr int kPollTimeoutMs = 100;

struct Config {
  std::string id = "node";
  std::uint16_t listen_port = 9000;
  std::string upstream_host;
  std::uint16_t upstream_port = 0;
};

void log(const Config& cfg, const std::string& msg) {
  std::printf("[%s] %s\n", cfg.id.c_str(), msg.c_str());
  std::fflush(stdout);
}

std::string describe(const sy::Message& m) {
  return std::string(sy::to_string(m.type)) + " origin=" + m.origin +
         " hops=" + std::to_string(m.hops) + " seq=" + std::to_string(m.seq) +
         " payload=" + m.payload;
}

bool parse_args(int argc, char** argv, Config& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--id" && has_value) {
      cfg.id = argv[++i];
    } else if (arg == "--listen" && has_value) {
      cfg.listen_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
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

// Upstream may boot after us, so a refused connect is expected, not fatal.
int dial_upstream(const Config& cfg, int attempts) {
  for (int i = 0; i < attempts; ++i) {
    const int fd = sy::dial(cfg.upstream_host, cfg.upstream_port);
    if (fd >= 0) return fd;
    ::sleep(1);
  }
  return -1;
}

struct Pending {
  bool active = false;
  std::uint32_t seq = 0;
  std::int64_t t1 = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    std::fprintf(stderr,
                 "usage: synchrony-node --id NAME --listen PORT "
                 "[--upstream HOST:PORT]\n");
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
  Pending pending;
  int upstream_fd = -1;
  std::uint32_t next_seq = 0;
  std::int64_t last_sync = 0;

  if (!cfg.upstream_host.empty()) {
    upstream_fd = dial_upstream(cfg, 30);
    if (upstream_fd < 0) {
      std::fprintf(stderr, "upstream %s:%u unreachable\n",
                   cfg.upstream_host.c_str(), cfg.upstream_port);
      return 1;
    }
    sy::set_nonblocking(upstream_fd);
    peers.emplace(upstream_fd, sy::Peer(upstream_fd));
    log(cfg, "upstream connected: " + cfg.upstream_host + ":" +
                 std::to_string(cfg.upstream_port));

    sy::Message hello;
    hello.type = sy::MsgType::Hello;
    hello.origin = cfg.id;
    hello.seq = next_seq++;
    hello.payload = "hello";
    peers.at(upstream_fd).queue(sy::encode(hello));
  } else {
    log(cfg, "no upstream, acting as source; clock is the reference");
  }

  const auto drop = [&](int fd, const char* why) {
    log(cfg, std::string("dropping peer: ") + why);
    ::close(fd);
    peers.erase(fd);
    if (fd == upstream_fd) {
      upstream_fd = -1;
      pending.active = false;
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

  for (;;) {
    std::vector<pollfd> pfds;
    pfds.push_back({listen_fd, POLLIN, 0});
    for (auto& [fd, peer] : peers) {
      pfds.push_back(
          {fd,
           static_cast<short>(POLLIN | (peer.wants_write() ? POLLOUT : 0)), 0});
    }

    if (::poll(pfds.data(), pfds.size(), kPollTimeoutMs) < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 1;
    }

    const std::int64_t raw_now = sy::now_ns();

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

      pending = {true, req.seq, sy::now_ns()};
      last_sync = raw_now;
      send_to(upstream_fd, req);
    }

    if (pfds[0].revents & POLLIN) {
      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd >= 0) {
        sy::set_nonblocking(fd);
        peers.emplace(fd, sy::Peer(fd));
        log(cfg, "downstream connected");
      }
    }

    for (std::size_t i = 1; i < pfds.size(); ++i) {
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

      if (!it->second.fill()) {
        drop(fd, "closed");
        continue;
      }

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

        if (msg.type == sy::MsgType::TimeReq) {
          sy::Message resp;
          resp.type = sy::MsgType::TimeResp;
          resp.origin = cfg.id;
          resp.seq = msg.seq;  // echoed, so the asker can match it
          resp.payload = sy::encode_i64(clock.now());
          send_to(fd, resp);
          if (peers.find(fd) == peers.end()) {
            gone = true;
            break;
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

          log(cfg, "sync upstream=" + msg.origin +
                       " rtt_us=" + std::to_string(rtt / 1000) +
                       " offset_us=" + std::to_string(offset / 1000));
          continue;
        }

        log(cfg, "recv " + describe(msg));

        if (upstream_fd < 0) {
          log(cfg, "delivered at source: " + describe(msg));
          continue;
        }
        if (msg.hops >= sy::kMaxHops) {
          log(cfg, "hop limit reached, dropping " + describe(msg));
          continue;
        }

        ++msg.hops;
        send_to(upstream_fd, msg);
      }

      if (gone) continue;
      if (fatal) drop(fd, "malformed frame");
    }
  }
}
