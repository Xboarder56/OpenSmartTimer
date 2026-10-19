#include "ResetTrace.h"
#include "WebApplication.h"
#include "Hardware.h"
#include "LocalNetwork.h"
#include "TimerController.h"
#include "CheckedJsonWriter.h"
#include "DeviceConfig.h"
#include "ReleaseUpdateConfig.h"
#if defined(SMART_TIMER_HARDWARE_V2)
#include "BluetoothApplication.h"
#endif
#ifndef SMART_TIMER_VERSION
#define SMART_TIMER_VERSION "development"
#endif

#include "generated_build_identity.h"

void WebApplication::sendStatus() {
  const uint32_t nowMs = millis();
  const String body = statusJson(nowMs);
  if (body.length() == 0) {
    sendJson(503, "{\"error\":\"status unavailable; read status before retrying\",\"code\":\"response_unavailable\"}");
    return;
  }
  sendJson(200, body);
  if (network_.stationConnected()) {
    network_.acknowledgeStationHandoff();
  }
}

String WebApplication::statusJson(uint32_t nowMs, const String* action,
                                   bool changed, bool duplicate, const String* operationId) {
  observeState(nowMs);
  const TimerSnapshot status = timer_.snapshot(nowMs);
  String body;
  CheckedJsonWriter<String> json(body);
  json.text("state", stateName(static_cast<uint8_t>(status.state)));
  json.number("remaining_ms", status.remainingMs);
  json.number("session_duration_ms", status.sessionDurationMs);
  json.boolean("sealed", status.sealed);
  json.boolean("pause_allowed", status.pauseAllowed);
  json.boolean("control_lock_allowed", status.sealAllowed);
  json.boolean("emergency_input_active", status.emergencyInputActive);
  json.number("max_duration_ms", Hardware::kMaximumDurationMs);
  json.text("api_version", "v1");
  json.text("version", SMART_TIMER_VERSION);
  json.text("device_id", Platform::deviceId().c_str());
  json.text("hardware_model", Platform::hardwareModel());
  json.text("chip_family", Platform::chipFamily());
  json.text("build_id", SMART_TIMER_BUILD_ID);
  json.text("boot_id", String(bootId_, HEX).c_str());
  const String activeSessionId = sessionId();
  if (activeSessionId.length()) json.text("session_id", activeSessionId.c_str());
  else json.null("session_id");
  json.number("state_revision", stateRevision_);
  json.number("latest_event_id", timer_.eventRevision());
  json.text("device_name", network_.deviceName().c_str());
  json.text("hostname", network_.hostName().c_str());
  json.number("uptime_ms", nowMs);
  json.number("server_time_ms", nowMs);
  json.text("last_release_reason", TimerController::releaseReasonName(status.lastReleaseReason));
  // A share link limits this session: it opens by itself when this reaches 0.
  if (status.holdLimited) json.number("share_link_limit_ms", status.holdLimitRemainingMs);
  else json.null("share_link_limit_ms");
#if defined(SMART_TIMER_RELAY)
  // Lets every local page show that someone else can control the timer.
  json.number("share_links_active", relayLiveLinks());
#endif
  json.text("ap_ssid", network_.accessPointName().c_str());
  json.text("network_mode", network_.modeName());
  json.boolean("ap_running", network_.accessPointRunning());
  json.text("ap_role", !network_.accessPointRunning() ? "off" :
            network_.standaloneMode() ? "primary" : "recovery");
  json.number("ap_clients", network_.accessPointClients());
  json.text("ap_security", network_.accessPointSecurity().c_str());
  json.boolean("ap_password_set", network_.accessPointPasswordSet());
  json.boolean("ap_uses_device_name", network_.accessPointUsesDeviceName());
  json.boolean("ota_armed", otaArmed(nowMs));
  json.number("ota_armed_remaining_ms", otaArmedRemainingMs(nowMs));
  json.boolean("ota_unverified_allowed", otaUnverifiedAllowed(nowMs));
  json.boolean("identifying", identifying(nowMs));
  json.boolean("station_connected", network_.stationConnected());
  json.text("station_ip", network_.stationAddress().c_str());
  json.text("station_mac", network_.stationMacAddress().c_str());
  json.text("connected_ssid", network_.stationName().c_str());
  json.text("saved_ssid", network_.savedStationName().c_str());
  json.text("target_ssid", network_.targetStationName().c_str());
  json.text("last_attempted_ssid", network_.lastAttemptedStationName().c_str());
  json.text("wifi_state", network_.stationState(nowMs).c_str());
  json.signedNumber("rssi_dbm", network_.stationRssi());
  json.text("last_wifi_error", network_.lastWifiError().c_str());
  json.number("retry_in_ms", network_.retryInMs(nowMs));
  json.boolean("station_retry_paused", network_.stationRetryPaused());
  if (action != nullptr) {
    json.object("mutation");
    json.text("action", action->c_str());
    json.boolean("changed", changed);
    json.boolean("duplicate", duplicate);
    if (operationId != nullptr && operationId->length()) json.text("operation_id", operationId->c_str());
    else json.null("operation_id");
    json.endObject();
  }
  // Never hand REST or SSE an incomplete JSON object after an allocation fails.
  if (!json.finish()) { ++statusJsonFailureCount_; return String(); }
  return body;
}

void WebApplication::sendWifiStatus() {
  const uint32_t nowMs = millis();
  String body;
  body.reserve(600);
  body = "{\"state\":\"" + jsonEscape(network_.stationState(nowMs)) + "\"";
  body += ",\"network_mode\":\"" +
          String(network_.modeName()) + "\"";
#if defined(SMART_TIMER_HARDWARE_V2)
  body += ",\"wifi_suspend_supported\":true";
  body += ",\"wifi_disconnect_restore_supported\":true";
#endif
  body += ",\"saved_ssid\":\"" + jsonEscape(network_.savedStationName()) + "\"";
  body += ",\"connected_ssid\":\"" + jsonEscape(network_.stationName()) + "\"";
  body += ",\"target_ssid\":\"" + jsonEscape(network_.targetStationName()) + "\"";
  body += ",\"last_attempted_ssid\":\"" +
          jsonEscape(network_.lastAttemptedStationName()) + "\"";
  body += ",\"rssi_dbm\":" + String(network_.stationRssi());
  body += ",\"station_ip\":\"" + jsonEscape(network_.stationAddress()) + "\"";
  body += ",\"station_mac\":\"" +
          jsonEscape(network_.stationMacAddress()) + "\"";
  body += ",\"last_error\":\"" + jsonEscape(network_.lastWifiError()) + "\"";
  body += ",\"retry_in_ms\":" + String(network_.retryInMs(nowMs));
  body += ",\"station_retry_paused\":" +
          String(network_.stationRetryPaused() ? "true" : "false");
  body += ",\"ap_running\":" +
          String(network_.accessPointRunning() ? "true" : "false");
  body += ",\"ap_role\":\"";
  body += !network_.accessPointRunning()
              ? "off"
              : network_.standaloneMode() ? "primary" : "recovery";
  body += "\"";
  body += ",\"ap_ssid\":\"" + jsonEscape(network_.accessPointName()) + "\"";
  body += ",\"ap_clients\":" + String(network_.accessPointClients());
  body += ",\"ap_security\":\"" +
          jsonEscape(network_.accessPointSecurity()) + "\"";
  body += ",\"ap_password_set\":" +
          String(network_.accessPointPasswordSet() ? "true" : "false");
  body += ",\"ap_uses_device_name\":" +
          String(network_.accessPointUsesDeviceName() ? "true" : "false") +
          "}";
  sendJson(200, body);
  if (network_.stationConnected()) {
    network_.acknowledgeStationHandoff();
  }
}

void WebApplication::sendAccessPointConfig() {
  String body;
  body.reserve(180);
  body = "{\"ssid\":\"" + jsonEscape(network_.accessPointName()) + "\"";
  body += ",\"security\":\"" + jsonEscape(network_.accessPointSecurity()) +
          "\"";
  body += ",\"password_set\":" +
          String(network_.accessPointPasswordSet() ? "true" : "false");
  body += ",\"uses_device_name\":" +
          String(network_.accessPointUsesDeviceName() ? "true" : "false");
  body += ",\"clients\":" + String(network_.accessPointClients()) + "}";
  sendJson(200, body);
}

void WebApplication::sendDiagnostics() {
  const uint32_t nowMs = millis();
  const TimerSnapshot status = timer_.snapshot(nowMs);
  String body;
  body.reserve(1500);
  body = "{\"uptime_ms\":" + String(nowMs);
  body += ",\"reset_reason\":\"" + jsonEscape(Platform::resetReason()) + "\"";
  // Where the previous run was when it reset; locates watchdog hangs.
  const ResetTrace::Record& trace = ResetTrace::previous();
  if (trace.magic == ResetTrace::kMagic) {
    body += ",\"last_reset_trace\":{\"phase\":\"" + String(ResetTrace::phaseName(trace.phase)) +
            "\",\"uri\":\"" + jsonEscape(trace.uri) + "\",\"uptime_ms\":" + String(trace.atMs) + "}";
  }
  const ResetTrace::Stall& stall = ResetTrace::stall();
  body += ",\"loop_stall\":{\"longest_ms\":" + String(stall.longestMs) +
          ",\"phase\":\"" + String(ResetTrace::phaseName(stall.phase)) +
          "\",\"uri\":\"" + jsonEscape(stall.uri) + "\",\"uptime_ms\":" + String(stall.atMs) +
          ",\"over_1s_count\":" + String(stall.overSecondCount) + "}";
  body += ",\"free_heap_bytes\":" + String(Platform::freeHeapBytes());
  body += ",\"session_controls_recovered\":" +
          String(network_.settings().sessionControlsRecovered() ? "true" : "false");
  body += ",\"minimum_observed_free_heap_bytes\":" +
          String(minimumObservedFreeHeapBytes_);
  body += ",\"heap_low_water\":{\"free_bytes\":" + String(heapLowWater_.freeBytes) +
          ",\"uptime_ms\":" + String(heapLowWater_.atMs) +
          ",\"sse_clients\":" + String(heapLowWater_.sseClients) +
          ",\"sse_pending_bytes\":" + String(heapLowWater_.ssePendingBytes) +
          ",\"tcp_active\":" + String(heapLowWater_.tcpActive) +
          ",\"tcp_time_wait\":" + String(heapLowWater_.tcpTimeWait) +
          ",\"tcp_queued_bytes\":" + String(heapLowWater_.tcpQueuedBytes) +
          ",\"last_uri\":\"" + jsonEscape(heapLowWater_.lastUri) + "\"}";
  body += ",\"max_free_block_bytes\":" +
          String(Platform::largestFreeBlockBytes());
  body += ",\"heap_fragmentation_percent\":" +
          String(Platform::heapFragmentationPercent());
  body += ",\"loop_stack_size_bytes\":" + String(Platform::loopStackSizeBytes());
  body += ",\"loop_stack_min_free_bytes\":" +
          String(Platform::loopStackMinimumFreeBytes());
  body += ",\"sketch_size_bytes\":" + String(Platform::sketchSizeBytes());
  body += ",\"free_sketch_space_bytes\":" +
          String(Platform::freeSketchSpaceBytes());
  body += ",\"flash_size_bytes\":" + String(Platform::flashSizeBytes());
  body += ",\"flash_layout\":\"";
  body += Platform::flashLayout();
  body += "\"";
  body += ",\"hardware_model\":\"";
  body += Platform::hardwareModel();
  body += "\"";
  body += ",\"chip_family\":\"";
  body += Platform::chipFamily();
  body += "\"";
  body += ",\"build_id\":\"" SMART_TIMER_BUILD_ID "\"";
  body += ",\"sse_clients\":" + String(eventStreamClientCount());
  body += ",\"sse_client_limit\":" +
          String(kEventStreamClientLimit);
  body += ",\"sse_accepted_count\":" +
          String(eventStreamAcceptedCount_);
  body += ",\"sse_closed_count\":" +
          String(eventStreamClosedCount_);
  body += ",\"sse_last_close_reason\":\"";
  body += eventStreamCloseReasonName(lastEventStreamCloseReason_);
  body += "\"";
  body += ",\"sdk_version\":\"" + jsonEscape(ESP.getSdkVersion()) + "\"";
  body += ",\"wifi_state\":\"" + jsonEscape(network_.stationState(nowMs)) + "\"";
  body += ",\"wifi_channel\":" + String(WiFi.channel());
  body += ",\"status_json_failure_count\":" + String(statusJsonFailureCount_);
#if defined(ESP8266)
  body += ",\"http_response_completed_count\":" + String(server_.responseCompletedCount());
  body += ",\"http_response_failed_count\":" + String(server_.responseFailedCount());
  body += ",\"http_response_last_failure\":\"" + String(server_.responseLastFailure()) + "\"";
#endif
  body += ",\"wifi_power_save_mode\":\"" + String(Platform::wifiPowerSaveMode()) + "\"";
  body += ",\"wifi_sa_query_timeout_count\":" + String(network_.stationSaQueryTimeoutCount());
  body += ",\"wifi_last_disconnect_rssi_dbm\":";
  body += network_.lastStationDisconnectRssi() == 127
              ? String("null") : String(network_.lastStationDisconnectRssi());
  body += ",\"station_mac\":\"" +
          jsonEscape(network_.stationMacAddress()) + "\"";
  body += ",\"rssi_dbm\":" + String(network_.stationRssi());
  body += ",\"ap_clients\":" + String(network_.accessPointClients());
  body += ",\"station_retry_paused\":" +
          String(network_.stationRetryPaused() ? "true" : "false");
  body += ",\"station_retry_in_ms\":" +
          String(network_.retryInMs(nowMs));
  body += ",\"wifi_connect_attempt_count\":" +
          String(network_.stationConnectAttemptCount());
  body += ",\"wifi_disconnect_count\":" +
          String(network_.stationDisconnectCount());
  body += ",\"wifi_unexpected_disconnect_count\":" +
          String(network_.stationUnexpectedDisconnectCount());
  body += ",\"wifi_dhcp_timeout_count\":" +
          String(network_.stationDhcpTimeoutCount());
  body += ",\"wifi_last_disconnect_reason\":" +
          String(network_.lastStationDisconnectReason());
  body += ",\"wifi_last_disconnect_reason_name\":\"" +
          jsonEscape(network_.lastStationDisconnectReasonName()) + "\"";
  body += ",\"wifi_last_disconnect_expected\":" +
          String(network_.lastStationDisconnectExpected() ? "true" : "false");
  body += ",\"wifi_last_disconnect_at_ms\":" +
          String(network_.lastStationDisconnectAtMs());
  body += ",\"wifi_last_connected_at_ms\":" +
          String(network_.lastStationConnectedAtMs());
  body += ",\"wifi_connected_duration_ms\":" +
          String(network_.stationConnectedDurationMs(nowMs));
  body += ",\"lock_output_energized\":" +
          String(status.state == TimerState::Open ? "false" : "true");
  body += ",\"emergency_input_active\":" +
          String(status.emergencyInputActive ? "true" : "false");
  body += ",\"event_count\":" + String(timer_.eventCount());
#if defined(SMART_TIMER_HARDWARE_V2)
  const BluetoothApplication* bluetooth = BluetoothApplication::active();
  body += ",\"bluetooth_initialized\":" +
          String(bluetooth != nullptr && bluetooth->initialized() ? "true"
                                                                  : "false");
  body += ",\"bluetooth_enabled\":" +
          String(bluetooth != nullptr && bluetooth->enabled() ? "true"
                                                              : "false");
  body += ",\"bluetooth_configured_enabled\":" +
          String(bluetooth != nullptr && bluetooth->configuredEnabled()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_paired\":" +
          String(bluetooth != nullptr && bluetooth->paired() ? "true"
                                                              : "false");
  body += ",\"bluetooth_synchronized\":" +
          String(bluetooth != nullptr && bluetooth->synchronized() ? "true"
                                                                   : "false");
  body += ",\"bluetooth_advertising\":" +
          String(bluetooth != nullptr && bluetooth->advertising() ? "true"
                                                                  : "false");
  body += ",\"bluetooth_connections\":" +
          String(bluetooth == nullptr ? 0 : bluetooth->connectionCount());
  body += ",\"bluetooth_connection_limit\":" +
          String(bluetooth == nullptr ? 0 : bluetooth->connectionLimit());
  body += ",\"bluetooth_last_error\":" +
          String(bluetooth == nullptr ? 0 : bluetooth->lastError());
  body += ",\"bluetooth_last_error_name\":\"";
  body += bluetooth == nullptr ? "none" : bluetooth->lastErrorName();
  body += "\"";
  body += ",\"bluetooth_connection_rssi_dbm\":";
  if (bluetooth == nullptr || bluetooth->connectionRssi() == 127) {
    body += "null";
  } else {
    body += String(bluetooth->connectionRssi());
  }
  body += ",\"bluetooth_connection_mtu\":" +
          String(bluetooth == nullptr ? 0 : bluetooth->connectionMtu());
  body += ",\"bluetooth_connection_encrypted\":" +
          String(bluetooth != nullptr && bluetooth->connectionEncrypted()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_connection_bonded\":" +
          String(bluetooth != nullptr && bluetooth->connectionBonded()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_control_enabled\":" +
          String(bluetooth != nullptr && bluetooth->controlEnabled() ? "true"
                                                                     : "false");
  body += ",\"bluetooth_control_authenticated\":" +
          String(bluetooth != nullptr && bluetooth->controlAuthenticated()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_control_authentication_failure_count\":" +
          String(bluetooth == nullptr
                     ? 0
                     : bluetooth->controlAuthenticationFailureCount());
  body += ",\"bluetooth_authentication_backoff_remaining_ms\":" +
          String(bluetooth == nullptr
                     ? 0
                     : bluetooth->authenticationBackoffRemainingMs(nowMs));
  body += ",\"bluetooth_control_command_count\":" +
          String(bluetooth == nullptr ? 0 : bluetooth->controlCommandCount());
  body += ",\"bluetooth_control_last_result\":\"";
  body += bluetooth == nullptr ? "unsupported"
                               : bluetooth->lastControlResultName();
  body += "\"";
  body += ",\"bluetooth_connection_mtu_sufficient\":" +
          String(bluetooth != nullptr && bluetooth->connectionMtuSufficient()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_bond_cleanup_pending\":" +
          String(bluetooth != nullptr && bluetooth->bondCleanupPending()
                     ? "true"
                     : "false");
  body += ",\"bluetooth_bond_cleanup_last_error\":" +
          String(bluetooth == nullptr ? 0
                                      : bluetooth->bondCleanupLastError());
#endif
  body += ",\"wifi_scan_state\":\"" +
          jsonEscape(network_.scanStateName()) + "\"}";
  sendJson(200, body);
}

#if defined(SMART_TIMER_HARDWARE_V2)
void WebApplication::handleBluetoothPairingStart() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "Bluetooth pairing requires an open timer", "timer_active");
    return;
  }
  BluetoothApplication* bluetooth = BluetoothApplication::active();
  if (!bluetooth) { sendError(503, "Bluetooth unavailable", "bluetooth_unavailable"); return; }
  uint32_t replaceId = 0;
  const String replace = requestValue("replace_app_id");
  const String label = requestValue("label");
  if ((replace.length() && (!HttpRequestFields::unsignedInteger(replace.c_str(), replaceId) || !replaceId)) ||
      (label.length() && !BleAppRegistry::validLabel(label.c_str()))) {
    sendError(400, "Use a positive app ID and a label of 1–24 UTF-8 bytes", "invalid_app");
    return;
  }
  if (replaceId && bluetooth->apps().find(replaceId) < 0) {
    sendError(404, "Approved app no longer exists", "app_not_found"); return;
  }
  if (!replaceId && bluetooth->apps().empty() < 0) {
    sendError(409, "Three apps are already approved. Remove or explicitly replace one.", "app_table_full"); return;
  }
  if (bluetooth && bluetooth->pairingActive(millis())) {
    sendError(409, "Cancel the current pairing first", "pairing_active"); return;
  }
  if (bluetooth == nullptr ||
      !bluetooth->startAppPairing(millis(), bootId_, replaceId, label)) {
    sendError(503, "Bluetooth pairing failed to start",
              "bluetooth_unavailable");
    return;
  }
  sendBluetoothStatus();
}

void WebApplication::handleBluetoothPairingCancel() {
  if (!allowRequest(true)) {
    return;
  }
  BluetoothApplication* bluetooth = BluetoothApplication::active();
  if (bluetooth == nullptr) {
    sendError(503, "Bluetooth is unavailable", "bluetooth_unavailable");
    return;
  }
  bluetooth->cancelPairing();
  sendBluetoothStatus();
}

void WebApplication::handleBluetoothPairingRevoke() {
  if (!allowRequest(true)) {
    return;
  }
  if (!timer_.isOpen()) {
    sendError(409, "Bluetooth pairing removal requires an open timer",
              "timer_active");
    return;
  }
  BluetoothApplication* bluetooth = BluetoothApplication::active();
  if (bluetooth == nullptr) {
    sendError(503, "Bluetooth is unavailable", "bluetooth_unavailable");
    return;
  }
  if (!bluetooth->revokePairing()) {
    sendError(503, "Bluetooth pairing could not be removed",
              "bluetooth_storage_failed");
    return;
  }
  sendBluetoothStatus();
}

void WebApplication::handleBluetoothAppUpdate(bool remove) {
  if (!allowRequest(true)) return;
  if (!timer_.isOpen()) {
    sendError(409, "App management requires an open timer", "timer_active"); return;
  }
  auto* bluetooth = BluetoothApplication::active();
  if (!bluetooth) { sendError(503, "Bluetooth unavailable", "bluetooth_unavailable"); return; }
  if (bluetooth->pairingActive(millis())) {
    sendError(409, "Cancel the current pairing first", "pairing_active"); return;
  }
  uint32_t id = 0;
  if (!HttpRequestFields::unsignedInteger(requestValue("app_id").c_str(), id) || !id) {
    sendError(400, "app_id must be a positive integer", "invalid_app"); return;
  }
  if (bluetooth->apps().find(id) < 0) {
    sendError(404, "Approved app no longer exists", "app_not_found"); return;
  }
  const String label = requestValue("label");
  if (!remove && !BleAppRegistry::validLabel(label.c_str())) {
    sendError(400, "Label must contain 1–24 UTF-8 bytes without controls", "invalid_label"); return;
  }
  if (!(remove ? bluetooth->revokeApp(id) : bluetooth->renameApp(id, label))) {
    sendError(503, "Approved app could not be saved", "bluetooth_storage_failed"); return;
  }
  sendBluetoothStatus();
}

void WebApplication::sendBluetoothStatus() {
  const BluetoothApplication* bluetooth = BluetoothApplication::active();
  if (bluetooth == nullptr) {
    sendError(503, "Bluetooth is unavailable", "bluetooth_unavailable");
    return;
  }
  const uint32_t nowMs = millis();
  String body;
  body.reserve(2300);
  body = "{\"supported\":true,\"protocol\":\"";
  body += bluetooth->protocolName();
  body += "\",\"mode\":\"bonded_authenticated_control\"";
  body += ",\"default_enabled\":false";
  body += ",\"configured_enabled\":" +
          String(bluetooth->configuredEnabled() ? "true" : "false");
  body += ",\"persistent_enable\":true,\"volatile_enable\":false";
  body += ",\"enable_path\":\"/api/v1/bluetooth/enable\"";
  body += ",\"disable_path\":\"/api/v1/bluetooth/disable\"";
  body += ",\"advertised_name\":\"";
  body += bluetooth->advertisedName();
  body += "\",\"initialized\":" +
          String(bluetooth->initialized() ? "true" : "false");
  body += ",\"enabled\":" +
          String(bluetooth->enabled() ? "true" : "false");
  body += ",\"synchronized\":" +
          String(bluetooth->synchronized() ? "true" : "false");
  body += ",\"advertising\":" +
          String(bluetooth->advertising() ? "true" : "false");
  body += ",\"connected\":" +
          String(bluetooth->connected() ? "true" : "false");
  body += ",\"subscribed\":" +
          String(bluetooth->subscribed() ? "true" : "false");
  body += ",\"connections\":" + String(bluetooth->connectionCount());
  body += ",\"connection_limit\":" +
          String(bluetooth->connectionLimit());
  body += ",\"approved_app_limit\":3,\"approved_app_count\":" + String(bluetooth->apps().count());
  body += ",\"active_app_id\":" + String(bluetooth->activeAppId());
  body += ",\"approved_apps\":[";
  bool firstApp = true;
  for (uint8_t slot = 0; slot < BleAppRegistry::kCapacity; ++slot) {
    const auto& app = bluetooth->apps().apps[slot];
    if (!app.id) continue;
    if (!firstApp) body += ',';
    firstApp = false;
    body += "{\"app_id\":" + String(app.id) + ",\"label\":\"" + jsonEscape(app.label) + "\"";
    body += ",\"connected\":" + String(bluetooth->activeAppId() == app.id ? "true" : "false");
    body += ",\"last_seen_at_ms\":";
    const uint32_t seen = bluetooth->appLastSeenMs(slot);
    body += seen ? String(seen) : String("null");
    body += '}';
  }
  body += ']';
  body += ",\"accepted_count\":" + String(bluetooth->acceptedCount());
  body += ",\"closed_count\":" + String(bluetooth->closedCount());
  body += ",\"notification_count\":" +
          String(bluetooth->notificationCount());
  body += ",\"last_error\":" + String(bluetooth->lastError());
  body += ",\"last_error_name\":\"";
  body += bluetooth->lastErrorName();
  body += "\"";
  body += ",\"last_disconnect_reason\":" +
          String(bluetooth->lastDisconnectReason());
  body += ",\"last_disconnect_reason_name\":\"";
  body += bluetooth->lastDisconnectReasonName();
  body += "\",\"connection_rssi_dbm\":";
  if (bluetooth->connectionRssi() == 127) {
    body += "null";
  } else {
    body += String(bluetooth->connectionRssi());
  }
  body += ",\"connection_mtu\":" + String(bluetooth->connectionMtu());
  body += ",\"connection_encrypted\":" +
          String(bluetooth->connectionEncrypted() ? "true" : "false");
  body += ",\"connection_bonded\":" +
          String(bluetooth->connectionBonded() ? "true" : "false");
  body += ",\"paired\":" +
          String(bluetooth->paired() ? "true" : "false");
  body += ",\"pairing_state\":\"";
  body += bluetooth->pairingStateName(nowMs);
  body += "\",\"pairing_active\":" +
          String(bluetooth->pairingActive(nowMs) ? "true" : "false");
  body += ",\"pairing_remaining_ms\":" +
          String(bluetooth->pairingRemainingMs(nowMs));
  body += ",\"pairing_failure\":\"";
  body += bluetooth->pairingFailureName();
  body += "\"";
  body += ",\"physical_pairing_supported\":true";
  body += ",\"control_supported\":true";
  body += ",\"control_default_enabled\":" +
          String(bluetooth->paired() ? "true" : "false");
  body += ",\"control_volatile\":false";
  body += ",\"control_enabled\":" +
          String(bluetooth->controlEnabled() ? "true" : "false");
  body += ",\"control_authenticated\":" +
          String(bluetooth->controlAuthenticated() ? "true" : "false");
  body += ",\"control_authentication\":\"bonded-hmac-sha256-112\"";
  body += ",\"command_integrity\":\"hmac-sha256-64-counter\"";
  body += ",\"control_authentication_failure_count\":" +
          String(bluetooth->controlAuthenticationFailureCount());
  body += ",\"control_authentication_failure_limit\":5";
  body += ",\"control_authentication_backoff_remaining_ms\":" +
          String(bluetooth->authenticationBackoffRemainingMs(nowMs));
  body += ",\"control_command_count\":" +
          String(bluetooth->controlCommandCount());
  body += ",\"control_last_result\":\"";
  body += bluetooth->lastControlResultName();
  body += "\",\"pairing_start_path\":\"/api/v1/bluetooth/pairing/start\"";
  body += ",\"pairing_cancel_path\":\"/api/v1/bluetooth/pairing/cancel\"";
  body += ",\"pairing_revoke_path\":\"/api/v1/bluetooth/pairing/revoke\"";
  body += ",\"control_boot_id\":" + String(bootId_);
  body += ",\"required_control_mtu\":35";
  body += ",\"connection_mtu_sufficient\":" +
          String(bluetooth->connectionMtuSufficient() ? "true" : "false");
  body += ",\"bond_cleanup_pending\":" +
          String(bluetooth->bondCleanupPending() ? "true" : "false");
  body += ",\"bond_cleanup_last_error\":" +
          String(bluetooth->bondCleanupLastError());
  body += ",\"peer_discovery_supported\":false";
  body += ",\"peer_control_supported\":false}";
  sendJson(200, body);
}
#endif

void WebApplication::sendEvents() {
  const String cursorValue = requestValue("after");
  const bool cursorSupplied = cursorValue.length() != 0;
  uint32_t after = 0;
  if (cursorSupplied) {
    if (!HttpRequestFields::unsignedInteger(cursorValue.c_str(), after)) {
      sendError(400, "after must be an unsigned event ID",
                "invalid_event_cursor");
      return;
    }
  }
  const String currentBootId = String(bootId_, HEX);
  const String requestedBootId = requestValue("boot_id");
  const uint32_t latest = timer_.eventRevision();
  const uint32_t oldest =
      timer_.eventCount() == 0 ? 0 : timer_.eventAt(0).id;
  bool cursorReset = requestedBootId.length() != 0 &&
                     requestedBootId != currentBootId;
  if (cursorSupplied && after > latest) {
    cursorReset = true;
  }
  const bool truncated = !cursorReset && cursorSupplied && after != 0 &&
                         oldest != 0 && after < oldest &&
                         oldest - after > 1;
  String body;
  body.reserve(2800);
  body = "{\"boot_id\":\"" + currentBootId + "\"";
  body += ",\"oldest_event_id\":" + String(oldest);
  body += ",\"latest_event_id\":" + String(latest);
  body += ",\"next_after\":" + String(latest);
  body += ",\"cursor_reset\":" + String(cursorReset ? "true" : "false");
  body += ",\"truncated\":" + String(truncated ? "true" : "false");
  body += ",\"events\":[";
  bool emitted = false;
  for (uint8_t index = 0; index < timer_.eventCount(); ++index) {
    const TimerEvent event = timer_.eventAt(index);
    if (!cursorReset && cursorSupplied && event.id <= after) {
      continue;
    }
    if (emitted) {
      body += ',';
    }
    emitted = true;
    body += "{\"id\":" + String(event.id) + ",\"type\":\"";
    body += TimerController::eventName(event.type);
    body += "\",\"at_ms\":" + String(event.atMs);
    body += ",\"value_ms\":" + String(event.valueMs);
    body += ",\"state\":\"";
    body += stateName(static_cast<uint8_t>(event.state));
    body += "\",\"sealed\":" + String(event.sealed ? "true" : "false");
    body += ",\"emergency_input_active\":" +
            String(event.emergencyInputActive ? "true" : "false");
    body += ",\"session_sequence\":" + String(event.sessionSequence);
#if defined(SMART_TIMER_HARDWARE_V2)
    body += ",\"bluetooth_app_id\":" + String(event.bluetoothAppId);
    body += ",\"via_share_link\":" + String(event.viaShareLink ? "true" : "false");
#endif
    if (event.type == TimerEventType::WifiDisconnected) {
      body += ",\"wifi_disconnect_reason\":" + String(event.valueMs);
      body += ",\"wifi_disconnect_reason_name\":\"";
      body += LocalNetwork::disconnectReasonName(
          static_cast<uint8_t>(event.valueMs));
      body += "\",\"unexpected\":true";
    }
    body += "}";
  }
  body += "]}";
  sendJson(200, body);
}

void WebApplication::sendCapabilities() {
  String body;
  body.reserve(1250);
  body = "{\"api_version\":\"v1\",\"max_duration_ms\":";
  body += String(Hardware::kMaximumDurationMs);
  body += ",\"hardware_model\":\"";
  body += Platform::hardwareModel();
  body += "\",\"chip_family\":\"";
  body += Platform::chipFamily();
  body += "\"";
  body += ",\"build_id\":\"" SMART_TIMER_BUILD_ID "\"";
  body += ",\"request_content_types\":[\"application/x-www-form-urlencoded\",\"application/json\"]";
  body += ",\"features\":{\"pause\":true,\"adjust\":true,\"random_duration\":true";
  body += ",\"lock_controls\":true,\"wifi_scan\":true,\"wifi_staged_credentials\":true";
  body += ",\"wifi_async_scan\":true";
  body += ",\"device_name\":true,\"identify_led\":true,\"diagnostics\":true";
  body += ",\"station_mac\":true";
  body += ",\"event_history\":true,\"emergency_input_status\":true";
  body += ",\"ap_configuration\":true,\"ap_wpa2\":true";
  body += ",\"adjust_operation_id\":true,\"mutation_operation_id\":true";
  body += ",\"session_id\":true,\"session_precondition\":true";
  body += ",\"boot_precondition\":true,\"wifi_disconnect_diagnostics\":true";
  body += ",\"release_alias\":true,\"event_cursor\":true";
  body += ",\"mdns_discovery\":true";
#if defined(SMART_TIMER_RELAY)
  body += ",\"relay\":true,\"relay_experimental\":true";
#endif
#if defined(SMART_TIMER_HARDWARE_V2)
  body += ",\"bluetooth_low_energy\":true,\"bluetooth_read_only\":false";
  body += ",\"bluetooth_control\":true,\"bluetooth_authenticated_control\":true";
  body += ",\"bluetooth_notifications\":true,\"bluetooth_peer_discovery\":false";
  body += ",\"bluetooth_bonding\":true,\"bluetooth_command_integrity\":true";
  body += ",\"bluetooth_physical_pairing\":true";
  body += ",\"bluetooth_event_notifications\":true,\"bluetooth_event_replay\":true";
  body += ",\"bluetooth_pairing_commit\":true,\"bluetooth_identify\":true";
  body += ",\"bluetooth_system_information\":true";
  body += ",\"bluetooth_peer_control\":false";
#else
  body += ",\"bluetooth_low_energy\":false,\"bluetooth_read_only\":false";
  body += ",\"bluetooth_control\":false,\"bluetooth_authenticated_control\":false";
  body += ",\"bluetooth_notifications\":false,\"bluetooth_peer_discovery\":false";
  body += ",\"bluetooth_bonding\":false,\"bluetooth_command_integrity\":false";
  body += ",\"bluetooth_physical_pairing\":false";
  body += ",\"bluetooth_event_notifications\":false,\"bluetooth_event_replay\":false";
  body += ",\"bluetooth_pairing_commit\":false,\"bluetooth_identify\":false";
  body += ",\"bluetooth_system_information\":false";
  body += ",\"bluetooth_peer_control\":false";
#endif
  body += ",\"sse\":true,\"sse_opt_in\":false,\"sse_ui_auto_connect\":true";
  body += ",\"ota\":true,\"legacy_api\":true}";
  body += ",\"limits\":{\"ap_clients\":" +
          String(network_.accessPointClientLimit()) +
          ",\"sse_clients\":" + String(kEventStreamClientLimit) + "}";
  body += ",\"sse\":{\"path\":\"/api/v1/events/stream\",\"default_enabled\":true,\"persistent\":false,\"rest_fallback\":true}";
  body += ",\"mutations\":{\"operation_id_max_length\":64,\"deduplication_entries\":8,\"scope\":\"current_boot\",\"boot_precondition_field\":\"if_boot_id\",\"session_precondition_field\":\"if_session_id\"}";
  body += ",\"events\":{\"history_entries\":16,\"cursor_parameter\":\"after\",\"boot_parameter\":\"boot_id\"}";
  body += ",\"discovery\":{\"mdns_service\":\"_smarttimer._tcp.local\",\"port\":80}";
#if defined(SMART_TIMER_HARDWARE_V2)
  body += ",\"bluetooth\":{\"protocol\":\"ost-ble/1\",\"mode\":\"bonded_authenticated_control\",\"inbound_connections\":1,\"controller_connections\":1,\"approved_app_limit\":3,\"status_notifications_ms\":15000,\"default_enabled\":false,\"persistent_enable\":true,\"control_volatile\":false,\"control_authentication\":\"bonded-hmac-sha256-112\",\"command_integrity\":\"hmac-sha256-64-counter\",\"capabilities_frame_bytes\":24,\"command_frame_bytes\":32,\"result_frame_bytes\":20,\"status_frame_bytes\":20,\"event_frame_bytes\":20,\"required_control_mtu\":35,\"event_history_entries\":16,\"system_information_frame_bytes\":44,\"system_information_uuid\":\"f00df299-464f-4921-9285-7749593b1144\",\"pairing_commit\":\"hmac-sha256-112\",\"path\":\"/api/v1/bluetooth\",\"enable_path\":\"/api/v1/bluetooth/enable\",\"disable_path\":\"/api/v1/bluetooth/disable\",\"pairing_start_path\":\"/api/v1/bluetooth/pairing/start\",\"pairing_cancel_path\":\"/api/v1/bluetooth/pairing/cancel\",\"pairing_revoke_path\":\"/api/v1/bluetooth/pairing/revoke\"}";
#endif
  body += ",\"ota\":{\"physical_arm_required\":true,\"firmware_only\":true";
  body += ",\"unverified_override\":true,\"unverified_override_volatile\":true";
  body += ",\"unverified_override_uploads\":1}";
  body += ",\"updates\":{\"configured\":";
  body += ReleaseUpdate::configured() ? "true" : "false";
  body += ",\"base_url\":\"" SMART_TIMER_UPDATE_BASE_URL "\",\"public_key\":\"" SMART_TIMER_UPDATE_PUBLIC_KEY "\"";
  body += ",\"target\":\"" + String(ReleaseUpdate::target()) + "\"";
  body += ",\"check_mode\":\"manual_browser\"}";
  body += "}";
  sendJson(200, body);
}

void WebApplication::sendDeviceName() {
  String body;
  body.reserve(180);
  body = "{\"name\":\"" + jsonEscape(network_.deviceName()) + "\"";
  body += ",\"hostname\":\"" + jsonEscape(network_.hostName()) + "\"";
  body += ",\"ap_ssid\":\"" + jsonEscape(network_.accessPointName()) + "\"";
  body += ",\"ap_uses_device_name\":" +
          String(network_.accessPointUsesDeviceName() ? "true" : "false") +
          "}";
  sendJson(200, body);
}

void WebApplication::sendMutationStatus(const String& action, bool changed,
                                        bool duplicate,
                                        const String& operationId) {
  const uint32_t nowMs = millis();
  const String body = statusJson(nowMs, &action, changed, duplicate, &operationId);
  if (body.length() == 0) {
    sendJson(503, "{\"error\":\"result unavailable; read status before retrying\",\"code\":\"response_unavailable\"}");
    return;
  }
  sendJson(200, body);
  if (network_.stationConnected()) {
    network_.acknowledgeStationHandoff();
  }
}

String WebApplication::jsonEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t index = 0; index < input.length(); ++index) {
    const char character = input[index];
    if (character == '"' || character == '\\') {
      output += '\\';
    }
    if (static_cast<uint8_t>(character) >= 0x20) {
      output += character;
    }
  }
  return output;
}

const char* WebApplication::stateName(uint8_t state) {
  switch (static_cast<TimerState>(state)) {
    case TimerState::Running:
      return "running";
    case TimerState::Paused:
      return "paused";
    case TimerState::Open:
    default:
      return "open";
  }
}
