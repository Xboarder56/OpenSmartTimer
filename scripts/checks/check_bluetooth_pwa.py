#!/usr/bin/env python3
"""Static safety and completeness checks for the Web Bluetooth companion."""

from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
import json
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PWA = ROOT / "bluetooth-app"


class AppParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = []
        self.buttons_without_type = []
        self.assets = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if "id" in values:
            self.ids.append(values["id"])
        if tag == "button" and "type" not in values:
            self.buttons_without_type.append(values.get("id", "<unnamed>"))
        if tag in {"script", "link", "img"}:
            target = values.get("src") or values.get("href")
            if target:
                self.assets.append(target)


def require(errors, condition, message):
    if not condition:
        errors.append(message)


def main():
    html = (PWA / "index.html").read_text(encoding="utf-8")
    js = (PWA / "app.js").read_text(encoding="utf-8")
    transport = (PWA / "ble-transport.mjs").read_text(encoding="utf-8")
    css = (PWA / "app.css").read_text(encoding="utf-8")
    worker = (PWA / "service-worker.js").read_text(encoding="utf-8")
    readme = (PWA / "README.md").read_text(encoding="utf-8")
    manifest = json.loads((PWA / "manifest.webmanifest").read_text(encoding="utf-8"))
    parser = AppParser()
    parser.feed(html)
    errors = []

    duplicates = sorted(name for name, count in Counter(parser.ids).items() if count > 1)
    require(errors, not duplicates, f"duplicate PWA ids: {', '.join(duplicates)}")
    require(errors, not parser.buttons_without_type,
            f"PWA buttons missing type: {', '.join(parser.buttons_without_type)}")
    referenced = set(re.findall(r"\$\('([A-Za-z][A-Za-z0-9_-]*)'\)", js))
    missing = sorted(referenced - set(parser.ids))
    require(errors, not missing, f"PWA JavaScript references missing ids: {', '.join(missing)}")

    require(errors, ".innerHTML" not in js, "PWA writes untrusted data with innerHTML")
    require(errors, "body.textContent = String(message)" in js,
            "wire log does not render untrusted messages as text")
    require(errors, "indexedDB.open(KEY_DB_NAME" in js,
            "controller credentials are not stored transactionally in IndexedDB")
    require(errors, "localStorage.setItem(KEY_STORAGE" not in js,
            "controller credential is still written to localStorage")
    require(errors, "localStorage.removeItem(LEGACY_KEY_STORAGE)" in js,
            "legacy localStorage credential is not removed after migration")

    sequence = [
        "credentialWrite('pending', secret)",
        "writeGatt(app.chars.controlCommand, commit, 'pairing commit')",
        "if (!deviceCommitConfirmed)",
        "authenticateWithKey(secret, 'newly paired')",
        "promotePendingCredential()",
    ]
    pairing_source = js[js.index("async function finishPairing"):
                        js.index("function buildCommand")]
    positions = [pairing_source.find(fragment) for fragment in sequence]
    require(errors, all(position >= 0 for position in positions) and positions == sorted(positions),
            "pairing does not persist pending, confirm commit, authenticate, then promote in order")
    require(errors, "confirmed[1] & 0x02" in js and "app.authenticated = true" in js,
            "application authentication is not confirmed from a fresh challenge")
    require(errors, "async function writeGatt" in js and "try {\n    await characteristic.writeValueWithResponse" not in js,
            "GATT write failures may still be swallowed")

    for feature, snippet in {
        "serialized commands": "commandQueue.then(() => executeCommand",
        "result correlation": "resultMatches(app.pendingResult.operation, result)",
        "lost-notification cached result": "readGatt(app.chars.controlResult)",
        "shared GATT queue": "gattQueue.run(() => characteristic.writeValueWithResponse(frame))",
        "persistent unknown outcomes": "persistPendingOperation(operation)",
        "reconnection reconciliation": "await reconcilePendingOperation()",
        "visible rejection feedback": "Command rejected",
        "honest unknown feedback": "Command outcome unknown",
        "persistent request IDs": "localStorage.setItem(REQUEST_ID_KEY",
        "correct output flag": "lock_output_energized: Boolean(frame[2] & 0x04)",
        "authenticated system diagnostics": "f00df299-464f-4921-9285-7749593b1144",
        "strict 24-byte capabilities": "readProtocolFrame(capabilities, 24, 'capabilities')",
        "explicit system frame contract": "capabilities[21] !== 44",
        "44-byte system decoder": "readProtocolFrame(frame, 44, 'system information')",
        "current-boot activity replay": "renderRecentActivity()",
        "settings panel": "id=\"settingsPanel\"",
        "developer setting": "id=\"developerModeToggle\"",
        "timer duration editor": "id=\"fixedDuration\"",
        "random duration editor": "id=\"randomFields\"",
        "quick presets": "class=\"preset-row\"",
        "release control": "id=\"stopButton\"",
        "physical guide": "id=\"hardwareGuide\"",
        "recent activity": "id=\"activityList\"",
        "heap diagnostics": "id=\"freeHeapLabel\"",
        "network diagnostics": "id=\"networkLabel\"",
    }.items():
        source = html if "id=\"" in snippet or "class=\"" in snippet else js
        require(errors, snippet in source, f"PWA missing {feature}")

    for snippet in ["await write(frame, attempt)", "result.opcode !== operation.opcode",
                    "result.boot_id !== operation.bootId", "result.session_sequence !== operation.sessionSequence",
                    "throw new UnknownCommandOutcome", "this.onTimeout()"]:
        require(errors, snippet in transport, f"Bluetooth transport missing safety guard: {snippet}")
    require(errors, "'./ble-management.mjs'" in worker,
            "offline worker does not cache the Bluetooth settings module")
    require(errors, ".innerHTML" not in (PWA / "ble-management.mjs").read_text(encoding="utf-8"),
            "Bluetooth settings module writes HTML")
    require(errors, "'./ble-transport.mjs'" in worker,
            "offline worker does not cache the command transport module")

    require(errors, "Hold the timer Reset button for 3–5 seconds" in html,
            "first-run UI does not tell the user to open physical pairing before Connect")
    require(errors, "OpenSmartTimer</strong>" not in html and "Smart Timer</strong>" in html,
            "Bluetooth app header is not branded Smart Timer")
    require(errors, readme.index("Open pairing before connecting") < readme.index("select **Connect**"),
            "documented pairing order is connection-first")
    require(errors, "serviceWorker.register('./service-worker.js'" in js,
            "PWA does not register its offline worker")
    require(errors, "../web/styles.css" in worker and "../web/i18n.js" in worker,
            "offline worker does not cache the shared theme and translations")
    require(errors, manifest.get("display") == "standalone" and manifest.get("start_url") == ".",
            "manifest is not configured as a standalone app")
    require(errors, "html[data-theme=\"light\"]" in css,
            "PWA-specific components lack light-theme styling")
    require(errors, "../web/styles.css" in html and "class=\"timer-workspace" in html,
            "PWA does not reuse the timer-hosted interface components")
    require(errors, "Content-Security-Policy" in html,
            "PWA lacks a restrictive content security policy")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Bluetooth PWA checks passed: {len(parser.ids)} unique ids")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
