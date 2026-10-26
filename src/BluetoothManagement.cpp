#include "BluetoothApplication.h"
#include "BleAccessPolicy.h"

#if defined(SMART_TIMER_HARDWARE_V2)

#include <cstring>

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include "DeviceConfig.h"
#include "HttpRequestFields.h"
#include "LocalNetwork.h"
#include "WebApplication.h"

extern "C" {
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
}

// Management extension, docs/BLUETOOTH_MANAGEMENT.md. GATT access runs on the
// NimBLE host task and only authenticates and queues a frame; loop() decrypts
// the secret and runs WebApplication's shared route handler.

namespace {
using namespace BleManagement;

bool routeFor(uint8_t opcode, WebApplication::ManagementRoute& route) {
  using Route = WebApplication::ManagementRoute;
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::WifiStatus: route = Route::WifiStatus; return true;
    case Opcode::AccessPoint: route = Route::AccessPoint; return true;
    case Opcode::DeviceName: route = Route::DeviceName; return true;
    case Opcode::Bluetooth: route = Route::Bluetooth; return true;
    case Opcode::Diagnostics: route = Route::Diagnostics; return true;
    case Opcode::Events: route = Route::Events; return true;
    case Opcode::Capabilities: route = Route::Capabilities; return true;
    case Opcode::Status: route = Route::Status; return true;
    case Opcode::WifiConfigure: route = Route::WifiConfigure; return true;
    case Opcode::WifiSuspend: route = Route::WifiSuspend; return true;
    case Opcode::WifiResume: route = Route::WifiResume; return true;
    case Opcode::WifiForget: route = Route::WifiForget; return true;
    case Opcode::AccessPointSet: route = Route::AccessPointSet; return true;
    case Opcode::DeviceNameSet: route = Route::DeviceNameSet; return true;
    case Opcode::Restart: route = Route::Restart; return true;
    case Opcode::AppLabel: route = Route::AppLabel; return true;
    case Opcode::AppRevoke: route = Route::AppRevoke; return true;
    case Opcode::PairingStart: route = Route::PairingStart; return true;
    case Opcode::PairingCancel: route = Route::PairingCancel; return true;
    case Opcode::PairingRevoke: route = Route::PairingRevoke; return true;
    case Opcode::WifiScanStart: route = Route::WifiScanStart; return true;
    case Opcode::WifiScanResults: route = Route::WifiScanResults; return true;
    case Opcode::OtaBegin: route = Route::OtaBegin; return true;
    case Opcode::OtaFinish: route = Route::OtaFinish; return true;
    case Opcode::OtaAbort: route = Route::OtaAbort; return true;
    case Opcode::OtaDownload: route = Route::OtaDownload; return true;
    default: return false;
  }
}

const char* timerStateName(TimerState state) {
  switch (state) {
    case TimerState::Running: return "running";
    case TimerState::Paused: return "paused";
    case TimerState::Open:
    default: return "open";
  }
}

bool sha256(const uint8_t* data, size_t length, uint8_t output[32]) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
                  mbedtls_sha256_update(&context, data, length) == 0 &&
                  mbedtls_sha256_finish(&context, output) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}
}  // namespace

bool BluetoothApplication::hmacParts(const uint8_t* key, size_t keyLength,
                                     const uint8_t* first, size_t firstLength,
                                     const uint8_t* second, size_t secondLength,
                                     const uint8_t* third, size_t thirdLength,
                                     uint8_t* output) {
  // Same construction as controlHmac(), over up to three concatenated parts.
  uint8_t innerPad[64];
  uint8_t outerPad[64];
  for (size_t index = 0; index < sizeof(innerPad); ++index) {
    const uint8_t keyByte = index < keyLength ? key[index] : 0;
    innerPad[index] = keyByte ^ 0x36;
    outerPad[index] = keyByte ^ 0x5c;
  }
  uint8_t innerDigest[32]{};
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool success = mbedtls_sha256_starts(&context, 0) == 0 &&
                 mbedtls_sha256_update(&context, innerPad, sizeof(innerPad)) == 0 &&
                 (firstLength == 0 || mbedtls_sha256_update(&context, first, firstLength) == 0) &&
                 (secondLength == 0 || mbedtls_sha256_update(&context, second, secondLength) == 0) &&
                 (thirdLength == 0 || mbedtls_sha256_update(&context, third, thirdLength) == 0) &&
                 mbedtls_sha256_finish(&context, innerDigest) == 0;
  mbedtls_sha256_free(&context);
  if (success) {
    mbedtls_sha256_init(&context);
    success = mbedtls_sha256_starts(&context, 0) == 0 &&
              mbedtls_sha256_update(&context, outerPad, sizeof(outerPad)) == 0 &&
              mbedtls_sha256_update(&context, innerDigest, sizeof(innerDigest)) == 0 &&
              mbedtls_sha256_finish(&context, output) == 0;
    mbedtls_sha256_free(&context);
  }
  memset(innerPad, 0, sizeof(innerPad));
  memset(outerPad, 0, sizeof(outerPad));
  memset(innerDigest, 0, sizeof(innerDigest));
  return success;
}

int BluetoothApplication::onManagementWrite(uint16_t connHandle,
                                            ble_gatt_access_ctxt* context) {
  if (managementDisconnectAtMs_ != 0) return BleAccessPolicy::kApplicationAuthorizationRequired;
  if (!connected_ || connHandle != connectionHandle_ || !connectionEncrypted_ ||
      !connectionBonded_ || !controlAuthenticated_) {
    managementAssembler_.reset();
    return BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
  }
  uint16_t copied = 0;
  if (ble_hs_mbuf_to_flat(context->om, managementScratch_, sizeof(managementScratch_),
                          &copied) != 0) {
    managementAssembler_.reset();
    return kAttFragmentError;
  }
  // A new request cannot start while the previous one waits for loop().
  if (copied >= 1 && managementScratch_[0] == 0 && pendingManagement_) {
    return kAttBusy;
  }
  switch (managementAssembler_.add(managementScratch_, copied)) {
    case Assembler::Result::Pending: return 0;
    case Assembler::Result::SequenceError: return kAttFragmentError;
    case Assembler::Result::Complete: break;
  }
  const uint8_t* frame = managementAssembler_.data();
  const size_t length = managementAssembler_.length();
  Request request;
  if (!parseRequest(frame, length, request) || !knownOpcode(request.opcode) ||
      (request.secretLength != 0 && !secretOpcode(request.opcode))) {
    managementAssembler_.reset();
    return kAttMalformed;
  }

  uint8_t sessionKey[kSessionKeySize]{};
  uint8_t previousDigest[32]{};
  portENTER_CRITICAL(&controlMux_);
  memcpy(sessionKey, sessionKey_, sizeof(sessionKey));
  const uint32_t previousCounter = lastCommandCounter_;
  const uint32_t previousManagementCounter = lastManagementCounter_;
  memcpy(previousDigest, lastManagementDigest_, sizeof(previousDigest));
  portEXIT_CRITICAL(&controlMux_);

  uint8_t mac[32]{};
  uint8_t digest[32]{};
  const bool macOk =
      hmacParts(sessionKey, sizeof(sessionKey),
                reinterpret_cast<const uint8_t*>(kRequestLabel), sizeof(kRequestLabel) - 1,
                frame, request.signedLength, nullptr, 0, mac) &&
      constantTimeEqual(mac, request.mac, kMacBytes);
  const bool digestOk = sha256(frame, length, digest);
  memset(sessionKey, 0, sizeof(sessionKey));
  memset(mac, 0, sizeof(mac));
  const bool exactRetry = request.counter == previousCounter &&
                          request.counter == previousManagementCounter &&
                          digestOk && memcmp(digest, previousDigest, sizeof(digest)) == 0;
  if (!macOk || !digestOk || !(request.counter > previousCounter || exactRetry)) {
    managementAssembler_.reset();
    failAuthentication(millis());
    return BleAccessPolicy::kApplicationAuthorizationRequired;
  }

  bool queued = false;
  portENTER_CRITICAL(&controlMux_);
  if (!pendingManagement_ && controlAuthenticated_) {
    if (!exactRetry) {
      lastCommandCounter_ = request.counter;
      lastManagementCounter_ = request.counter;
      memcpy(lastManagementDigest_, digest, sizeof(digest));
    }
    memcpy(pendingManagementFrame_, frame, length);
    pendingManagementLength_ = length;
    pendingManagementGeneration_ = authorizationGeneration_;
    pendingManagementAppId_ = authenticatedAppId_;
    pendingManagementAppSlot_ = authenticatedAppSlot_;
    pendingManagement_ = true;
    queued = true;
  }
  portEXIT_CRITICAL(&controlMux_);
  managementAssembler_.reset();
  return queued ? 0 : kAttBusy;
}

int BluetoothApplication::onManagementRead(uint16_t connHandle,
                                           ble_gatt_access_ctxt* context) {
  const bool receipt = managementReceiptReadable(connHandle, millis());
  if (!connected_ || connHandle != connectionHandle_ || !connectionEncrypted_ ||
      !connectionBonded_ || (!controlAuthenticated_ && !receipt) ||
      (managementDisconnectAtMs_ != 0 && !receipt)) {
    return BleAccessPolicy::authorizationError(connectionEncrypted_, connectionBonded_);
  }
  portENTER_CRITICAL(&controlMux_);
  const size_t length = managementResponseLength_;
  memcpy(managementScratch_, managementResponseValue_, length);
  portEXIT_CRITICAL(&controlMux_);
  // NimBLE serves read-blob offsets from the full appended value.
  return os_mbuf_append(context->om, managementScratch_, length) == 0
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}

void BluetoothApplication::clearManagementState() {
  // Host task only (connection changes and authentication).
  portENTER_CRITICAL(&controlMux_);
  pendingManagement_ = false;
  pendingManagementLength_ = 0;
  managementResponseLength_ = 0;
  managementDisconnectAtMs_ = 0;
  managementTerminalReplyReady_ = false;
  managementTerminalConnection_ = BLE_HS_CONN_HANDLE_NONE;
  lastManagementCounter_ = 0;
  memset(lastManagementDigest_, 0, sizeof(lastManagementDigest_));
  memset(pendingManagementFrame_, 0, sizeof(pendingManagementFrame_));
  portEXIT_CRITICAL(&controlMux_);
  managementAssembler_.reset();
}

void BluetoothApplication::deferManagementDisconnect() {
  managementTerminalReplyReady_ = false;
  managementTerminalConnection_ = connectionHandle_;
  const uint32_t deadline = millis() + BleAccessPolicy::kReceiptDrainMs;
  managementDisconnectAtMs_ = deadline == 0 ? 1 : deadline;
}

bool BluetoothApplication::managementReceiptReadable(uint16_t connection, uint32_t nowMs) const {
  return BleAccessPolicy::receiptReadable(nowMs, managementDisconnectAtMs_,
      managementTerminalReplyReady_, connected_, connectionEncrypted_,
      connection == connectionHandle_ && connection == managementTerminalConnection_);
}

void BluetoothApplication::processManagementRequest(uint32_t nowMs) {
  if (!pendingManagement_) return;
  uint8_t frame[kMaxFrameBytes];
  size_t length = 0;
  uint32_t generation = 0, appId = 0;
  uint8_t slot = 0;
  uint8_t sessionKey[kSessionKeySize]{};
  portENTER_CRITICAL(&controlMux_);
  length = pendingManagementLength_;
  memcpy(frame, pendingManagementFrame_, length);
  generation = pendingManagementGeneration_;
  appId = pendingManagementAppId_;
  slot = pendingManagementAppSlot_;
  memcpy(sessionKey, sessionKey_, sizeof(sessionKey));
  portEXIT_CRITICAL(&controlMux_);
  const auto finish = [&]() {
    memset(sessionKey, 0, sizeof(sessionKey));
    memset(frame, 0, sizeof(frame));
    portENTER_CRITICAL(&controlMux_);
    pendingManagement_ = false;
    pendingManagementLength_ = 0;
    portEXIT_CRITICAL(&controlMux_);
  };

  Request request;
  const bool authorized = connected_ && connectionEncrypted_ && connectionBonded_ &&
                          controlAuthenticated_ &&
                          generation == authorizationGeneration_ &&
                          appId == authenticatedAppId_ && slot < BleAppRegistry::kCapacity &&
                          config_.bluetoothApps().find(appId) == slot;
  if (!authorized || !parseRequest(frame, length, request) || managementHost_ == nullptr) {
    finish();
    return;
  }

  const auto errorBody = [&](const char* message, const char* code) {
    String body = "{\"error\":\"";
    body += message;
    body += "\",\"code\":\"";
    body += code;
    body += "\",\"state\":\"";
    body += timerStateName(timer_.snapshot(nowMs).state);
    body += "\"}";
    return body;
  };

  if (request.opcode == static_cast<uint8_t>(Opcode::Continue)) {
    HttpRequestFields fields;
    const auto* id = fields.parse(reinterpret_cast<const char*>(request.body), request.bodyLength)
                         ? fields.find("request_id") : nullptr;
    const auto* offsetField = id ? fields.find("offset") : nullptr;
    uint32_t requestId = 0, offset = 0;
    const bool valid = offsetField && fields.count() == 2 &&
        id->type == HttpRequestFields::Type::Integer &&
        offsetField->type == HttpRequestFields::Type::Integer &&
        HttpRequestFields::unsignedInteger(fields.value(*id), requestId) &&
        HttpRequestFields::unsignedInteger(fields.value(*offsetField), offset);
    if (valid && managementReplyGeneration_ == generation && requestId == managementRequestId_ &&
        offset < managementBody_.length() && publishManagementSegment(offset, request.counter)) {
      sendManagementReady();
    } else {
      storeManagementReply(request.opcode, request.requestId, request.counter, 409,
                           errorBody("no stored response has that request ID and offset",
                                     "response_unavailable"));
    }
    finish();
    return;
  }

  int status = 500;
  String body;
  if (request.bootId != bootId_) {
    status = 409;
    body = errorBody("the request's boot ID does not match the current device boot", "stale_boot");
  } else {
    uint8_t secret[kMaxSecretBytes]{};
    bool secretOk = true;
    if (request.secretLength != 0) {
      uint8_t encryptionKey[32]{};
      uint8_t iv[16];
      uint8_t streamBlock[16]{};
      size_t streamOffset = 0;
      secretIv(request.counter, request.requestId, iv);
      mbedtls_aes_context aes;
      mbedtls_aes_init(&aes);
      secretOk = hmacParts(sessionKey, sizeof(sessionKey),
                           reinterpret_cast<const uint8_t*>(kEncryptionLabel),
                           sizeof(kEncryptionLabel) - 1, nullptr, 0, nullptr, 0,
                           encryptionKey) &&
                 mbedtls_aes_setkey_enc(&aes, encryptionKey, 128) == 0 &&
                 mbedtls_aes_crypt_ctr(&aes, request.secretLength, &streamOffset, iv,
                                       streamBlock, request.secret, secret) == 0;
      mbedtls_aes_free(&aes);
      memset(encryptionKey, 0, sizeof(encryptionKey));
      memset(streamBlock, 0, sizeof(streamBlock));
    }

    // Remembered results make an exact retry safe after a lost reply.
    uint8_t semantic[32]{};
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    const bool hashed = mbedtls_sha256_starts(&hash, 0) == 0 &&
        mbedtls_sha256_update(&hash, frame + 1, 7) == 0 &&  // opcode, request ID, boot ID
        mbedtls_sha256_update(&hash, request.body, request.bodyLength) == 0 &&
        mbedtls_sha256_update(&hash, secret, request.secretLength) == 0 &&
        mbedtls_sha256_finish(&hash, semantic) == 0;
    mbedtls_sha256_free(&hash);
    ManagementResult* results = managementResults_[slot];
    ManagementResult* remembered = nullptr;
    bool conflict = false;
    if (mutatingOpcode(request.opcode)) {
      for (uint8_t i = 0; i < kManagementResultCapacity; ++i) {
        if (results[i].appId == appId && results[i].requestId == request.requestId) {
          if (memcmp(results[i].digest, semantic, sizeof(results[i].digest)) == 0) {
            remembered = &results[i];
          } else {
            conflict = true;
          }
          break;
        }
      }
    }

    WebApplication::ManagementRoute route;
    if (!secretOk || !hashed) {
      status = 500;
      body = errorBody("the request could not be decoded", "internal_error");
    } else if (conflict) {
      status = 409;
      body = errorBody("request_id was already used for a different request", "operation_id_conflict");
    } else if (remembered != nullptr) {
      status = remembered->status;
      body = remembered->body;
    } else if (request.opcode == static_cast<uint8_t>(Opcode::OtaChunk)) {
      if (request.bodyLength < 5) {
        status = 400;
        body = errorBody("a firmware chunk needs an offset and data", "invalid_ota_chunk");
      } else {
        const auto reply = managementHost_->manageOtaChunk(
            readLe32(request.body), request.body + 4, request.bodyLength - 4u);
        status = reply.status;
        body = reply.body;
      }
    } else if (request.opcode == static_cast<uint8_t>(Opcode::BluetoothOff)) {
      // Handled here so the reply is sent before the radio stops.
      if (!timer_.isOpen()) {
        status = 409;
        body = errorBody("Bluetooth disable requires an open timer", "timer_active");
      } else if (managementHost_->mutationTransitionActive(nowMs)) {
        status = 409;
        body = errorBody("device transition already pending", "request_failed");
      } else if (network_.wifiSuspended()) {
        status = 409;
        body = errorBody("restore Wi-Fi before turning Bluetooth off", "wifi_disabled");
      } else if (!config_.setBluetoothEnabled(false)) {
        status = 500;
        body = errorBody("Bluetooth setting could not be saved", "settings_save_failed");
      } else {
        // Persist before acknowledging. Keep the radio alive briefly to send
        // the receipt; the later shutdown must not perform another NVS write.
        status = 202;
        body = "{\"bluetooth_enabled\":false,\"disconnecting\":true}";
        disableRadioAtMs_ = (nowMs + 750) == 0 ? 1 : nowMs + 750;
      }
    } else if (routeFor(request.opcode, route)) {
      timer_.setBluetoothEventAppId(appId);
      managementDispatchActive_ = true;
      const auto reply = managementHost_->manage(
          route, reinterpret_cast<const char*>(request.body), request.bodyLength,
          reinterpret_cast<const char*>(secret), request.secretLength);
      managementDispatchActive_ = false;
      timer_.setBluetoothEventAppId(0);
      status = reply.status;
      body = reply.body;
      if (managementDisconnectAtMs_ != 0 && status >= 200 && status < 300) {
        // One segment, signed with the request's captured key after revocation
        // has already erased live authority. No continuation is needed/allowed.
        body = "{\"approval_removed\":true,\"disconnecting\":true}";
      }
    } else {
      status = 400;
      body = errorBody("unsupported management opcode", "unsupported_opcode");
    }
    memset(secret, 0, sizeof(secret));

    if (body.length() > kMaxBodyBytes) {
      status = 413;
      body = errorBody("the response exceeds the Bluetooth management limit", "response_too_large");
    }
    if (mutatingOpcode(request.opcode) && remembered == nullptr && !conflict &&
        status >= 200 && status < 300) {
      uint8_t& cursor = managementResultCursor_[slot];
      ManagementResult& entry = results[cursor];
      entry.appId = appId;
      entry.requestId = request.requestId;
      memcpy(entry.digest, semantic, sizeof(entry.digest));
      entry.status = static_cast<uint16_t>(status);
      entry.body = body;
      cursor = static_cast<uint8_t>((cursor + 1) % kManagementResultCapacity);
    }
  }
  if (body.length() > kMaxBodyBytes) {
    status = 413;
    body = errorBody("the response exceeds the Bluetooth management limit", "response_too_large");
  }
  storeManagementReply(request.opcode, request.requestId, request.counter, status, body, sessionKey);
  finish();
}

void BluetoothApplication::storeManagementReply(uint8_t opcode, uint16_t requestId,
                                                uint32_t counter, int status,
                                                const String& body, const uint8_t* responseKey) {
  managementOpcode_ = opcode;
  managementRequestId_ = requestId;
  managementStatus_ = static_cast<uint16_t>(status);
  managementBody_ = body;
  managementReplyGeneration_ = authorizationGeneration_;
  if (publishManagementSegment(0, counter, responseKey)) {
    if (managementDisconnectAtMs_ != 0) managementTerminalReplyReady_ = true;
    sendManagementReady();
  }
}

bool BluetoothApplication::publishManagementSegment(uint16_t offset, uint32_t counter,
                                                    const uint8_t* responseKey) {
  const size_t total = managementBody_.length();
  if (offset > total) return false;
  const size_t segment = total - offset < kMaxSegmentBytes ? total - offset : kMaxSegmentBytes;
  uint8_t value[kMaxFrameBytes];
  writeResponseHeader(value, managementOpcode_, managementRequestId_, managementStatus_,
                      static_cast<uint16_t>(total), offset, static_cast<uint16_t>(segment));
  memcpy(value + kResponseHeaderBytes, managementBody_.c_str() + offset, segment);
  uint8_t sessionKey[kSessionKeySize]{};
  portENTER_CRITICAL(&controlMux_);
  memcpy(sessionKey, responseKey != nullptr ? responseKey : sessionKey_, sizeof(sessionKey));
  portEXIT_CRITICAL(&controlMux_);
  uint8_t counterBytes[4];
  writeLe32(counterBytes, counter);
  uint8_t mac[32]{};
  const bool ok = hmacParts(sessionKey, sizeof(sessionKey),
                            reinterpret_cast<const uint8_t*>(kResponseLabel),
                            sizeof(kResponseLabel) - 1, counterBytes, sizeof(counterBytes),
                            value, kResponseHeaderBytes + segment, mac);
  memset(sessionKey, 0, sizeof(sessionKey));
  if (!ok) return false;
  memcpy(value + kResponseHeaderBytes + segment, mac, kMacBytes);
  memset(mac, 0, sizeof(mac));
  portENTER_CRITICAL(&controlMux_);
  memcpy(managementResponseValue_, value, kResponseHeaderBytes + segment + kMacBytes);
  managementResponseLength_ = kResponseHeaderBytes + segment + kMacBytes;
  portEXIT_CRITICAL(&controlMux_);
  return true;
}

void BluetoothApplication::sendManagementReady() {
  if (!connected_ || (!controlAuthenticated_ && !managementReceiptReadable(connectionHandle_, millis())) ||
      !managementResponseSubscribed_ ||
      managementResponseValueHandle_ == 0 || connectionHandle_ == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  uint8_t ready[kReadyFrameBytes];
  writeReadyFrame(ready, managementOpcode_, managementRequestId_, managementStatus_,
                  static_cast<uint16_t>(managementBody_.length()));
  os_mbuf* packet = ble_hs_mbuf_from_flat(ready, sizeof(ready));
  if (packet == nullptr) {
    lastError_ = BLE_HS_ENOMEM;
    return;
  }
  const int result = ble_gatts_notify_custom(connectionHandle_, managementResponseValueHandle_, packet);
  if (result == 0) {
    notificationCount_ = notificationCount_ + 1;
  } else if (result != BLE_HS_ENOTCONN) {
    lastError_ = result;
  }
}

#endif  // SMART_TIMER_HARDWARE_V2
