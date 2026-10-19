Import("env")

"""Experimental relay build switch (V2 only).

platformio.ini excludes the relay by default (build_src_filter) and this
script adds it back only when enabled, so a disabled build contains no relay
code, API routes, capability flag or UI section (the page hides it when
/api/v1/relay is absent). Set it once in [opensmarttimer] `relay`, or for one
build with OPENSMARTTIMER_RELAY=enabled|disabled.
"""

import os

ENABLED = {"1", "on", "true", "yes", "enabled"}
DISABLED = {"0", "off", "false", "no", "disabled"}

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
setting = os.environ.get("OPENSMARTTIMER_RELAY") or env.GetProjectOption("custom_relay", "disabled")
value = setting.strip().lower()
if value not in ENABLED | DISABLED:
    raise SystemExit(f"relay option must be enabled or disabled, not {setting!r}")
enabled = value in ENABLED

if enabled:
    env.Append(CPPDEFINES=[("SMART_TIMER_RELAY", 1)],
               CPPPATH=[os.path.join(project_dir, "include", "relay")])
    current = env.get("SRC_FILTER") or ["+<*>"]
    if isinstance(current, str):
        current = [current]
    # Later patterns win: re-include what platformio.ini excluded.
    env.Replace(SRC_FILTER=list(current) + ["+<relay/>", "+<WebRelay.cpp>"])

state = "enabled" if enabled else "disabled"
env["OPENSMARTTIMER_RELAY"] = state  # read by build_identity.py
os.makedirs(build_dir, exist_ok=True)
marker = os.path.join(build_dir, "relay-option.txt")
existing = open(marker, encoding="utf-8").read() if os.path.isfile(marker) else None
if existing != state + "\n":
    with open(marker, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(state + "\n")
print(f"OpenSmartTimer experimental relay: {state}")
