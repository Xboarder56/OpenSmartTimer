Import("env")

import gzip
import os
import re
from pathlib import Path
from runpy import run_path


project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
output_path = os.path.join(build_dir, "generated_web_assets.h")
patch_update_policy = run_path(os.path.join(project_dir, "scripts/build/update_source.py"))["patch_update_policy"]
asset_paths = [
    "web/index.html",
    "web/styles.css",
    "web/i18n.js",
    "web/appearance.js",
    "web/updates.js",
    "web/app.js",
]


def symbol_for(path):
    return "web_" + re.sub(r"[^a-zA-Z0-9]", "_", path)


def byte_lines(data):
    values = [f"0x{value:02x}" for value in data]
    return [", ".join(values[index : index + 16]) for index in range(0, len(values), 16)]


os.makedirs(build_dir, exist_ok=True)
parts = ["#pragma once", "", "#include <Arduino.h>", ""]
for relative_path in asset_paths:
    absolute_path = os.path.join(project_dir, relative_path)
    with open(absolute_path, "rb") as source:
        data = source.read()
    if relative_path == "web/index.html":
        data = patch_update_policy(data, Path(project_dir))
    compressed = gzip.compress(data, compresslevel=9, mtime=0)
    # Python versions disagree on the header's OS byte (255 or 3 for Unix),
    # which made images built on macOS and in CI differ. Fix it at 255,
    # "unknown"; the header has no CRC, so nothing else depends on it.
    compressed = compressed[:9] + b"\xff" + compressed[10:]
    symbol = symbol_for(relative_path)
    parts.append(f"static const uint8_t {symbol}[] PROGMEM = {{")
    parts.extend(f"  {line}," for line in byte_lines(compressed))
    parts.append("};")
    parts.append(f"static constexpr size_t {symbol}_len = sizeof({symbol});")
    parts.append("")

contents = "\n".join(parts)
existing = None
if os.path.exists(output_path):
    with open(output_path, "r", encoding="utf-8") as current:
        existing = current.read()
if existing != contents:
    with open(output_path, "w", encoding="utf-8", newline="\n") as generated:
        generated.write(contents)

env.Append(CPPPATH=[build_dir])
env.Depends(
    "$BUILD_DIR/${PROGNAME}.elf",
    [os.path.join(project_dir, path) for path in asset_paths +
     ["include/ReleaseUpdateConfig.h", "scripts/build/update_source.py"]],
)
