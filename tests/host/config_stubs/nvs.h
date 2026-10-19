#pragma once
#include <cassert>
#include <cstring>
#include <vector>
using nvs_handle_t = int;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, NVS_READWRITE = 2;
namespace fake {
inline std::vector<uint8_t> durable, staged;
inline bool failSet = false, failCommit = false;
inline unsigned commits = 0;
}
inline int nvs_open(const char* name, int, nvs_handle_t* handle) {
  assert(!strcmp(name, "opensmarttimer")); *handle = 1; return ESP_OK;
}
inline void nvs_close(nvs_handle_t) { fake::staged.clear(); }
inline int nvs_get_blob(nvs_handle_t, const char* name, void* out, size_t* size) {
  assert(!strcmp(name, "ble_apps"));
  if (fake::durable.empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (*size < fake::durable.size()) return -1;
  *size = fake::durable.size(); memcpy(out, fake::durable.data(), *size); return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t, const char* name, const void* data, size_t size) {
  assert(!strcmp(name, "ble_apps"));
  if (fake::failSet) return -1;
  const auto* bytes = static_cast<const uint8_t*>(data);
  fake::staged.assign(bytes, bytes + size); return ESP_OK;
}
inline int nvs_commit(nvs_handle_t) {
  if (fake::failCommit) return -1;
  fake::durable = fake::staged; ++fake::commits; return ESP_OK;
}
