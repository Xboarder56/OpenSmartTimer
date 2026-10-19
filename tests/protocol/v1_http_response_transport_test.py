"""Exercise the actual V1 sender and timer against bounded fake TCP backpressure."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = r'''
#pragma once
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>
using PGM_P = const char*;
#define memcpy_P std::memcpy
#define strlen_P std::strlen
extern uint32_t now;
inline uint32_t millis() { return now; }
struct String {
  std::string value;
  inline static bool failReserve = false;
  String() = default;
  String(const char* s) : value(s) {}
  template<typename N> explicit String(N n) : value(std::to_string(n)) {}
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  bool reserve(size_t n) { if (failReserve) return false; value.reserve(n); return true; }
  bool concat(const String& s) { if (!reserve(length()+s.length())) return false; value += s.value; return true; }
  bool concat(const char* s) { return concat(String(s)); }
  bool concat(char c) { if (!reserve(length()+1)) return false; value += c; return true; }
};
struct Socket {
  std::string wire;
  uint32_t ackDelay = 0, lastWrite = 0;
  size_t occupied = 0, capacity = 1072, maxWrite = 512;
  unsigned writes = 0;
  bool connected = true, aborted = false, stopped = false;
};
struct WiFiClient {
  std::shared_ptr<Socket> state;
  bool connected() { return state && state->connected; }
  int availableForWrite() {
    if (state->occupied && uint32_t(now-state->lastWrite) >= state->ackDelay) state->occupied = 0;
    return state->capacity-state->occupied;
  }
  void setTimeout(unsigned t) { if (t != 0) std::abort(); }
  size_t write(const uint8_t* bytes, size_t count) {
    if (++state->writes != 1 || count > 512) std::abort();
    count = std::min(count, state->maxWrite);
    state->wire.append(reinterpret_cast<const char*>(bytes), count);
    state->occupied += count; state->lastWrite = now;
    return count;
  }
  void abort() { if (state) { state->aborted = true; state->connected = false; } }
  void stop(unsigned t) { if (t != 0) std::abort(); state->stopped = true; state->connected = false; }
};
enum HTTPMethod { HTTP_GET, HTTP_HEAD };
enum HTTPClientStatus { HC_NONE, HC_WAIT_READ, HC_WAIT_CLOSE };
struct ESP8266WebServer {
  explicit ESP8266WebServer(int) {}
  unsigned parses = 0;
  std::function<void()> nextRequest;
  void attach(const std::shared_ptr<Socket>& s, bool keep = true, HTTPMethod method = HTTP_GET) {
    _currentClient.state = s; _keepAlive = keep; _currentMethod = method;
  }
  void handleClient() {
    ++parses;
    if (nextRequest) { auto handler = std::move(nextRequest); nextRequest = {}; handler(); }
    _currentStatus = HC_WAIT_CLOSE; _statusChange = now;
  }
  static String responseCodeToString(int) { return String("OK"); }
  uint32_t idleStarted() const { return _statusChange; }
 protected:
  struct Listener { bool hasClient() { return false; } } _server;
  WiFiClient _currentClient;
  HTTPMethod _currentMethod = HTTP_GET;
  HTTPClientStatus _currentStatus = HC_NONE;
  uint8_t _currentVersion = 1;
  uint32_t _statusChange = 0;
  bool _keepAlive = true, _chunked = false;
  String _responseHeaders;
};
'''
CHECKS = r'''
#include "CooperativeWebServer.h"
#include "TimerController.h"
#include "Hardware.h"
#include <array>
#include <cassert>
#include <iostream>
uint32_t now;
std::array<uint8_t, 17> pins{};
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t value) { pins.at(pin) = value; }
int digitalRead(uint8_t pin) { return pins.at(pin); }
void tick(CooperativeWebServer& server, const std::shared_ptr<Socket>& socket) {
  socket->writes = 0;
  server.handleClient();
}
int main() {
  const std::string payload(1103, 'x');
  for (auto delay : {0U, 1100U, 1500U, 3000U, 7000U}) {
    now = 0; pins.fill(LOW);
    TimerController timer; timer.begin();
    assert(timer.start(3000, 0) && timer.setSealed(true, 0));
    CooperativeWebServer server(80);
    auto socket = std::make_shared<Socket>(); socket->ackDelay = delay;
    server.attach(socket);
    String body(payload.c_str());
    server.nextRequest = [&] { server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", body); };
    tick(server, socket); body = String("mutated after send");
    assert(server.responsePending());
    server.nextRequest = [&] { server.send(200, "text/plain", "NEXT"); };
    for (now=1; server.responsePending() && now<=5000; ++now) {
      timer.tick(now); tick(server, socket);
      assert(server.parses == 1); // no next header can enter this reply
    }
    timer.tick(3000);
    assert(timer.isOpen() && pins[Hardware::kLockPinA] == LOW && pins[Hardware::kLockPinB] == LOW);
    const auto start = socket->wire.find("\r\n\r\n")+4;
    assert(socket->wire.find("Content-Length: 1103\r\n") != std::string::npos);
    if (delay < 5000) {
      assert(socket->wire.substr(start) == payload && server.responseCompletedCount() == 1);
      assert(server.idleStarted() == now-1 && !socket->aborted);
      tick(server, socket); assert(server.parses == 2);
    } else {
      assert(socket->aborted && server.responseFailedCount() == 1 && now == 5001);
      assert(std::string(server.responseLastFailure()) == "send_timeout");
      assert(socket->wire.find("NEXT") == std::string::npos);
    }
  }
  // Short writes retain every byte; flash assets are not copied into an owned
  // String, and the same cursor crosses the header/body boundary correctly.
  for (bool head : {false, true}) {
    now = 0;
    CooperativeWebServer server(80); auto socket = std::make_shared<Socket>();
    socket->maxWrite = 73; server.attach(socket, false, head ? HTTP_HEAD : HTTP_GET);
    const std::string asset(40000, '\x8b');
    server.nextRequest = [&] { server.sendHeader("Content-Encoding", "gzip"); server.send_P(200, "application/javascript", asset.data(), asset.size()); };
    do { tick(server, socket); ++now; } while (server.responsePending());
    const auto start = socket->wire.find("\r\n\r\n")+4;
    assert(socket->wire.substr(start) == (head ? "" : asset));
    assert(socket->wire.find("Content-Length: 40000\r\n") != std::string::npos);
    assert(socket->stopped && !socket->aborted && server.responseCompletedCount() == 1);
  }
  // Allocation failure emits no misleading partial header; the connection
  // fails explicitly and a subsequent request on a new connection succeeds.
  CooperativeWebServer server(80); auto socket = std::make_shared<Socket>(); server.attach(socket);
  String::failReserve = true; server.send(200, "text/plain", "body"); String::failReserve = false;
  assert(socket->aborted && socket->wire.empty() && !server.responsePending());
  socket = std::make_shared<Socket>(); server.attach(socket); server.send(200, "text/plain", "");
  while (server.responsePending()) { ++now; tick(server, socket); }
  assert(socket->wire.find("Content-Length: 0\r\n") != std::string::npos);
  // Emergency input and unsigned clock wrap remain independent of a stalled
  // response, which is aborted on peer closure without reusing that socket.
  pins.fill(LOW); TimerController timer; timer.begin();
  now = 0xfffffff0U; assert(timer.start(3000, now) && timer.setSealed(true, now));
  socket = std::make_shared<Socket>(); socket->capacity = 0; server.attach(socket);
  server.send(200, "application/json", payload.c_str());
  for (unsigned step=0; step<=500; ++step, ++now) {
    if (step == 500) timer.setEmergencyInputActive(true, now);
    timer.tick(now); tick(server, socket); assert(server.responsePending());
  }
  assert(timer.isOpen() && pins[Hardware::kLockPinA] == LOW && pins[Hardware::kLockPinB] == LOW);
  socket->connected = false; tick(server, socket);
  assert(!server.responsePending() && socket->aborted && std::string(server.responseLastFailure()) == "peer_closed");
  socket = std::make_shared<Socket>(); socket->capacity = 0; server.attach(socket);
  now = 0xfffffff0U; server.send(200, "text/plain", "wrap"); now += 5000; tick(server, socket);
  assert(socket->aborted && std::string(server.responseLastFailure()) == "send_timeout");
  std::cout << "Actual V1 response transport: delayed ACKs, partial writes, framing, flash/HEAD, allocation/peer failure, timeout/wrap, timer expiry and emergency input passed\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / 'ESP8266WebServer.h').write_text(SDK)
    (directory / 'main.cpp').write_text(CHECKS)
    binary = directory / 'response-transport'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DESP8266=1', '-I' + str(directory), '-I' + str(ROOT / 'tests/host'),
                    '-I' + str(ROOT / 'include'), str(ROOT / 'src/CooperativeWebServer.cpp'),
                    str(ROOT / 'src/TimerController.cpp'), str(directory / 'main.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
