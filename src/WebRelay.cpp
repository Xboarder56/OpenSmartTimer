// Experimental relay owner API (V2 only). Excluded from V1 by build_src_filter;
// off until the owner enrolls or enables it. See docs/API.md "Relay".
#if defined(SMART_TIMER_RELAY)
#include "WebApplication.h"

#include "Crypto.h"
#include "HttpRequestFields.h"
#include "RelayRuntime.h"
#include "TimerController.h"

using opensmarttimer::relay::GrantState;
using opensmarttimer::relay::RelayLinkInfo;
using opensmarttimer::relay::RelayOpResult;
using opensmarttimer::relay::RelayRuntime;
using opensmarttimer::relay::RelayStatusInfo;

namespace {
const char* grantName(GrantState state) {
  return opensmarttimer::relay::RelayClient::grantStateName(state);
}

}  // namespace

void WebApplication::registerRelayRoutes() {
  const auto readJobId = [this](uint32_t& id) {
    char boot[12];
    snprintf(boot, sizeof(boot), "%lx", static_cast<unsigned long>(bootId_));
    if (requestValue("if_boot_id") != boot) {
      sendError(409, "read relay status and use its current boot_id", "stale_boot");
      return false;
    }
    if (!HttpRequestFields::unsignedInteger(requestValue("job_id").c_str(), id) || !id) {
      sendError(400, "job_id must be next_job_id from relay status", "invalid_relay_job_id");
      return false;
    }
    return true;
  };
  server_.on("/api/v1/relay", HTTP_GET, [this]() {
    if (allowRequest(false)) sendRelayStatus(200);
  });

  server_.on("/api/v1/relay/enroll", HTTP_POST, [this]() {
    if (!allowRequest(true, true)) return;
    char boot[12];
    snprintf(boot, sizeof(boot), "%lx", static_cast<unsigned long>(bootId_));
    if (requestValue("if_boot_id") != boot) {
      sendError(409, "read relay status and use its current boot_id", "stale_boot");
      return;
    }
    if (!timer_.isOpen()) {
      sendError(409, "relay enrollment requires an open timer", "timer_active");
      return;
    }
    const String origin = requestValue("relay_url");
    const String code = requestValue("enrollment_code");
    const String pinText = requestValue("spki_pin");
    const String partnerOrigin = requestValue("partner_origin");
    uint8_t pin[opensmarttimer::relay::kSpkiPinBytes];
    if (pinText.length() != 43 ||
        opensmarttimer::relay::b64urlDecode(pinText.c_str(), pinText.length(), pin,
                                            sizeof(pin)) != int(sizeof(pin))) {
      sendError(400, "spki_pin must be the relay's base64url SHA-256 SPKI pin",
                "invalid_spki_pin");
      return;
    }
    sendRelayResult(int(RelayRuntime::active()->enroll(origin.c_str(), code.c_str(), pin, partnerOrigin.c_str())));
  });

  server_.on("/api/v1/relay/enroll/cancel", HTTP_POST, [this]() {
    if (!allowRequest(true, true)) return;
    char boot[12];
    snprintf(boot, sizeof(boot), "%lx", static_cast<unsigned long>(bootId_));
    uint32_t id = 0;
    if (requestValue("if_boot_id") != boot) {
      sendError(409, "read relay status and use its current boot_id", "stale_boot");
      return;
    }
    if (!HttpRequestFields::unsignedInteger(requestValue("setup_request_id").c_str(), id) || !id) {
      sendError(400, "setup_request_id must match the pending setup", "invalid_relay_request");
      return;
    }
    sendRelayResult(int(RelayRuntime::active()->cancelEnrollment(id)));
  });

  server_.on("/api/v1/relay/enable", HTTP_POST, [this]() {
    if (!allowRequest(true, true)) return;
    if (!timer_.isOpen()) {
      sendError(409, "enabling the relay requires an open timer", "timer_active");
      return;
    }
    sendRelayResult(int(RelayRuntime::active()->enable()));
  });

  // Always available, including during a session: ends every share link.
  server_.on("/api/v1/relay/disable", HTTP_POST, [this]() {
    if (allowRequest(true, true)) sendRelayResult(int(RelayRuntime::active()->disable()));
  });

  server_.on("/api/v1/relay/forget", HTTP_POST, [this]() {
    if (!allowRequest(true, true)) return;
    if (!timer_.isOpen()) {
      sendError(409, "forgetting the relay requires an open timer", "timer_active");
      return;
    }
    sendRelayResult(int(RelayRuntime::active()->forget()));
  });

  server_.on("/api/v1/relay/links", HTTP_GET, [this]() {
    if (!allowRequest(false)) return;
    String body = "{\"links\":[";
    struct Visit { String* body; bool first; } visit{&body, true};
    const RelayOpResult result = RelayRuntime::active()->forEachLink(
        [](const RelayLinkInfo& link, void* context) {
          Visit& v = *static_cast<Visit*>(context);
          if (!v.first) *v.body += ",";
          v.first = false;
          *v.body += "{\"fp_hash\":\"" + String(link.fp_hash) + "\",\"label\":\"" +
                     jsonEscape(String(link.label)) + "\",\"expires_in_ms\":" +
                     String(link.expires_in_ms) + ",\"state\":\"" + grantName(link.state) +
                     "\",\"url_available\":" + (link.url_available ? "true" : "false") + "}";
        },
        &visit);
    if (result != RelayOpResult::Ok) {
      sendRelayResult(int(result));
      return;
    }
    body += "]}";
    sendJson(200, body);
  });

  // New links need a live relay connection; the link is usable once the
  // relay confirms it (share URL returns link_not_ready until then).
  server_.on("/api/v1/relay/links", HTTP_POST, [this, readJobId]() {
    if (!allowRequest(true, true)) return;
    uint32_t jobId;
    if (!readJobId(jobId)) return;
    uint32_t ttlSeconds = 0;
    if (!HttpRequestFields::unsignedInteger(requestValue("ttl_s").c_str(), ttlSeconds) ||
        ttlSeconds < 60 || ttlSeconds > RelayRuntime::kMaxLinkTtlMs / 1000) {
      sendError(400, "ttl_s must be between 60 and 86400 seconds", "invalid_link_ttl");
      return;
    }
    const String label = requestValue("label");
    if (label.length() > 23) {
      sendError(400, "label must be at most 23 characters", "invalid_link_label");
      return;
    }
    const RelayOpResult result =
        RelayRuntime::active()->mint(jobId, ttlSeconds * 1000UL, label.c_str());
    sendRelayResult(int(result));
  });

  server_.on("/api/v1/relay/links/share", HTTP_GET, [this]() {
    if (!allowRequest(false)) return;
    const String fpHash = requestValue("fp_hash");
    static char url[1024];
    GrantState state = GrantState::Unknown;
    const RelayOpResult result =
        RelayRuntime::active()->shareUrl(fpHash.c_str(), url, sizeof(url), &state);
    if (result == RelayOpResult::NotReady) {
      sendJson(409, "{\"error\":\"the relay has not confirmed this link yet\","
                    "\"code\":\"link_not_ready\",\"state\":\"" + String(grantName(state)) + "\"}");
      return;
    }
    if (result != RelayOpResult::Ok) {
      sendRelayResult(int(result));
      return;
    }
    sendJson(200, "{\"fp_hash\":\"" + fpHash + "\",\"state\":\"ready\",\"share_url\":\"" +
                      jsonEscape(String(url)) + "\"}");
    memset(url, 0, sizeof(url));
  });

  // Extend keeps the same URL: adds ttl_s to the link's expiry (from now if it
  // expired this boot). Works during a session, like revoke.
  server_.on("/api/v1/relay/links/extend", HTTP_POST, [this, readJobId]() {
    if (!allowRequest(true, true)) return;
    uint32_t jobId;
    if (!readJobId(jobId)) return;
    uint32_t ttlSeconds = 0;
    if (!HttpRequestFields::unsignedInteger(requestValue("ttl_s").c_str(), ttlSeconds) ||
        ttlSeconds < 60 || ttlSeconds > RelayRuntime::kMaxLinkTtlMs / 1000) {
      sendError(400, "ttl_s must be between 60 and 86400 seconds", "invalid_link_ttl");
      return;
    }
    sendRelayResult(int(RelayRuntime::active()->extend(
        jobId, requestValue("fp_hash").c_str(), ttlSeconds * 1000UL)));
  });

  server_.on("/api/v1/relay/links/revoke", HTTP_POST, [this, readJobId]() {
    if (!allowRequest(true, true)) return;
    uint32_t jobId;
    if (readJobId(jobId))
      sendRelayResult(int(RelayRuntime::active()->revoke(jobId, requestValue("fp_hash").c_str())));
  });
}

uint32_t WebApplication::relayLiveLinks() const {
  RelayStatusInfo info{};
  RelayRuntime::active()->status(&info);
  return info.live_links;
}

void WebApplication::sendRelayStatus(int status) {
  RelayStatusInfo info{};
  RelayRuntime::active()->status(&info);
  String body = "{\"experimental\":true,\"enabled\":";
  body += info.enabled ? "true" : "false";
  body += ",\"enrolled\":" + String(info.enrolled ? "true" : "false");
  body += ",\"running\":" + String(info.running ? "true" : "false");
  body += ",\"enrolling\":" + String(info.enrolling ? "true" : "false");
  body += ",\"setup_pending\":" + String(info.setup_pending ? "true" : "false");
  body += ",\"setup_state\":\"" + String(RelayRuntime::setupStateName(info.setup_state)) + "\"";
  body += ",\"setup_request_id\":" + String(info.setup_request_id);
  body += ",\"setup_remaining_ms\":" + String(info.setup_remaining_ms);
  body += ",\"partner_origin\":\"" + jsonEscape(String(info.partner_origin)) + "\"";
  body += ",\"setup_relay_origin\":\"" + jsonEscape(String(info.setup_relay_origin)) + "\"";
  body += ",\"setup_partner_origin\":\"" + jsonEscape(String(info.setup_partner_origin)) + "\"";
  body += ",\"stopping\":" + String(info.stopping ? "true" : "false");
  body += ",\"maintenance\":" + String(info.maintenance ? "true" : "false");
  body += ",\"paused_for_bluetooth\":" + String(info.paused_for_bluetooth ? "true" : "false");
  body += ",\"boot_id\":\"" + String(info.boot_id, HEX) + "\"";
  body += ",\"next_job_id\":" + String(info.next_job_id);
  const char* jobState = info.job_state == opensmarttimer::relay::RelayOwnerJob::State::Pending ? "pending"
      : info.job_state == opensmarttimer::relay::RelayOwnerJob::State::Running ? "running"
      : info.job_state == opensmarttimer::relay::RelayOwnerJob::State::Done ? "done" : "idle";
  body += ",\"job\":{\"id\":" + String(info.job_id) + ",\"state\":\"" + jobState +
      "\",\"result\":\"" + RelayRuntime::resultName(info.job_result) +
      "\",\"fp_hash\":\"" + String(info.job_fp_hash) + "\"}";
  body += ",\"state\":\"" + String(info.paused_for_bluetooth ? "paused"
                                     : info.stopping ? "stopping"
                                     : RelayRuntime::stateName(info.state)) + "\"";
  body += ",\"sharing_available\":" + String(info.sharing_available ? "true" : "false");
  body += ",\"relay_host\":\"" + jsonEscape(String(info.relay_host)) + "\"";
  body += ",\"timer_id\":\"" + jsonEscape(String(info.timer_id)) + "\"";
  body += ",\"last_error\":\"" + jsonEscape(String(info.last_error)) + "\"";
  body += ",\"reconnects\":" + String(info.reconnects);
  body += ",\"live_links\":" + String(info.live_links);
  body += ",\"link_limit\":" + String(RelayRuntime::kMaxLiveLinks);
  body += ",\"task_stack_min_free_bytes\":" + String(info.task_stack_min_free_bytes);
  body += ",\"free_heap_bytes\":" + String(info.free_heap_bytes);
  body += ",\"largest_free_block_bytes\":" + String(info.largest_free_block_bytes);
  body += ",\"tls_arena\":{\"size_bytes\":" + String(info.tls_arena_bytes) +
          ",\"free_bytes\":" + String(info.tls_arena_free_bytes) +
          ",\"minimum_free_bytes\":" + String(info.tls_arena_minimum_free_bytes) +
          ",\"fallback_count\":" + String(info.tls_arena_fallback_count) + "}}";
  sendJson(status, body);
}

void WebApplication::sendRelayResult(int value) {
  const RelayOpResult result = static_cast<RelayOpResult>(value);
  const char* code = RelayRuntime::resultName(result);
  switch (result) {
    case RelayOpResult::Ok: sendRelayStatus(200); return;
    case RelayOpResult::Accepted: sendRelayStatus(202); return;
    case RelayOpResult::InvalidRequest: sendError(400, "invalid relay request", code); return;
    case RelayOpResult::OriginRequired: sendError(409, "approve the hosted control page during setup first", code); return;
    case RelayOpResult::NotFound: sendError(404, "share link not found", code); return;
    case RelayOpResult::RateLimited: sendError(429, "too many share links this hour", code); return;
    case RelayOpResult::NotEnrolled: sendError(409, "enroll the relay first", code); return;
    case RelayOpResult::NotRunning: sendError(409, "the relay is disabled", code); return;
    case RelayOpResult::TableFull: sendError(409, "no room for another share link", code); return;
    case RelayOpResult::NotReady: sendError(409, "the relay has not confirmed this link yet", code); return;
    case RelayOpResult::NotConnected:
      sendError(503, "new share links need a live relay connection", code);
      return;
    case RelayOpResult::Busy: sendError(503, "the relay is busy; retry shortly", code); return;
    case RelayOpResult::Paused:
      sendError(409, "the relay is paused while a Bluetooth app is connected or pairing", code);
      return;
    case RelayOpResult::Conflict:
      sendError(409, "job_id is stale or was used for another operation; read relay status", code); return;
    case RelayOpResult::TimerActive:
      sendError(409, "this relay operation requires an open timer", code); return;
    case RelayOpResult::StorageFailed:
      sendError(503, "could not save relay settings", code);
      return;
    case RelayOpResult::Unavailable:
      sendError(503, "not enough memory to start the relay", code);
      return;
  }
  sendError(500, "relay request failed", code);
}
#endif
