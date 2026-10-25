# OpenSmartTimer documentation

Guides for installing, operating and integrating OpenSmartTimer on **MagBound Smart Timer V1 (Legacy Timer)** and **MagBound Smart Timer V2**. Read [Safety](../SAFETY.md) before installing or using the firmware. The [project overview](../README.md) describes model support and features.

## Install and use

| Guide | Purpose |
| --- | --- |
| [Installation and recovery](INSTALLATION.md) | Select an image, install firmware, update, reset settings and restore vendor firmware |
| [Hardware](HARDWARE.md) | GPIO assignments, indicators, buttons, accessory limitations and flash layout |
| [Hardware bench notes](HARDWARE_BENCH_NOTES.md) | Dated UART, jack and continuity measurements behind the hardware reference |
| [GitHub firmware updates](GITHUB_UPDATES.md) | Signed updates, release-notes dialogs and publisher configuration |
| [Bluetooth app](../bluetooth-app/README.md) | V2 browser requirements, pairing, controls and offline use |
| [Remote sharing](REMOTE_SHARING.md) | Connect a relay and create, extend, revoke or reactivate share links |
| [Changelog](../CHANGELOG.md) | User-facing changes by firmware version |

## Operate and integrate

| Guide | Purpose |
| --- | --- |
| [Security](../SECURITY.md) | Trust boundaries, hosting responsibilities and vulnerability reporting |
| [Relay operator guide](../relay/OPERATOR.md) | Self-hosting, enrollment policy, owner/admin accounts, TLS and backups |
| [GitHub Pages hosting](GITHUB_PAGES.md) | Deploy the Bluetooth app and signed firmware feeds |
| [Local API](API.md) | REST/SSE endpoints, examples, schemas and retry rules |
| [Bluetooth protocol](BLUETOOTH.md) | Pairing, authentication, commands and event replay |
| [Bluetooth management](BLUETOOTH_MANAGEMENT.md) | Settings, Wi-Fi provisioning, diagnostics and OTA over Bluetooth |
| [Relay protocol](RELAY_PROTOCOL.md) | Capabilities, encrypted channels, freshness and replay protection |

Users provide relay hosting or choose an operator they trust; the project operates no hosted relay. The trusted control page requires an independent HTTPS origin and hosting credentials isolated from the relay.

## Build and contribute

| Guide | Purpose |
| --- | --- |
| [Building](BUILDING.md) | Pinned dependencies, target builds, artifacts and source/relinking materials |
| [Architecture](ARCHITECTURE.md) | Timer authority, state machines, storage, transports and resource limits |
| [V2 framework profile](../framework/esp32c3/README.md) | Framework inputs, retained capabilities and package verification |
| [Relay implementation](../relay/README.md) and [firmware integration](RELAY_FIRMWARE.md) | Server and V2 transport structure |
| [Testing and verification](VALIDATION.md) | Automated coverage and physical verification limits |
| [Test setup](../tests/README.md) and [relay tests](../relay/tests/README.md) | Dependencies and commands |
| [Contributing](../CONTRIBUTING.md) | Change review, protocol consistency and licensing |

Test firmware with the controlled load disconnected. Automatic rollback before Wi-Fi starts is unverified; it is not a supported recovery method. Application images belong in the Firmware field, never a filesystem field.
