#!/usr/bin/env python3
"""Schema graph and compatibility regressions, independent of preview state."""

from copy import deepcopy
import json
from pathlib import Path
import unittest

from api_contract import Draft202012Validator, REGISTRY, SCHEMAS, validate

CASES = json.loads((Path(__file__).resolve().parents[1] /
                   "vectors/local-api/regressions.json").read_text())


class SchemaContractTests(unittest.TestCase):
    def test_schema_definitions_and_all_references_resolve_offline(self):
        ids = [schema["$id"] for schema in SCHEMAS.values()]
        self.assertEqual(len(ids), len(set(ids)))

        def references(value, resolver):
            if isinstance(value, dict):
                if "$ref" in value:
                    resolver.lookup(value["$ref"])
                for child in value.values():
                    references(child, resolver)
            elif isinstance(value, list):
                for child in value:
                    references(child, resolver)

        for name, schema in SCHEMAS.items():
            with self.subTest(schema=name):
                Draft202012Validator.check_schema(schema)
                references(schema, REGISTRY.resolver(schema["$id"]))

    def test_representative_response_examples(self):
        for case in CASES:
            with self.subTest(case=case["name"]):
                validate(case["schema"], case["payload"], case["name"])

    def test_network_modes_and_capability_types(self):
        for mode, state in (("wifi", "connected"), ("ap", "standalone"), ("bluetooth", "disabled")):
            validate("wifi-status", {"network_mode": mode, "state": state})
        for changes in ({"network_mode": "unknown"}, {"state": "unknown"},
                        {"wifi_suspend_supported": "true"}, {"retry_in_ms": -1}):
            payload = {**CASES[0]["payload"], **changes}
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                validate("wifi-status", payload)

    def test_optional_mutation_id_does_not_relax_request_id(self):
        for value in (None, "phone-123"):
            payload = deepcopy(CASES[1]["payload"])
            payload["mutation"]["operation_id"] = value
            validate("timer-mutation-response", payload)
        for value in (None, "", "\n", "x" * 65, 123):
            with self.subTest(value=value), self.assertRaises(AssertionError):
                validate("timer-start-request", {"duration_ms": 1000, "operation_id": value})
        for value in ("", "\n", "x" * 65, 123):
            payload = deepcopy(CASES[1]["payload"])
            payload["mutation"]["operation_id"] = value
            with self.subTest(value=value), self.assertRaises(AssertionError):
                validate("timer-mutation-response", payload)

    def test_errors_are_extensible_but_still_structured(self):
        for code in ("request_too_large", "ambiguous_request", "duplicate_field", "invalid_field",
                     "invalid_field_type", "response_unavailable", "ota_not_armed",
                     "invalid_wifi_credentials", "wifi_disabled", "network_mode_busy",
                     "boot_validation_pending", "future_device_error"):
            validate("error-response", {"error": "Example failure", "code": code, "state": "open"})
        for payload in ({"error": "missing code"}, {"code": "missing_message"},
                        {"error": "", "code": ""}, {"error": "bad type", "code": 42},
                        {"error": "bad state", "code": "request_failed", "state": "disabled"}):
            with self.subTest(payload=payload), self.assertRaises(AssertionError):
                validate("error-response", payload)

    def test_fixed_and_random_start_are_exclusive(self):
        validate("timer-start-request", {"duration_ms": 1000})
        validate("timer-start-request", {"min_ms": 1000, "max_ms": 2000})
        for payload in ({"duration_ms": 1000, "max_ms": 2000},
                        {"duration_ms": 1000, "min_ms": 1000},
                        {"duration_ms": 1000, "min_ms": 1000, "max_ms": 2000},
                        {"min_ms": 1000}, {"max_ms": 2000}, {"duration_ms": 0}):
            with self.subTest(payload=payload), self.assertRaises(AssertionError):
                validate("timer-start-request", payload)

    def test_ap_generated_name_and_kept_password(self):
        for payload in (
            {"security": "wpa2", "use_device_name": True, "keep_password": True, "ssid": "", "password": ""},
            {"security": "wpa2", "ssid": "Example", "password": "example-passphrase"},
            {"security": "open", "use_device_name": True, "password": ""},
        ):
            validate("wifi-ap-update-request", payload)
        for payload in (
            {"security": "open", "ssid": ""},
            {"security": "wpa2", "ssid": "Example", "password": ""},
            {"security": "wpa2", "ssid": "Example", "password": "short"},
            {"security": "wpa2", "ssid": "Example", "keep_password": False},
        ):
            with self.subTest(payload=payload), self.assertRaises(AssertionError):
                validate("wifi-ap-update-request", payload)

    def test_status_and_sse_require_identity_and_correct_types(self):
        for field, value in (("remaining_ms", "0"), ("build_id", "preview"),
                             ("boot_id", "invalid-boot"), ("wifi_state", "unknown")):
            payload = {**CASES[1]["payload"], field: value}
            with self.subTest(field=field), self.assertRaises(AssertionError):
                validate("status", payload)
        payload = deepcopy(CASES[1]["payload"])
        del payload["boot_id"]
        with self.assertRaises(AssertionError):
            validate("status", payload)

    def test_ota_prepare_uses_ready_and_bluetooth_name_can_be_uninitialized(self):
        with self.assertRaises(AssertionError):
            validate("ota-prepare-response", {"reserved": True, "quiet_window_ms": 10000})
        base = {"supported": True, "protocol": "ost-ble/1", "mode": "bonded_authenticated_control"}
        for name in ("", "OST-ab1202"):
            validate("bluetooth-status", {**base, "advertised_name": name})
        with self.assertRaises(AssertionError):
            validate("bluetooth-status", {**base, "advertised_name": "OST-PREV02"})


if __name__ == "__main__":
    unittest.main()
