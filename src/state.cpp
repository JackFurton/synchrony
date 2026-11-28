#include "state.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>

namespace sy {

bool load_state(const std::string& path, ClockState& out) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (f == nullptr) return false;
  ClockState s;
  const int n = std::fscanf(f, "%" SCNd64 " %" SCNd64, &s.offset_ns,
                            &s.hlc_floor);
  std::fclose(f);
  if (n != 2) return false;
  out = s;
  return true;
}

bool save_state(const std::string& path, const ClockState& s) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;

  char buf[64];
  const int len = std::snprintf(buf, sizeof(buf), "%" PRId64 " %" PRId64 "\n",
                                s.offset_ns, s.hlc_floor);
  if (::write(fd, buf, static_cast<std::size_t>(len)) != len ||
      ::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) return false;

  // The rename is only durable once the directory entry is.
  const auto slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  const int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

}  // namespace sy
