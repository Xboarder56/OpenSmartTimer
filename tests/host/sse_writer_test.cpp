#include "SseWriteCursor.h"
#include "SocketWrite.h"
#include <cassert>
#include <iostream>
#include <string>
#include <unistd.h>
#include <fcntl.h>

int main() {
  alarm(5);  // A transport regression must fail CI, not hang it.
  using Result = SseWriteCursor::Result;
  SseWriteCursor cursor;
  const std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc\n\n\r\n";
  std::string output;
  cursor.begin(wire.size(), 100);
  for (uint32_t now = 100; cursor.active(); ++now) {
    const auto result = cursor.pump(now, 5000, [&](size_t offset, size_t length) {
      assert(length <= 512);
      const size_t count = length > 3 ? 3 : length;
      output += wire.substr(offset, count);
      return static_cast<int>(count);
    });
    assert(result == Result::Pending || result == Result::Complete);
  }
  assert(output == wire);  // Partial headers/chunks are never duplicated.
  cursor.begin(100, 0);
  assert(cursor.pump(4999, 5000, [](size_t, size_t) { return 0; }) == Result::Pending);
  assert(cursor.pump(5000, 5000, [](size_t, size_t) { assert(false); return 0; }) == Result::TimedOut);
  cursor.begin(100, UINT32_MAX - 100);
  assert(cursor.pump(4899, 5000, [](size_t, size_t) { return 1; }) == Result::TimedOut);
  cursor.begin(100, 10);
  assert(cursor.pump(10, 5000, [](size_t, size_t) { return -1; }) == Result::Failed);
  cursor.begin(100, 10);
  assert(cursor.pump(10, 5000, [](size_t, size_t n) { return static_cast<int>(n + 1); }) == Result::Failed);

  // Real socket backpressure exercises the exact nonblocking ESP32 send helper.
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  // Darwin's AF_UNIX backpressure differs from lwIP TCP; make the fixture
  // nonblocking explicitly as well as exercising the MSG_DONTWAIT helper.
  assert(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
  const std::string bytes(512, 'x');
  unsigned attempts = 0;
  while (SocketWrite::once(sockets[0], reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) > 0)
    assert(++attempts < 10000);
  cursor.begin(wire.size(), 100);
  assert(cursor.pump(101, 5000, [&](size_t offset, size_t count) {
    return SocketWrite::once(sockets[0], reinterpret_cast<const uint8_t*>(wire.data() + offset), count);
  }) == Result::Pending);
  assert(cursor.offset() == 0);
  assert(cursor.pump(5100, 5000, [](size_t, size_t) { assert(false); return 0; }) == Result::TimedOut);
  close(sockets[0]); close(sockets[1]);
  std::cout << "SSE partial-write, timeout and socket-backpressure tests passed\n";
}
