#pragma once
// Host stub of esp_transport_ws.h (values match ESP-IDF 5.5).
#include <stdbool.h>
#include "esp_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum ws_transport_opcodes {
    WS_TRANSPORT_OPCODES_CONT = 0x00, WS_TRANSPORT_OPCODES_TEXT = 0x01,
    WS_TRANSPORT_OPCODES_BINARY = 0x02, WS_TRANSPORT_OPCODES_CLOSE = 0x08,
    WS_TRANSPORT_OPCODES_PING = 0x09, WS_TRANSPORT_OPCODES_PONG = 0x0a,
    WS_TRANSPORT_OPCODES_FIN = 0x80, WS_TRANSPORT_OPCODES_NONE = 0x100,
} ws_transport_opcodes_t;
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent);
int esp_transport_ws_set_path(esp_transport_handle_t t, const char* path);
int esp_transport_ws_set_subprotocol(esp_transport_handle_t t, const char* sub);
int esp_transport_ws_send_raw(esp_transport_handle_t t, ws_transport_opcodes_t opcode, const char* b, int len, int timeout_ms);
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t t);
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t t);
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t t);
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t t);
#ifdef __cplusplus
}
#endif
