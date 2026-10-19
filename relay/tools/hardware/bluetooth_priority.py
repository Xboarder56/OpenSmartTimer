"""Bench-only: Bluetooth priority with a live share link, plus heap minimum.

Restart V2 (clean minimum-heap counter), mint a link, sample relay state while
the full Bluetooth bench runs, then confirm the relay resumed, the same link is
still live and a partner page connects. Never prints share URLs or keys.
"""
import json
import atexit
import subprocess
import threading
import time
import urllib.error
import urllib.request

from bench_config import OUT, ble_python, timer

T = timer()
HERE = __file__.rsplit("/", 1)[0]
results = []


def get(path):
    return json.load(urllib.request.urlopen(T + path, timeout=5))


def check(name, ok, detail=None):
    results.append(ok)
    print(json.dumps({"PASS" if ok else "FAIL": name, **({"detail": detail} if detail is not None else {})}), flush=True)


def wait(fn, seconds, label):
    end = time.time() + seconds
    while time.time() < end:
        try:
            value = fn()
            if value:
                return value
        except Exception:
            pass
        time.sleep(2)
    raise SystemExit(label + " timed out")


original_configured = get('/api/v1/bluetooth')['configured_enabled']
restored = False
def restore_configuration():
    global restored
    if restored or original_configured:
        return
    result = get('/api/v1/bluetooth/disable')
    assert result['configured_enabled'] is False, 'Bluetooth preference restoration failed'
    restored = True
atexit.register(restore_configuration)

req = urllib.request.Request(T + "/api/v1/system/restart", data=b"{}", headers={"Content-Type": "application/json"}, method="POST")
print("restart", urllib.request.urlopen(req, timeout=5).status, flush=True)
time.sleep(8)
if not original_configured:
    wait(lambda: get('/api/v1/bluetooth/enable')['enabled'], 30, 'temporary Bluetooth enable')
wait(lambda: get("/api/v1/bluetooth").get("initialized"), 90, "bluetooth init")
wait(lambda: get("/api/v1/relay")["state"] == "connected", 90, "relay connected")
minted = subprocess.run(["python3", f"{HERE}/relay_bench.py", "mint", "scn-ble", "3600"], capture_output=True, text=True)
fp = json.loads(minted.stdout.splitlines()[1])["job"]["fp_hash"]
check("link minted before Bluetooth", bool(fp))

samples, stop = [], threading.Event()


def sampler():
    while not stop.is_set():
        try:
            r = get("/api/v1/relay")
            samples.append({"t": time.time(), "state": r["state"], "paused": r.get("paused_for_bluetooth"),
                            "live": r.get("live_links"), "running": r.get("running")})
        except Exception as e:
            samples.append({"t": time.time(), "error": str(e)[:60]})
        stop.wait(2)


thread = threading.Thread(target=sampler, daemon=True)
thread.start()
started = time.time()
ble = subprocess.run([ble_python(), f"{HERE}/bluetooth_bench.py"], capture_output=True, text=True, cwd=HERE)
ble_done = time.time()
open(OUT / "bluetooth-bench.log", "w").write(ble.stdout + ble.stderr)
passes = sum(1 for line in ble.stdout.splitlines() if line.startswith("PASS"))
check("Bluetooth bench passes with the relay enrolled and a live link", ble.returncode == 0 and passes >= 23,
      {"exit": ble.returncode, "passes": passes, "minutes": round((ble_done - started) / 60, 1)})

resumed = wait(lambda: (lambda r: r["state"] == "connected" and not r.get("paused_for_bluetooth") and r)(get("/api/v1/relay")), 60, "relay resume")
resume_s = round(time.time() - ble_done, 1)
stop.set()
thread.join()
paused = [s for s in samples if s.get("paused")]
check("relay paused while Bluetooth was active", len(paused) > 0,
      {"paused_samples": len(paused), "of": len(samples), "states_seen": sorted({s.get("state", "err") for s in samples})})
check("relay resumed after Bluetooth idle", True, {"seconds_after_bench": resume_s})
relay_now = get("/api/v1/relay")
check("no storage lease left after the Bluetooth pause", relay_now["maintenance"] is False, {"maintenance": relay_now["maintenance"]})
boot = get("/api/v1/status")["boot_id"]
start = urllib.request.Request(T + "/api/v1/timer/start", data=json.dumps({"duration_ms": 5000, "if_boot_id": boot, "operation_id": f"ble-relay-{int(time.time())}"}).encode(),
                               headers={"Content-Type": "application/json"}, method="POST")
try:
    started_state = json.load(urllib.request.urlopen(start, timeout=5)).get("state")
except urllib.error.HTTPError as e:
    started_state = json.load(e).get("code")
check("local start works after Bluetooth", started_state == "running", {"result": started_state})
wait(lambda: get("/api/v1/status")["state"] == "open", 20, "5 s session end")
links = get("/api/v1/relay/links")["links"]
check("same link still live after Bluetooth", [l["fp_hash"] for l in links] == [fp], {"live": len(links)})

url = json.load(open(OUT / "links.json"))["scn-ble"]["url"]
view = subprocess.run(["node", f"{HERE}/partner.cjs", url, "view", str(OUT / "shots/bluetooth-partner.png")], capture_output=True, text=True, timeout=90)
out = json.loads(view.stdout.strip().splitlines()[-1])
check("partner page connects after Bluetooth", out.get("after", {}).get("connection") == "online", {"connection": out.get("after", {}).get("connection")})

d = get("/api/v1/system/diagnostics")
check("minimum observed heap since boot", d["minimum_observed_free_heap_bytes"] >= 20000,
      {"min_observed": d["minimum_observed_free_heap_bytes"], "free_now": d["free_heap_bytes"], "largest_block": d["max_free_block_bytes"]})
subprocess.run(["python3", f"{HERE}/relay_bench.py", "revoke", fp], capture_output=True)
restore_configuration()
check('original Bluetooth preference restored', get('/api/v1/bluetooth')['configured_enabled'] == original_configured)
print(json.dumps({"summary": f"{sum(results)}/{len(results)} passed"}))
raise SystemExit(0 if all(results) else 1)
