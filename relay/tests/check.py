#!/usr/bin/env python3
"""Relay-only source gate. No hardware, container, production state, or firmware build."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from host_build import COMMON, ROOT_FIRMWARE_FLAGS, ROOT_FIRMWARE_SOURCES, compile_host

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


run([sys.executable, "-m", "pytest", "-q"])
run([os.environ.get("NODE", "node"), "--test", *sorted((ROOT / "tests").glob("*.test.cjs"))])
with tempfile.TemporaryDirectory(prefix="ostrelay-host-") as tmp:
    for name, sources in [
        ("ecdsa_p256_test", COMMON),
        ("capability_token_test", COMMON),
        ("relay_json_test", ["../src/relay/RelayJson.cpp"]),
        ("relay_client_test", COMMON + ["../src/relay/RelayClient.cpp", "../src/relay/RelayClientSecure.cpp"]),
        ("relay_endpoint_test", COMMON + ["../src/relay/RelayEndpoint.cpp"]),
        ("relay_owner_job_test", []),
        ("relay_setup_approval_test", []),
    ]:
        binary = Path(tmp) / name
        compile_host(name, sources, binary)
        run([binary])
    # Relay adapter over the actual firmware TimerController/shared rules.
    binary = Path(tmp) / "timer_authority_control_test"
    compile_host("timer_authority_control_test",
                 ["../src/relay/TimerAuthorityControl.cpp", *ROOT_FIRMWARE_SOURCES],
                 binary, ROOT_FIRMWARE_FLAGS)
    run([binary])
    # Real EspWssTransport against a host model of ESP-IDF transport_ws.
    binary = Path(tmp) / "esp_wss_transport_test"
    compile_host("esp_wss_transport_test",
                 ["../src/relay/EspWssTransport.cpp", "../src/relay/RelayEndpoint.cpp",
                  "../src/relay/RelayJson.cpp", "../src/relay/RelayEncoding.cpp"],
                 binary, ["-DESP_PLATFORM", "-Itests/firmware/esp_stubs"])
    run([binary])
print("Relay source gate passed")
