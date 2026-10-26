"""Offline Draft 2020-12 validation shared by preview API integration tests."""

import json
from pathlib import Path
from urllib.parse import urlsplit

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError as error:
    raise SystemExit(
        "API contract checks require requirements-dev.txt. Install it in a venv "
        "and run PYTHON=<venv>/bin/python ./scripts/check.sh all."
    ) from error


SCHEMA_DIR = Path(__file__).resolve().parents[2] / "docs/schemas/local"
SCHEMAS = {
    path.name: json.loads(path.read_text())
    for path in sorted(SCHEMA_DIR.glob("*.schema.json"))
}
# Explicit local resources only. A missing reference fails; no network resolver.
REGISTRY = Registry().with_resources(
    (schema["$id"], Resource.from_contents(schema)) for schema in SCHEMAS.values()
)
VALIDATORS = {
    name: Draft202012Validator(schema, registry=REGISTRY)
    for name, schema in SCHEMAS.items()
}

# Success response mapping is deliberately explicit: a newly exercised route
# needs a schema or a documented coverage decision, never a silent skip.
RESPONSES = {
    ("GET", "/status"): "status",
    ("GET", "/capabilities"): "capabilities",
    ("GET", "/events"): "events-list",
    ("GET", "/wifi/status"): "wifi-status",
    ("GET", "/wifi/scan"): "wifi-scan-response",
    ("POST", "/wifi/scan"): "wifi-scan-response",
    ("GET", "/timer/controls"): "timer-controls",
    ("POST", "/timer/controls"): "timer-controls",
    ("GET", "/wifi/ap"): "wifi-ap",
    ("PUT", "/wifi/ap"): "wifi-ap-update-response",
    ("POST", "/wifi/configure"): "wifi-configure-response",
    ("POST", "/wifi/forget"): "wifi-forget-response",
    ("GET", "/device/name"): "device-name",
    ("PUT", "/device/name"): "device-name-update-response",
    ("GET", "/system/diagnostics"): "system-diagnostics",
    ("POST", "/system/identify"): "system-identify-response",
    ("POST", "/system/restart"): "system-restart-response",
    ("POST", "/ota/prepare"): "ota-prepare-response",
    ("POST", "/ota/download"): "ota-download-response",
}
for action in ("start", "stop", "release", "pause", "resume", "adjust", "seal", "lock-controls"):
    RESPONSES[("POST", "/timer/" + action)] = "timer-mutation-response"
for method, suffix in (
    ("GET", ""), ("GET", "/enable"), ("GET", "/disable"),
    ("POST", "/pairing/start"), ("POST", "/pairing/cancel"),
    ("POST", "/pairing/revoke"), ("GET", "/apps"),
    ("PUT", "/apps/label"), ("POST", "/apps/revoke"),
):
    RESPONSES[(method, "/bluetooth" + suffix)] = "bluetooth-status"

REQUESTS = {
    ("POST", "/timer/start"): "timer-start-request",
    ("POST", "/timer/controls"): "timer-controls-request",
    ("POST", "/timer/adjust"): "timer-adjust-request",
    ("PUT", "/wifi/ap"): "wifi-ap-update-request",
    ("POST", "/wifi/configure"): "wifi-configure-request",
    ("PUT", "/device/name"): "device-name-update-request",
    ("POST", "/ota/prepare"): "ota-prepare-request",
    ("POST", "/ota/download"): "ota-download-request",
}


def validate(name, payload, context=""):
    """Raise an actionable failure identifying the schema and JSON field."""
    for error in VALIDATORS[name + ".schema.json"].iter_errors(payload):
        path = "/" + "/".join(map(str, error.absolute_path))
        raise AssertionError(f"{context} [{name}] {path}: {error.message}")


def validate_response(path, method, status, payload, request_body=None):
    route = urlsplit(path).path.removeprefix("/api/v1")
    context = f"{method} {path} -> {status}"
    if status >= 400:
        validate("error-response", payload, context)
    else:
        key = (method, route)
        assert key in RESPONSES, f"Missing success response schema mapping: {context}"
        validate(RESPONSES[key], payload, context)
        # Invalid requests are intentionally tested elsewhere. Only requests
        # accepted by the preview must satisfy their documented JSON schema.
        if request_body is not None and key in REQUESTS:
            validate(REQUESTS[key], request_body, context + " request")
    return status, payload


if __name__ == "__main__":
    from argparse import ArgumentParser
    parser = ArgumentParser(description="Validate a saved JSON API reply locally; no device access.")
    parser.add_argument("schema", choices=[name.removesuffix(".schema.json") for name in SCHEMAS])
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    validate(args.schema, json.loads(args.capture.read_text()), str(args.capture))
    print(f"{args.capture}: valid {args.schema}")
