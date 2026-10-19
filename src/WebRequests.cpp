#include "ResetTrace.h"
#include "WebApplication.h"

bool WebApplication::validateRequestFields() {
  requestFields_.clear();
  requestIsJson_ = false;
  String contentType = server_.header("Content-Type");
  const int parameters = contentType.indexOf(';');
  if (parameters >= 0) contentType = contentType.substring(0, parameters);
  contentType.trim();
  // ESP8266WebServer exposes URL-encoded input as BOTH parsed arguments and
  // a synthetic "plain" body (ESP32 exposes only the parsed arguments).
  // That raw form copy is not a second JSON input. Keep duplicate checks on
  // the parsed fields and continue rejecting real JSON + query mixtures.
  const bool formEncoded = contentType == "application/x-www-form-urlencoded";
  const bool bodyPresent = server_.hasArg("plain") && !formEncoded;
  size_t fieldBytes = 0;
  unsigned fieldCount = 0;
  const unsigned argumentCount = static_cast<unsigned>(server_.args());
  for (unsigned i = 0; i < argumentCount; ++i) {
    const String name = server_.argName(i);
    if (name == "plain") continue;
    ++fieldCount;
    fieldBytes += name.length() + server_.arg(i).length();
    if (fieldCount > HttpRequestFields::kMaximumFields ||
        fieldBytes > HttpRequestFields::kMaximumBodyBytes) {
      sendError(413, "request fields exceed the supported limit", "request_too_large");
      return false;
    }
    if (bodyPresent) {
      sendError(400, "do not mix JSON with query or form fields", "ambiguous_request");
      return false;
    }
    for (unsigned j = 0; j < i; ++j) {
      if (server_.argName(j) == name) {
        sendError(400, "duplicate request field", "duplicate_field");
        return false;
      }
    }
  }
  if (!bodyPresent) return true;
  const String body = server_.arg("plain");
  if (!requestFields_.parse(body.c_str(), body.length()) ||
      (server_.uri().startsWith("/api/") &&
       !requestFields_.validateForRoute(server_.uri().c_str()))) {
    sendRequestFieldError();
    return false;
  }
  requestIsJson_ = true;
  return true;
}

void WebApplication::sendRequestFieldError() {
  switch (requestFields_.error()) {
    case HttpRequestFields::Error::TooLarge:
      sendError(413, "JSON exceeds the supported size or field limit", "request_too_large");
      break;
    case HttpRequestFields::Error::DuplicateField:
      sendError(400, "duplicate JSON field", "duplicate_field");
      break;
    case HttpRequestFields::Error::InvalidField:
      sendError(400, "unknown field for this endpoint", "invalid_field");
      break;
    case HttpRequestFields::Error::InvalidType:
      sendError(400, "JSON field has the wrong type", "invalid_field_type");
      break;
    default:
      sendError(400, "expected one complete flat JSON object with integer numbers", "invalid_json");
      break;
  }
}

bool WebApplication::allowRequest(bool mutation, bool interlockOwner) {
  if (capture_ == nullptr) ResetTrace::request(server_.uri().c_str());
  if (mutation && !interlockOwner && mutationInterlockActive()) {
    sendError(409, "relay maintenance is keeping the timer open", "relay_maintenance");
    return false;
  }
  if (otaQuietActive(millis())) {
    sendOtaBusy();
    return false;
  }
  if (mutation && (restartAtMs_ != 0 || pendingNetworkAtMs_ != 0)) {
    sendError(409, "device transition already pending");
    return false;
  }
  if (capture_ != nullptr) {
    // Bluetooth management: the frame was authenticated with the app's session
    // key and its fields were validated for this route by manage(). Browser
    // origin checks do not apply to a GATT request.
    return true;
  }
  // DNS rebinding: a public website re-pointed at the timer sends its own
  // name as Host. The timer's own access point is exempt, because setup there
  // runs under whatever name the phone's captive-portal check used.
  if (!requestFromAccessPoint() &&
      !HttpRequestFields::localHost(server_.header("Host").c_str())) {
    sendError(403, "open the timer by its IP address or .local name", "host_rejected");
    return false;
  }
  if (mutation && server_.header("Sec-Fetch-Site") == "cross-site") {
    sendError(403, "cross-site request rejected");
    return false;
  }
  if (mutation && !sameOrigin()) {
    sendError(403, "origin rejected");
    return false;
  }
  return validateRequestFields();
}

bool WebApplication::requestFromAccessPoint() {
  const IPAddress accessPoint = WiFi.softAPIP();
  return accessPoint != IPAddress(0, 0, 0, 0) && server_.client().localIP() == accessPoint;
}

bool WebApplication::streamFromAccessPoint(WiFiClient& client) {
  const IPAddress accessPoint = WiFi.softAPIP();
  return accessPoint != IPAddress(0, 0, 0, 0) && client.localIP() == accessPoint;
}

bool WebApplication::sameOrigin() const {
  const String origin = server_.header("Origin");
  return origin.length() == 0 || origin == "http://" + server_.header("Host");
}

String WebApplication::requestValue(const String& name) const {
  if (capture_ != nullptr) {
    if (name == "password" && captureSecret_ != nullptr) {
      String secret;
      secret.reserve(captureSecretLength_);
      for (size_t i = 0; i < captureSecretLength_; ++i) secret += captureSecret_[i];
      return secret;
    }
    const auto* field = requestFields_.find(name.c_str());
    return field ? String(requestFields_.value(*field)) : String();
  }
  if (requestIsJson_ && server_.hasArg("plain")) {
    const auto* field = requestFields_.find(name.c_str());
    return field ? String(requestFields_.value(*field)) : String();
  }
  return server_.hasArg(name) ? server_.arg(name) : String();
}
