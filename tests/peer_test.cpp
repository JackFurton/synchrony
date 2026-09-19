#include "peer.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <vector>

#include "net.hpp"
#include "wire.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                   #cond);                                               \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

std::size_t drain(int fd) {
  std::size_t total = 0;
  std::uint8_t chunk[4096];
  for (;;) {
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) return total;
    total += static_cast<std::size_t>(n);
  }
}

void writes_reach_the_other_end() {
  int fds[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  sy::set_nonblocking(fds[0]);
  sy::set_nonblocking(fds[1]);

  sy::Message msg;
  msg.origin = "node1";
  msg.payload = "ping";

  sy::Peer peer(fds[0]);
  CHECK(peer.queue(sy::encode(msg)));
  CHECK(peer.flush());
  CHECK(!peer.wants_write());

  sy::Peer other(fds[1]);
  CHECK(other.fill());
  sy::Message got;
  CHECK(sy::decode(other.inbound(), got) == sy::Decoded::Ok);
  CHECK(got.origin == "node1");
  CHECK(got.payload == "ping");

  ::close(fds[0]);
  ::close(fds[1]);
}

// The point of the outbound buffer: a peer that never reads must not be able to
// block us. flush() keeps returning true on EAGAIN, and once the buffer is full
// queue() refuses rather than growing without bound.
void a_peer_that_never_reads_cannot_block_us() {
  int fds[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  sy::set_nonblocking(fds[0]);
  sy::set_nonblocking(fds[1]);

  sy::Peer peer(fds[0]);
  const std::vector<std::uint8_t> blob(64 * 1024, 0xAB);

  int accepted = 0;
  while (peer.queue(blob)) {
    CHECK(peer.flush());
    ++accepted;
    if (accepted > 1000) break;  // guard against an infinite loop on failure
  }

  CHECK(accepted > 0);
  CHECK(accepted <= 1000);
  CHECK(peer.dropped() == 1);
  CHECK(peer.wants_write());

  while (peer.wants_write()) {
    CHECK(drain(fds[1]) > 0);
    CHECK(peer.flush());
  }
  CHECK(peer.queue(blob));

  ::close(fds[0]);
  ::close(fds[1]);
}

void closed_peer_reports_eof() {
  int fds[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  sy::set_nonblocking(fds[0]);

  sy::Peer peer(fds[0]);
  CHECK(peer.fill());  // nothing to read yet, but still alive
  ::close(fds[1]);
  CHECK(!peer.fill());

  ::close(fds[0]);
}

}  // namespace

int main() {
  writes_reach_the_other_end();
  a_peer_that_never_reads_cannot_block_us();
  closed_peer_reports_eof();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
