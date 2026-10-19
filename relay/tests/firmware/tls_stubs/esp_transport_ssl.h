#pragma once
// Host contract doubles; real ESP TLS handshakes are hardware qualification.
#include "esp_transport.h"
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
esp_transport_handle_t esp_transport_ssl_init();
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t, esp_err_t (*)(void*));
