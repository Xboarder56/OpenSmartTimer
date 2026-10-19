"""Bench settings from the environment. Nothing bench-specific is hardcoded:
addresses, the relay's .env (admin token, SPKI pin) and outputs are set here.

  OST_TIMER            V2 timer under test, e.g. http://192.168.4.1 (required)
  OST_RELAY_URL        relay public origin, e.g. https://relay.example.com
  OST_RELAY_ADMIN      relay admin port (default http://127.0.0.1:8080)
  OST_RELAY_ENV        the relay's .env file (admin token and SPKI pin)
  OST_RELAY_CONTAINER  relay container name, for the restart test
  OST_BLE_PYTHON       Python with bleak installed (default: this Python)
  OST_BLE_KEY          Bluetooth app key file for the Bluetooth bench
  OST_BENCH_OUT        output folder (default ./out; git-ignored: holds share links)
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = Path(os.environ.get("OST_BENCH_OUT", HERE / "out"))
(OUT / "shots").mkdir(parents=True, exist_ok=True)


def need(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SystemExit(f"Set {name} (see relay/tools/hardware/README.md).")
    return value.rstrip("/")


def timer() -> str:
    return need("OST_TIMER")


def relay_env() -> dict[str, str]:
    path = Path(need("OST_RELAY_ENV"))
    return dict(line.split("=", 1) for line in path.read_text().splitlines()
                if "=" in line and not line.lstrip().startswith("#"))


def admin_url() -> str:
    return os.environ.get("OST_RELAY_ADMIN", "http://127.0.0.1:8080").rstrip("/")


def ble_python() -> str:
    return os.environ.get("OST_BLE_PYTHON", sys.executable)
