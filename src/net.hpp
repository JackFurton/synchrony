#pragma once

#include <string>

namespace sy {

// Both return -1 on failure and leave the reason in errno.
int listen_on(std::uint16_t port);
int dial(const std::string& host, std::uint16_t port);

bool write_all(int fd, const void* data, std::size_t len);

}  // namespace sy
