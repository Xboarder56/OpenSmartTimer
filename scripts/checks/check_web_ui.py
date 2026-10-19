#!/usr/bin/env python3
"""Static checks for the embedded, dependency-free web interface."""

from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
import gzip
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web"


class UiParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ids = []
        self.external_assets = []
        self.buttons_without_type = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if "id" in values:
            self.ids.append(values["id"])
        if tag in {"script", "link", "img"}:
            target = values.get("src") or values.get("href")
            if target and not target.startswith(("/", "#")):
                self.external_assets.append(target)
        if tag == "button" and "type" not in values:
            self.buttons_without_type.append(values.get("id", "<unnamed>"))

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)


def main():
    html = (WEB / "index.html").read_text(encoding="utf-8")
    js = (WEB / "app.js").read_text(encoding="utf-8")
    i18n = (WEB / "i18n.js").read_text(encoding="utf-8")
    appearance = (WEB / "appearance.js").read_text(encoding="utf-8")
    css = (WEB / "styles.css").read_text(encoding="utf-8")
    parser = UiParser()
    parser.feed(html)

    errors = []
    duplicate_ids = sorted(key for key, count in Counter(parser.ids).items() if count > 1)
    if duplicate_ids:
        errors.append(f"duplicate HTML ids: {', '.join(duplicate_ids)}")

    referenced_ids = set(re.findall(r"\$\('([A-Za-z][A-Za-z0-9_-]*)'\)", js))
    missing_ids = sorted(referenced_ids - set(parser.ids))
    if missing_ids:
        errors.append(f"JavaScript references missing ids: {', '.join(missing_ids)}")

    if parser.external_assets:
        errors.append(f"non-local UI assets: {', '.join(parser.external_assets)}")
    if parser.buttons_without_type:
        errors.append(f"buttons missing explicit type: {', '.join(parser.buttons_without_type)}")

    # The build replaces this development-only CSP marker with the one
    # configured Pages path and public release API, never remote assets.
    inspected_html = html.replace("connect-src 'self' https://*.github.io https://api.github.com;", "connect-src 'self';")
    external_urls = sorted(set(re.findall(r"https?://[^\s\"')]+", inspected_html + js + i18n + css)))
    if external_urls:
        errors.append(f"external URLs present: {', '.join(external_urls)}")

    required_files = {"/styles.css", "/i18n.js", "/appearance.js", "/updates.js", "/app.js"}
    linked_files = set(re.findall(r"(?:href|src)=\"([^\"]+)\"", html))
    missing_links = sorted(required_files - linked_files)
    if missing_links:
        errors.append(f"required embedded assets not linked: {', '.join(missing_links)}")

    # When sealed, both adjustment rails are hidden. Keep the timer explicitly
    # anchored in the middle track instead of allowing grid auto-placement to
    # move and resize it into the first track.
    if not re.search(r"\.timer-center\s*\{[^}]*grid-column:\s*2[;}]", css):
        errors.append("timer center is not anchored to the desktop middle grid column")

    for feature, snippet in {
        "jittered status polling": "Math.random() * 451",
        "asynchronous scan start": "mutate('/api/v1/wifi/scan')",
        "asynchronous scan status": "request('/api/v1/wifi/scan')",
        "OTA quiet-window preparation": "mutate('/api/v1/ota/prepare'",
        "explicit Recovery AP state": "station_retry_paused",
        "emergency release notification": "Emergency release activated",
        "immediate unlock action": "Unlock now",
        "duration drag adjustment": "addEventListener('pointermove'",
        "duration wheel adjustment": "addEventListener('wheel'",
        "random duration scrubbing": "['randomMin', 'randomMax'].forEach",
        "two-choice network mode selector": "['apMode', 'wifiMode'].forEach",
        "automatic SSE connection": "new EventSource('/api/v1/events/stream')",
        "safe SSE failure fallback": "REST polling resumed; reconnecting automatically",
        "browser-local theme switch": "sessionStorage.setItem('smartTimerTheme'",
        "inactive AP status hiding": "classList.toggle('hidden', !next.ap_running)",
        "safe timer mutation identity": "if_session_id = status.session_id",
        "boot-scoped timer mutation identity": "if_boot_id = status.boot_id",
        "V2-only Bluetooth diagnostics": "next.chip_family === 'ESP32-C3'",
        "persistent Bluetooth radio toggle": "'/api/v1/bluetooth/disable'",
        "authenticated Bluetooth pairing": "mutate('/api/v1/bluetooth/pairing/start', fields)",
        "cancelable Bluetooth pairing": "mutate('/api/v1/bluetooth/pairing/cancel')",
        "Bluetooth controller revocation": "mutate('/api/v1/bluetooth/pairing/revoke')",
        "Bluetooth connection count": "bluetoothConnections",
        "main-page hardware guide": "openHardwareGuide",
    }.items():
        if snippet not in js:
            errors.append(f"missing {feature}")

    if "touch-action: none" not in css:
        errors.append("duration scrubbers do not reserve touch gestures")
    if 'class="language-switch"' not in html:
        errors.append("missing segmented language switch")
    if "user-select: none" not in css or "-webkit-tap-highlight-color: transparent" not in css:
        errors.append("segmented controls retain browser selection/tap highlighting")
    if ".adjust-button.minus:hover" in css or ".adjust-button.plus:hover" in css:
        errors.append("touch adjustment buttons retain colored sticky hover states")
    if not re.search(r"\.language-switch\s*\{[^}]*overflow:\s*hidden", css) or not re.search(r"\.theme-switch\s*\{[^}]*overflow:\s*hidden", css):
        errors.append("segmented selector bubbles are not clipped to their controls")
    if not re.search(r"\.theme-switch::before\s*\{[^}]*top:\s*2px;[^}]*bottom:\s*2px;[^}]*left:\s*2px;[^}]*width:\s*calc\(50% - 2px\)", css):
        errors.append("theme selector bubble does not use the enlarged geometry")
    if (not re.search(r'\.language-switch\[data-language="zh-Hans"\]::before\s*\{[^}]*left:\s*50%', css) or
            not re.search(r'\.theme-switch\[data-theme="dark"\]::before\s*\{[^}]*left:\s*50%', css) or
            "translateX(100%)" in css):
        errors.append("segmented selector bubbles still depend on transform-based sliding")
    if (not re.search(r"\.language-switch\s*\{[^}]*contain:\s*paint", css) or
            not re.search(r"\.theme-switch\s*\{[^}]*contain:\s*paint", css)):
        errors.append("segmented selector bubbles are not paint-contained for mobile Safari")
    if not re.search(r"\*,\s*\*::before,\s*\*::after\s*\{[^}]*box-sizing:\s*border-box", css):
        errors.append("pseudo-elements do not include borders in their declared geometry")
    if (not re.search(r"\.language-switch\s*\{[^}]*height:\s*44px;[^}]*min-height:\s*44px", css) or
            not re.search(r"\.theme-switch\s*\{[^}]*height:\s*44px;[^}]*min-height:\s*44px", css)):
        errors.append("header selectors must retain 44px touch targets")
    if "grid-template-columns: repeat(4, minmax(0, 1fr)) minmax(58px, 1.15fr)" not in css or ".preset-row > .reset-preset { grid-column:" in css:
        errors.append("quick presets are not centered or Reset lacks protected width")
    if not re.search(r"\.preset-button\s*\{[^}]*height:\s*44px;[^}]*align-items:\s*center", css):
        errors.append("preset values are not vertically centered")
    if (".preset-button.active, .preset-button:hover" in css or
            not re.search(r"@media \(hover: hover\)\s*\{\s*button:hover:not\(:disabled\)", css) or
            "if (event.detail !== 0) event.currentTarget.blur();" not in js):
        errors.append("Reset can retain a touch-hover/focus highlight")
    if "margin: 9px 0 -11px" in css or html.index('id="durationGestureHint"') < html.index('id="randomFields"'):
        errors.append("duration gesture hint can overlap the random range fields")
    if 'id="durationLimitBanner"' not in html or "maximum_duration_reached" not in js:
        errors.append("maximum-duration warning banner is missing")
    if 'id="deviceMac"' not in html or "next.station_mac" not in js:
        errors.append("station MAC is missing from device information")
    if 'id="connectionShortcut"' not in html or "$('networkSection').scrollIntoView" not in js:
        errors.append("connection status does not open Network setup")
    if "On-device control" in html:
        errors.append("redundant on-device status label remains visible")
    if '<span class="range-arrow" aria-hidden="true">—</span>' not in html:
        errors.append("random duration range separator is still visually ambiguous")
    if 'id="bluetoothPairButton"' not in html or 'id="bluetoothRevokeButton"' not in html:
        errors.append("Bluetooth paired-controller management is missing")
    if ('id="hardwareGuideOpen"' not in html or 'id="hardwareGuide"' not in html or
            'id="guideBluetoothPairing"' not in html or
            'id="guideShortPressDescription"' not in html or
            'id="guideFactoryResetDescription"' not in html or
            "setText('guideShortPressDescription', t(bluetoothSupported" not in js or
            "setText('guideFactoryResetDescription', t(bluetoothSupported" not in js):
        errors.append("button and LED guide is missing or not hardware-aware")
    if ('id="bluetoothKeyPanel"' in html or 'id="bluetoothControlKey"' in html or
            "bluetoothAccessKey" in js or "access_key" in js):
        errors.append("web UI still exposes the Bluetooth application key")
    if (".settings-section .field-label" not in css or
            ".settings-section label:not(.mode-switch)" in css or
            not re.search(r"\.settings-section \.mode-switch\s*\{[^}]*display:\s*flex;[^}]*margin-top:\s*0", css) or
            not re.search(r"\.settings-section \.mode-switch input\s*\{[^}]*width:\s*1px;[^}]*min-height:\s*1px;[^}]*margin:\s*0", css)):
        errors.append("settings label rules can override the Bluetooth switch geometry")
    if (not re.search(r"\.bluetooth-actions\s*\{[^}]*display:\s*flex;[^}]*flex-wrap:\s*wrap", css) or
            not re.search(r"\.bluetooth-actions button\s*\{[^}]*width:\s*auto;[^}]*flex:\s*0 0 auto", css)):
        errors.append("Bluetooth pairing action can stretch into an oversized button")
    if 'id="bluetoothBadge"' in html or not re.search(r'id="bluetoothRevokeButton"[^>]*class="[^"]*hidden', html):
        errors.append("Bluetooth UI retains redundant status or exposes revocation before pairing")
    if ('id="bluetoothPeerScanButton"' in html or
            "/api/v1/bluetooth/peers" in js):
        errors.append("unfinished timer-peer discovery remains in the RC interface")
    if any(not re.search(r"\." + name + r"\s*\{[^}]*width:\s*84px;[^}]*min-width:\s*84px", css) for name in ("language-switch", "theme-switch")):
        errors.append("phone segmented selectors do not use the widened geometry")
    if 'html[data-theme="light"]' not in css:
        errors.append("missing light theme palette")
    if not re.search(r'id="wifiMode"[^>]+type="radio"', html) or not re.search(r'id="apMode"[^>]+type="radio"', html):
        errors.append("network mode selector is not a two-radio control")
    if ".mode-option input:checked + span" not in css:
        errors.append("network mode selector lacks a selected pill state")

    table = i18n[i18n.index("simplifiedChinese = Object.freeze({"):i18n.index("\n  });")]
    keys = [single or double for single, double in re.findall(
        r"""(?m)^\s*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)")\s*:""", table)]
    repeated = sorted({key for key in keys if keys.count(key) > 1})
    if repeated:
        errors.append(f"translation keys defined more than once (the later one silently wins): {', '.join(repeated)}")

    for feature, snippet in {
        "Simplified Chinese translations": "simplifiedChinese",
        "per-tab language preference": "sessionStorage.setItem('smartTimerLanguage'",
        "Chinese document language": "zh-Hans",
    }.items():
        if snippet not in i18n:
            errors.append(f"missing {feature}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    raw_bytes = sum(len(value.encode("utf-8")) for value in (html, css, i18n, appearance, js))
    gzip_bytes = sum(len(gzip.compress(value.encode("utf-8"), compresslevel=9, mtime=0)) for value in (html, css, i18n, appearance, js))
    print(f"Web UI checks passed: {len(parser.ids)} unique ids, {raw_bytes} raw bytes, {gzip_bytes} gzip bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
