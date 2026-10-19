#!/usr/bin/env python3
"""Serve the embedded UI with a small in-memory API for visual previewing."""

from argparse import ArgumentParser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
import json
import random
import re
import secrets
import time


PROJECT_ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = PROJECT_ROOT / "web"
VERSION_MATCH = re.search(
    r"(?m)^firmware_version\s*=\s*([0-9A-Za-z][0-9A-Za-z.-]*)\s*$",
    (PROJECT_ROOT / "platformio.ini").read_text(encoding="utf-8"),
)
if VERSION_MATCH is None:
    raise RuntimeError("platformio.ini must define firmware_version")
FIRMWARE_VERSION = VERSION_MATCH.group(1)
MAX_DURATION_MS = 24 * 60 * 60 * 1000
PREVIEW_BUILD_ID = "000000000000000000000000"


class PreviewState:
    def __init__(self, hardware_v2=False):
        self.hardware_v2 = bool(hardware_v2)
        self.sse_client_limit = 3 if self.hardware_v2 else 2  # matches the firmware
        self.started_at = time.monotonic()
        self.boot_id = f"{random.getrandbits(32):08x}"
        self.state_revision = 1
        self.event_sequence = 1
        self.session_sequence = 0
        self.sse_clients = 0
        self.sse_accepted_count = 0
        self.sse_closed_count = 0
        self.sse_last_close_reason = "none"
        self.state = "open"
        self.sealed = False
        self.pause_allowed = True
        self.control_lock_allowed = True
        self.emergency_input_active = False
        self.remaining_ms = 0
        self.session_duration_ms = 0
        self.deadline = 0.0
        self.device_name = "Smart Timer Preview"
        self.hostname = "smart-timer-preview"
        self.station_mac = "A4:CF:12:34:56:78"
        self.ap_uses_device_name = True
        self.ap_custom_ssid = ""
        self.ap_security = "open"
        self.ap_password_set = False
        self.wifi_state = "connected"
        self.connected_ssid = "Workshop-IoT"
        self.saved_ssid = "Workshop-IoT"
        self.last_wifi_error = ""
        self.wifi_connect_attempt_count = 1
        self.wifi_disconnect_count = 0
        self.wifi_unexpected_disconnect_count = 0
        self.wifi_dhcp_timeout_count = 0
        self.wifi_last_disconnect_reason = 0
        self.wifi_last_disconnect_reason_name = "none"
        self.wifi_last_disconnect_expected = False
        self.wifi_last_disconnect_at_ms = 0
        self.wifi_last_connected_at_ms = 1
        self.target_ssid = ""
        self.last_attempted_ssid = ""
        self.last_release_reason = "boot"
        self.scan_started_at = 0.0
        self.events = [{
            "id": 1, "type": "boot", "at_ms": 0, "value_ms": 0,
            "state": "open", "sealed": False,
            "emergency_input_active": False, "session_sequence": 0,
        }]
        self.recent_operations = {}
        self.recent_operation_order = []
        self.bluetooth_initialized = False
        self.bluetooth_enabled = False
        self.bluetooth_configured_enabled = False
        self.bluetooth_paired = False
        self.bluetooth_apps = []
        self.bluetooth_pairing_state = "idle"
        self.bluetooth_pairing_failure = "none"
        self.bluetooth_pairing_until = 0.0
        self.bluetooth_control_enabled = False
        self.bluetooth_control_authenticated = False
        self.bluetooth_control_key = ""
        self.bluetooth_control_command_count = 0
        # Experimental relay (V2): a local stand-in with no network activity.
        self.relay = {"enabled": False, "enrolled": False, "relay_host": "", "timer_id": "", "partner_origin": ""}
        self.relay_setup = None
        self.relay_setup_id = 0
        self.relay_setup_until = 0.0
        self.relay_setup_state = "idle"
        self.relay_links = []
        self.relay_job = {"id": 0, "state": "idle", "result": "ok", "fp_hash": "", "request": None}

    def ap_ssid(self):
        if self.ap_uses_device_name:
            return f"{self.hostname}-PREVIEW".upper()
        return self.ap_custom_ssid

    def uptime_ms(self):
        return round((time.monotonic() - self.started_at) * 1000)

    def event(self, event_type, value_ms=0):
        self.state_revision += 1
        self.event_sequence += 1
        self.events.append({
            "id": self.event_sequence,
            "type": event_type,
            "at_ms": self.uptime_ms(),
            "value_ms": value_ms,
            "state": self.state,
            "sealed": self.sealed,
            "emergency_input_active": self.emergency_input_active,
            "session_sequence": self.session_sequence,
        })
        self.events = self.events[-16:]

    def update(self):
        if self.state == "running":
            self.remaining_ms = max(0, round((self.deadline - time.monotonic()) * 1000))
            if self.remaining_ms == 0:
                self.open("expired")

    def open(self, reason="user"):
        self.state = "open"
        self.sealed = False
        self.remaining_ms = 0
        self.session_duration_ms = 0
        self.deadline = 0.0
        self.last_release_reason = reason
        event_type = {
            "expired": "expired",
            "emergency": "emergency_release",
        }.get(reason, "opened_by_user")
        self.event(event_type)

    def set_emergency_input(self, active):
        active = bool(active)
        if active == self.emergency_input_active:
            return
        self.emergency_input_active = active
        self.event("emergency_input_activated" if active else "emergency_input_cleared")
        if active and self.state != "open":
            self.open("emergency")

    def note_wifi_disconnect(self, reason=200, name="beacon_timeout", expected=False):
        self.wifi_disconnect_count += 1
        self.wifi_last_disconnect_reason = reason
        self.wifi_last_disconnect_reason_name = name
        self.wifi_last_disconnect_expected = bool(expected)
        self.wifi_last_disconnect_at_ms = self.uptime_ms()
        if not expected:
            self.wifi_unexpected_disconnect_count += 1
            self.event("wifi_disconnected", reason)
            self.events[-1].update({
                "wifi_disconnect_reason": reason,
                "wifi_disconnect_reason_name": name,
                "unexpected": True,
            })

    def start(self, duration_ms):
        if self.emergency_input_active:
            raise ValueError("Emergency input is active")
        if not 0 < duration_ms <= MAX_DURATION_MS:
            raise ValueError("Duration must be between one millisecond and 24 hours")
        self.state = "running"
        self.session_sequence += 1
        self.sealed = False
        self.remaining_ms = duration_ms
        self.session_duration_ms = duration_ms
        self.deadline = time.monotonic() + duration_ms / 1000
        self.event("started", duration_ms)

    def pause(self):
        self.update()
        self.state = "paused"
        self.event("paused", self.remaining_ms)

    def resume(self):
        self.deadline = time.monotonic() + self.remaining_ms / 1000
        self.state = "running"
        self.event("resumed", self.remaining_ms)

    def adjust(self, delta_ms):
        self.update()
        self.remaining_ms = max(1, min(MAX_DURATION_MS, self.remaining_ms + delta_ms))
        self.session_duration_ms = max(self.session_duration_ms, self.remaining_ms)
        if self.state == "running":
            self.deadline = time.monotonic() + self.remaining_ms / 1000
        self.event("adjusted", delta_ms)

    def snapshot(self):
        self.update()
        return {
            "state": self.state,
            "sealed": self.sealed,
            "pause_allowed": self.pause_allowed,
            "control_lock_allowed": self.control_lock_allowed,
            "emergency_input_active": self.emergency_input_active,
            "remaining_ms": self.remaining_ms,
            "session_duration_ms": self.session_duration_ms,
            "max_duration_ms": MAX_DURATION_MS,
            "api_version": "v1",
            "hardware_model": "MagBound Smart Timer V2" if self.hardware_v2 else "MagBound Smart Timer V1",
            "chip_family": "ESP32-C3" if self.hardware_v2 else "ESP8266",
            "build_id": PREVIEW_BUILD_ID,
            "station_connected": self.wifi_state == "connected",
            "station_ip": "192.168.1.42" if self.wifi_state == "connected" else "",
            "station_mac": self.station_mac,
            "ap_running": self.wifi_state != "connected",
            "ap_ssid": self.ap_ssid(),
            "ap_clients": 1 if self.wifi_state != "connected" else 0,
            "ap_security": self.ap_security,
            "ap_password_set": self.ap_password_set,
            "ap_uses_device_name": self.ap_uses_device_name,
            "version": f"{FIRMWARE_VERSION}-preview",
            "device_id": "preview2" if self.hardware_v2 else "preview1",
            "boot_id": self.boot_id,
            "session_id": f"{self.boot_id}-{self.session_sequence}" if self.session_sequence else None,
            "state_revision": self.state_revision,
            "latest_event_id": self.event_sequence,
            "device_name": self.device_name,
            "hostname": self.hostname,
            "uptime_ms": self.uptime_ms(),
            "server_time_ms": self.uptime_ms(),
            "last_release_reason": self.last_release_reason,
            "share_link_limit_ms": None,
            "ota_armed": False,
            "ota_armed_remaining_ms": 0,
            "ota_unverified_allowed": False,
            "identifying": False,
            "wifi_state": self.wifi_state,
            "connected_ssid": self.connected_ssid,
            "saved_ssid": self.saved_ssid,
            "target_ssid": self.target_ssid,
            "last_attempted_ssid": self.last_attempted_ssid,
            "rssi_dbm": -43 if self.wifi_state == "connected" else 0,
            "last_wifi_error": self.last_wifi_error,
            "retry_in_ms": 0,
            "network_mode": "ap" if not self.saved_ssid else "wifi",
            "ap_role": "off" if self.wifi_state == "connected" else ("primary" if not self.saved_ssid else "recovery"),
            "station_retry_paused": self.wifi_state != "connected" and bool(self.saved_ssid),
            **({"share_links_active": len(self.relay_links)} if self.hardware_v2 else {}),
        }

    def relay_status(self):
        if self.relay_setup and (time.monotonic() >= self.relay_setup_until or self.state != "open"):
            self.relay_setup_state = "expired" if self.state == "open" else "cancelled"
            self.relay_setup = None
        connected = self.relay["enabled"] and self.relay["enrolled"]
        return {
            "experimental": True, "enabled": self.relay["enabled"], "enrolled": self.relay["enrolled"],
            "running": self.relay["enabled"], "enrolling": False, "stopping": False, "maintenance": False,
            "paused_for_bluetooth": False,
            "partner_origin": self.relay["partner_origin"],
            "setup_pending": self.relay_setup is not None, "setup_state": self.relay_setup_state,
            "setup_request_id": self.relay_setup_id,
            "setup_remaining_ms": max(0, round((self.relay_setup_until - time.monotonic()) * 1000)) if self.relay_setup else 0,
            "setup_relay_origin": self.relay_setup[0] if self.relay_setup else "",
            "setup_partner_origin": self.relay_setup[1] if self.relay_setup else "",
            "boot_id": self.boot_id, "next_job_id": self.relay_job["id"] + 1,
            "job": {k: self.relay_job[k] for k in ("id", "state", "result", "fp_hash")},
            "state": "connected" if connected else ("disabled" if self.relay["enrolled"] else "not_enrolled"),
            "sharing_available": connected, "relay_host": self.relay["relay_host"],
            "timer_id": self.relay["timer_id"], "last_error": "", "reconnects": 0,
            "live_links": len(self.relay_links), "link_limit": 1, "task_stack_min_free_bytes": 6000 if connected else 0,
            "free_heap_bytes": 90000, "largest_free_block_bytes": 60000,
        }

    def approve_relay_setup(self):
        """In-process test gesture; deliberately has no HTTP route."""
        self.relay_status()
        if not self.relay_setup:
            return False
        origin, partner, _code, _pin = self.relay_setup
        self.relay.update(enabled=True, enrolled=True, relay_host=origin[8:].rstrip("/"),
                          timer_id="previewTimer01", partner_origin=partner)
        self.relay_setup = None
        self.relay_setup_state = "complete"
        self.relay_links.clear()
        return True

    def bluetooth_status(self):
        return {
            "supported": True,
            "protocol": "ost-ble/1",
            "mode": "bonded_authenticated_control",
            "default_enabled": False,
            "configured_enabled": self.bluetooth_configured_enabled,
            "persistent_enable": True,
            "volatile_enable": False,
            "enable_path": "/api/v1/bluetooth/enable",
            "disable_path": "/api/v1/bluetooth/disable",
            "advertised_name": "OST-ab1202" if self.bluetooth_initialized else "",
            "initialized": self.bluetooth_initialized,
            "enabled": self.bluetooth_enabled,
            "synchronized": self.bluetooth_initialized,
            "advertising": self.bluetooth_enabled,
            "connected": False,
            "subscribed": False,
            "connections": 0,
            "connection_limit": 1,
            "approved_app_limit": 3,
            "approved_app_count": len(self.bluetooth_apps),
            "approved_apps": self.bluetooth_apps,
            "active_app_id": 0,
            "accepted_count": 0,
            "closed_count": 0,
            "notification_count": 0,
            "last_error": 0,
            "last_error_name": "none",
            "last_disconnect_reason": 0,
            "last_disconnect_reason_name": "none",
            "connection_rssi_dbm": None,
            "connection_mtu": 23,
            "required_control_mtu": 35,
            "connection_mtu_sufficient": False,
            "connection_encrypted": False,
            "connection_bonded": False,
            "paired": self.bluetooth_paired,
            "pairing_state": self.bluetooth_pairing_state,
            "pairing_active": self.bluetooth_pairing_state not in {"idle", "complete", "failed"} and self.bluetooth_pairing_until > time.monotonic(),
            "pairing_remaining_ms": max(0, round((self.bluetooth_pairing_until - time.monotonic()) * 1000)),
            "pairing_failure": self.bluetooth_pairing_failure,
            "physical_pairing_supported": True,
            "peer_discovery_supported": False,
            "control_supported": True,
            "control_default_enabled": False,
            "control_volatile": False,
            "control_enabled": self.bluetooth_control_enabled,
            "control_authenticated": self.bluetooth_control_authenticated,
            "control_authentication": "bonded-hmac-sha256-112",
            "command_integrity": "hmac-sha256-64-counter",
            "control_authentication_failure_count": 0,
            "control_authentication_failure_limit": 5,
            "control_authentication_backoff_remaining_ms": 0,
            "control_command_count": self.bluetooth_control_command_count,
            "control_last_result": "unauthorized",
            "pairing_start_path": "/api/v1/bluetooth/pairing/start",
            "pairing_cancel_path": "/api/v1/bluetooth/pairing/cancel",
            "pairing_revoke_path": "/api/v1/bluetooth/pairing/revoke",
            "control_boot_id": 0x12345678,
            "bond_cleanup_pending": False,
            "bond_cleanup_last_error": 0,
            "peer_control_supported": False,
        }


STATE = PreviewState()


class PreviewHandler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_ROOT), **kwargs)

    def send_json(self, payload, status=200):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_form(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 2048:
            raise ValueError("Request exceeds preview field limit")
        body = self.rfile.read(length).decode()
        if "application/json" in self.headers.get("Content-Type", ""):
            def unique_fields(pairs):
                result = {}
                for key, value in pairs:
                    if key in result:
                        raise ValueError("Duplicate JSON field")
                    result[key] = value
                return result
            values = json.loads(body, object_pairs_hook=unique_fields)
            if not isinstance(values, dict) or len(values) > 16:
                raise ValueError("Expected bounded flat JSON object")
            if urlparse(self.path).query:
                raise ValueError("Do not mix JSON and query fields")
            strings = {"ssid", "password", "security", "name", "operation_id", "if_boot_id", "if_session_id"}
            numbers = {"duration_ms", "min_ms", "max_ms", "delta_ms", "image_size"}
            booleans = {"use_device_name", "keep_password", "standalone", "sealed", "allow_pause", "allow_control_lock"}
            for key, value in values.items():
                if key == "if_session_id" and value is None:
                    continue
                if ((key in strings and type(value) is not str) or
                    (key in numbers and type(value) is not int) or
                    (key in booleans and type(value) is not bool) or
                    isinstance(value, (dict, list, float)) or
                    (isinstance(value, str) and "\0" in value)):
                    raise ValueError("Invalid JSON field type")
            return values
        values = parse_qs(body + ("&" if body else "") + urlparse(self.path).query, keep_blank_values=True)
        if len(values) > 16 or any(len(item) != 1 for item in values.values()):
            raise ValueError("Duplicate or excessive form fields")
        return {key: item[-1] for key, item in values.items()}

    def send_error_json(self, message, status=400, code="request_failed"):
        self.send_json({"error": message, "code": code, "state": STATE.state}, status)

    def send_chunk(self, payload):
        self.wfile.write(f"{len(payload):x}\r\n".encode() + payload + b"\r\n")

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/v1/events/stream":
            if STATE.sse_clients >= STATE.sse_client_limit:
                self.send_error_json(
                    "Live-update client limit reached", 503,
                    "sse_client_limit"
                )
                return
            STATE.sse_clients += 1
            STATE.sse_accepted_count += 1
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Transfer-Encoding", "chunked")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                self.send_chunk(b"retry: 5000\n\n")
                last_revision = -1
                last_sent = 0.0
                while True:
                    snapshot = STATE.snapshot()
                    now = time.monotonic()
                    if snapshot["state_revision"] != last_revision or now - last_sent >= 15:
                        payload = json.dumps(snapshot, separators=(",", ":"))
                        frame = (
                            f"id: {snapshot['boot_id']}:{snapshot['latest_event_id']}:{snapshot['state_revision']}\n"
                            f"event: status\ndata: {payload}\n\n"
                        ).encode()
                        self.send_chunk(frame)
                        self.wfile.flush()
                        last_revision = snapshot["state_revision"]
                        last_sent = now
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                STATE.sse_clients = max(0, STATE.sse_clients - 1)
                STATE.sse_closed_count += 1
                STATE.sse_last_close_reason = "peer_closed"
            return
        if path == "/api/v1/status":
            self.send_json(STATE.snapshot())
            return
        if path == "/api/v1/wifi/scan":
            scanning = (
                STATE.scan_started_at > 0
                and time.monotonic() - STATE.scan_started_at < 0.1
            )
            self.send_json({
                "state": (
                    "scanning" if scanning
                    else "ready" if STATE.scan_started_at
                    else "idle"
                ),
                "retry_in_ms": 50 if scanning else 0,
                "networks": [] if scanning or not STATE.scan_started_at else [
                    {"ssid": "Workshop-IoT", "rssi": -43, "secure": True},
                    {"ssid": "Guest", "rssi": -68, "secure": True},
                    {"ssid": "Device-Lab", "rssi": -55, "secure": False},
                ],
            })
            return
        if path == "/api/v1/wifi/status":
            snapshot = STATE.snapshot()
            self.send_json({
                "state": snapshot["wifi_state"],
                "saved_ssid": snapshot["saved_ssid"],
                "connected_ssid": snapshot["connected_ssid"],
                "target_ssid": snapshot["target_ssid"],
                "last_attempted_ssid": snapshot["last_attempted_ssid"],
                "rssi_dbm": snapshot["rssi_dbm"],
                "station_ip": snapshot["station_ip"],
                "station_mac": snapshot["station_mac"],
                "last_error": snapshot["last_wifi_error"],
                "retry_in_ms": 0,
                "network_mode": snapshot["network_mode"],
                "ap_role": snapshot["ap_role"],
                "station_retry_paused": snapshot["station_retry_paused"],
                "ap_running": snapshot["ap_running"],
                "ap_ssid": snapshot["ap_ssid"],
                "ap_clients": snapshot["ap_clients"],
                "ap_security": snapshot["ap_security"],
                "ap_password_set": snapshot["ap_password_set"],
                "ap_uses_device_name": snapshot["ap_uses_device_name"],
            })
            return
        if path == "/api/v1/timer/controls":
            self.send_json({"allow_pause": STATE.pause_allowed, "allow_control_lock": STATE.control_lock_allowed, "recovered": False})
            return
        if path == "/api/v1/wifi/ap":
            snapshot = STATE.snapshot()
            self.send_json({
                "ssid": snapshot["ap_ssid"],
                "security": snapshot["ap_security"],
                "password_set": snapshot["ap_password_set"],
                "uses_device_name": snapshot["ap_uses_device_name"],
                "clients": snapshot["ap_clients"],
            })
            return
        if path == "/api/v1/system/diagnostics":
            diagnostics = {
                "uptime_ms": STATE.uptime_ms(),
                "reset_reason": "Software/System restart",
                "hardware_model": "MagBound Smart Timer V2" if STATE.hardware_v2 else "MagBound Smart Timer V1",
                "chip_family": "ESP32-C3" if STATE.hardware_v2 else "ESP8266",
                "build_id": PREVIEW_BUILD_ID,
                "loop_stall": {"longest_ms": 180, "phase": "web_request", "uri": "/api/v1/status",
                               "uptime_ms": 81234, "over_1s_count": 0},
                "free_heap_bytes": 43120,
                "minimum_observed_free_heap_bytes": 39760,
                "session_controls_recovered": False,
                "max_free_block_bytes": 37184,
                "heap_fragmentation_percent": 8,
                "loop_stack_size_bytes": 8192 if STATE.hardware_v2 else 4096,
                "loop_stack_min_free_bytes": 3104 if STATE.hardware_v2 else 1712,
                "sketch_size_bytes": 390000,
                "free_sketch_space_bytes": 610000,
                "flash_size_bytes": 4194304,
                "flash_layout": "4M/2M OTA",
                "sse_clients": STATE.sse_clients,
                "sse_client_limit": STATE.sse_client_limit,
                "sse_accepted_count": STATE.sse_accepted_count,
                "sse_closed_count": STATE.sse_closed_count,
                "sse_last_close_reason": STATE.sse_last_close_reason,
                "sdk_version": "preview",
                "wifi_state": STATE.wifi_state,
                "wifi_channel": 6,
                "status_json_failure_count": 0,
                "wifi_power_save_mode": "none" if STATE.hardware_v2 else "min_modem",
                "wifi_sa_query_timeout_count": 0,
                "wifi_last_disconnect_rssi_dbm": None,
                "station_mac": STATE.station_mac,
                "rssi_dbm": -43,
                "ap_clients": 0,
                "station_retry_paused": STATE.snapshot()["station_retry_paused"],
                "station_retry_in_ms": 0,
                "wifi_connect_attempt_count": STATE.wifi_connect_attempt_count,
                "wifi_disconnect_count": STATE.wifi_disconnect_count,
                "wifi_unexpected_disconnect_count": STATE.wifi_unexpected_disconnect_count,
                "wifi_dhcp_timeout_count": STATE.wifi_dhcp_timeout_count,
                "wifi_last_disconnect_reason": STATE.wifi_last_disconnect_reason,
                "wifi_last_disconnect_reason_name": STATE.wifi_last_disconnect_reason_name,
                "wifi_last_disconnect_expected": STATE.wifi_last_disconnect_expected,
                "wifi_last_disconnect_at_ms": STATE.wifi_last_disconnect_at_ms,
                "wifi_last_connected_at_ms": STATE.wifi_last_connected_at_ms,
                "wifi_connected_duration_ms": STATE.uptime_ms() if STATE.wifi_state == "connected" else 0,
                "lock_output_energized": STATE.state != "open",
                "emergency_input_active": STATE.emergency_input_active,
                "event_count": len(STATE.events),
            }
            if STATE.hardware_v2:
                diagnostics.update({
                    "bluetooth_initialized": STATE.bluetooth_initialized,
                    "bluetooth_enabled": STATE.bluetooth_enabled,
                    "bluetooth_configured_enabled": STATE.bluetooth_configured_enabled,
                    "bluetooth_paired": STATE.bluetooth_paired,
                    "bluetooth_synchronized": STATE.bluetooth_initialized,
                    "bluetooth_advertising": STATE.bluetooth_enabled,
                    "bluetooth_connections": 0,
                    "bluetooth_connection_limit": 1,
                    "bluetooth_last_error": 0,
                    "bluetooth_last_error_name": "none",
                    "bluetooth_connection_rssi_dbm": None,
                    "bluetooth_connection_mtu": 23,
                    "bluetooth_connection_encrypted": False,
                    "bluetooth_connection_bonded": False,
                    "bluetooth_control_enabled": STATE.bluetooth_control_enabled,
                    "bluetooth_control_authenticated": STATE.bluetooth_control_authenticated,
                    "bluetooth_control_authentication_failure_count": 0,
                    "bluetooth_authentication_backoff_remaining_ms": 0,
                    "bluetooth_control_command_count": STATE.bluetooth_control_command_count,
                    "bluetooth_control_last_result": "unauthorized",
                    "bluetooth_connection_mtu_sufficient": False,
                    "bluetooth_bond_cleanup_pending": False,
                    "bluetooth_bond_cleanup_last_error": 0,
                })
            self.send_json(diagnostics)
            return
        if path.startswith("/api/v1/relay") and not STATE.hardware_v2:
            self.send_error_json("Not found", 404, "not_found")
            return
        if path == "/api/v1/relay":
            self.send_json(STATE.relay_status())
            return
        if path == "/api/v1/relay/links":
            self.send_json({"links": [{**{k: link[k] for k in ("fp_hash", "label", "expires_in_ms", "state")}, "url_available": True} for link in STATE.relay_links]})
            return
        if path == "/api/v1/relay/links/share":
            fp_hash = parse_qs(urlparse(self.path).query).get("fp_hash", [""])[0]
            link = next((item for item in STATE.relay_links if item["fp_hash"] == fp_hash), None)
            if link is None:
                self.send_error_json("Share link not found", 404, "link_not_found")
            else:
                self.send_json({"fp_hash": fp_hash, "state": "ready", "share_url": f"https://{STATE.relay['relay_host']}/s/#preview-{fp_hash}"})
            return
        if path in {"/api/v1/bluetooth", "/api/v1/bluetooth/apps"}:
            if STATE.hardware_v2:
                self.send_json(STATE.bluetooth_status())
            else:
                self.send_error_json("not found", 404)
            return
        if path == "/api/v1/bluetooth/enable":
            if STATE.hardware_v2:
                if STATE.state != "open":
                    self.send_error_json("Bluetooth enable requires an open timer", 409, "timer_active")
                else:
                    STATE.bluetooth_initialized = True
                    STATE.bluetooth_enabled = True
                    STATE.bluetooth_configured_enabled = True
                    self.send_json(STATE.bluetooth_status())
            else:
                self.send_error_json("not found", 404)
            return
        if path == "/api/v1/bluetooth/disable":
            if not STATE.hardware_v2:
                self.send_error_json("not found", 404)
            elif STATE.state != "open":
                self.send_error_json("Bluetooth disable requires an open timer", 409, "timer_active")
            else:
                STATE.bluetooth_enabled = False
                STATE.bluetooth_configured_enabled = False
                STATE.bluetooth_control_enabled = False
                STATE.bluetooth_control_authenticated = False
                STATE.bluetooth_control_key = ""
                STATE.bluetooth_pairing_state = "idle"
                STATE.bluetooth_pairing_until = 0.0
                self.send_json(STATE.bluetooth_status())
            return
        if path == "/api/v1/events":
            query = parse_qs(urlparse(self.path).query, keep_blank_values=True)
            try:
                raw_after = query.get("after", ["0"])[-1]
                after = int(raw_after)
                if not raw_after.isascii() or not raw_after.isdecimal() or after > 0xffffffff:
                    raise ValueError
            except ValueError:
                self.send_error_json("Invalid event cursor", 400, "invalid_event_cursor")
                return
            requested_boot = query.get("boot_id", [""])[-1]
            oldest = STATE.events[0]["id"] if STATE.events else 0
            latest = STATE.event_sequence
            cursor_reset = bool(requested_boot and requested_boot != STATE.boot_id) or after > latest
            truncated = not cursor_reset and after and oldest - after > 1
            events = STATE.events if cursor_reset else [event for event in STATE.events if event["id"] > after]
            self.send_json({
                "boot_id": STATE.boot_id,
                "oldest_event_id": oldest,
                "latest_event_id": latest,
                "next_after": latest,
                "cursor_reset": cursor_reset,
                "truncated": bool(truncated),
                "events": events,
            })
            return
        if path == "/api/v1/capabilities":
            v2 = STATE.hardware_v2
            capabilities = {
                "api_version": "v1",
                "max_duration_ms": MAX_DURATION_MS,
                "hardware_model": "MagBound Smart Timer V2" if v2 else "MagBound Smart Timer V1",
                "chip_family": "ESP32-C3" if v2 else "ESP8266",
                "build_id": PREVIEW_BUILD_ID,
                "request_content_types": ["application/x-www-form-urlencoded", "application/json"],
                "features": {"pause": True, "adjust": True, "adjust_operation_id": True, "mutation_operation_id": True, "session_id": True, "session_precondition": True, "boot_precondition": True, "wifi_disconnect_diagnostics": True, "station_mac": True, "release_alias": True, "event_cursor": True, "mdns_discovery": True, "device_name": True, "identify_led": True, "emergency_input_status": True, "ap_configuration": True, "ap_wpa2": True, "wifi_async_scan": True, "sse": True, "sse_opt_in": False, "sse_ui_auto_connect": True, "bluetooth_low_energy": v2, "bluetooth_read_only": False, "bluetooth_control": v2, "bluetooth_authenticated_control": v2, "bluetooth_notifications": v2, "bluetooth_peer_discovery": False, "bluetooth_peer_control": False, "bluetooth_bonding": v2, "bluetooth_command_integrity": v2, "bluetooth_physical_pairing": v2, "bluetooth_event_notifications": v2, "bluetooth_event_replay": v2, "bluetooth_pairing_commit": v2, "bluetooth_identify": v2, "relay": v2, "relay_experimental": v2},
                "limits": {"ap_clients": 4, "sse_clients": STATE.sse_client_limit},
                "sse": {"path": "/api/v1/events/stream", "default_enabled": True, "persistent": False, "rest_fallback": True},
                "mutations": {"operation_id_max_length": 64, "deduplication_entries": 8, "scope": "current_boot", "boot_precondition_field": "if_boot_id", "session_precondition_field": "if_session_id"},
                "events": {"history_entries": 16, "cursor_parameter": "after", "boot_parameter": "boot_id"},
                "discovery": {"mdns_service": "_smarttimer._tcp.local", "port": 80},
                "ota": {"physical_arm_required": True, "firmware_only": True, "unverified_override": True, "unverified_override_volatile": True, "unverified_override_uploads": 1},
                "updates": {"configured": False, "base_url": "", "public_key": "", "target": "magbound_v2_esp32c3" if v2 else "magbound_v1_esp8266", "check_mode": "manual_browser"},
            }
            if v2:
                capabilities["bluetooth"] = {"protocol": "ost-ble/1", "mode": "bonded_authenticated_control", "inbound_connections": 1, "controller_connections": 1, "status_notifications_ms": 15000, "default_enabled": False, "persistent_enable": True, "control_volatile": False, "control_authentication": "bonded-hmac-sha256-112", "command_integrity": "hmac-sha256-64-counter", "capabilities_frame_bytes": 24, "command_frame_bytes": 32, "result_frame_bytes": 20, "status_frame_bytes": 20, "event_frame_bytes": 20, "required_control_mtu": 35, "event_history_entries": 16, "system_information_frame_bytes": 44, "system_information_uuid": "f00df299-464f-4921-9285-7749593b1144", "pairing_commit": "hmac-sha256-112", "path": "/api/v1/bluetooth", "enable_path": "/api/v1/bluetooth/enable", "disable_path": "/api/v1/bluetooth/disable", "pairing_start_path": "/api/v1/bluetooth/pairing/start", "pairing_cancel_path": "/api/v1/bluetooth/pairing/cancel", "pairing_revoke_path": "/api/v1/bluetooth/pairing/revoke"}
            self.send_json(capabilities)
            return
        if path == "/api/v1/device/name":
            snapshot = STATE.snapshot()
            self.send_json({"name": STATE.device_name, "hostname": STATE.hostname, "ap_ssid": snapshot["ap_ssid"], "ap_uses_device_name": STATE.ap_uses_device_name})
            return
        if path.startswith("/api/"):
            self.send_error_json("not found", 404)
            return
        super().do_GET()

    def handle_relay_post(self, path, values):
        relay = STATE.relay
        if not STATE.hardware_v2:
            self.send_error_json("Not found", 404, "not_found")
        elif path in {"/api/v1/relay/enroll", "/api/v1/relay/enable", "/api/v1/relay/forget"} and STATE.state != "open":
            self.send_error_json("The relay settings require an open timer", 409, "timer_active")
        elif path == "/api/v1/relay/enroll":
            origin = str(values.get("relay_url", ""))
            partner = str(values.get("partner_origin", ""))
            if values.get("if_boot_id") != STATE.boot_id:
                self.send_error_json("Read the current boot_id", 409, "stale_boot")
                return
            if not origin.startswith("https://") or not partner.startswith("https://") or partner.rstrip("/").lower() == origin.rstrip("/").lower() or len(str(values.get("enrollment_code", ""))) < 8 or len(str(values.get("spki_pin", ""))) != 43:
                self.send_error_json("Invalid relay request", 400, "invalid_relay_request")
                return
            STATE.relay_status()
            setup = (origin, partner, values["enrollment_code"], values["spki_pin"])
            if STATE.relay_setup and STATE.relay_setup != setup:
                self.send_error_json("Another setup is awaiting approval", 409, "relay_busy")
                return
            if not STATE.relay_setup:
                STATE.relay_setup_id += 1
                STATE.relay_setup_until = time.monotonic() + 60
                STATE.relay_setup = setup
                STATE.relay_setup_state = "pending"
            self.send_json(STATE.relay_status(), 202)
        elif path == "/api/v1/relay/enroll/cancel":
            if values.get("if_boot_id") != STATE.boot_id or str(values.get("setup_request_id")) != str(STATE.relay_setup_id) or not STATE.relay_setup:
                self.send_error_json("Setup changed", 409, "relay_job_conflict")
                return
            STATE.relay_setup = None
            STATE.relay_setup_state = "cancelled"
            self.send_json(STATE.relay_status())
        elif path == "/api/v1/relay/enable":
            if not relay["enrolled"]:
                self.send_error_json("Enroll the relay first", 409, "relay_not_enrolled")
                return
            relay["enabled"] = True
            self.send_json(STATE.relay_status())
        elif path == "/api/v1/relay/disable":
            STATE.relay_setup = None
            STATE.relay_setup_state = "cancelled"
            relay["enabled"] = False
            STATE.relay_links.clear()
            self.send_json(STATE.relay_status(), 202)
        elif path == "/api/v1/relay/forget":
            STATE.relay_setup = None
            STATE.relay_setup_state = "cancelled"
            relay.update(enabled=False, enrolled=False, relay_host="", timer_id="", partner_origin="")
            STATE.relay_links.clear()
            self.send_json(STATE.relay_status(), 202)
        elif path in {"/api/v1/relay/links", "/api/v1/relay/links/revoke", "/api/v1/relay/links/extend"}:
            # Asynchronous owner job, completed immediately in the preview.
            job = STATE.relay_job
            request = (path, str(values.get("ttl_s", "")), str(values.get("label", "")), str(values.get("fp_hash", "")))
            if str(values.get("if_boot_id", "")) != STATE.boot_id:
                self.send_error_json("read relay status and use its current boot_id", 409, "stale_boot")
                return
            try:
                job_id = int(values.get("job_id", 0))
            except (TypeError, ValueError):
                job_id = 0
            if job_id <= 0:
                self.send_error_json("job_id must be next_job_id from relay status", 400, "invalid_relay_job_id")
                return
            if job_id == job["id"]:
                if request != job["request"]:
                    self.send_error_json("job_id is stale or was used for another operation; read relay status", 409, "relay_job_conflict")
                else:
                    self.send_json(STATE.relay_status(), 202)
                return
            if job_id != job["id"] + 1:
                self.send_error_json("job_id is stale or was used for another operation; read relay status", 409, "relay_job_conflict")
                return
            if path in {"/api/v1/relay/links", "/api/v1/relay/links/extend"}:
                ttl = int(values.get("ttl_s", "0") or 0)
                if not 60 <= ttl <= 86400:
                    self.send_error_json("ttl_s must be between 60 and 86400 seconds", 400, "invalid_link_ttl")
                    return
                if STATE.state != "open" and path == "/api/v1/relay/links":
                    self.send_error_json("this relay operation requires an open timer", 409, "timer_active")
                    return
            if not relay["enabled"]:
                self.send_error_json("the relay is disabled", 409, "relay_disabled")
                return
            fp_hash, result = "", "ok"
            if path == "/api/v1/relay/links" and len(STATE.relay_links) >= 1:
                result = "link_table_full"  # the firmware allows one live link
            elif path == "/api/v1/relay/links":
                fp_hash = secrets.token_urlsafe(16)[:22]
                STATE.relay_links.append({"fp_hash": fp_hash, "label": str(values.get("label", ""))[:23], "expires_in_ms": ttl * 1000, "state": "ready"})
            elif path == "/api/v1/relay/links/extend":
                fp_hash = str(values.get("fp_hash", ""))
                link = next((item for item in STATE.relay_links if item["fp_hash"] == fp_hash), None)
                if link is None:
                    result = "link_not_found"
                else:
                    link["expires_in_ms"] = min(86400000, link["expires_in_ms"] + ttl * 1000)
            else:
                fp_hash = str(values.get("fp_hash", ""))
                before = len(STATE.relay_links)
                STATE.relay_links[:] = [link for link in STATE.relay_links if link["fp_hash"] != fp_hash]
                result = "ok" if len(STATE.relay_links) < before else "link_not_found"
            job.update(id=job_id, state="done", result=result, fp_hash=fp_hash if result == "ok" else "", request=request)
            self.send_json(STATE.relay_status(), 202)
        else:
            self.send_error_json("Not found", 404, "not_found")

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/api/v1/ota":
            self.send_error_json("OTA is disabled in the desktop preview", 403, "ota_not_armed")
            return
        if path == "/api/v1/ota/prepare":
            self.send_error_json("OTA is disabled in the desktop preview", 403, "ota_not_armed")
            return
        try:
            values = self.read_form()
            if path.startswith("/api/v1/relay"):
                self.handle_relay_post(path, values)
                return
            if path in {"/api/v1/bluetooth/apps/label", "/api/v1/bluetooth/apps/revoke"}:
                if not STATE.hardware_v2:
                    self.send_error_json("Bluetooth unavailable", 404, "bluetooth_unavailable")
                elif STATE.state != "open":
                    self.send_error_json("App management requires an open timer", 409, "timer_active")
                elif STATE.bluetooth_status()["pairing_active"]:
                    self.send_error_json("Cancel pairing first", 409, "pairing_active")
                else:
                    app_id = int(values.get("app_id", "0"))
                    app = next((a for a in STATE.bluetooth_apps if a["app_id"] == app_id), None)
                    label = values.get("label", "")
                    if app_id <= 0:
                        self.send_error_json("app_id must be positive", 400, "invalid_app")
                    elif app is None:
                        self.send_error_json("App not found", 404, "app_not_found")
                    elif path.endswith("/revoke"):
                        STATE.bluetooth_apps.remove(app)
                        STATE.bluetooth_paired = bool(STATE.bluetooth_apps)
                        self.send_json(STATE.bluetooth_status())
                    elif not label or len(label.encode("utf-8")) > 24 or any(ord(c) < 32 or ord(c) == 127 for c in label):
                        self.send_error_json("Invalid app label", 400, "invalid_label")
                    else:
                        app["label"] = label
                        self.send_json(STATE.bluetooth_status())
                return
            if path == "/api/v1/bluetooth/pairing/start":
                if not STATE.hardware_v2:
                    self.send_error_json("Bluetooth is unavailable", 404, "bluetooth_unavailable")
                elif STATE.state != "open":
                    self.send_error_json("Bluetooth control enable requires an open timer", 409, "timer_active")
                elif STATE.bluetooth_status()["pairing_active"]:
                    self.send_error_json("Cancel pairing first", 409, "pairing_active")
                elif values.get("replace_app_id") and not any(a["app_id"] == int(values["replace_app_id"]) for a in STATE.bluetooth_apps):
                    self.send_error_json("App not found", 404, "app_not_found")
                elif not values.get("replace_app_id") and len(STATE.bluetooth_apps) >= 3:
                    self.send_error_json("All three app slots are used", 409, "app_table_full")
                else:
                    STATE.bluetooth_initialized = True
                    STATE.bluetooth_enabled = True
                    STATE.bluetooth_configured_enabled = True
                    STATE.bluetooth_pairing_state = "waiting_for_connection"
                    STATE.bluetooth_pairing_failure = "none"
                    STATE.bluetooth_control_authenticated = False
                    STATE.bluetooth_pairing_until = time.monotonic() + 120
                    STATE.bluetooth_control_key = ""
                    self.send_json(STATE.bluetooth_status())
                return
            if path == "/api/v1/bluetooth/pairing/cancel":
                if not STATE.hardware_v2:
                    self.send_error_json("Bluetooth is unavailable", 404, "bluetooth_unavailable")
                else:
                    STATE.bluetooth_pairing_state = "idle"
                    STATE.bluetooth_pairing_until = 0.0
                    self.send_json(STATE.bluetooth_status())
                return
            if path == "/api/v1/bluetooth/pairing/revoke":
                if not STATE.hardware_v2:
                    self.send_error_json("Bluetooth is unavailable", 404, "bluetooth_unavailable")
                elif STATE.state != "open":
                    self.send_error_json("Bluetooth pairing removal requires an open timer", 409, "timer_active")
                else:
                    STATE.bluetooth_paired = False
                    STATE.bluetooth_apps = []
                    STATE.bluetooth_pairing_state = "idle"
                    STATE.bluetooth_pairing_until = 0.0
                    STATE.bluetooth_control_enabled = False
                    STATE.bluetooth_control_authenticated = False
                    STATE.bluetooth_control_key = ""
                    self.send_json(STATE.bluetooth_status())
                return
            if path == "/api/v1/wifi/scan":
                STATE.scan_started_at = time.monotonic()
                self.send_json({"state": "scanning", "retry_in_ms": 50, "networks": []}, 202)
                return
            if path == "/api/v1/timer/controls":
                if STATE.state != "open":
                    self.send_error_json("Session-control settings require an open timer", 409, "timer_active")
                    return
                supplied = {key: str(values[key]).lower() for key in ("allow_pause", "allow_control_lock") if key in values}
                if not supplied or any(value not in {"true", "false", "1", "0"} for value in supplied.values()):
                    self.send_error_json("Send allow_pause and/or allow_control_lock as booleans", 400, "invalid_field")
                    return
                if "allow_pause" in supplied:
                    STATE.pause_allowed = supplied["allow_pause"] in {"true", "1"}
                if "allow_control_lock" in supplied:
                    STATE.control_lock_allowed = supplied["allow_control_lock"] in {"true", "1"}
                STATE.state_revision += 1
                self.send_json({"allow_pause": STATE.pause_allowed, "allow_control_lock": STATE.control_lock_allowed, "recovered": False})
                return
            timer_paths = {
                "/api/v1/timer/start", "/api/v1/timer/stop",
                "/api/v1/timer/release", "/api/v1/timer/pause",
                "/api/v1/timer/resume", "/api/v1/timer/adjust",
                "/api/v1/timer/seal", "/api/v1/timer/lock-controls",
            }
            if path in timer_paths:
                operation_id = str(values.get("operation_id", ""))
                if len(operation_id) > 64 or any(ord(character) < 0x20 or ord(character) > 0x7E for character in operation_id):
                    self.send_error_json("Invalid operation ID", 400, "invalid_operation_id")
                    return

                if path == "/api/v1/timer/start":
                    if "duration_ms" in values and ("min_ms" in values or "max_ms" in values):
                        raise ValueError("Choose fixed or random duration, not both")
                    if "duration_ms" in values:
                        duration = int(values["duration_ms"])
                        operation = ("start", duration, 0)
                    else:
                        minimum = int(values["min_ms"])
                        maximum = int(values["max_ms"])
                        operation = ("start", minimum, maximum)
                        duration = random.randint(minimum, maximum)
                elif path in {"/api/v1/timer/stop", "/api/v1/timer/release"}:
                    operation = ("release", 0, 0)
                elif path == "/api/v1/timer/pause":
                    operation = ("pause", 0, 0)
                elif path == "/api/v1/timer/resume":
                    operation = ("resume", 0, 0)
                elif path == "/api/v1/timer/adjust":
                    operation = ("adjust", int(values["delta_ms"]), 0)
                else:
                    if path == "/api/v1/timer/seal" and str(values.get("sealed", "")).lower() not in {"1", "true"}:
                        self.send_error_json("sealed must be true", 400, "invalid_sealed_value")
                        return
                    operation = ("lock_controls", 0, 0)

                if operation_id in STATE.recent_operations:
                    if STATE.recent_operations[operation_id] != operation:
                        self.send_error_json("Operation ID conflict", 409, "operation_id_conflict")
                        return
                    result = STATE.snapshot()
                    result["mutation"] = {"action": operation[0], "changed": False, "duplicate": True, "operation_id": operation_id}
                    self.send_json(result)
                    return

                requested_boot = values.get("if_boot_id", "")
                if requested_boot and requested_boot != STATE.boot_id:
                    self.send_error_json("Stale device boot", 409, "stale_boot")
                    return

                if path != "/api/v1/timer/start":
                    requested_session = values.get("if_session_id", "")
                    if requested_session and requested_session != STATE.snapshot()["session_id"]:
                        self.send_error_json("Stale timer session", 409, "stale_session")
                        return

                changed = True
                if path == "/api/v1/timer/start":
                    if STATE.state != "open":
                        self.send_error_json("Timer is already active", 409, "timer_active")
                        return
                    if STATE.emergency_input_active:
                        self.send_error_json("Emergency input is active", 409, "emergency_input_active")
                        return
                    STATE.start(duration)
                elif path in {"/api/v1/timer/stop", "/api/v1/timer/release"}:
                    if STATE.state == "open":
                        changed = False
                    elif STATE.sealed:
                        self.send_error_json("Controls are sealed", 409, "release_not_allowed")
                        return
                    else:
                        STATE.open()
                elif path == "/api/v1/timer/pause":
                    if STATE.state == "paused":
                        changed = False
                    elif not STATE.pause_allowed:
                        self.send_error_json("This control is switched off in the timer's settings", 409, "control_disabled")
                        return
                    elif STATE.state != "running" or STATE.sealed:
                        self.send_error_json("Pause is not allowed", 409, "pause_not_allowed")
                        return
                    else:
                        STATE.pause()
                elif path == "/api/v1/timer/resume":
                    if STATE.state == "running":
                        changed = False
                    elif STATE.state != "paused" or STATE.sealed:
                        self.send_error_json("Resume is not allowed", 409, "resume_not_allowed")
                        return
                    elif STATE.emergency_input_active:
                        self.send_error_json("Emergency input is active", 409, "emergency_input_active")
                        return
                    else:
                        STATE.resume()
                elif path == "/api/v1/timer/adjust":
                    if STATE.state == "open" or STATE.sealed:
                        self.send_error_json("Adjust is not allowed", 409, "adjust_not_allowed")
                        return
                    if operation[1] > 0 and STATE.emergency_input_active:
                        self.send_error_json("Emergency input is active", 409, "emergency_input_active")
                        return
                    if operation[1] > 0 and STATE.remaining_ms + operation[1] > MAX_DURATION_MS:
                        self.send_error_json("Maximum lock duration reached", 409, "maximum_duration_reached")
                        return
                    changed = operation[1] != 0
                    if changed:
                        STATE.adjust(operation[1])
                else:
                    if STATE.sealed and STATE.state != "open":
                        changed = False
                    elif not STATE.control_lock_allowed:
                        self.send_error_json("This control is switched off in the timer's settings", 409, "control_disabled")
                        return
                    elif STATE.state != "running":
                        self.send_error_json("Only a running timer can be sealed", 409, "lock_controls_not_allowed")
                        return
                    else:
                        STATE.sealed = True
                        STATE.event("controls_locked", STATE.remaining_ms)

                if operation_id:
                    STATE.recent_operations[operation_id] = operation
                    STATE.recent_operation_order.append(operation_id)
                    if len(STATE.recent_operation_order) > 8:
                        expired = STATE.recent_operation_order.pop(0)
                        del STATE.recent_operations[expired]
                result = STATE.snapshot()
                result["mutation"] = {"action": operation[0], "changed": changed, "duplicate": False, "operation_id": operation_id or None}
                self.send_json(result)
                return
            elif path == "/api/v1/wifi/configure":
                ssid = values["ssid"]
                STATE.last_attempted_ssid = ssid
                if values.get("password") == "wrong":
                    STATE.wifi_state = "failed"
                    STATE.connected_ssid = ""
                    STATE.last_wifi_error = "wrong password"
                else:
                    STATE.wifi_state = "connected"
                    STATE.connected_ssid = ssid
                    STATE.saved_ssid = ssid
                    STATE.last_wifi_error = ""
                self.send_json({"connecting": True, "wifi_state": "connecting"}, 202)
                return
            elif path == "/api/v1/wifi/forget":
                STATE.wifi_state = "standalone"
                STATE.connected_ssid = ""
                STATE.saved_ssid = ""
                STATE.last_wifi_error = ""
                STATE.target_ssid = ""
                self.send_json({"standalone": True}, 202)
                return
            elif path == "/api/v1/system/identify":
                self.send_json({"identifying": True, "duration_ms": 5000})
                return
            elif path == "/api/v1/system/restart":
                self.send_json({"restarting": True}, 202)
                return
            else:
                self.send_error_json("Not found", 404, "not_found")
                return
            self.send_json(STATE.snapshot())
        except (KeyError, TypeError, ValueError) as error:
            self.send_error_json(str(error))

    def do_PUT(self):
        path = urlparse(self.path).path
        if path == "/api/v1/bluetooth/apps/label":
            return self.do_POST()
        if path not in {"/api/v1/device/name", "/api/v1/wifi/ap"}:
            self.send_error_json("Not found", 404, "not_found")
            return
        try:
            values = self.read_form()
            if path == "/api/v1/wifi/ap":
                use_device_name = str(values.get("use_device_name", "false")).lower() in {"1", "true"}
                keep_password = str(values.get("keep_password", "false")).lower() in {"1", "true"}
                security = str(values.get("security", ""))
                ssid = str(values.get("ssid", "")).strip()
                password = str(values.get("password", ""))
                standalone = str(values.get("standalone", "false")).lower() in {"1", "true"}
                if security not in {"open", "wpa2"}:
                    raise ValueError("Security must be open or WPA2")
                if not use_device_name and not 0 < len(ssid) <= 32:
                    raise ValueError("SSID must be 1-32 characters")
                if security == "wpa2" and not (keep_password and STATE.ap_password_set) and not 8 <= len(password) <= 63:
                    raise ValueError("WPA2 password must be 8-63 characters")
                STATE.ap_uses_device_name = use_device_name
                STATE.ap_custom_ssid = "" if use_device_name else ssid
                STATE.ap_security = security
                STATE.ap_password_set = security == "wpa2"
                if standalone:
                    STATE.wifi_state = "standalone"
                    STATE.connected_ssid = ""
                    STATE.saved_ssid = ""
                    STATE.last_wifi_error = ""
                    STATE.target_ssid = ""
                STATE.event("access_point_configured")
                self.send_json({"updated": True, "restarting": True, "ap_ssid": STATE.ap_ssid(), "ap_security": STATE.ap_security, "standalone": standalone}, 202)
                return
            name = str(values["name"]).strip()
            if not 0 < len(name) <= 32:
                raise ValueError("Name must be 1-32 characters")
            STATE.device_name = name
            hostname = "-".join("".join(character.lower() if character.isalnum() else " " for character in name).split())
            STATE.hostname = hostname[:24] or "smarttimer"
            self.send_json({"updated": True, "restarting": True, "name": name, "hostname": STATE.hostname, "ap_ssid": STATE.ap_ssid(), "ap_security": STATE.ap_security})
        except (KeyError, TypeError, ValueError) as error:
            self.send_error_json(str(error))

    def log_message(self, message, *args):
        print(f"{self.address_string()} - {message % args}")


def main():
    global STATE
    parser = ArgumentParser(description="Preview the Smart Timer web UI with mock device state")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--v2", action="store_true", help="Preview V2 Bluetooth pairing, control, and diagnostics")
    args = parser.parse_args()
    STATE = PreviewState(hardware_v2=args.v2)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), PreviewHandler)
    print(f"Smart Timer {'V2' if args.v2 else 'V1'} UI preview: http://127.0.0.1:{args.port}")
    print("Press Ctrl-C to stop. This mock does not validate firmware or hardware safety.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
