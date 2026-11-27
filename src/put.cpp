#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net.hpp"
#include "wire.hpp"

namespace {

bool write_all(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

// Hands writes to any node, which relays them up to source like any other
// message. The HLC is left at zero: this process is not part of the chain's
// clock domain, and source stamps the write when it applies it.
int main(int argc, char** argv) {
  const auto usage = [] {
    std::fprintf(stderr,
                 "usage: synchrony-put HOST:PORT {KEY=VALUE | -d KEY}...\n");
    return 2;
  };
  if (argc < 3) return usage();

  const std::string hostport = argv[1];
  const auto colon = hostport.rfind(':');
  if (colon == std::string::npos) return usage();

  std::vector<std::uint8_t> out;
  for (int i = 2; i < argc; ++i) {
    sy::Message m;
    m.origin = "put";
    m.seq = static_cast<std::uint32_t>(i - 2);

    const std::string arg = argv[i];
    if (arg == "-d") {
      if (++i == argc) return usage();
      m.type = sy::MsgType::Delete;
      m.payload = argv[i];
      if (m.payload.empty() || m.payload.find('=') != std::string::npos) {
        return usage();
      }
    } else {
      const auto eq = arg.find('=');
      if (eq == 0 || eq == std::string::npos) return usage();
      m.type = sy::MsgType::Put;
      m.payload = arg;
    }
    const auto frame = sy::encode(m);
    out.insert(out.end(), frame.begin(), frame.end());
  }

  const int fd = sy::dial(hostport.substr(0, colon),
                          static_cast<std::uint16_t>(
                              std::atoi(hostport.c_str() + colon + 1)));
  if (fd < 0) {
    std::fprintf(stderr, "dial %s: %s\n", hostport.c_str(),
                 std::strerror(errno));
    return 1;
  }
  if (!write_all(fd, out)) {
    std::fprintf(stderr, "write: %s\n", std::strerror(errno));
    return 1;
  }
  ::close(fd);
  return 0;
}
