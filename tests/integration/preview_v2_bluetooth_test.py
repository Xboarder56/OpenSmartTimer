#!/usr/bin/env python3
"""Exercise the V2-only authenticated Bluetooth control preview contract."""

from http.server import ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import importlib.util
import json

from api_contract import validate_response


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "preview_ui_v2", ROOT / "tools" / "preview_ui.py"
)
PREVIEW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREVIEW)


class QuietHandler(PREVIEW.PreviewHandler):
    def log_message(self, message, *args):
        pass


def main():
    PREVIEW.STATE = PREVIEW.PreviewState(hardware_v2=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"

    def request(path, method="GET", fields=None):
        call = Request(base + path, data=json.dumps(fields).encode() if fields is not None else b"" if method == "POST" else None,
                       method=method, headers={"Content-Type": "application/json"} if fields is not None else {})
        try:
            with urlopen(call, timeout=2) as response:
                return validate_response(path, method, response.status, json.load(response), fields)
        except HTTPError as error:
            return validate_response(path, method, error.code, json.load(error))

    try:
        code, status = request("/api/v1/status")
        assert code == 200 and status["chip_family"] == "ESP32-C3"
        assert len(status["build_id"]) == 24
        code, capabilities = request("/api/v1/capabilities")
        assert code == 200
        assert capabilities["features"]["bluetooth_peer_discovery"] is False
        assert capabilities["features"]["bluetooth_peer_control"] is False
        assert capabilities["features"]["bluetooth_control"] is True
        assert capabilities["features"]["bluetooth_authenticated_control"] is True
        assert capabilities["features"]["bluetooth_bonding"] is True
        assert capabilities["features"]["bluetooth_command_integrity"] is True
        assert capabilities["features"]["bluetooth_physical_pairing"] is True
        assert capabilities["features"]["bluetooth_event_notifications"] is True
        assert capabilities["features"]["bluetooth_event_replay"] is True
        assert capabilities["features"]["bluetooth_pairing_commit"] is True
        assert capabilities["features"]["bluetooth_identify"] is True
        assert capabilities["bluetooth"]["inbound_connections"] == 1
        assert capabilities["bluetooth"]["controller_connections"] == 1
        assert capabilities["bluetooth"]["protocol"] == "ost-ble/1"
        assert capabilities["bluetooth"]["capabilities_frame_bytes"] == 24
        assert capabilities["bluetooth"]["command_frame_bytes"] == 32
        assert capabilities["bluetooth"]["system_information_frame_bytes"] == 44
        assert capabilities["bluetooth"]["required_control_mtu"] == 35
        assert capabilities["bluetooth"]["event_history_entries"] == 16
        assert capabilities["build_id"] == status["build_id"]

        code, bluetooth = request("/api/v1/bluetooth")
        assert code == 200 and bluetooth["initialized"] is False
        assert bluetooth["enabled"] is False
        assert bluetooth["control_supported"] is True
        assert bluetooth["control_enabled"] is False
        assert bluetooth["paired"] is False
        assert bluetooth["persistent_enable"] is True
        PREVIEW.STATE.start(5000)
        code, active = request("/api/v1/bluetooth/enable")
        assert code == 409 and active["code"] == "timer_active"
        PREVIEW.STATE.open()

        code, bluetooth = request("/api/v1/bluetooth/enable")
        assert code == 200 and bluetooth["initialized"] is True
        assert bluetooth["enabled"] is True
        assert bluetooth["default_enabled"] is False
        assert bluetooth["configured_enabled"] is True
        assert bluetooth["volatile_enable"] is False
        assert bluetooth["connection_rssi_dbm"] is None
        assert bluetooth["last_disconnect_reason_name"] == "none"
        assert bluetooth["peer_discovery_supported"] is False
        assert bluetooth["required_control_mtu"] == 35
        assert bluetooth["last_error_name"] == "none"

        code, control = request("/api/v1/bluetooth/pairing/start", "POST")
        assert code == 200 and control["control_enabled"] is False
        assert control["paired"] is False
        assert control["pairing_active"] is True
        assert control["pairing_state"] == "waiting_for_connection"
        assert "access_key" not in control
        code, control_status = request("/api/v1/bluetooth")
        assert code == 200 and control_status["control_enabled"] is False
        assert "access_key" not in control_status
        code, canceled = request("/api/v1/bluetooth/pairing/cancel", "POST")
        assert code == 200 and canceled["pairing_active"] is False
        assert canceled["pairing_state"] == "idle"
        assert canceled["paired"] is False

        code, diagnostics = request("/api/v1/system/diagnostics")
        assert code == 200 and diagnostics["bluetooth_initialized"] is True
        assert diagnostics["bluetooth_enabled"] is True
        assert diagnostics["bluetooth_configured_enabled"] is True
        assert diagnostics["bluetooth_paired"] is False
        assert diagnostics["bluetooth_control_enabled"] is False
        assert diagnostics["bluetooth_authentication_backoff_remaining_ms"] == 0
        assert diagnostics["bluetooth_connection_mtu_sufficient"] is False
        assert diagnostics["bluetooth_bond_cleanup_pending"] is False
        assert diagnostics["minimum_observed_free_heap_bytes"] > 0
        assert diagnostics["bluetooth_last_error_name"] == "none"
        assert diagnostics["build_id"] == status["build_id"]

        PREVIEW.STATE.start(5000)
        code, active = request("/api/v1/bluetooth/disable")
        assert code == 409 and active["code"] == "timer_active"
        code, stopped = request("/api/v1/bluetooth/pairing/revoke", "POST")
        assert code == 409 and stopped["code"] == "timer_active"
        PREVIEW.STATE.open()
        code, stopped = request("/api/v1/bluetooth/pairing/revoke", "POST")
        assert code == 200 and stopped["control_enabled"] is False
        assert stopped["paired"] is False
        # Fixture approvals stand in for physical BLE ceremonies; preview does
        # not mint keys or pretend to qualify on-device pairing.
        PREVIEW.STATE.bluetooth_apps = [
            {"app_id": i, "label": f"App {i}", "connected": False, "last_seen_at_ms": None}
            for i in (1, 2, 3)
        ]
        code, apps = request("/api/v1/bluetooth/apps")
        assert code == 200 and apps["approved_app_count"] == 3 and apps["connection_limit"] == 1
        code, full = request("/api/v1/bluetooth/pairing/start", "POST")
        assert code == 409 and full["code"] == "app_table_full"
        code, renamed = request("/api/v1/bluetooth/apps/label", "PUT", {"app_id": 2, "label": "我的手机"})
        assert code == 200 and renamed["approved_apps"][1]["label"] == "我的手机"
        code, invalid = request("/api/v1/bluetooth/apps/label", "PUT", {"app_id": 2, "label": "中" * 9})
        assert code == 400 and invalid["code"] == "invalid_label"
        code, replacement = request("/api/v1/bluetooth/pairing/start", "POST", {"replace_app_id": 2})
        assert code == 200 and replacement["approved_app_count"] == 3
        code, blocked = request("/api/v1/bluetooth/apps/revoke", "POST", {"app_id": 1})
        assert code == 409 and blocked["code"] == "pairing_active"
        request("/api/v1/bluetooth/pairing/cancel", "POST")
        code, removed = request("/api/v1/bluetooth/apps/revoke", "POST", {"app_id": 2})
        assert code == 200 and [a["app_id"] for a in removed["approved_apps"]] == [1, 3]
        code, absent = request("/api/v1/bluetooth/apps/revoke", "POST", {"app_id": 2})
        assert code == 404 and absent["code"] == "app_not_found"
        code, cleared = request("/api/v1/bluetooth/pairing/revoke", "POST")
        assert code == 200 and cleared["approved_apps"] == []
        code, stopped = request("/api/v1/bluetooth/disable")
        assert code == 200 and stopped["enabled"] is False
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)

    print("V2 Bluetooth preview tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
