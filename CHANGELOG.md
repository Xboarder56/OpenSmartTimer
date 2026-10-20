# Changelog

User-facing changes to OpenSmartTimer firmware, its browser apps and the relay. Versions follow `firmware_version` in `platformio.ini`. An **Unreleased** label identifies changes that have not been published.

## [Unreleased]

### Firmware updates

- During an online update, every progress step now appears under **Online update**, and **Install update** for a file stays disabled until a file is chosen.

## [1.0.0-rc.2] - 2026-10-19

The first public release candidate of OpenSmartTimer: open replacement firmware for the MagBound Smart Timer V1 and V2, with a built-in web page, a Bluetooth app for the V2 and an optional self-hosted relay for sharing.

Read the [safety, warranty and liability notice](SAFETY.md) before installing. This firmware controls a physical lock that can restrain a person. It is experimental and provided as is, without warranty of any kind; you install and use it at your own risk. Power the timer from a battery only, never from a wall outlet, whenever it controls a lock.

### Timer and local interface

- Replacement firmware for the MagBound Smart Timer V1 (ESP8266) and V2 (ESP32-C3), with the timer's web page built in.
- Start, pause, resume and adjust a countdown, choose a random duration, and lock software controls until the timer ends. Two saved settings, **Allow pause** and **Allow locking controls**, switch either control off for every interface. If the saved choice is damaged, both turn off until it is set again; if the settings are lost entirely, both return to allowed, so check them after a reset.
- Timer expiry and the physical emergency input command release without any network.
- Wi-Fi and access-point setup, device naming, history and diagnostics, in English or Simplified Chinese, light or dark.
- A local REST and event-stream API, plus the original app's routes. It answers only requests addressed to the timer's IP address or a local name such as `.local`, which blocks DNS-rebinding attacks from websites; cross-site browser requests are refused where the browser identifies them. This is not authentication: anyone on your network can control the timer.

### Bluetooth (V2 Smart Timer only)

- Control from an installable browser app, after pairing approved with a press on the timer. Off by default.
- Works without Wi-Fi; saved network settings are kept.

### Firmware updates

- Manual **Check for updates** against signed GitHub releases, prereleases included, or install from a file. Every install needs an open timer and a short Reset press.
- Opening the timer page or Bluetooth app checks at most once a day while the timer is unlocked and shows a dismissible notice; it can be switched off. Nothing installs automatically.

### Remote sharing (V2 Smart Timer only, experimental)

- Off by default, with no independent security review. Use it only with a relay and a partner you trust, and never as a way to release.
- Runs through a relay you host yourself; the project provides no hosted relay.
- Owners create, extend and revoke share links. When a link holding a session expires, the timer commands release, even while paused or with controls locked. The emergency input ends every link and turns sharing off.

### Known limits

- Hardware checks for this release are partial: the maintainer's own bench report on one unit of each model ([docs/qualification](docs/qualification/README.md)).
- V1 has no Bluetooth or remote sharing and allows two live-update viewers at a time (V2 three). Web Bluetooth needs a supported browser.
- Remote sharing pauses while a Bluetooth app is connected and resumes a few seconds after it disconnects.
- Under heavy load (several live pages, Bluetooth and sharing at once), a timer can occasionally take a few seconds to answer one request, for example while a lost Wi-Fi packet is resent, and a timed release can be delayed by those seconds. The emergency input does not wait for this: it cuts the lock output through an interrupt on both models. On V2, a press during a Bluetooth flash write takes effect when the write ends.
- Automatic rollback before Wi-Fi connects is unverified.

### License

- Original code is licensed under the AGPL version 3, with a narrow [Espressif linking permission](LICENSE-ESPRESSIF-EXCEPTION). Each release includes its source materials and dependency notices.
