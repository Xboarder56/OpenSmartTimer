#pragma once
// Host stub of ESP-IDF's esp_transport API for transport tests only.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct esp_transport_item_t* esp_transport_handle_t;
int esp_transport_connect(esp_transport_handle_t t, const char* host, int port, int timeout_ms);
int esp_transport_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms);
int esp_transport_poll_read(esp_transport_handle_t t, int timeout_ms);
int esp_transport_close(esp_transport_handle_t t);
int esp_transport_destroy(esp_transport_handle_t t);
#ifdef __cplusplus
}
#endif
