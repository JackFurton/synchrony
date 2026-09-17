#include "net.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace sy {

int listen_on(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(fd, 16) < 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

int dial(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* res = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0) {
    errno = EHOSTUNREACH;
    return -1;
  }

  int fd = -1;
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  return fd;
}

bool write_all(int fd, const void* data, std::size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  while (len > 0) {
    const ssize_t n = ::write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace sy
