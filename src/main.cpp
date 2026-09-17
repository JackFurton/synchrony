#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "net.hpp"
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

  int upstream_fd = -1;
  if (!cfg.upstream_host.empty()) {
    upstream_fd = dial_upstream(cfg, 30);
    if (upstream_fd < 0) {
      std::fprintf(stderr, "upstream %s:%u unreachable\n",
                   cfg.upstream_host.c_str(), cfg.upstream_port);
      return 1;
    }
    log(cfg, "upstream connected: " + cfg.upstream_host + ":" +
                 std::to_string(cfg.upstream_port));

    const auto frame = sy::encode_frame("hello path=" + cfg.id);
    sy::write_all(upstream_fd, frame.data(), frame.size());
  } else {
    log(cfg, "no upstream, acting as source");
  }

  std::map<int, std::vector<std::uint8_t>> peers;  // fd -> unconsumed bytes

  for (;;) {
    std::vector<pollfd> pfds;
    pfds.push_back({listen_fd, POLLIN, 0});
    for (const auto& [fd, _] : peers) pfds.push_back({fd, POLLIN, 0});

    if (::poll(pfds.data(), pfds.size(), -1) < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
      return 1;
    }

    if (pfds[0].revents & POLLIN) {
      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd >= 0) {
        peers.emplace(fd, std::vector<std::uint8_t>{});
        log(cfg, "downstream connected");
      }
    }

    for (std::size_t i = 1; i < pfds.size(); ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const int fd = pfds[i].fd;

      std::uint8_t chunk[4096];
      const ssize_t n = ::read(fd, chunk, sizeof(chunk));
      if (n <= 0) {
        log(cfg, "downstream closed");
        ::close(fd);
        peers.erase(fd);
        continue;
      }

      auto& buf = peers[fd];
      buf.insert(buf.end(), chunk, chunk + n);

      std::string payload;
      while (sy::decode_frame(buf, payload)) {
        log(cfg, "recv: " + payload);
        if (upstream_fd >= 0) {
          const auto frame = sy::encode_frame(payload + ">" + cfg.id);
          sy::write_all(upstream_fd, frame.data(), frame.size());
          log(cfg, "forwarded upstream");
        } else {
          log(cfg, "delivered at source: " + payload);
        }
      }
    }
  }
}
