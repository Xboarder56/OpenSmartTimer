#if defined(ESP_PLATFORM)
#include "EspPlatform.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "ITimerControl.h"
#include "TlsArena.h"

namespace opensmarttimer { namespace relay {

uint32_t EspClock::uptime_ms() { return uint32_t(esp_timer_get_time() / 1000); }

void EspEntropy::fill(uint8_t* out, size_t len) { esp_fill_random(out, len); }

namespace {
// Missing and unreadable are both "not present" to callers; a write that
// cannot commit is reported as a failure.
bool withHandle(const char* ns, nvs_open_mode_t mode, nvs_handle_t* handle) {
    return nvs_open(ns, mode, handle) == ESP_OK;
}
}  // namespace

bool EspNvs::readBlob(const char* ns, const char* key, uint8_t* out, size_t* inout_len) {
    nvs_handle_t handle;
    if (!withHandle(ns, NVS_READONLY, &handle)) return false;
    const bool ok = nvs_get_blob(handle, key, out, inout_len) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool EspNvs::writeBlob(const char* ns, const char* key, const uint8_t* data, size_t len) {
    if (!writable()) return false;
    nvs_handle_t handle;
    if (!withHandle(ns, NVS_READWRITE, &handle)) return false;
    const bool ok = nvs_set_blob(handle, key, data, len) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool EspNvs::eraseKey(const char* ns, const char* key) {
    if (!writable()) return false;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(ns, NVS_READWRITE, &handle);
    if (opened != ESP_OK) return opened == ESP_ERR_NVS_NOT_FOUND;
    const esp_err_t erased = nvs_erase_key(handle, key);
    const bool ok = (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

bool EspNvs::eraseNamespace(const char* ns) {
    if (!writable()) return false;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(ns, NVS_READWRITE, &handle);
    if (opened != ESP_OK) return opened == ESP_ERR_NVS_NOT_FOUND;
    const bool ok = nvs_erase_all(handle) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

void RelayIdentity::setBootId(uint32_t boot_id) {
    snprintf(boot_id_, sizeof(boot_id_), "%lx", static_cast<unsigned long>(boot_id));
}

void RelayIdentity::setTimerId(const char* timer_id) {
    strncpy(timer_id_, timer_id ? timer_id : "", sizeof(timer_id_) - 1);
    timer_id_[sizeof(timer_id_) - 1] = '\0';
}

// With a reserved TLS arena, a new connection allocates there, so the gate
// measures the arena rather than the general heap.
size_t heapLargestFreeBlock() {
    return TlsArena::reserved() ? TlsArena::largestFreeBlock()
                                : heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}
size_t heapFree() {
    return TlsArena::reserved() ? TlsArena::freeBytes() : heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

}}  // namespace opensmarttimer::relay
#endif
