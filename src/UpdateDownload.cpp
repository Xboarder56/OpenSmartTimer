#include "UpdateDownload.h"

#if defined(SMART_TIMER_HARDWARE_V2) && defined(ESP_PLATFORM)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReleaseUpdateConfig.h"
#include "TlsArena.h"
#include "UpdateFeedRecord.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace UpdateDownload {
namespace {

// TlsArena serves this task as well as the relay's (the relay is paused
// while a download runs).
constexpr char kTaskName[] = "ota-download";
constexpr uint32_t kStackBytes = 12288;
constexpr UBaseType_t kTaskPriority = 1;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kReadTimeoutMs = 15000;
constexpr uint32_t kHandoffTimeoutMs = 15000;
constexpr uint32_t kArenaWaitMs = 8000;
constexpr size_t kArenaIdleBytes = 40 * 1024;
constexpr uint32_t kRecordMaxBytes = 16384;
constexpr size_t kHeaderMaxBytes = 3072;
constexpr char kFileLabel[] = "MagBoundV2-ESP32C3";

portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
// Guarded by lock.
Progress shared;
uint8_t mailbox[kChunkBytes];
size_t mailboxLength = 0;
bool mailboxFull = false;
bool taskRunning = false;
// Written by the task before it publishes Ready; read by loop() after.
ReleaseUpdate::Descriptor verified;
char imageFile[sizeof(UpdateFeed::Record::file)];
// Written by start() before the task exists.
char expectedVersion[ReleaseUpdate::kVersionBytes];
char installedVersion[ReleaseUpdate::kVersionBytes];
volatile bool imageGo = false;
volatile bool stopRequested = false;
const char* sinkError = nullptr;
mbedtls_x509_crt emptyChain;

void setPhase(Phase phase) {
  portENTER_CRITICAL(&lock);
  shared.phase = phase;
  portEXIT_CRITICAL(&lock);
}

void fail(const char* code) {
  portENTER_CRITICAL(&lock);
  if (shared.phase != Phase::Failed) {
    shared.phase = Phase::Failed;
    strncpy(shared.error, code, sizeof(shared.error) - 1);
  }
  portEXIT_CRITICAL(&lock);
}

// The signed record, not the certificate, establishes trust; accepting the
// server's certificate keeps the CA bundle and a clock out of the image.
int acceptServer(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
  *flags = 0;
  return 0;
}

esp_err_t attachVerifier(void* conf) {
  mbedtls_ssl_config* ssl = static_cast<mbedtls_ssl_config*>(conf);
  mbedtls_x509_crt_init(&emptyChain);
  emptyChain.version = 3;  // non-empty marker, as esp_crt_bundle does
  mbedtls_ssl_conf_ca_chain(ssl, &emptyChain, nullptr);
  mbedtls_ssl_conf_verify(ssl, acceptServer, nullptr);
  return ESP_OK;
}

struct Site {
  char host[64];
  char basePath[64];
};

bool parseSite(Site& site) {
  const char* base = SMART_TIMER_UPDATE_BASE_URL;
  if (strncmp(base, "https://", 8) != 0) return false;
  const char* host = base + 8;
  const char* slash = strchr(host, '/');
  if (slash == nullptr || static_cast<size_t>(slash - host) >= sizeof(site.host)) return false;
  memcpy(site.host, host, slash - host);
  site.host[slash - host] = '\0';
  const size_t pathLength = strlen(slash);
  if (pathLength >= sizeof(site.basePath) || slash[pathLength - 1] != '/') return false;
  memcpy(site.basePath, slash, pathLength + 1);
  return true;
}

using Sink = bool (*)(void*, const uint8_t*, size_t);

// One HTTP/1.1 GET with a declared Content-Length (chunked replies are
// refused). exact requires the declared length to equal maxLength.
bool get(const Site& site, const char* path, uint32_t maxLength, bool exact,
         Sink sink, void* context, const char*& error) {
  esp_transport_handle_t transport = esp_transport_ssl_init();
  if (transport == nullptr) {
    error = "memory";
    return false;
  }
  esp_transport_ssl_crt_bundle_attach(transport, attachVerifier);
  bool ok = false;
  do {
    if (esp_transport_connect(transport, site.host, 443, kConnectTimeoutMs) < 0) {
      error = "unreachable";
      break;
    }
    char request[256];
    const int requestLength = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: OpenSmartTimer\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", path, site.host);
    if (requestLength <= 0 || static_cast<size_t>(requestLength) >= sizeof(request) ||
        esp_transport_write(transport, request, requestLength, kReadTimeoutMs) != requestLength) {
      error = "unreachable";
      break;
    }
    char head[kHeaderMaxBytes + 1];
    size_t used = 0;
    char* end = nullptr;
    while (end == nullptr && used < kHeaderMaxBytes) {
      const int got = esp_transport_read(transport, head + used, kHeaderMaxBytes - used,
                                         kReadTimeoutMs);
      if (got <= 0) break;
      used += static_cast<size_t>(got);
      head[used] = '\0';
      end = strstr(head, "\r\n\r\n");
    }
    if (end == nullptr) {
      error = used == 0 ? "unreachable" : "bad_response";
      break;
    }
    const size_t bodyStart = static_cast<size_t>(end + 4 - head);
    *end = '\0';  // header text only, for the searches below
    if (strncmp(head, "HTTP/1.1 200 ", 13) != 0 && strncmp(head, "HTTP/1.0 200 ", 13) != 0) {
      error = strncmp(head + 9, "404", 3) == 0 ? "not_published" : "bad_response";
      break;
    }
    const char* declared = strcasestr(head, "\r\ncontent-length:");
    if (declared == nullptr || strcasestr(head, "\r\ntransfer-encoding:") != nullptr) {
      error = "bad_response";
      break;
    }
    char* numberEnd = nullptr;
    const unsigned long length = strtoul(declared + 17, &numberEnd, 10);
    if (numberEnd == declared + 17 || length > maxLength || (exact && length != maxLength)) {
      error = "bad_response";
      break;
    }
    uint32_t received = 0;
    size_t initial = used - bodyStart;
    if (initial > length) initial = length;
    if (initial != 0 && !sink(context, reinterpret_cast<uint8_t*>(head + bodyStart), initial)) {
      error = sinkError;
      break;
    }
    received = static_cast<uint32_t>(initial);
    uint8_t buffer[kChunkBytes];
    while (received < length) {
      if (stopRequested) {
        error = "cancelled";
        break;
      }
      size_t want = length - received;
      if (want > sizeof(buffer)) want = sizeof(buffer);
      const int got = esp_transport_read(transport, reinterpret_cast<char*>(buffer),
                                         static_cast<int>(want), kReadTimeoutMs);
      if (got <= 0) {
        error = "download_failed";
        break;
      }
      if (!sink(context, buffer, static_cast<size_t>(got))) {
        error = sinkError;
        break;
      }
      received += static_cast<uint32_t>(got);
    }
    ok = received == length;
  } while (false);
  esp_transport_close(transport);
  esp_transport_destroy(transport);
  return ok;
}

bool recordSink(void* context, const uint8_t* data, size_t length) {
  auto* parser = static_cast<UpdateFeed::Parser*>(context);
  if (parser->feed(data, length) == UpdateFeed::Result::Invalid) {
    sinkError = "record_invalid";
    return false;
  }
  return true;
}

// Hands bytes to loop() one mailbox load at a time.
bool imageSink(void*, const uint8_t* data, size_t length) {
  while (length != 0) {
    const size_t piece = length < kChunkBytes ? length : kChunkBytes;
    const TickType_t started = xTaskGetTickCount();
    for (;;) {
      if (stopRequested) {
        sinkError = "cancelled";
        return false;
      }
      portENTER_CRITICAL(&lock);
      const bool free = !mailboxFull;
      if (free) {
        memcpy(mailbox, data, piece);
        mailboxLength = piece;
        mailboxFull = true;
        shared.receivedBytes += static_cast<uint32_t>(piece);
      }
      portEXIT_CRITICAL(&lock);
      if (free) break;
      if (xTaskGetTickCount() - started > pdMS_TO_TICKS(kHandoffTimeoutMs)) {
        sinkError = "install_stalled";
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    data += piece;
    length -= piece;
  }
  return true;
}

bool waitFor(volatile bool& flag, uint32_t timeoutMs) {
  const TickType_t started = xTaskGetTickCount();
  while (!flag && !stopRequested) {
    if (xTaskGetTickCount() - started > pdMS_TO_TICKS(timeoutMs)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return flag && !stopRequested;
}

void run() {
  Site site;
  if (!parseSite(site)) {
    fail("not_configured");
    return;
  }
  // The relay pauses while a download runs; give it a moment to release
  // its TLS memory so this connection can use the same reserved block.
  if (TlsArena::reserved()) {
    const TickType_t started = xTaskGetTickCount();
    while (TlsArena::freeBytes() < kArenaIdleBytes && !stopRequested &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(kArenaWaitMs)) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  char path[160];
  snprintf(path, sizeof(path), "%supdates/%s.json", site.basePath, ReleaseUpdate::target());
  UpdateFeed::Parser parser;
  const char* error = "download_failed";
  if (!get(site, path, kRecordMaxBytes, false, recordSink, &parser, error)) {
    fail(error);
    return;
  }
  const UpdateFeed::Record& record = parser.record();
  if (parser.finish() != UpdateFeed::Result::Complete ||
      !UpdateFeed::valid(record, ReleaseUpdate::target(), kFileLabel) ||
      !ReleaseUpdate::validVersion(record.version)) {
    fail("record_invalid");
    return;
  }
  if (expectedVersion[0] != '\0' && strcmp(expectedVersion, record.version) != 0) {
    fail("version_changed");
    return;
  }
  if (!ReleaseUpdate::newer(record.version, installedVersion)) {
    fail("up_to_date");
    return;
  }
  ReleaseUpdate::Descriptor descriptor;
  strncpy(descriptor.version, record.version, sizeof(descriptor.version) - 1);
  descriptor.size = record.size;
  descriptor.crc32 = static_cast<uint32_t>(strtoul(record.crc32, nullptr, 16));
  if (!ReleaseUpdate::decodeHex(record.sha256, descriptor.sha256, sizeof(descriptor.sha256)) ||
      !ReleaseUpdate::decodeHex(record.signature, descriptor.signature,
                                sizeof(descriptor.signature))) {
    fail("record_invalid");
    return;
  }
  if (!ReleaseUpdate::hasVerificationMemory()) {
    fail("memory");
    return;
  }
  if (!ReleaseUpdate::verify(descriptor, ReleaseUpdate::target(), SMART_TIMER_UPDATE_PUBLIC_KEY)) {
    fail("signature_invalid");
    return;
  }
  verified = descriptor;
  strncpy(imageFile, record.file, sizeof(imageFile) - 1);
  portENTER_CRITICAL(&lock);
  shared.totalBytes = descriptor.size;
  strncpy(shared.version, descriptor.version, sizeof(shared.version) - 1);
  shared.phase = Phase::Ready;
  portEXIT_CRITICAL(&lock);

  if (!waitFor(imageGo, kHandoffTimeoutMs)) {
    fail(stopRequested ? "cancelled" : "install_not_started");
    return;
  }
  setPhase(Phase::Downloading);
  snprintf(path, sizeof(path), "%supdates/images/%s", site.basePath, imageFile);
  if (!get(site, path, descriptor.size, true, imageSink, nullptr, error)) {
    fail(error);
    return;
  }
  // Wait until loop() has taken the last chunk.
  const TickType_t started = xTaskGetTickCount();
  for (;;) {
    portENTER_CRITICAL(&lock);
    const bool empty = !mailboxFull;
    portEXIT_CRITICAL(&lock);
    if (empty) break;
    if (stopRequested || xTaskGetTickCount() - started > pdMS_TO_TICKS(kHandoffTimeoutMs)) {
      fail(stopRequested ? "cancelled" : "install_stalled");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  setPhase(Phase::Downloaded);
}

void taskEntry(void*) {
  run();
  portENTER_CRITICAL(&lock);
  taskRunning = false;
  portEXIT_CRITICAL(&lock);
  vTaskDelete(nullptr);
}

}  // namespace

bool start(const char* expected, const char* installed) {
  portENTER_CRITICAL(&lock);
  const bool busy = taskRunning || (shared.phase != Phase::Idle && shared.phase != Phase::Failed);
  if (!busy) {
    taskRunning = true;
    shared = Progress();
    shared.phase = Phase::Checking;
    mailboxFull = false;
    mailboxLength = 0;
  }
  portEXIT_CRITICAL(&lock);
  if (busy) return false;
  strncpy(expectedVersion, expected ? expected : "", sizeof(expectedVersion) - 1);
  expectedVersion[sizeof(expectedVersion) - 1] = '\0';
  strncpy(installedVersion, installed, sizeof(installedVersion) - 1);
  installedVersion[sizeof(installedVersion) - 1] = '\0';
  imageGo = false;
  stopRequested = false;
  sinkError = nullptr;
  if (xTaskCreate(&taskEntry, kTaskName, kStackBytes, nullptr, kTaskPriority, nullptr) != pdPASS) {
    portENTER_CRITICAL(&lock);
    taskRunning = false;
    shared.phase = Phase::Failed;
    strncpy(shared.error, "memory", sizeof(shared.error) - 1);
    portEXIT_CRITICAL(&lock);
    return false;
  }
  return true;
}

Progress progress() {
  portENTER_CRITICAL(&lock);
  const Progress copy = shared;
  portEXIT_CRITICAL(&lock);
  return copy;
}

bool active() {
  const Phase phase = progress().phase;
  return phase == Phase::Checking || phase == Phase::Ready ||
         phase == Phase::Downloading || phase == Phase::Downloaded;
}

bool descriptor(ReleaseUpdate::Descriptor& out) {
  if (progress().phase != Phase::Ready) return false;
  out = verified;
  return true;
}

void beginImage() { imageGo = true; }

bool takeChunk(uint8_t* out, size_t& length) {
  portENTER_CRITICAL(&lock);
  const bool full = mailboxFull;
  if (full) {
    memcpy(out, mailbox, mailboxLength);
    length = mailboxLength;
    mailboxFull = false;
  }
  portEXIT_CRITICAL(&lock);
  return full;
}

void finish(bool installed, const char* error) {
  stopRequested = true;
  portENTER_CRITICAL(&lock);
  if (installed) {
    shared.phase = Phase::Downloaded;  // reported as installing until the restart
  } else if (shared.phase != Phase::Failed) {
    shared.phase = Phase::Failed;
    strncpy(shared.error, error ? error : "install_failed", sizeof(shared.error) - 1);
  }
  portEXIT_CRITICAL(&lock);
}

}  // namespace UpdateDownload

#endif
