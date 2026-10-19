"""Bench-only: the relay server restarts while a link is live. The timer must
reconnect by itself and the same share URL must work again."""
import json, subprocess, time, urllib.request
from bench_config import OUT, need, timer
T = timer(); HERE = __file__.rsplit("/", 1)[0]; ok = []
CONTAINER = need("OST_RELAY_CONTAINER")
def get(p): return json.load(urllib.request.urlopen(T + p, timeout=5))
def check(name, cond, detail=None):
    ok.append(bool(cond)); print(json.dumps({"PASS" if cond else "FAIL": name, **({"detail": detail} if detail is not None else {})}), flush=True)
def partner(url):
    out = subprocess.run(["node", f"{HERE}/partner.cjs", url, "view", str(OUT / "shots/restart-partner.png")], capture_output=True, text=True, timeout=120)
    return json.loads(out.stdout.strip().splitlines()[-1]).get("after", {}).get("connection")
for l in get("/api/v1/relay/links")["links"]:
    subprocess.run(["python3", f"{HERE}/relay_bench.py", "revoke", l["fp_hash"]], capture_output=True)
m = subprocess.run(["python3", f"{HERE}/relay_bench.py", "mint", "scn-restart", "3600"], capture_output=True, text=True)
fp = json.loads(m.stdout.splitlines()[1])["job"]["fp_hash"]
url = json.load(open(OUT / "links.json"))["scn-restart"]["url"]
check("partner online before restart", partner(url) == "online")
before = get("/api/v1/relay")["reconnects"]
subprocess.run(["docker", "restart", CONTAINER], capture_output=True, check=True)
t0 = time.time(); r = {}
while time.time() - t0 < 120:
    r = get("/api/v1/relay")
    if r["state"] == "connected" and r["reconnects"] > before: break
    time.sleep(2)
check("timer reconnects after the relay restarts", r["state"] == "connected" and r["reconnects"] > before,
      {"seconds": round(time.time() - t0), "reconnects": r["reconnects"]})
check("same link still live on the timer", [l["fp_hash"] for l in get("/api/v1/relay/links")["links"]] == [fp])
check("same share URL works after the restart", partner(url) == "online")
subprocess.run(["python3", f"{HERE}/relay_bench.py", "revoke", fp], capture_output=True)
print(json.dumps({"summary": f"{sum(ok)}/{len(ok)} passed"}))
raise SystemExit(0 if all(ok) else 1)
