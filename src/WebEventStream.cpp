#include "ResetTrace.h"
#include "WebApplication.h"
#include "Hardware.h"
#include "LocalNetwork.h"
#include "TimerController.h"
#if defined(ESP32)
#include "SocketWrite.h"
#endif
#if defined(SMART_TIMER_HARDWARE_V2)
#include "BluetoothApplication.h"
#endif
#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

namespace {
constexpr uint32_t kEventStreamHeaderTimeoutMs = 2000;
constexpr uint32_t kEventStreamWriteTimeoutMs = 5000;
constexpr uint16_t kEventStreamMaximumHeaderBytes = 2048;
constexpr size_t kEventStreamMaximumWireBytes = 3072;
constexpr char kHeaders[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-store\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: keep-alive\r\n"
    "X-Content-Type-Options: nosniff\r\n\r\n";

int writeWithoutWaiting(WiFiClient& client, const uint8_t* bytes, size_t length) {
#if defined(ESP8266)
  const int capacity = client.availableForWrite();
  if (capacity <= 0) return 0;
  if (length > static_cast<size_t>(capacity)) length = static_cast<size_t>(capacity);
  // No ACK wait and no retry loop under lwIP memory pressure.
  client.setTimeout(0);
  return static_cast<int>(client.write(bytes, length));
#else
  // NetworkClient inherits Print::availableForWrite() == 0. Bypass its
  // select/retry based write() as well: one MSG_DONTWAIT send is bounded even
  // when the peer has stopped reading. Never change socket-global flags.
  return SocketWrite::once(client.fd(), bytes, length);
#endif
}
}  // namespace

void WebApplication::registerEventStreamHook() {
#if defined(ESP8266)
  server_.addHook(
      [this](const String& method, const String& url, WiFiClient* client,
             ESP8266WebServer::ContentTypeFunction) {
        // Every request on V1, including assets and uploads.
        ResetTrace::request(url.c_str());
        return handleEventStreamHook(method, url, client);
      });
#else
  server_.on("/api/v1/events/stream", HTTP_GET,
             [this]() { handleEventStreamRequest(); });
#endif
}

#if defined(ESP8266)
ESP8266WebServer::ClientFuture WebApplication::handleEventStreamHook(
    const String& method, const String& url, WiFiClient* client) {
  if (url != "/api/v1/events/stream") {
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  }
  if (method != "GET") {
    rejectEventStream(*client, 405, "Method Not Allowed",
                      "{\"error\":\"GET required\",\"code\":\"method_not_allowed\"}");
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }
  if (otaQuietActive(millis())) {
    rejectEventStream(
        *client, 503, "Service Unavailable",
        "{\"error\":\"firmware update in progress\",\"code\":\"ota_in_progress\"}",
        30);
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }

  if (acceptEventStream(*client, false)) {
    return ESP8266WebServer::CLIENT_IS_GIVEN;
  }

  rejectEventStream(
      *client, 503, "Service Unavailable",
      "{\"error\":\"live-update client limit reached\",\"code\":\"sse_client_limit\"}",
      15);
  return ESP8266WebServer::CLIENT_IS_GIVEN;
}
#else
void WebApplication::handleEventStreamRequest() {
  WiFiClient client = server_.client();
  if (!requestFromAccessPoint() &&
      !HttpRequestFields::localHost(server_.header("Host").c_str())) {
    rejectEventStream(client, 403, "Forbidden",
        "{\"error\":\"open the timer by its IP address or .local name\",\"code\":\"host_rejected\"}");
    return;
  }
  if (otaQuietActive(millis())) {
    rejectEventStream(
        client, 503, "Service Unavailable",
        "{\"error\":\"firmware update in progress\",\"code\":\"ota_in_progress\"}",
        30);
    return;
  }
  if (!acceptEventStream(client, true)) {
    rejectEventStream(
        client, 503, "Service Unavailable",
        "{\"error\":\"live-update client limit reached\",\"code\":\"sse_client_limit\"}",
        15);
  }
}
#endif

bool WebApplication::acceptEventStream(WiFiClient& client,
                                       bool headersComplete) {
  for (EventStreamClient& slot : eventStreamClients_) {
    if (!slot.client || !slot.client.connected()) {
      if (slot.acceptedAtMs != 0) {
        releaseEventStream(slot, EventStreamCloseReason::PeerClosed);
      }
      slot = EventStreamClient();
      slot.client = client;
      slot.client.setNoDelay(true);
      slot.headersComplete = headersComplete;
      const uint32_t acceptedAtMs = millis();
      slot.acceptedAtMs = acceptedAtMs == 0 ? 1 : acceptedAtMs;
      ++eventStreamAcceptedCount_;
      return true;
    }
  }
  return false;
}

void WebApplication::rejectEventStream(WiFiClient& client, int status,
                                       const char* statusText,
                                       const String& body,
                                       uint16_t retryAfterSeconds) {
  String wire = "HTTP/1.1 " + String(status) + " " + statusText +
      "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n";
  if (retryAfterSeconds) wire += "Retry-After: " + String(retryAfterSeconds) + "\r\n";
  wire += "Content-Length: " + String(body.length()) + "\r\n\r\n" + body;
  // Rejections are small, best effort and bounded; a slow fourth client must
  // not monopolize the timer loop merely to receive its error page.
  writeWithoutWaiting(client, reinterpret_cast<const uint8_t*>(wire.c_str()), wire.length());
#if defined(ESP8266)
  client.stop(1);
#else
  client.stop();
#endif
}

uint8_t WebApplication::eventStreamClientCount() {
  uint8_t count = 0;
  for (EventStreamClient& slot : eventStreamClients_) {
    if (slot.client && slot.client.connected()) {
      ++count;
    }
  }
  return count;
}

bool WebApplication::queueEventStreamStatus(EventStreamClient& slot,
                                             uint32_t nowMs,
                                             const String& body) {
  if (body.length() == 0) return false;
  String prefix = slot.lastSentAtMs == 0 ? "retry: 5000\n\n" : "";
  prefix += "id: " + String(bootId_, HEX) + ':' +
      String(timer_.eventRevision()) + ':' + String(stateRevision_) +
      "\nevent: status\ndata: ";
  const size_t frameLength = prefix.length() + body.length() + 2;
  char chunkHeader[14];
  const int count = snprintf(chunkHeader, sizeof(chunkHeader), "%x\r\n",
                             static_cast<unsigned int>(frameLength));
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(chunkHeader)) return false;
  const size_t wireLength = count + frameLength + 2;
  if (wireLength > kEventStreamMaximumWireBytes || !slot.pendingWire.reserve(wireLength)) return false;
  slot.pendingWire = chunkHeader;
  slot.pendingWire += prefix;
  slot.pendingWire += body;
  slot.pendingWire += "\n\n\r\n";
  if (slot.pendingWire.length() != wireLength) return false;
  slot.pendingRevision = stateRevision_;
  slot.writeCursor.begin(wireLength, nowMs);
  return true;
}

void WebApplication::serviceEventStreams(uint32_t nowMs) {
  if (otaQuietActive(nowMs)) {
    closeEventStreams();
    return;
  }
  String sharedStatus;
  bool frameInFlight = false;
  for (const EventStreamClient& slot : eventStreamClients_) {
    if (slot.responseStarted && slot.writeCursor.active()) frameInFlight = true;
  }
  for (EventStreamClient& slot : eventStreamClients_) {
    if (slot.acceptedAtMs == 0) continue;
    if (!slot.client || !slot.client.connected()) {
      releaseEventStream(slot, EventStreamCloseReason::PeerClosed);
      continue;
    }
    if (!slot.headersComplete) {
      uint16_t readThisLoop = 0;
      while (slot.client.available() && readThisLoop < 256) {
        const int value = slot.client.read();
        if (value < 0) break;
        ++readThisLoop;
        ++slot.headerBytes;
        if (value == '\n') {
          if (slot.headerLineLength == 0) {
            slot.headersComplete = true;
            break;
          }
          // DNS rebinding: remember whether the Host line names the timer.
          const size_t used = slot.headerLineLength < sizeof(slot.headerLine) - 1
              ? slot.headerLineLength : sizeof(slot.headerLine) - 1;
          slot.headerLine[used] = '\0';
          HttpRequestFields::noteStreamHeaderLine(slot.headerLine, used, slot.headerLineLength,
                                                  slot.hostLines, slot.hostTrusted);
          slot.headerLineLength = 0;
        } else if (value != '\r' && slot.headerLineLength < 0xFFFF) {
          if (slot.headerLineLength < sizeof(slot.headerLine) - 1) {
            slot.headerLine[slot.headerLineLength] = static_cast<char>(value);
          }
          ++slot.headerLineLength;
        }
      }
      if (slot.headerBytes > kEventStreamMaximumHeaderBytes) {
        releaseEventStream(slot, EventStreamCloseReason::HeaderTooLarge);
        continue;
      }
      if (static_cast<int32_t>(nowMs - slot.acceptedAtMs) >=
          static_cast<int32_t>(kEventStreamHeaderTimeoutMs)) {
        releaseEventStream(slot, EventStreamCloseReason::HeaderTimeout);
        continue;
      }
      if (!slot.headersComplete) continue;
      if (!slot.hostTrusted && !streamFromAccessPoint(slot.client)) {
        rejectEventStream(slot.client, 403, "Forbidden",
            "{\"error\":\"open the timer by its IP address or .local name\",\"code\":\"host_rejected\"}");
        releaseEventStream(slot, EventStreamCloseReason::HostRejected);
        continue;
      }
    }
    // An SSE GET has no request body or pipelined requests. Do not drain an
    // unbounded incoming stream in the main loop.
    if (slot.client.available()) {
      releaseEventStream(slot, EventStreamCloseReason::HeaderTooLarge);
      continue;
    }
    if (!slot.writeCursor.active()) {
      if (!slot.responseStarted) {
        slot.writeCursor.begin(sizeof(kHeaders) - 1, nowMs);
      } else {
        const bool refreshDue = slot.lastSentAtMs == 0 ||
            nowMs - slot.lastSentAtMs >= kEventStreamRefreshMs;
        if (slot.lastSentRevision == stateRevision_ && !refreshDue) continue;
        // Viewers take turns: one frame in flight at a time. Three copies queued
        // at once, on top of a command's response and the TCP queue, drove the
        // V1 below 4 KB free. A stalled viewer still times out after 5 s.
        if (frameInFlight) continue;
        if (sharedStatus.length() == 0) sharedStatus = statusJson(nowMs);
        if (!queueEventStreamStatus(slot, nowMs, sharedStatus)) {
          releaseEventStream(slot, EventStreamCloseReason::ResponseWriteFailed);
          continue;
        }
      }
    }
    const char* wire = slot.responseStarted ? slot.pendingWire.c_str() : kHeaders;
    const auto result = slot.writeCursor.pump(nowMs, kEventStreamWriteTimeoutMs,
        [&](size_t offset, size_t count) {
          return writeWithoutWaiting(slot.client,
              reinterpret_cast<const uint8_t*>(wire + offset), count);
        });
    if (result == SseWriteCursor::Result::TimedOut) {
      releaseEventStream(slot, EventStreamCloseReason::SendTimeout);
    } else if (result == SseWriteCursor::Result::Failed) {
      releaseEventStream(slot, EventStreamCloseReason::ResponseWriteFailed);
    } else if (result != SseWriteCursor::Result::Complete && slot.responseStarted) {
      frameInFlight = true;  // still sending: later viewers wait their turn
    } else if (result == SseWriteCursor::Result::Complete) {
      if (!slot.responseStarted) {
        slot.responseStarted = true;
      } else {
        slot.lastSentRevision = slot.pendingRevision;
        slot.lastSentAtMs = nowMs == 0 ? 1 : nowMs;
        slot.pendingWire = String();
        frameInFlight = true;  // at most one frame per pass; TCP drains between
        if (network_.stationConnected()) network_.acknowledgeStationHandoff();
      }
      slot.writeCursor = SseWriteCursor();
    }
  }
}

void WebApplication::closeEventStreams() {
  for (EventStreamClient& slot : eventStreamClients_) {
    if (slot.acceptedAtMs != 0) {
      releaseEventStream(slot, EventStreamCloseReason::OtaQuiet);
    }
  }
}

void WebApplication::releaseEventStream(EventStreamClient& slot,
                                        EventStreamCloseReason reason) {
  if (slot.client) {
#if defined(ESP8266)
    slot.client.abort();  // Do not wait for a stalled peer to acknowledge.
#else
    slot.client.stop();
#endif
  }
  if (slot.acceptedAtMs != 0) {
    ++eventStreamClosedCount_;
    lastEventStreamCloseReason_ = reason;
  }
  slot = EventStreamClient();
}

const char* WebApplication::eventStreamCloseReasonName(
    EventStreamCloseReason reason) {
  switch (reason) {
    case EventStreamCloseReason::PeerClosed:
      return "peer_closed";
    case EventStreamCloseReason::HeaderTooLarge:
      return "request_headers_too_large";
    case EventStreamCloseReason::HeaderTimeout:
      return "request_header_timeout";
    case EventStreamCloseReason::ResponseWriteFailed:
      return "response_write_failed";
    case EventStreamCloseReason::SendTimeout:
      return "send_timeout";
    case EventStreamCloseReason::OtaQuiet:
      return "ota_quiet";
    case EventStreamCloseReason::HostRejected:
      return "host_rejected";
    case EventStreamCloseReason::None:
    default:
      return "none";
  }
}
