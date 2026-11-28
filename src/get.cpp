#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net.hpp"
#include "read.hpp"
#include "wire.hpp"

// Prints one line per key: key=value, or the key and why there is no value.
// The last line goes to stderr and says how stale the replica might be.
int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: synchrony-get HOST:PORT KEY...\n");
    return 2;
  }
  const std::string hostport = argv[1];
  const auto colon = hostport.rfind(':');
  if (colon == std::string::npos) {
    std::fprintf(stderr, "usage: synchrony-get HOST:PORT KEY...\n");
    return 2;
  }

  const int fd = sy::dial(hostport.substr(0, colon),
                          static_cast<std::uint16_t>(
                              std::atoi(hostport.c_str() + colon + 1)));
  if (fd < 0) {
    std::fprintf(stderr, "dial %s: %s\n", hostport.c_str(),
                 std::strerror(errno));
    return 1;
  }
  const timeval timeout{5, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sy::Message req;
  req.type = sy::MsgType::Get;
  req.origin = "get";
  req.payload = sy::encode_get(std::vector<std::string>(argv + 2, argv + argc));
  const auto out = sy::encode(req);
  for (std::size_t done = 0; done < out.size();) {
    const ssize_t n = ::write(fd, out.data() + done, out.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      std::fprintf(stderr, "write: %s\n", std::strerror(errno));
      return 1;
    }
    done += static_cast<std::size_t>(n);
  }

  std::vector<std::uint8_t> in;
  sy::Message resp;
  for (;;) {
    const sy::Decoded d = sy::decode(in, resp);
    if (d == sy::Decoded::Ok) break;
    if (d == sy::Decoded::Malformed) {
      std::fprintf(stderr, "malformed response\n");
      return 1;
    }
    std::uint8_t chunk[4096];
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      std::fprintf(stderr, "read: %s\n",
                   n == 0 ? "closed" : std::strerror(errno));
      return 1;
    }
    in.insert(in.end(), chunk, chunk + n);
  }
  ::close(fd);

  sy::ReadResult result;
  if (resp.type != sy::MsgType::GetResp ||
      !sy::decode_get_resp(resp.payload, result)) {
    std::fprintf(stderr, "malformed response\n");
    return 1;
  }
  for (const auto& r : result.reads) {
    switch (r.status) {
      case sy::Read::Status::Live:
        std::printf("%s=%s\n", r.key.c_str(), r.value.c_str());
        break;
      case sy::Read::Status::Deleted:
        std::printf("%s deleted\n", r.key.c_str());
        break;
      case sy::Read::Status::Missing:
        std::printf("%s missing\n", r.key.c_str());
        break;
    }
  }
  std::fflush(stdout);
  std::fprintf(stderr, "from %s, current as of %lldms ago\n", resp.origin.c_str(),
               static_cast<long long>((result.now - result.current_as_of) /
                                      1'000'000));
  return 0;
}
