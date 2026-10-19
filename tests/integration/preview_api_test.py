#!/usr/bin/env python3
"""Exercise the browser preview's modern API contract end-to-end."""

from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from argparse import ArgumentParser
import importlib.util
import json
import time

from api_contract import validate, validate_response


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("preview_ui", ROOT / "tools" / "preview_ui.py")
PREVIEW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREVIEW)


class QuietHandler(PREVIEW.PreviewHandler):
    def log_message(self, message, *args):
        pass


def main():
    parser = ArgumentParser()
    parser.add_argument("--base-url", help="Use an already-running preview server")
    args = parser.parse_args()
    server = None
    thread = None
    if args.base_url:
        base = args.base_url.rstrip("/")
    else:
        server = ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"

    def request(path, method="GET", payload=None):
        body = None if payload is None else json.dumps(payload).encode()
        headers = {} if body is None else {"Content-Type": "application/json"}
        call = Request(base + path, data=body, headers=headers, method=method)
        try:
            with urlopen(call, timeout=2) as response:
                return validate_response(path, method, response.status, json.load(response), payload)
        except HTTPError as error:
            raw = error.read()
            try:
                payload = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                payload = {}
            return validate_response(path, method, error.code, payload)

    try:
        # Preview mirrors rejection behavior; the C++ host suite separately
        # exercises the exact parser compiled into both firmware images.
        for raw in ['{"duration_ms":1000', '{"duration_ms":1000} trailing',
                    '{"duration_ms":1000,"duration_ms":2000}',
                    '{"nested":{"if_boot_id":"valid"},"if_boot_id":"stale"}',
                    '{"duration_ms":"1000"}', '{"duration_ms":true}',
                    '{"duration_ms":1000,"min_ms":1000,"max_ms":2000}']:
            call = Request(base + "/api/v1/timer/start", data=raw.encode(),
                           headers={"Content-Type": "application/json"}, method="POST")
            try:
                with urlopen(call, timeout=2) as response:
                    raise AssertionError(f"Malformed JSON accepted: {response.status}")
            except HTTPError as error:
                assert error.code == 400
                validate("error-response", json.load(error), "malformed JSON request")
        for after in ["-1", "4294967296", "1.5", "abc"]:
            assert request("/api/v1/events?after=" + after)[0] == 400
        code, status = request("/api/v1/status")
        assert code == 200
        for field in (
            "session_duration_ms",
            "device_name",
            "hostname",
            "wifi_state",
            "ota_armed_remaining_ms",
            "ota_unverified_allowed",
            "last_release_reason",
            "emergency_input_active",
            "ap_security",
            "ap_password_set",
            "ap_uses_device_name",
            "network_mode",
            "ap_role",
            "station_retry_paused",
            "station_mac",
            "boot_id",
            "session_id",
            "state_revision",
            "latest_event_id",
            "build_id",
        ):
            assert field in status

        code, capabilities = request("/api/v1/capabilities")
        assert code == 200 and capabilities["features"]["wifi_async_scan"]
        assert capabilities["features"]["adjust_operation_id"]
        assert capabilities["features"]["mutation_operation_id"]
        assert capabilities["features"]["session_precondition"]
        assert capabilities["features"]["boot_precondition"]
        assert capabilities["features"]["wifi_disconnect_diagnostics"]
        assert capabilities["features"]["station_mac"]
        assert capabilities["features"]["release_alias"]
        assert capabilities["features"]["event_cursor"]
        assert capabilities["features"]["mdns_discovery"]
        assert capabilities["mutations"]["deduplication_entries"] == 8
        assert capabilities["mutations"]["boot_precondition_field"] == "if_boot_id"
        assert capabilities["discovery"]["mdns_service"] == "_smarttimer._tcp.local"
        assert capabilities["features"]["sse"]
        assert capabilities["sse"]["default_enabled"] is True
        assert capabilities["sse"]["rest_fallback"] is True
        assert capabilities["features"]["sse_ui_auto_connect"] is True
        assert capabilities["features"]["bluetooth_low_energy"] is False
        assert capabilities["features"]["bluetooth_peer_discovery"] is False
        assert capabilities["features"]["bluetooth_peer_control"] is False
        assert capabilities["ota"]["unverified_override"] is True
        assert capabilities["ota"]["unverified_override_volatile"] is True
        assert capabilities["ota"]["unverified_override_uploads"] == 1
        assert capabilities["limits"] == {"ap_clients": 4, "sse_clients": PREVIEW.STATE.sse_client_limit}
        assert capabilities["build_id"] == status["build_id"]
        code, bluetooth = request("/api/v1/bluetooth")
        assert code == 404 and bluetooth["code"] == "request_failed"
        code, bluetooth = request("/api/v1/bluetooth/enable")
        assert code == 404 and bluetooth["code"] == "request_failed"
        code, _ = request("/api/v1/bluetooth/peers")
        assert code == 404
        code, _ = request("/api/v1/bluetooth/peers/scan")
        assert code == 404
        code, diagnostics = request("/api/v1/system/diagnostics")
        assert code == 200
        assert diagnostics["max_free_block_bytes"] <= diagnostics["free_heap_bytes"]
        assert "heap_fragmentation_percent" in diagnostics
        assert diagnostics["flash_layout"] == "4M/2M OTA"
        assert diagnostics["sse_client_limit"] == PREVIEW.STATE.sse_client_limit
        assert diagnostics["sse_accepted_count"] == 0
        assert diagnostics["sse_closed_count"] == 0
        assert diagnostics["sse_last_close_reason"] == "none"
        assert diagnostics["wifi_connect_attempt_count"] == 1
        assert diagnostics["wifi_disconnect_count"] == 0
        assert diagnostics["wifi_unexpected_disconnect_count"] == 0
        assert diagnostics["wifi_dhcp_timeout_count"] == 0
        assert diagnostics["wifi_last_disconnect_reason_name"] == "none"
        assert diagnostics["station_mac"] == status["station_mac"]
        assert diagnostics["build_id"] == status["build_id"]
        code, wifi_status = request("/api/v1/wifi/status")
        assert code == 200 and wifi_status["network_mode"] == "wifi"
        code, name = request("/api/v1/device/name")
        assert code == 200 and name["name"] == status["device_name"]
        code, ota = request("/api/v1/ota/prepare", "POST",
                            {"image_size": 1000, "image_crc32": "1234abcd"})
        assert code == 403 and ota["code"] == "ota_not_armed"
        code, scan = request("/api/v1/wifi/scan", "POST", {})
        assert code == 202 and scan["state"] == "scanning"
        time.sleep(0.12)
        code, scan = request("/api/v1/wifi/scan")
        assert code == 200 and scan["state"] == "ready" and scan["networks"]
        code, access_point = request("/api/v1/wifi/ap")
        assert code == 200 and access_point["security"] == "open"
        assert "password" not in access_point

        PREVIEW.STATE.set_emergency_input(True)
        code, blocked = request(
            "/api/v1/timer/start", "POST", {"duration_ms": 5000}
        )
        assert code == 409
        assert blocked["code"] == "emergency_input_active"
        assert PREVIEW.STATE.events[-1]["type"] == "emergency_input_activated"
        PREVIEW.STATE.set_emergency_input(False)
        assert PREVIEW.STATE.events[-1]["type"] == "emergency_input_cleared"

        code, access_point = request(
            "/api/v1/wifi/ap", "PUT",
            {
                "ssid": "PRIVATE-TIMER",
                "security": "wpa2",
                "password": "local-only-password",
                "use_device_name": False,
                "keep_password": False,
            },
        )
        assert code == 202 and access_point["ap_ssid"] == "PRIVATE-TIMER"
        code, access_point = request("/api/v1/wifi/ap")
        assert code == 200 and access_point["password_set"]
        assert "password" not in access_point

        code, access_point = request(
            "/api/v1/wifi/ap", "PUT",
            {
                "ssid": "",
                "security": "wpa2",
                "password": "",
                "use_device_name": True,
                "keep_password": True,
                "standalone": True,
            },
        )
        assert code == 202 and access_point["ap_security"] == "wpa2"
        assert access_point["standalone"] is True
        assert request("/api/v1/wifi/status")[1]["state"] == "standalone"

        code, renamed = request(
            "/api/v1/device/name", "PUT", {"name": "Workshop Timer"}
        )
        assert code == 200 and renamed["hostname"] == "workshop-timer"

        code, stale_boot = request(
            "/api/v1/timer/start", "POST",
            {"duration_ms": 5000, "if_boot_id": "previous-boot"},
        )
        assert code == 409 and stale_boot["code"] == "stale_boot"

        code, started = request(
            "/api/v1/timer/start", "POST",
            {"duration_ms": 5000, "operation_id": "preview-start-1", "if_boot_id": status["boot_id"]}
        )
        assert code == 200 and started["state"] == "running"
        assert started["session_duration_ms"] == 5000
        assert started["session_id"]
        assert started["mutation"]["changed"] is True
        code, repeated_start = request(
            "/api/v1/timer/start", "POST",
            {"duration_ms": 5000, "operation_id": "preview-start-1", "if_boot_id": "ffffffffffffffff"}
        )
        assert code == 200 and repeated_start["mutation"]["duplicate"] is True
        paused = request(
            "/api/v1/timer/pause", "POST",
            {"operation_id": "preview-pause-1", "if_boot_id": status["boot_id"], "if_session_id": started["session_id"]},
        )[1]
        assert paused["state"] == "paused"
        repeated_pause = request(
            "/api/v1/timer/pause", "POST",
            {"operation_id": "preview-pause-1", "if_session_id": started["session_id"]},
        )[1]
        assert repeated_pause["mutation"]["duplicate"] is True
        code, stale = request(
            "/api/v1/timer/adjust", "POST",
            {"delta_ms": 1000, "if_boot_id": status["boot_id"], "if_session_id": "stale-session"},
        )
        assert code == 409 and stale["code"] == "stale_session"
        code, adjusted = request(
            "/api/v1/timer/adjust", "POST",
            {"delta_ms": 5000, "operation_id": "preview-adjust-1"},
        )
        assert code == 200
        adjusted_remaining = adjusted["remaining_ms"]
        code, replayed = request(
            "/api/v1/timer/adjust", "POST",
            {"delta_ms": 5000, "operation_id": "preview-adjust-1"},
        )
        assert code == 200 and replayed["remaining_ms"] == adjusted_remaining
        code, conflict = request(
            "/api/v1/timer/adjust", "POST",
            {"delta_ms": 1000, "operation_id": "preview-adjust-1"},
        )
        assert code == 409 and conflict["code"] == "operation_id_conflict"
        code, maximum = request(
            "/api/v1/timer/adjust", "POST",
            {"delta_ms": PREVIEW.MAX_DURATION_MS, "if_boot_id": status["boot_id"], "if_session_id": started["session_id"]},
        )
        assert code == 409 and maximum["code"] == "maximum_duration_reached"
        assert request("/api/v1/timer/resume", "POST", {})[1]["state"] == "running"
        assert request("/api/v1/timer/lock-controls", "POST", {})[1]["sealed"]
        code, error = request("/api/v1/timer/stop", "POST", {})
        assert code == 409 and "error" in error and "code" in error

        PREVIEW.STATE.open()
        PREVIEW.STATE.saved_ssid = "Workshop-IoT"
        PREVIEW.STATE.connected_ssid = "Workshop-IoT"
        PREVIEW.STATE.wifi_state = "connected"
        code, _ = request(
            "/api/v1/wifi/configure",
            "POST",
            {"ssid": "Broken Network", "password": "wrong"},
        )
        assert code == 202
        failed_wifi = request("/api/v1/wifi/status")[1]
        assert failed_wifi["state"] == "failed"
        assert failed_wifi["network_mode"] == "wifi"
        assert failed_wifi["ap_role"] == "recovery"
        assert failed_wifi["station_retry_paused"] is True

        PREVIEW.STATE.note_wifi_disconnect()
        code, wifi_diagnostics = request("/api/v1/system/diagnostics")
        assert code == 200
        assert wifi_diagnostics["wifi_disconnect_count"] == 1
        assert wifi_diagnostics["wifi_unexpected_disconnect_count"] == 1
        assert wifi_diagnostics["wifi_last_disconnect_reason"] == 200
        assert wifi_diagnostics["wifi_last_disconnect_reason_name"] == "beacon_timeout"
        assert wifi_diagnostics["wifi_last_disconnect_expected"] is False

        code, _ = request("/api/v1/system/identify", "POST", {})
        assert code == 200
        code, events = request("/api/v1/events")
        assert code == 200 and events["events"]
        assert all(
            {"state", "sealed", "emergency_input_active", "session_sequence"}
            <= event.keys()
            for event in events["events"]
        )
        assert any(event["type"] == "emergency_input_activated" for event in events["events"])
        assert any(event["type"] == "emergency_input_cleared" for event in events["events"])
        wifi_event = next(event for event in events["events"] if event["type"] == "wifi_disconnected")
        assert wifi_event["value_ms"] == 200
        assert wifi_event["wifi_disconnect_reason_name"] == "beacon_timeout"
        assert wifi_event["unexpected"] is True
        cursor = events["events"][-2]["id"]
        code, incremental = request(
            f"/api/v1/events?after={cursor}&boot_id={events['boot_id']}"
        )
        assert code == 200 and incremental["events"]
        assert all(event["id"] > cursor for event in incremental["events"])
        assert incremental["cursor_reset"] is False
        code, reset_cursor = request(
            f"/api/v1/events?after={events['latest_event_id']}&boot_id=another-boot"
        )
        assert code == 200 and reset_cursor["cursor_reset"] is True

        PREVIEW.STATE.open()
        code, released = request(
            "/api/v1/timer/release", "POST",
            {"operation_id": "already-open-release"},
        )
        assert code == 200 and released["mutation"]["changed"] is False

        streams = []
        try:
            for _ in range(PREVIEW.STATE.sse_client_limit):
                stream = urlopen(base + "/api/v1/events/stream", timeout=2)
                streams.append(stream)
                assert stream.status == 200
                assert stream.headers.get_content_type() == "text/event-stream"
                assert stream.headers.get("Transfer-Encoding") == "chunked"
                payload = None
                for _ in range(8):
                    line = stream.readline().decode().strip()
                    if line.startswith("data: "):
                        payload = json.loads(line[6:])
                        break
                assert payload is not None
                validate("status", payload, "initial SSE status")
                assert payload["boot_id"] == status["boot_id"]
                assert payload["state_revision"] >= status["state_revision"]
                if len(streams) == 1:
                    prior_revision = payload["state_revision"]
                    PREVIEW.STATE.set_emergency_input(True)
                    updated = None
                    for _ in range(12):
                        line = stream.readline().decode().strip()
                        if line.startswith("data: "):
                            updated = json.loads(line[6:])
                            break
                    assert updated is not None
                    validate("status", updated, "updated SSE status")
                    assert updated["emergency_input_active"] is True
                    assert updated["state_revision"] > prior_revision
                    PREVIEW.STATE.set_emergency_input(False)

            try:
                urlopen(base + "/api/v1/events/stream", timeout=2)
                raise AssertionError("an SSE client beyond the limit was accepted")
            except HTTPError as error:
                assert error.code == 503
                payload = json.load(error)
                validate("error-response", payload, "SSE capacity rejection")
                assert payload["code"] == "sse_client_limit"

            # Occupied long-lived stream slots must not consume ordinary REST
            # request capacity.
            code, live_status = request("/api/v1/status")
            assert code == 200 and live_status["boot_id"] == status["boot_id"]
            code, live_diagnostics = request("/api/v1/system/diagnostics")
            assert code == 200
            assert live_diagnostics["sse_clients"] == PREVIEW.STATE.sse_client_limit
            assert live_diagnostics["sse_accepted_count"] == PREVIEW.STATE.sse_client_limit
        finally:
            for stream in streams:
                stream.close()

        code, forgotten = request("/api/v1/wifi/forget", "POST", {})
        assert code == 202 and forgotten["standalone"]
        code, restart = request("/api/v1/system/restart", "POST", {})
        assert code == 202 and restart["restarting"]
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    print("preview API tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
