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

#include "net.hpp"
#include "peer.hpp"
#include "wire.hpp"

namespace {

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
  int upstream_fd = -1;
  std::uint32_t next_seq = 0;

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
    log(cfg, "no upstream, acting as source");
  }

  const auto drop = [&](int fd, const char* why) {
    log(cfg, std::string("dropping peer: ") + why);
    ::close(fd);
    peers.erase(fd);
    if (fd == upstream_fd) upstream_fd = -1;
  };

  for (;;) {
    std::vector<pollfd> pfds;
    pfds.push_back({listen_fd, POLLIN, 0});
    for (auto& [fd, peer] : peers) {
      pfds.push_back({fd, static_cast<short>(POLLIN | (peer.wants_write()
                                                           ? POLLOUT
                                                           : 0)),
                      0});
    }

    if (::poll(pfds.data(), pfds.size(), -1) < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 1;
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
      const auto it = peers.find(fd);
      if (it == peers.end()) continue;
      sy::Peer& peer = it->second;

      if (pfds[i].revents & POLLOUT) {
        if (!peer.flush()) {
          drop(fd, "write failed");
          continue;
        }
      }

      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

      if (!peer.fill()) {
        drop(fd, "closed");
        continue;
      }

      bool fatal = false;
      for (;;) {
        sy::Message msg;
        const sy::Decoded status = sy::decode(peer.inbound(), msg);
        if (status == sy::Decoded::Incomplete) break;
        if (status == sy::Decoded::Malformed) {
          fatal = true;
          break;
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
        sy::Peer& up = peers.at(upstream_fd);
        if (!up.queue(sy::encode(msg))) {
          log(cfg, "upstream buffer full, dropped " + describe(msg));
          continue;
        }
        if (!up.flush()) {
          drop(upstream_fd, "write failed");
          break;
        }
      }

      if (fatal) drop(fd, "malformed frame");
    }
  }
}
