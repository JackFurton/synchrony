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
  if (::write(fd, buf, static_cast<std::size_t>(len)) != len) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return false;
  }
  ::close(fd);
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace sy
