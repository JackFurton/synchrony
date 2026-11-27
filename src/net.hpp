#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sy {

// All return -1 on failure and leave the reason in errno.
int listen_on(std::uint16_t port);
int dial(const std::string& host, std::uint16_t port);
int set_nonblocking(int fd);

// Starts a non-blocking connect. Wait for the fd to be writable, then
// connect_error says how it went: 0 for connected, otherwise an errno.
int dial_async(const std::string& host, std::uint16_t port);
int connect_error(int fd);

}  // namespace sy
