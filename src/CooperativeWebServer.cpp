#include "CooperativeWebServer.h"

#if defined(ESP8266)
#include <algorithm>

void CooperativeWebServer::sendHeader(const String& name, const String& value,
                                      bool first) {
  // All application headers are appended. Their order has no HTTP semantics.
  (void)first;
  const size_t length = _responseHeaders.length() + name.length() + value.length() + 4;
  if (length > kMaximumHeaderBytes ||
      !_responseHeaders.reserve(length) || !_responseHeaders.concat(name) ||
      !_responseHeaders.concat(": ") || !_responseHeaders.concat(value) ||
      !_responseHeaders.concat("\r\n")) headerFailed_ = true;
}

bool CooperativeWebServer::prepare(int code, const char* contentType,
                                   size_t length) {
  if (pending_) { fail("reply_overlap"); return false; }
  const String reason = responseCodeToString(code);
  const String size(length);
  const size_t headerCapacity = 160 + _responseHeaders.length() +
                                (contentType ? strlen_P(contentType) : 9);
  if (_keepAlive && _server.hasClient()) _keepAlive = false;
  // A known length is required: this path deliberately never starts chunked
  // framing or lets the SDK's synchronous finalizer append to an owned reply.
  _chunked = false;
  if (headerFailed_ || reason.length() == 0 || size.length() == 0 ||
      headerCapacity > kMaximumHeaderBytes || !header_.reserve(headerCapacity) ||
      !header_.concat("HTTP/1.") || !header_.concat(String(_currentVersion)) ||
      !header_.concat(' ') || !header_.concat(String(code)) ||
      !header_.concat(' ') || !header_.concat(reason) ||
      !header_.concat("\r\nContent-Type: ") ||
      !header_.concat(contentType ? contentType : "text/html") ||
      !header_.concat("\r\nContent-Length: ") || !header_.concat(size) ||
      !header_.concat(_keepAlive ? "\r\nConnection: keep-alive\r\nKeep-Alive: timeout=2000\r\n"
                               : "\r\nConnection: close\r\n") ||
      !header_.concat(_responseHeaders) || !header_.concat("\r\n") ||
      header_.length() > kMaximumHeaderBytes) {
    fail("allocation_failed");
    return false;
  }
  _responseHeaders = String();
  headerFailed_ = false;
  return true;
}

void CooperativeWebServer::send(int code, const char* contentType,
                               const String& body) {
  if (!prepare(code, contentType, body.length())) return;
  if (_currentMethod != HTTP_HEAD && body.length() != 0 &&
      (!body_.reserve(body.length()) || !body_.concat(body))) {
    fail("allocation_failed");
    return;
  }
  pending_ = true;
  cursor_.begin(header_.length() + body_.length(), millis());
}

void CooperativeWebServer::send_P(int code, PGM_P contentType, PGM_P body,
                                 size_t length) {
  if (!prepare(code, contentType, length)) return;
  flashBody_ = reinterpret_cast<const uint8_t*>(body);
  pending_ = true;
  cursor_.begin(header_.length() + (_currentMethod == HTTP_HEAD ? 0 : length), millis());
}

void CooperativeWebServer::handleClient() {
  if (!pending_) ESP8266WebServer::handleClient();
  // Never parse a pipelined request until the preceding reply has completed.
  if (pending_) pump(millis());
}

void CooperativeWebServer::pump(uint32_t nowMs) {
  if (!_currentClient.connected()) { fail("peer_closed"); return; }
  const auto result = cursor_.pump(nowMs, kSendTimeoutMs,
      [this](size_t offset, size_t requested) {
        const int capacity = _currentClient.availableForWrite();
        if (capacity <= 0) return 0;
        size_t length = std::min(requested, static_cast<size_t>(capacity));
        const uint8_t* bytes;
        uint8_t flashChunk[SseWriteCursor::kBytesPerLoop];
        if (offset < header_.length()) {
          length = std::min(length, header_.length() - offset);
          bytes = reinterpret_cast<const uint8_t*>(header_.c_str()) + offset;
        } else {
          offset -= header_.length();
          if (flashBody_) {
            // write_P() invokes the SDK's blocking Stream sender. Copy only
            // this bounded chunk to RAM; never duplicate a whole gzip asset.
            memcpy_P(flashChunk, flashBody_ + offset, length);
            bytes = flashChunk;
          } else {
            bytes = reinterpret_cast<const uint8_t*>(body_.c_str()) + offset;
          }
        }
        _currentClient.setTimeout(0);
        return static_cast<int>(_currentClient.write(bytes, length));
      });
  if (result == SseWriteCursor::Result::Pending) return;
  if (result != SseWriteCursor::Result::Complete) {
    fail(result == SseWriteCursor::Result::TimedOut ? "send_timeout" : "write_failed");
    return;
  }
  ++completed_;
  release();
  // The SDK starts its idle close timer when the route returns, before our
  // reply drains. Start that idle interval again after completion.
  _statusChange = nowMs;
  if (!_keepAlive) {
    _currentClient.stop(0);  // Queue FIN after the bytes, without waiting for ACK.
    _currentClient = WiFiClient();
    _currentStatus = HC_NONE;
  }
}

void CooperativeWebServer::release() {
  pending_ = false;
  header_ = String();
  body_ = String();
  flashBody_ = nullptr;
  _responseHeaders = String();
  headerFailed_ = false;
}

void CooperativeWebServer::fail(const char* reason) {
  ++failed_;
  lastFailure_ = reason;
  // A partial Content-Length reply cannot be repaired by another HTTP error.
  // Abort its socket so the next request starts on a fresh connection.
  _currentClient.abort();
  _currentClient = WiFiClient();
  _currentStatus = HC_NONE;
  _keepAlive = false;
  release();
}
#endif
