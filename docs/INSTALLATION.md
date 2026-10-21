# OpenSmartTimer installation and rollback

## Read before flashing

Read the [safety, warranty, and liability notice](../SAFETY.md) first. This unofficial firmware can control a human restraint; installation or use means accepting risks that software testing cannot eliminate. Installing can leave the timer permanently unusable, with no verified way back, and will likely end any support from its manufacturer. The firmware is provided as is, without warranty; you install it at your own risk.

> [!CAUTION]
> Automatic rollback before Wi-Fi starts remains unverified and is not a supported recovery procedure. V2 delays application confirmation until its local recovery services are healthy, but the unchanged vendor bootloader has not been proven to honor pending-image rollback. Do not deliberately test this on the only recoverable unit. If new firmware cannot start Wi-Fi, another web update may be impossible. External 3.3 V UART ROM communication has been verified on one V2, but UART reflashing and broken-image recovery have not; see [V2 UART service](HARDWARE.md#v2-uart-service). Native USB-C data remains unverified and the connector is treated as power-only.

Perform initial validation with the controlled lock/load disconnected and stable power connected. A wall supply is acceptable only for this bench work: once the lock is connected, power the timer from a battery only, never from a wall outlet ([safety notice](../SAFETY.md#battery-power-only)). Preserve private copies of the original matching vendor application and filesystem images before replacing them.

This guide applies only to **MagBound Smart Timer V1 (Legacy Timer)** and **MagBound Smart Timer V2**. Choose the filename containing the exact product generation and controller. Never cross-flash the V1 ESP8266 and V2 ESP32-C3 images.

The Smart Timer release is one **application firmware** image. Its web interface is embedded in that image. If the existing vendor updater shows separate Firmware and FileSystem fields, select this image under **Firmware only**.

## Choose and verify an artifact

Download the image for your model and its accompanying manifests from
[GitHub releases](https://github.com/xboarder56/OpenSmartTimer/releases), or
[build from source](BUILDING.md). Use `BUILD.json` to match the hardware model,
embedded `build_id`, size and CRC32. Verify `SHA256SUMS` in the directory
containing the downloaded files:

```sh
shasum -a 256 -c SHA256SUMS
```

On systems with GNU coreutils, `sha256sum -c SHA256SUMS` is equivalent. Local
build packages are staged under `dist/firmware/`. Checksums detect changed
bytes but do not authenticate the publisher; signed GitHub updates verify
publisher metadata as described in [GitHub updates](GITHUB_UPDATES.md).

## Update an existing Smart Timer installation

1. Confirm the timer is open. Network changes, restart, scans, and OTA are rejected while it is active.
2. If using the timer's AP, disconnect other phones or tablets before uploading.
3. Press and release the Reset button within two seconds. The blue LED double-flashes while OTA is armed for five minutes.
4. Open **Device settings → Firmware**. It shows **Ready to update** while OTA is armed. Under **Install from a file**, select the release `.bin`.
5. Confirm the filename and version, then start the upload. The browser checks size and CRC32 before sending it.
6. Keep power connected while the image uploads, validates, copies, and restarts.
7. Reconnect to the timer and confirm **Installed version** in the Firmware section.

An update preserves supported network, device-name and Bluetooth settings. The
retained vendor filesystem is also left untouched. For browser-assisted signed
updates, choose **Check for updates** in the same Firmware section and review the release-notes
dialog before selecting **Install**; see [GitHub updates](GITHUB_UPDATES.md).

## First replacement of V1 vendor firmware

Before changing the device, record its state and version:

```sh
curl http://192.168.4.1/isAlive
curl http://192.168.4.1/getVersion
curl http://192.168.4.1/getDeviceId
curl http://192.168.4.1/getTimer
curl http://192.168.4.1/getWifiMode
```

Stop if the timer is not open or the hardware/flash generation is unknown. Older vendor version 1.0.1 or earlier may require the vendor's matching intermediate updater before accepting a normal application. Follow the vendor procedure for the installed generation; do not guess on the only recoverable unit.

For the replacement upload:

1. Disconnect the controlled load and use stable power.
2. Join the vendor AP and disable client sleep/roaming for the update.
3. Upload the Smart Timer `.bin` to the vendor page's **Firmware** field only.
4. Do not interrupt power. Wait for the success response and restart.
5. Join the generated network similar to `SMARTTIMER-12AB34` and open `http://192.168.4.1`.
6. Complete Wi-Fi/AP setup, then perform the post-install checks below before reconnecting the load.

If no AP appears, wait several minutes before one controlled power cycle. If it still does not appear, web recovery is exhausted.

[V1 UART service](HARDWARE.md#v1-uart-service-observations) documents the
external 3.3 V adapter, verified programming-mode button sequence and flash
read on one unloaded V1. ROM access and return to the installed application
were verified; UART reflashing and broken-image recovery remain untested.

## First replacement of V2 vendor firmware

The V2 image targets ESP32-C3 and preserves the vendor partition layout. Confirm
the installed vendor generation and perform the unloaded post-install checks
for your actual board before connecting a load.

The V2 application filename is:

```text
OpenSmartTimer-<version>-MagBoundV2-ESP32C3.bin
```

Before any V2 upload:

1. Retain private copies of the matching V2 application and SPIFFS images.
2. Verify the vendor application is the analyzed 0.3.0 generation: 1,287,440 bytes, CRC32 `bf421ef6`, SHA-256 `17939fea969c2cc36bd0a146d98c901f841b5196a618c32c7632bd2e5bdd9e5a`.
3. Disconnect the lock/load and every accessory. Use stable power.
4. Upload only the V2 ESP32-C3 OpenSmartTimer image to the vendor **Firmware** field. Do not write the SPIFFS field.
5. If it boots, validate GPIO3/GPIO6 lock indication, GPIO5 blue indication, GPIO4 Reset behavior, and GPIO0 emergency behavior before reconnecting anything.
6. Keep GPIO1 accessory behavior disabled until its jack mapping and electrical levels are independently confirmed.

The V2 build preserves the original 4 MB partition boundaries, including app slots at `0x10000` and `0x150000` and SPIFFS at `0x290000`. Matching the partition map prevents an application OTA from overwriting the retained filesystem, but it does not guarantee recovery from a non-booting image. A new V2 image defers confirmation until fail-open GPIO, storage, network/AP, and web recovery have remained healthy for five seconds. This helps only when the unchanged vendor bootloader participates in ESP-IDF pending-image rollback; that behavior remains unverified.

[V2 UART service](HARDWARE.md#v2-uart-service) documents the external adapter
pinout, observed internal button functions and read-only ROM queries. A
partition-table read confirmed the documented layout on one V2; valid OTA
metadata does not prove automatic rollback. UART reflashing and broken-image
recovery remain untested. Keep the load disconnected during service: the
amber LED was dim and solid in ROM mode, with output voltage/current
unmeasured, and fully off after returning to normal firmware. External UART
success does not verify native USB-C data; treat USB-C as power-only.

## Post-install checks

With the controlled load still disconnected, confirm:

- Boot, software restart, and power loss leave both outputs released.
- A short timer energizes both outputs and natural expiration releases them.
- Pause, resume, adjustment, and Unlock work before controls are locked.
- The emergency input releases ordinary and control-locked sessions and blocks a new start while held.
- Wi-Fi Mode, AP Mode, failed-credential recovery, and the ten-second settings reset remain reachable.
- Blue and amber indicators match the [hardware guide](HARDWARE.md).
- Device Information reports the expected model/controller, firmware version, 4 MB flash, target-specific layout, and plausible free heap.
- REST remains usable with SSE clients connected.

Only reconnect the controlled load after the board's electrical behavior has been validated for that installation.

## Settings reset and network recovery

While the timer is open, hold the Reset button for ten seconds. Warning flashes accelerate after eight seconds; at ten seconds the firmware resets:

- saved router SSID and password;
- custom AP name and AP password;
- friendly device name.

The timer then restarts with its default generated open AP. This operation does not change installed firmware or erase the retained vendor filesystem. A short Reset-button press arms OTA instead, so continue holding until the reset completes.

## Unverified application escape hatch

Ordinary OTA accepts Smart Timer images and exact allowlisted vendor rollback images. If a legitimate future vendor application is rejected only because it is not recognized:

1. Arm OTA with one short Reset-button press.
2. Press and release the Reset button a second time during the same window.
3. Confirm the triple-blue pattern and the prominent unverified-firmware warning in the interface.

This permits one upload and bypasses only the product-marker/vendor-allowlist check. Target chip/header, OTA capacity, declared size, CRC32, open-timer, same-origin, and physical-presence checks still apply. It does not make an unknown image electrically safe or provide rollback if that image fails to boot.

## Return to original vendor firmware

Manual web rollback requires OpenSmartTimer to boot far enough to serve its
updater. It cannot recover a non-booting image, and it does not make mismatched
vendor application/filesystem generations safe.

The Smart Timer updater exposes one application field because it does not write a filesystem. To restore a matching vendor generation while Smart Timer firmware still boots:

1. Open the timer and arm OTA with a short Reset-button press.
2. Upload the privately retained matching vendor **main application** image.
3. After restart, use the vendor application's two-field updater.
4. If its original interface is already intact, no filesystem write is required.
5. Only if that interface needs repair, upload the matching filesystem image through the vendor **FileSystem** field.

Known rollback application metadata:

| Vendor generation | Main size | Main CRC32 | Matching filesystem size |
| --- | ---: | --- | ---: |
| 0.3.0 | 483,472 bytes | `4d66232b` | 2,072,576 bytes |
| 1.3.0 | 493,136 bytes | `c7225497` | 2,072,576 bytes |

For V2, the analyzed vendor 0.3.0 application allowlist is 1,287,440 bytes with CRC32 `bf421ef6`. Its retained V2 SPIFFS partition is reused after application rollback; restore a filesystem only through the matching vendor updater and only when necessary.

Vendor binaries are intentionally not included in this repository. Do not mix application and filesystem generations. If Smart Timer firmware does not boot far enough to provide Wi-Fi, this web rollback path is unavailable.
