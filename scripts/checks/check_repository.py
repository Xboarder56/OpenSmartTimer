#!/usr/bin/env python3
"""Validate development-tool syntax and the PlatformIO hook paths without running tools."""

import ast
import configparser
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def main():
    roots = [ROOT / name for name in ("scripts", "tests", "tools", "relay/tools")]
    python_files = sorted(path for root in roots for path in root.rglob("*.py")
                          if "__pycache__" not in path.parts)
    for path in python_files:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    shell_files = sorted(path for root in roots for path in root.rglob("*.sh"))
    for path in shell_files:
        subprocess.run(["bash", "-n", str(path)], check=True)

    ini = configparser.ConfigParser(interpolation=None)
    ini.read(ROOT / "platformio.ini")
    for section in ini.sections():
        if not section.startswith("env:"):
            continue
        for entry in ini.get(section, "extra_scripts", fallback="").splitlines():
            if not entry.strip():
                continue
            path = ROOT / entry.strip().split(":", 1)[-1]
            assert path.is_file(), f"Missing PlatformIO hook: {path}"
    check_relay_boundary(ini)
    print(f"Repository tools: {len(python_files)} Python and {len(shell_files)} shell files valid; build hooks resolve")


# The experimental relay is V2-only. Keep V1 and V2 one codebase: relay code
# lives in src/relay/ and include/relay/ plus src/WebRelay.cpp, and shared
# files carry the SMART_TIMER_RELAY switch only at these wiring points.
RELAY_SWITCH_ALLOWED = {
    "src/main.cpp", "src/WebApi.cpp", "src/WebStatus.cpp", "src/WebRelay.cpp",
    "include/WebApplication.h",
}
RELAY_HEADER_USERS = {"src/main.cpp", "src/WebRelay.cpp"}


def check_relay_boundary(ini):
    relay_headers = {path.name for path in (ROOT / "include" / "relay").glob("*.h")}
    for path in sorted([*(ROOT / "src").rglob("*"), *(ROOT / "include").rglob("*")]):
        if not path.is_file() or path.suffix not in {".cpp", ".h"}:
            continue
        relative = str(path.relative_to(ROOT))
        if relative.startswith(("src/relay/", "include/relay/")):
            continue
        text = path.read_text(encoding="utf-8")
        if "SMART_TIMER_RELAY" in text:
            assert relative in RELAY_SWITCH_ALLOWED, (
                f"{relative} uses SMART_TIMER_RELAY; put relay behavior in src/relay "
                "or an existing wiring point instead of branching shared code")
        for header in relay_headers:
            if f'#include "{header}"' in text:
                assert relative in RELAY_HEADER_USERS, (
                    f"{relative} includes relay header {header} outside the wiring points")
    # One switch: every environment excludes the relay; only relay_option.py
    # (V2 only, driven by [opensmarttimer] relay) adds it back.
    assert ini.get("opensmarttimer", "relay", fallback="") in {"enabled", "disabled"}, (
        "[opensmarttimer] relay must be enabled or disabled")
    for section in ini.sections():
        if not section.startswith("env:"):
            continue
        flags = ini.get(section, "build_flags", fallback="")
        source_filter = ini.get(section, "build_src_filter", fallback="")
        scripts = ini.get(section, "extra_scripts", fallback="")
        assert "SMART_TIMER_RELAY" not in flags and "include/relay" not in flags, (
            f"[{section}] enable the relay through [opensmarttimer] relay, not build_flags")
        assert "-<relay/>" in source_filter and "-<WebRelay.cpp>" in source_filter, (
            f"[{section}] must exclude src/relay/ and src/WebRelay.cpp by default")
        if "relay_option.py" in scripts:
            assert "SMART_TIMER_HARDWARE_V2" in flags, f"[{section}] relay is V2-only"
            assert ini.get(section, "custom_relay", fallback="") == "${opensmarttimer.relay}", (
                f"[{section}] custom_relay must follow [opensmarttimer] relay")
            assert scripts.index("relay_option.py") < scripts.index("build_identity.py"), (
                f"[{section}] relay_option.py must run before build_identity.py")


if __name__ == "__main__":
    main()
