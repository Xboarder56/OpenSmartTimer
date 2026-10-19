#!/usr/bin/env python3
"""Drive the experimental relay on a real V2 timer and record its resources.

Uses only the timer's local HTTP API (no dependencies). Steps: confirm relay
support, enroll (optional), wait for a live connection, create a share link,
wait for relay confirmation, print the share URL, then optionally sample
status for a soak period and write a CSV of heap, relay-task stack and
reconnects. Never flashes firmware or changes the timer state beyond the
relay settings.

Examples:
  device_relay_check.py --timer http://smarttimer.local \\
      --relay-url https://relay.example.com --code ENROLLCODE --pin PIN43CHARS
  device_relay_check.py --timer http://192.168.1.50 --soak-minutes 60 --csv soak.csv
"""
import argparse
import csv
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def call(base, method, path, body=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(base + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read() or b"{}")
        except json.JSONDecodeError:
            return error.code, {}


def wait_for(base, predicate, seconds, label):
    deadline = time.monotonic() + seconds
    status = {}
    while time.monotonic() < deadline:
        _, status = call(base, "GET", "/api/v1/relay")
        if predicate(status):
            return status
        if status.get("last_error") and not status.get("enrolling") and status.get("state") != "connected":
            raise SystemExit(f"{label} failed: {status['last_error']}")
        time.sleep(2)
    raise SystemExit(f"{label} timed out; last status: {json.dumps(status)}")


def run_job(base, path, fields, seconds=60):
    """Submit an asynchronous owner job with the current next_job_id and wait for it."""
    _, status = call(base, "GET", "/api/v1/relay")
    job_id = status["next_job_id"]
    code, body = call(base, "POST", path, {"job_id": job_id, "if_boot_id": status["boot_id"], **fields})
    if code != 202:
        raise SystemExit(f"{path} refused: {code} {body}")
    status = wait_for(base, lambda s: s["job"]["id"] == job_id and s["job"]["state"] == "done",
                      seconds, f"Job {job_id}")
    if status["job"]["result"] != "ok":
        raise SystemExit(f"{path} job {job_id} failed: {status['job']['result']}")
    return status["job"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--timer", required=True, help="timer base URL, e.g. http://smarttimer.local")
    parser.add_argument("--relay-url", help="relay HTTPS origin, to enroll")
    parser.add_argument("--code", help="single-use enrollment code from the relay operator")
    parser.add_argument("--pin", help="relay SPKI pin (43-character base64url)")
    parser.add_argument("--ttl", type=int, default=900, help="share-link lifetime in seconds")
    parser.add_argument("--label", default="Hardware check")
    parser.add_argument("--soak-minutes", type=float, default=0, help="sample status for this long")
    parser.add_argument("--csv", help="write soak samples here")
    args = parser.parse_args()
    base = args.timer.rstrip("/")

    _, capabilities = call(base, "GET", "/api/v1/capabilities")
    if not capabilities.get("features", {}).get("relay"):
        raise SystemExit("This timer image has no relay support (features.relay is false).")
    print(f"Timer {capabilities.get('hardware_model')} build {capabilities.get('build_id')}")

    if args.relay_url:
        if not (args.code and args.pin):
            raise SystemExit("--relay-url needs --code and --pin")
        code, body = call(base, "POST", "/api/v1/relay/enroll",
                          {"relay_url": args.relay_url, "enrollment_code": args.code, "spki_pin": args.pin})
        if code != 202:
            raise SystemExit(f"enroll refused: {code} {body}")
        print("Enrolling…")
    status = wait_for(base, lambda s: s.get("state") == "connected" and s.get("sharing_available"),
                      90, "Relay connection")
    print(f"Connected to {status['relay_host']} as {status['timer_id']}; "
          f"heap {status['free_heap_bytes']} B, largest block {status['largest_free_block_bytes']} B, "
          f"relay stack low-water {status['task_stack_min_free_bytes']} B")

    job = run_job(base, "/api/v1/relay/links", {"ttl_s": args.ttl, "label": args.label})
    fp_hash = job["fp_hash"]
    deadline = time.monotonic() + 30
    while True:
        code, share = call(base, "GET", "/api/v1/relay/links/share?" + urllib.parse.urlencode({"fp_hash": fp_hash}))
        if code == 200:
            break
        if time.monotonic() > deadline:
            raise SystemExit(f"link never confirmed: {code} {share}")
        time.sleep(1)
    print(f"Share link ready ({args.ttl} s): {share['share_url']}")

    if args.soak_minutes <= 0:
        return 0
    samples = []
    end = time.monotonic() + args.soak_minutes * 60
    lowest = {}
    while time.monotonic() < end:
        _, status = call(base, "GET", "/api/v1/relay")
        sample = {key: status.get(key) for key in ("state", "reconnects", "free_heap_bytes",
                                                    "largest_free_block_bytes", "task_stack_min_free_bytes",
                                                    "live_links", "last_error")}
        sample["t_s"] = round(time.monotonic() - (end - args.soak_minutes * 60))
        samples.append(sample)
        for key in ("free_heap_bytes", "largest_free_block_bytes", "task_stack_min_free_bytes"):
            if isinstance(sample[key], int):
                lowest[key] = min(lowest.get(key, sample[key]), sample[key])
        time.sleep(10)
    if args.csv:
        with open(args.csv, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(samples[0]))
            writer.writeheader()
            writer.writerows(samples)
    print(f"Soak {args.soak_minutes} min: lowest {lowest}; reconnects {samples[-1]['reconnects']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
