# OpenSmartTimer hardware reference

## Supported product generations

This firmware is specific to MagBound carrier boards. It is not generic ESP-12 or ESP32-C3 firmware.

| Product | Controller | Flash contract |
| --- | --- | --- |
| MagBound Smart Timer V1 (Legacy Timer) | ESP8266 ESP-12F | 4 MB, DOUT, 40 MHz, `4M/2M OTA` |
| MagBound Smart Timer V2 | ESP32-C3 | 4 MB, DIO, 80 MHz, two 1.25 MiB OTA slots |

Everything here was worked out by inspecting one unit of each model; other production runs may differ, and several circuits remain untraced. Electronic state and indicator LEDs cannot prove that an external lock mechanically released. Read the [safety, warranty, and liability notice](../SAFETY.md) before installation or use.

## GPIO maps

### V1 — ESP8266

| Function | GPIO | Direction | Active level | Firmware behavior |
| --- | ---: | --- | --- | --- |
| Lock driver A | 4 | Output | HIGH | Preloaded LOW; mirrored with GPIO13 |
| Lock driver B | 13 | Output | HIGH | Preloaded LOW; mirrored with GPIO4 |
| Blue status LED | 12 | Output | HIGH | Network and maintenance feedback only |
| Emergency input | 10 | Input | HIGH | Left 3.5 mm jack in the board photo, nearest the magnet (lock) jack; continuously commands release and blocks start/resume while asserted |
| Reset button | 5 | Input | HIGH | Short release arms OTA; second short release permits one unverified upload; ten-second hold resets settings |

GPIO10 normally shares the ESP8266 flash interface. DOUT mode is therefore a compatibility requirement, not a performance preference.

#### V1 board layout

![MagBound Smart Timer V1 — front, annotated](../images/board-v1-front-annotated.jpg)

Photographed with the enclosure lid open. Callout numbers match the image
above; `images/board-v1-front-annotated.svg` is its editable overlay.

| # | Silkscreen | What it is |
| ---: | --- | --- |
| 1 | *(module label `AI ESP-12F`)* | ESP8266 **ESP-12F** module (AI-Thinker variant). PCB antenna trace to the far left. 4 MB in-package flash. |
| 2 | `TX RX GND` | 3.3 V UART service pads at the top-left. Inaccessible with the enclosure closed. Do not connect 5 V. |
| 3 | `SW3` (topmost switch) | **The user-accessible Reset button** (V1 GPIO5). The same physical function as V2's `WIFI RST` switch — silkscreen differs across board revisions but the role is identical. Short/medium/ten-second gestures below. |
| 4 | `SW1` | Third tactile switch. Electrically unverified — possible boot/flash control. **Do not press during reset or update.** |
| 5 | `SW2` | Hardware reset for the ESP module (uptime and current-boot history restart). Not user-facing. |
| 6 | *(TO-252, `L74…`)* | Lock-driver MOSFET, gate driven by mirrored GPIO4 + GPIO13. |
| 7 | *(2-pin board connector, pigtail ending in a male barrel plug)* | **12 V lock output.** Carries the switched 12 V rail to the male barrel plug that feeds the magnets. It is an output: never connect a power supply to it. |
| 8 | *(2 × silver electrolytics, `220 50V`)* | 220 µF 50 V bulk capacitance for the coil-driver stage. |
| 9 | *(2 × black 3.5 mm jacks, bottom center)* | **Left jack = emergency input** (the jack nearest the magnet (lock) jack); right jack reserved. In the board photos this is the opposite side from the V2. See "Accessory jacks" below. |
| 10 | *(SOIC-8 IC + inductor `330`, right side)* | Buck converter producing the ESP-12F 3.3 V supply from the 12 V input. |

12 V DC power enters through the separate barrel socket at the right end of the
board (not numbered in the photo). The V1 therefore has two barrel connectors:
the socket is the power input and the male plug on the pigtail is the output to
the magnets. Whenever the lock is connected, feed the socket from a 12 V battery
pack, never from a mains adapter ([safety notice](../SAFETY.md#battery-power-only)).

The plastic tray around the V1 PCB is a 3D-printed enclosure that opens
at the top-right; the enclosure itself is not part of the electrical
contract.

### V2 — ESP32-C3

| Function | GPIO | Direction | Active level | Firmware behavior and validation |
| --- | ---: | --- | --- | --- |
| Lock driver | 3 | Output | HIGH | Preloaded LOW; commanded by the timer state machine |
| Amber lock indicator | 6 | Output | HIGH | Mirrors GPIO3; never used for maintenance patterns |
| Blue status LED | 5 | Output | HIGH | Network and maintenance status |
| Emergency input | 0 | Input | HIGH | Right 3.5 mm accessory jack (`Accessories 2`), nearest the magnet (lock) jack; continuously commands release while asserted |
| Reset/config button | 4 | Input | HIGH | Marked `WIFI RST` on the PCB; the interface calls it the **Reset button**. OTA, 3–5 second BLE pairing, and ten-second factory-reset gestures |
| Auxiliary input | 1 | Input | HIGH | Expected at the left 3.5 mm accessory jack (`Accessories 1`); not physically verified. Initialized but deliberately inactive |

These are product-specific assignments, not generic ESP32 development-board defaults. The V2 build uses ESP32-C3, Arduino-ESP32 3.3.0, ESP-IDF 5.5, 4 MB flash, 80 MHz, and DIO mode.

The right `Accessories 2` emergency input's contact-level carrier circuit has
not been independently traced. The left `Accessories 1` jack's GPIO1 assignment
remains provisional. Confirm each unit with the controlled load disconnected
before relying on its electrical behavior.

#### V2 board layout

![MagBound Smart Timer V2 — front, annotated](../images/board-v2-front-annotated.jpg)

Photographed board, front. Callout numbers match the image above; `images/board-v2-front-annotated.svg` is its editable overlay.

| # | Silkscreen | What it is |
| ---: | --- | --- |
| 1 | *(module label `ESPRESSIF ESP32-C3-WROOM-02`)* | ESP32-C3-WROOM-02 module. Chip ID 5, 4 MB in-package flash, 80 MHz, DIO mode. FCC/CE marking on the module lid. PCB antenna trace at the extreme left. |
| 2 | `BOOT` | Internal tactile switch. Marking suggests a programming control, but its wiring is not electrically verified here. **Do not press during reset or update.** |
| 3 | `FLASH` | Second internal tactile switch adjacent to `BOOT`; wiring and exact role are unverified. Firmware does not assign it an application action. **Do not use as a recovery procedure.** |
| 4 | `WIFI RST` | **The user-accessible Reset button** (V2 GPIO4). The silkscreen calls it `WIFI RST`; the interface, this document, and every user-facing message call it the **Reset button**. |
| 5 | *(TO-252, `L74…`)* | Lock-driver MOSFET. Gate driven from GPIO3 via a small SOT-23 device just above it. |
| 6 | *(2-pin JST-XH, marked `+`)* | **12 V DC accessory output.** Polarized 2-pin connector that carries the on-board switched 12 V rail to an external barrel-jack pigtail (which then drives the magnetic-lock coil). Verify polarity on an unpowered board before wiring anything else. |
| 7 | *(2 × silver electrolytics, `N3P 220 50V`)* | Capacitors marked 220 µF, 50 V near the output stage. Component voltage ratings alone do not establish the actual rail voltage or wiring. |
| 8 | *(2 × black 3.5 mm jacks, bottom center)* | Left jack = **Accessories 1**, right jack = **Accessories 2**, the **emergency input** (the jack nearest the magnet (lock) jack). In the board photos this is the opposite side from the V1. See "Accessory jacks" below. |
| 9 | *(SOIC-8 IC + inductors, right of the caps)* | Switching converter that produces the 12 V accessory rail from USB-C 5 V input. A separate on-board regulator (small SOT-23 near the ESP module) generates the ESP32-C3 3.3 V supply. |
| 10 | *(USB-C receptacle, far right)* | 5 V power input over USB-C. Whenever the lock is connected, use a USB power bank, never a wall charger ([safety notice](../SAFETY.md#battery-power-only)). Data-line routing to the ESP32-C3 has not been electrically confirmed; treat as **power-only** until proven otherwise. |

The back of the board (photo below) is largely **residual 3D-printed
filament** left over from the vendor's glued enclosure — the two large
hatched patches are plastic that the board could not be cleanly removed
from, not copper heat-spreading pour. Genuine PCB features on the back
are limited to the two 3.5 mm jacks' through-hole solder pads (bottom
center) and a small cluster of pads near the left edge; anything else in
the photo is filament residue rather than routing. No serviceable
silkscreen is visible.

![MagBound Smart Timer V2 — back (residual filament visible)](../images/board-v2-back.jpg)

#### V2 PCB switches

Paralleling the V1 PCB switch table, on the photographed V2 board:

| Silkscreen | User-facing role | Firmware behavior |
| --- | --- | --- |
| `WIFI RST` | The Reset button (GPIO4). Only switch a user should press. | See "Reset button gestures" section below — the same short/medium/ten-second gesture set applies to V2. |
| `BOOT` | Internal control; exact wiring unverified. | No application action assigned. Its name is not proof of a usable programming/recovery path. |
| `FLASH` | Internal control; exact wiring unverified. | No application action assigned. Do not assume it is wired alongside `BOOT`. |

Firmware does not use these switches as user controls. Their electrical
effects during operation or power-on have not been qualified; users should
not press them or rely on them for recovery.

#### V2 PCB connectors

| Position | Connector | Purpose | Verification status |
| --- | --- | --- | --- |
| Far right | USB-C receptacle | Board power in (5 V). Data-line routing to the ESP32-C3 is **not** electrically confirmed by this document. | Power ingress exercised on one unloaded unit; enumeration as a USB device not tested. |
| Top center | 2-pin JST-XH (polarized, `+` marked) | **12 V DC accessory output.** Carries the on-board switched 12 V rail to an external barrel-jack pigtail that drives the magnetic-lock coil (switched by the GPIO3-driven MOSFET). | Confirmed as the 12 V accessory output; polarity pinout not electrically re-verified in this document. |
| Bottom center (left jack) | 3.5 mm TRS/TRRS — `Accessories 1` | See "Accessory jacks" table. | Inferred GPIO1; not exercised in OpenSmartTimer. |
| Bottom center (right jack) | 3.5 mm TRS/TRRS — `Accessories 2` | See "Accessory jacks" table. | GPIO0 emergency function exercised on one unloaded unit. |

Do not apply external voltage to the 2-pin JST or either 3.5 mm jack
based on this document. Trace the carrier circuit on an unpowered
disconnected board before wiring anything but a compatible dry-contact
accessory or the intended magnetic-lock coil.

## Accessory jacks

On both models the emergency input is the 3.5 mm jack **nearest the magnet
(lock) jack**. In the board photos that is the left jack on V1 and the right jack on
V2. It is how each board is wired, not a firmware setting, so check which jack
your accessory is plugged into and test it before every use.

| | V1 | V2 |
| --- | --- | --- |
| Emergency input | **Left** jack in the board photo: the jack nearest the magnet (lock) jack. Dry contact on GPIO10; functionally tested | **Right** jack in the board photo (`Accessories 2`): the jack nearest the magnet (lock) jack. GPIO0; functionally tested on one unloaded unit; circuit tracing pending |
| Other jack | Right jack. Reserved; no verified GPIO assignment | Left jack (`Accessories 1`). Inferred GPIO1 auxiliary/start input; intentionally inactive in OpenSmartTimer |

Do not apply external voltage to either jack based on this document. Use only a compatible dry-contact accessory until the carrier circuit and plug contacts are electrically traced.

## Fail-open initialization

Both target builds call the timer controller before Serial, EEPROM, Wi-Fi, or HTTP initialization. Output latches are written LOW before their pins switch to output mode:

- V1: GPIO4 and GPIO13 LOW.
- V2: direct ESP-IDF latch writes preload GPIO3 and GPIO6 LOW before Arduino enables output mode; successful writes are required by the delayed OTA-confirmation gate.

The firmware does not restore an active timer after reset. A held emergency input is read during initialization, remains release-enforcing, and rejects a later start without requiring a new edge. V2 GPIO1 cannot start a timer in this build.

## Indicators

The amber indicator represents the commanded lock-driver state:

- Amber on: firmware is commanding the lock output energized.
- Amber off: firmware is commanding release.

The blue indicator reports network state unless a higher-priority maintenance pattern is active:

| Blue pattern | Meaning |
| --- | --- |
| Solid | Connected to router Wi-Fi, or intentional standalone AP Mode is available |
| 500 ms on / 500 ms off | Recovery AP is available because saved Wi-Fi cannot be reached |
| Fast 150 ms blink | Neither router Wi-Fi nor an AP is currently available |
| Rapid 125 ms blink for five seconds | Identify request |
| Solid during transfer and validation | Firmware upload is in progress |
| Rapid 80 ms on / 80 ms off | Firmware accepted; restart is pending |
| Two short flashes every two seconds | OTA armed |
| Three short flashes every two seconds | One unverified firmware upload permitted |
| One slow flash every 1.6 seconds | V2 Bluetooth pairing is waiting for an app |
| Pairing confirmation pattern | V2 app is connected and waiting for a short Reset press |
| Warning blink accelerating after eight seconds | Reset button is held toward the ten-second settings reset |

Maintenance patterns are accepted only while the timer is open. Amber is never used for maintenance feedback because it follows the lock command.

## Reset button gestures

The interface calls the user-accessible control the **Reset button**,
regardless of what the PCB silk-screen calls it. V1's topmost switch is
labeled `SW3`; V2's is labeled `WIFI RST`. They are the same button in
different silk-screen conventions.

- Release within two seconds while open: arm OTA for five minutes.
- Release a second time during that window: permit one structurally compatible but otherwise unrecognized application image.
- On V2, release after 3–5 seconds while open: enable Bluetooth and open a two-minute app-pairing window.
- Hold ten seconds while open: reset router Wi-Fi, AP configuration, device name, and supported Bluetooth settings, then restart with a generated open AP.

The ten-second operation does not change firmware or erase the retained filesystem.

### V1 PCB switches

On the inspected V1 PCB:

| PCB marking | Current status |
| --- | --- |
| `SW3` | User-accessible Reset button used by firmware |
| `SW2` | Functionally confirmed hardware reset: uptime and current-boot history restart |
| `SW1` | Electrically unverified; possible boot/flash control, not proven |
| `RX GND TX` | Visible 3.3 V UART service pads, inaccessible in the assembled enclosure |

Do not hold V1 `SW1` while resetting or updating. If it reaches GPIO0, asserting it during reset could enter the ESP8266 ROM loader. Continuity mapping must be performed only on an unpowered, disconnected board.

### V2 PCB switches

See "V2 PCB switches" under "V2 board layout" above. In short: `WIFI RST`
is the Reset button; the internal `BOOT` and `FLASH` controls are unverified
and should not be pressed during normal use.

## Flash layouts and rollback

### V1

`eagle.flash.4m2m.ld` provides the application OTA area used by this project. The retained vendor filesystem occupies physical `0x200000–0x3F9FFF`; EEPROM emulation uses the Arduino core's final settings sector. Routine OpenSmartTimer OTA does not intentionally write either area. Web OTA back to a matching vendor V1 application has been completed successfully on physical hardware.

### V2

The generated partition table preserves the original V2 layout:

| Partition | Offset | Size |
| --- | ---: | ---: |
| NVS | `0x009000` | 20 KiB |
| OTA data | `0x00E000` | 8 KiB |
| App 0 | `0x010000` | 1,280 KiB |
| App 1 | `0x150000` | 1,280 KiB |
| SPIFFS | `0x290000` | 1,408 KiB |
| Core dump | `0x3F0000` | 64 KiB |

The V2 OTA application image begins at its application slot; it is not a merged whole-flash image. Two slots allow OTA replacement but do not by themselves guarantee automatic rollback after a bad boot. The original filesystem is not included or redistributed. OpenSmartTimer configuration uses the dedicated NVS namespace `opensmarttimer`; the V1 build uses its existing fixed EEPROM sector because ESP8266 EEPROM emulation has no named namespace.

Application-only web OTA from the tested OpenSmartTimer V2 baseline back to the exact vendor 0.3.0 application completed successfully, and the retained vendor filesystem remained usable. Automatic rollback before the OpenSmartTimer web updater becomes reachable remains unverified and should not be treated as a recovery procedure.

The USB-C connector proves neither USB data routing nor ROM-loader access. Treat it as power-only until enumeration or electrical tracing confirms otherwise.

## Bluetooth

The V2 ESP32-C3 firmware includes a
NimBLE peripheral with encrypted bonding, persistent application
authentication, integrity-protected control, status/event notifications,
Identify, and physical no-Wi-Fi pairing. The radio preference, three approved
app credentials, and bonds survive restart until explicitly revoked or
factory-reset. The application permits one inbound connection, and the
framework is built for that single link with four bonds, the extra bond
reserved for pairing replacement. Central, scanner and GATT-client support is
not compiled in; timer discovery and mirroring are not implemented.
Authenticated management includes settings and physically armed OTA.
The ESP8266 V1 has no BLE support. See [Bluetooth Low Energy design](BLUETOOTH.md)
for UUIDs, frames, authentication, validation, security limitations, and the
unimplemented two-timer control boundary.

See [Installation](INSTALLATION.md) before replacing or restoring firmware.
