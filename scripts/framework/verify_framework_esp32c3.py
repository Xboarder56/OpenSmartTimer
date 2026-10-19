#!/usr/bin/env python3
"""Verify the installed slim ESP32-C3 framework against its committed lock."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
LOCK_PATH = ROOT / "framework/esp32c3/lock.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_config(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            result[line[2:-11]] = "n"
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path,
                        default=ROOT / ".framework/esp32c3-libs")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--allow-pending-lock", action="store_true")
    args = parser.parse_args()
    lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    package = args.package
    profile_path = package / "opensmarttimer-framework.json"
    sdkconfig_path = package / "esp32c3/sdkconfig"
    dependency_path = package / "esp32c3/dependencies.lock"
    for path in (profile_path, sdkconfig_path, dependency_path,
                 package / "package.json", package / "tools.json"):
        require(path.is_file(), f"framework package is missing {path.name}")

    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    package_manifest = json.loads(
        (package / "package.json").read_text(encoding="utf-8")
    )
    require(profile["profile"] == lock["profile"], "framework profile mismatch")
    require(package_manifest["version"] == lock["package_version"],
            "framework package version mismatch")
    for field in ("builder_commit", "arduino_commit", "esp_idf_commit",
                  "tinyusb_commit", "builder_image", "source_date_epoch"):
        require(profile[field] == lock[field], f"framework {field} mismatch")

    actual_sdkconfig = sha256(sdkconfig_path)
    actual_dependencies = sha256(dependency_path)
    require(profile["sdkconfig_sha256"] == actual_sdkconfig,
            "profile sdkconfig hash mismatch")
    require(profile["dependencies_lock_sha256"] == actual_dependencies,
            "profile dependency-lock hash mismatch")
    if not args.allow_pending_lock:
        require(lock["sdkconfig_sha256"] == actual_sdkconfig,
                "locked sdkconfig hash mismatch")
        require(lock["dependencies_lock_sha256"] == actual_dependencies,
                "locked dependency hash mismatch")

    expected = {
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_APP_REPRODUCIBLE_BUILD": "y",
        "CONFIG_LWIP_IPV6": "y",
        "CONFIG_LWIP_PPP_SUPPORT": "n",
        "CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT": "n",
        "CONFIG_ESP_WIFI_FTM_ENABLE": "n",
        "CONFIG_BT_NIMBLE_ENABLED": "y",
        # One peripheral connection, sized to leave heap for the relay's TLS.
        "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": "1",
        "CONFIG_BT_NIMBLE_MAX_BONDS": "4",
        "CONFIG_BT_NIMBLE_MAX_CCCDS": "16",
        "CONFIG_BT_NIMBLE_ROLE_CENTRAL": "n",
        "CONFIG_BT_NIMBLE_ROLE_OBSERVER": "n",
        "CONFIG_BT_NIMBLE_GATT_CLIENT": "n",
        "CONFIG_BT_NIMBLE_ROLE_PERIPHERAL": "y",
        "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT": "8",
        "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT": "12",
        "CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT": "12",
        "CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT": "16",
        "CONFIG_BT_CTRL_BLE_MAX_ACT": "3",
        # The controller's connection feature; connectable advertising needs it.
        "CONFIG_BT_CTRL_BLE_MASTER": "y",
        "CONFIG_BT_CTRL_BLE_ADV": "y",
        "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE": "y",
        "CONFIG_BT_CTRL_BLE_SCAN": "n",
        "CONFIG_BT_NIMBLE_GATT_SERVER": "y",
        "CONFIG_BT_NIMBLE_SECURITY_ENABLE": "y",
        "CONFIG_BT_NIMBLE_NVS_PERSIST": "y",
        "CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT": "n",
        "CONFIG_MBEDTLS_TLS_CLIENT": "y",
        "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE": "y",
        "CONFIG_MBEDTLS_HAVE_TIME": "y",
        "CONFIG_MBEDTLS_HAVE_TIME_DATE": "y",
        "CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS": "y",
        "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": "y",
        "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_RSA": "y",
        "CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA": "y",
        "CONFIG_MBEDTLS_ECDSA_C": "y",
        "CONFIG_MBEDTLS_SHA512_C": "y",
        "CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED": "y",
        "CONFIG_MBEDTLS_ECP_DP_SECP384R1_ENABLED": "y",
        "CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED": "y",
        "CONFIG_COMPILER_CXX_EXCEPTIONS": "n",
        "CONFIG_LIBC_NEWLIB_NANO_FORMAT": "y",
        "CONFIG_ESP_COREDUMP_ENABLE_TO_NONE": "y",
        "CONFIG_MBEDTLS_ERROR_STRINGS": "n",
    }
    config = read_config(sdkconfig_path)
    for key, value in expected.items():
        # Kconfig may omit a symbol entirely when one of its dependencies is
        # disabled.  An omitted boolean is therefore equivalent to `n`, but an
        # omitted required/enabled value must still fail verification.
        actual = config.get(key, "n" if value == "n" else None)
        require(actual == value,
                f"framework config mismatch: {key}={config.get(key)!r}, expected {value!r}")
    # These are the headers PlatformIO actually compiles the app against.
    # Check both packaged memory variants, not only the builder's sdkconfig.
    for variant in ("dio_qspi", "qio_qspi"):
        header_path = package / "esp32c3" / variant / "include/sdkconfig.h"
        require(header_path.is_file(), f"missing {variant} sdkconfig.h")
        macros = dict(re.findall(r"^#define (CONFIG_\w+) (\S+)$",
                                 header_path.read_text(encoding="utf-8"), re.MULTILINE))
        for key, value in expected.items():
            header_value = "1" if value == "y" else None if value == "n" else value
            actual = macros.get(key)
            seen = {key}
            # sdkconfig.h retains renamed Kconfig options as macro aliases.
            while actual is not None and actual.startswith("CONFIG_"):
                require(actual not in seen, f"cyclic {variant} config alias: {key}")
                seen.add(actual)
                actual = macros.get(actual)
            require(actual == header_value,
                    f"{variant} config mismatch: {key}={actual!r}, expected {header_value!r}")
    dependencies = dependency_path.read_text(encoding="utf-8")
    for library in ("libesp_http_client.a", "libesp_https_ota.a", "libapp_update.a",
                    "libesp-tls.a", "libmbedtls.a", "libmbedcrypto.a", "libbt.a"):
        path = package / "esp32c3/lib" / library
        require(path.is_file() and path.stat().st_size > 8,
                f"required HTTPS/OTA/BLE framework library missing: {library}")
    require("espressif/esp-tflite-micro:" not in dependencies,
            "TensorFlow Lite dependency unexpectedly present")
    require("espressif/esp_matter:" not in dependencies,
            "Matter dependency unexpectedly present")
    require("version: 1.8.2" in dependencies and "espressif/mdns:" in dependencies,
            "mDNS is not pinned to 1.8.2")

    if args.archive is not None:
        actual_archive = sha256(args.archive)
        if not args.allow_pending_lock:
            require(lock["archive_sha256"] == actual_archive,
                    "locked framework archive hash mismatch")
        print(f"framework archive sha256={actual_archive}")
    print(f"framework {lock['profile']} verified")
    print(f"sdkconfig sha256={actual_sdkconfig}")
    print(f"dependencies sha256={actual_dependencies}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, OSError, RuntimeError, ValueError) as error:
        print(f"framework verification failed: {error}")
        raise SystemExit(1)
