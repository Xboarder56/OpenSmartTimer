#!/usr/bin/env python3
"""Wait for the owner's physical OTA arm on a bench timer, then install an image
and confirm the timer reports the expected build ID. Never arms anything itself.

  python3 tools/install_when_armed.py --timer http://<timer> --image <image.bin>

The expected build ID comes from BUILD.json next to the image (a packaged
candidate, e.g. a copy of dist/firmware), else from generated_build_identity.h
(a PlatformIO build folder).
"""
import argparse, json, re, time, uuid, zlib, urllib.request
from pathlib import Path
parser = argparse.ArgumentParser()
parser.add_argument("--timer", required=True)
parser.add_argument("--image", required=True, type=Path)
args = parser.parse_args()
TIMER = args.timer.rstrip("/")
image = args.image.read_bytes()
crc = format(zlib.crc32(image) & 0xFFFFFFFF, "08x")
manifest = args.image.parent / "BUILD.json"
if manifest.exists():  # packaged candidate: the ID the release records
    build_id = next(a["build_id"] for a in json.loads(manifest.read_text())["artifacts"] if a["file"] == args.image.name)
else:
    header = (args.image.parent / "generated_build_identity.h").read_text()
    build_id = re.search(r'SMART_TIMER_BUILD_ID "([0-9a-f]{24})"', header).group(1)
art = {"build_id": build_id,
       "file": args.image.name}

def get(path):
    return json.load(urllib.request.urlopen(TIMER + path, timeout=10))
def log(msg): print(time.strftime("%H:%M:%S"), msg, flush=True)

log(f"waiting for OTA arm; target build {art['build_id']} ({len(image)} B, crc {crc})")
deadline = time.time() + 6 * 3600
while time.time() < deadline:
    try:
        s = get("/api/v1/status")
    except Exception as e:
        time.sleep(3); continue
    if s.get("build_id") == art["build_id"]:
        log("already running the target build"); break
    if s.get("ota_armed") and s.get("state") == "open" and not s.get("ota_unverified_allowed"):
        log("armed; preparing upload")
        body = f"image_size={len(image)}&image_crc32={crc}".encode()
        req = urllib.request.Request(TIMER + "/api/v1/ota/prepare", data=body, method="POST",
                                     headers={"Content-Type": "application/x-www-form-urlencoded"})
        log("prepare: " + urllib.request.urlopen(req, timeout=15).read().decode()[:200])
        time.sleep(2)
        b = uuid.uuid4().hex
        parts = [f"--{b}\r\nContent-Disposition: form-data; name=\"image_size\"\r\n\r\n{len(image)}\r\n".encode(),
                 f"--{b}\r\nContent-Disposition: form-data; name=\"image_crc32\"\r\n\r\n{crc}\r\n".encode(),
                 f"--{b}\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"{art['file']}\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n".encode() + image + b"\r\n",
                 f"--{b}--\r\n".encode()]
        req = urllib.request.Request(TIMER + "/api/v1/ota", data=b"".join(parts), method="POST",
                                     headers={"Content-Type": f"multipart/form-data; boundary={b}"})
        log("upload: " + urllib.request.urlopen(req, timeout=180).read().decode()[:200])
        for _ in range(60):
            time.sleep(3)
            try:
                s = get("/api/v1/status")
                log(f"after restart: build {s['build_id']} state {s['state']} uptime {s['uptime_ms']}")
                break
            except Exception:
                pass
        break
    time.sleep(2)
