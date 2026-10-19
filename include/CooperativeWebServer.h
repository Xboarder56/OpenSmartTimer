#pragma once

#if defined(ESP8266)
#include <ESP8266WebServer.h>
#include "SseWriteCursor.h"

// One owned reply at a time. Request parsing stays in the pinned SDK; response
// writes return to the timer loop instead of waiting for TCP acknowledgements.
class CooperativeWebServer : public ESP8266WebServer {
 public:
  explicit CooperativeWebServer(int port) : ESP8266WebServer(port) {}
  void handleClient();
  void sendHeader(const String& name, const String& value, bool first = false);
  void send(int code, const char* contentType, const String& body);
  void send_P(int code, PGM_P contentType, PGM_P body, size_t length);
  bool responsePending() const { return pending_; }
  uint32_t responseCompletedCount() const { return completed_; }
  uint32_t responseFailedCount() const { return failed_; }
  const char* responseLastFailure() const { return lastFailure_; }

 private:
  bool prepare(int code, const char* contentType, size_t length);
  void pump(uint32_t nowMs);
  void fail(const char* reason);
  void release();
  static constexpr uint32_t kSendTimeoutMs = 5000;
  static constexpr size_t kMaximumHeaderBytes = 1024;
  String header_, body_;
  const uint8_t* flashBody_ = nullptr;
  SseWriteCursor cursor_;
  bool pending_ = false;
  bool headerFailed_ = false;
  uint32_t completed_ = 0, failed_ = 0;
  const char* lastFailure_ = "none";
};
#endif
