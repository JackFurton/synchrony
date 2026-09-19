#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sy {

// All return -1 on failure and leave the reason in errno.
int listen_on(std::uint16_t port);
int dial(const std::string& host, std::uint16_t port);
int set_nonblocking(int fd);

}  // namespace sy
