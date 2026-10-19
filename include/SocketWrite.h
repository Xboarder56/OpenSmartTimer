#pragma once

#include <cstddef>
#include <cstdint>
#include <cerrno>
#if defined(ESP32)
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif

namespace SocketWrite {
// Shared by ESP32 and the host socket test. Never retries or waits in select.
inline int once(int fd, const uint8_t* bytes, size_t length) {
  if (fd < 0) return -1;
  const int sent = static_cast<int>(::send(fd, bytes, length, MSG_DONTWAIT));
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
  return sent;
}
}  // namespace SocketWrite
