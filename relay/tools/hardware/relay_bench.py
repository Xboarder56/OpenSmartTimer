#!/usr/bin/env python3
"""Bench-only relay hardware harness: owner (timer local API) + operator (admin API).

Usage:
  relay_bench.py status
  relay_bench.py enroll
  relay_bench.py mint LABEL [TTL_S]
  relay_bench.py revoke FP_HASH
  relay_bench.py links
  relay_bench.py operator
  relay_bench.py local METHOD PATH [JSON]
  relay_bench.py disable | enable | forget
"""
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from bench_config import OUT, admin_url, need, relay_env, timer

HERE = Path(__file__).resolve().parent
TIMER = timer()
RELAY_URL = need("OST_RELAY_URL") if "enroll" in sys.argv[1:2] else ""
ADMIN = admin_url()
ENV = relay_env()
LINKS = OUT / "links.json"  # share URLs are bearer secrets: git-ignored output


def call(base, method, path, body=None, headers=None, timeout=15):
    data = json.dumps(body).encode() if body is not None else None
    h = dict(headers or {})
    if data:
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return e.code, {"raw": raw.decode(errors="replace")[:200]}


def admin(method, path):
    return call(ADMIN, method, path, headers={"Authorization": "Bearer " + ENV["OSTRELAY_ADMIN_TOKEN"]})


def relay_status():
    return call(TIMER, "GET", "/api/v1/relay")[1]


def log(obj):
    print(json.dumps(obj, sort_keys=True), flush=True)


def wait(predicate, seconds, label):
    end = time.time() + seconds
    s = {}
    while time.time() < end:
        s = relay_status()
        if predicate(s):
            return s
        time.sleep(1)
    raise SystemExit(f"{label} timed out: {json.dumps(s)}")


def job(path, fields):
    s = relay_status()
    jid = s["next_job_id"]
    code, body = call(TIMER, "POST", path, {"job_id": jid, "if_boot_id": s["boot_id"], **fields})
    log({"submit": path, "job_id": jid, "http": code, "code": body.get("code")})
    if code != 202:
        return code, body
    s = wait(lambda s: s["job"]["id"] == jid and s["job"]["state"] == "done", 60, f"job {jid}")
    return 200, s["job"]


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "status"
    if cmd == "status":
        log(relay_status())
    elif cmd == "enroll":
        code, body = admin("POST", "/admin/enroll-code")
        assert code == 200, body
        current = relay_status()
        code, body = call(TIMER, "POST", "/api/v1/relay/enroll", {
            "relay_url": RELAY_URL, "enrollment_code": body["enrollment_code"],
            "spki_pin": ENV["OSTRELAY_SPKI_PIN"], "partner_origin": ENV["OSTRELAY_PARTNER_ORIGIN"],
            "if_boot_id": current["boot_id"]})
        log({"enroll_http": code, "code": body.get("code"), "state": body.get("state")})
        if code != 202:
            raise SystemExit("Setup was not accepted")
        print("Review the two addresses in the timer's local UI and briefly press Reset to approve.", flush=True)
        setup_id = body["setup_request_id"]
        t0 = time.time()
        s = wait(lambda s: s["setup_request_id"] != setup_id or s["setup_state"] in {"failed", "expired", "cancelled"}
                 or (s["setup_state"] == "complete" and s["state"] == "connected" and s["sharing_available"]), 90, "enroll")
        log({"seconds": round(time.time() - t0, 1), **{k: s[k] for k in (
            "state", "enrolled", "relay_host", "timer_id", "last_error", "sharing_available",
            "free_heap_bytes", "largest_free_block_bytes", "task_stack_min_free_bytes")}})
    elif cmd == "mint":
        label = argv[2]
        ttl = int(argv[3]) if len(argv) > 3 else 3600
        code, j = job("/api/v1/relay/links", {"ttl_s": ttl, "label": label})
        log({"mint": label, "http": code, "job": j})
        if code == 200 and j.get("result") == "ok":
            fp = j["fp_hash"]
            end = time.time() + 30
            while True:
                c, share = call(TIMER, "GET", "/api/v1/relay/links/share?fp_hash=" + fp)
                if c == 200 or time.time() > end:
                    break
                time.sleep(1)
            log({"share_http": c, "state": share.get("state"), "code": share.get("code")})
            if c == 200:
                links = json.loads(LINKS.read_text()) if LINKS.exists() else {}
                links[label] = {"fp_hash": fp, "url": share["share_url"]}
                LINKS.write_text(json.dumps(links, indent=2))
                LINKS.chmod(0o600)
                print("Share URL saved to private bench output.")
    elif cmd == "revoke":
        code, j = job("/api/v1/relay/links/revoke", {"fp_hash": argv[2]})
        log({"revoke": argv[2], "http": code, "job": j})
    elif cmd == "links":
        log(call(TIMER, "GET", "/api/v1/relay/links")[1])
    elif cmd == "operator":
        _, timers = admin("GET", "/admin/timers")
        for t in timers:
            _, tokens = admin("GET", "/admin/tokens/" + t["timer_id"])
            t["tokens"] = tokens
        _, health = admin("GET", "/admin/health")
        log({"health": health, "timers": timers})
    elif cmd == "local":
        body = json.loads(argv[4]) if len(argv) > 4 else None
        log(call(TIMER, argv[2], argv[3], body))
    elif cmd in ("disable", "enable", "forget"):
        log(call(TIMER, "POST", "/api/v1/relay/" + cmd))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
