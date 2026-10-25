# Hardware bench notes

Dated observations behind the conclusions in [Hardware](HARDWARE.md). Each was
made on **one unloaded unit** of each model, with the lock disconnected; other
production runs may differ. These are records, not procedures: follow
[Hardware](HARDWARE.md) and [Installation](INSTALLATION.md) for anything you do
on a timer, and never probe a powered socket with metal.

## V1 UART: flash reads and ROM-mode indicators

The first 1 MiB of flash was subsequently read at 115200 baud with esptool's
temporary **RAM stub** (ESP8266 ROM alone cannot perform this read):

```sh
esptool.py --chip esp8266 --port "<PORT>" --baud 115200 \
  --before no_reset --after no_reset --connect-attempts 3 \
  read_flash --no-progress 0x0 0x100000 tmp/v1-program-region.bin
```

The eboot and application headers at `0x0` and `0x1000` specified DOUT,
4 MB and 40 MHz. The Arduino whole-image CRC validated, and the application
contained the V1 product marker. The first 4 KiB exactly matched the
[release verifier's](../scripts/release/verify_release.py) eboot baseline,
SHA-256 `2b84c131751a030115773d73137caa22bcd0f867e89e826a9567788ce022793a`.
No flash was written; filesystem and settings regions were not read.
A previous 460800-baud attempt failed with corrupt serial data, so only
115200 is validated here. A subsequent `read_flash_status` query did not
connect after stub exit. Re-entering ROM mode with the physical button
sequence allowed `read_flash_status` to succeed with `--no-stub`, using
the same options as the ID query in [V1 UART service](HARDWARE.md#v1-uart-service). It returned raw status `0x0000`;
no status bits or flash contents were written. This records the observed
value without assuming a device-specific status-register interpretation.

After releasing `SW1` and tapping `SW2`, a receive-only 115200-baud capture
showed `OpenSmartTimer firmware starting` and the owner confirmed the amber
indicator was fully off. This verifies return to the installed application,
not UART reflashing or broken-image recovery.

In ROM programming mode, the owner reported **both the amber lock indicator
and blue carrier-board indicator dimly lit**. A third LED near the ESP-12F module's antenna briefly
flashed during the ID queries, then went off; during the longer flash read,
the module's blue LED was mostly lit with blinking that appeared to track
the UART adapter's activity LED. Its circuit has not been traced.
Espressif documents [UART TX also appearing on GPIO2 in ESP8266 ROM download
mode](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html);
if that LED is connected there, UART activity could explain the flash.
This is a possible explanation, not a verified LED assignment. The firmware's
carrier status LED remains GPIO12. Output voltage/current were not measured;
ROM-mode LED appearance does not establish whether the lock output is
energized or released, and application fail-open initialization is not
running in that mode.

## V2 UART: read-only metadata queries

While in ROM mode with the monitor closed, before the return-to-application
step of [V2 UART service](HARDWARE.md#v2-uart-service), the same esptool
4.5.1 command prefix can read service metadata. Replace the final `chip_id`
with one of the suffixes below. Create the ignored `tmp/` directory first
and keep raw reads private; do not commit vendor images or captures.

| Command suffix (esptool 4.x syntax) | Observation on the tested unit |
| --- | --- |
| `flash_id` | JEDEC manufacturer `0x5e`, device `0x4016`; detected flash size 4 MB |
| `get_security_info` | Raw flags `0x00000000`, `Flash_Crypt_Cnt` `0x0`, ROM `Chip_ID` 5 and API version 3 |
| `read_flash_status` | Raw status `0x0200` |
| `read_flash --no-progress 0x8000 0x1000 tmp/v2-partition-table-sector.bin` | 4 KiB partition-table sector; validated MD5 footer and [matching layout](HARDWARE.md#v2) |
| `read_flash --no-progress 0xe000 0x2000 tmp/v2-otadata.bin` | 8 KiB OTA metadata; both record CRCs valid, both states `VALID` |
| `read_flash --no-progress 0x0 0x8000 tmp/v2-bootloader-region.bin` | 32 KiB before the partition table; bootloader checksum and embedded SHA-256 validated |
| `read_flash --no-progress 0x10000 0x200 tmp/v2-app0-header.bin` | App 0 header and descriptor |
| `read_flash --no-progress 0x150000 0x200 tmp/v2-app1-header.bin` | App 1 header and descriptor |

ROM `Chip_ID` 5 identifies the ESP32-C3 chip type; it is not a unique device
ID. The raw security/status values above are observations on this unit,
not a security assessment or a statement about signed application OTA.
No keys, NVS contents or filesystem contents were read.

All three image headers reported DIO, 80 MHz, 4 MB, chip type 5 and
`hash_appended=1`. Both app descriptors contained version `afa5cdd-dirty`,
project `arduino-lib-builder` and IDF string `b66b5448`, with blank compile
date/time. These descriptor strings do not identify the installed product
firmware version; use the device UI/API for its version and build identity.
Valid OTA records are a snapshot of selection metadata, not proof of the
runtime slot or automatic rollback. Do not write OTA metadata to test recovery.

These checks verify external UART ROM access and read-only inspection, not
firmware restoration or recovery from a non-booting image. No stub was
uploaded or flash, eFuses, settings or firmware written. Native USB-C data
routing remains unverified; treat that connector as power-only.

## V1 auxiliary jack

### Passive measurements

Module-pad left/right positions below refer to the **metal-shield side,
antenna pointing up**. The pin picture in the linked ESP-12F PDF reverses
those sides; do not transfer its left/right positions to this board view.

During an owner-operated check with every power source disconnected, the
spare jack nearest the Wi-Fi antenna had a reported **0 kΩ** reading and
continuity beep from its sleeve to the ESP-12F's bottommost left pad, with
the antenna pointing up. That pad is **VCC/3.3 V** in the
[module pinout](https://aithinker-static.oss-cn-shenzhen.aliyuncs.com/docs/Public%20Document%20Center/WiFi/esp8266/Specification/ESP-12F%20Specification-20220807.pdf).
This supports a supply-contact role for the V1 sleeve; exact low-ohm
resistance and a complete physical trace remain unverified. A later ROM-mode
test below identifies a GPIO9 input response. An earlier tip
reading of **10.02 MΩ** to the described second right-side pad does not
identify the signal: in this orientation that pad is RX/GPIO3, unlike V2's
second right-side GPIO1 pad. Neither observation qualifies a direct cable
between independently powered timers.

A subsequent unpowered **sleeve → UART-header GND** reading was
**188.5 Ω**. This is not a near-zero ground bond or evidence of a discrete
protective series resistor. It does not measure the sleeve's powered
voltage or establish isolation between two timers' supplies.

With the meter changed to DC voltage, V1 powered normally, the lock
disconnected and the cable's far end otherwise free, the owner then
reported **+3.3 V from sleeve to UART-header GND** (red on sleeve, black
on GND). This confirms the sleeve's powered potential relative to local
ground, consistent with the earlier sleeve-to-VCC reading and V2's
approximately +3.27 V sleeve measurement. It does not establish source
current capability, reverse-current protection or a safe peer cable.

With one probe on the sleeve, the owner also reported the amber indicator
lit when the other probe touched the second left-side pad from the bottom,
and blue at the third. These positions are GPIO13 (one mirrored lock-driver
output in firmware) and GPIO12 (blue status), respectively, with the antenna
up. Meter-current illumination is a possible explanation; it does not
identify the tip signal or establish lock-output energization.

Further unpowered tip-to-pad checks gave **10.8 kΩ to GPIO15** (second
from bottom on the right) and **131 kΩ to GPIO13** (second from bottom on
the left). Neither establishes a near-zero auxiliary-tip connection to
that pad; an in-circuit resistance reading does not by itself identify
the signal route.

With the cable seated in V1's spare jack and all power disconnected, the
owner later reported **5.8 kΩ between tip and sleeve** at the cable's free
end. This is an in-circuit resistance reading, not a discrete resistor
value or evidence of a near-zero tip-to-sleeve short. Reports of both
contacts beeping to one end of R9 do not by themselves identify a common
net. With R9 end A defined as the end nearest SW1/R10 and end B as its
opposite end, the owner subsequently reported **tip → A = 0.00 kΩ** and
**tip → B = 0.97–0.98 kΩ**. This supports end A as an accessible point on
the auxiliary-tip net, with an approximately 1 kΩ path toward end B,
consistent with R9's visible `1001` marking. Exact low-ohm resistance, end
B's rail connection and the hidden ESP pad were initially unverified.
The owner then reported **B → UART-header GND = 0 kΩ**, supporting R9 as
an approximately 1 kΩ pull-down from the auxiliary-tip net to ground.
The hidden ESP pad was not accessible for a continuity measurement. The earlier sleeve
beep to end A must not be treated as a near-zero sleeve-to-tip bond; the
numeric tip-to-sleeve reading was 5.8 kΩ.

A static inspection of the owner's manufacturer `legacy_0.3.0` image
(SHA-256 `65e6548a30d26ce6eddec80f4fe4b7717e347ca04e24df54259a2bc6a0926e09`)
recovered application initialization of GPIO12, GPIO13 and GPIO4 as outputs,
and GPIO5 and GPIO10 as inputs. Its main input handler reads GPIO5 and
GPIO10; the asserted GPIO10 path includes driving GPIO13 and GPIO4 LOW.
This corroborates the V1 firmware assignments. No separate spare-jack pin
was identified in those paths; this image does not establish its physical
route or prove the spare is electrically unusable. The vendor image is not
included in this repository, and no manufacturer firmware was flashed.

A deeper review of the same image also checked GPIO function references,
direct input-register reads, ROM GPIO calls, ADC entry points and UART
initialization. The additional input reads identified were the updater's
boot-mode check and SDK sleep/wakeup logic; neither identified an auxiliary
handler. GPIO9's mux address appears in a generic table covering all GPIOs,
which is not evidence of an auxiliary assignment. The reviewed application
still yielded only GPIO5/GPIO10 input handling. GPIO9 was a board-photo
hypothesis at this stage, not a manufacturer-firmware finding. This static analysis does
not resolve every computed address or indirect call and cannot prove the
spare jack is unwired or unusable by different firmware.

On 21 October 2026, a temporary web-installed V1 observer (build
`9ee056d9083a4202ca3f7d1d`) clamped lock writes inactive and rejected timer
starts. It sampled input registers without changing candidate pin modes
or muxes. During 188 successful diagnostic samples, GPIO10 went HIGH and
returned LOW, with its sampled change counter rising from 0 to 10; GPIO9
remained LOW with no counted changes. No reset was observed. The owner
reported operating the original switch in both jacks, without individually
marking the two attempts in the capture. Counts are transitions, not presses.

GPIO9 retained mux value `0x10` (bus function 1), while the configured
GPIO10 input had `0x30` (GPIO function 3). Thus a quiet GPIO9 observation
does not establish that the spare is disconnected or identify another
GPIO: its input observability in that configuration was unverified.
Those initial captures did not change GPIO9's mode or test output drive,
a transmitted signal or a direct peer connection.

A separate owner-reported hold of roughly three seconds using the original
switch in the antenna-side spare jack produced no changes on the monitored
digital inputs across 80 successful diagnostic samples. GPIO9 and GPIO10
remained LOW, with no observed reset or HTTP errors. GPIO9's mux was still
unchanged. This aux-only attempt did not resolve the spare's MCU route or
establish that its electrical contact signal was absent.

### GPIO9 response in ROM mode

On 21 October 2026, with the lock disconnected, a temporary UART diagnostic
used the ESP8266 ROM loader to select GPIO9's GPIO input function with no
internal pulls and its output disabled. It verified that configuration
before announcing capture. The application was not running; no RAM stub,
firmware image or flash-status change was uploaded or written.

With the original switch in the antenna-side auxiliary jack, the owner
held and released its button. GPIO9 started LOW, became HIGH at capture
time 16.616 s, then returned LOW at 18.612 s. Across 349 recorded samples,
GPIO9 had two transitions and GPIO10 stayed LOW (GPIO10 was observed
without reconfiguration). This supports an **active-HIGH GPIO9 auxiliary
input on the inspected V1**. Capture was deliberately stopped after the
owner confirmed release; the original GPIO9 mux and pin configuration
were restored and verified, with no cleanup errors.
After the owner reset V1 and reported the amber indicator fully off, HTTP
confirmed the same installed observation build running in the open state,
with lock drive inhibited and GPIO9 back at mux `0x10`. UART disconnected
before the startup message could be captured.

This resolves the input-response question that the earlier preserved-mux
captures could not answer. It does not establish that GPIO9 can be used
while the normal application accesses flash, that it can transmit, or that
the two powered auxiliary jacks can be connected directly. The module's
flash-related wiring and the supply-connected sleeves still require care.

### GPIO9 input test during normal firmware execution

On 22 October 2026, the owner requested a web-OTA bench test instead of a
separate ROM-controlled flash-read experiment. Build
`9a265c3352631e788d124291` kept the lock outputs inhibited and booted with
the GPIO9 test inactive. A single explicit HTTP request queued a temporary
GPIO9 input configuration, with output disabled and no pulls. Configuration
and restoration routines were verified in instruction RAM; the rest of the
application continued to execute normally from flash. Activation was not
persisted or automatically repeated after reset.

The request returned `pending`. Before an accessory-button press was
requested, HTTP stopped responding. Four requests timed out, then the
device returned with a new boot ID and reset reason **Hardware Watchdog**.
The capture contained 61 successful samples and one observed restart, with
only `inactive` and `pending` test states; an active state was never
verified. After reboot, the same bench build was open, lock drive remained
inhibited, the test was inactive and GPIO9's mux was back at `0x10`.
Capture was stopped. At the owner's request, one subsequent activation
was captured on the same build. It reproduced the failure: `pending`, four
HTTP timeouts, then a new boot ID with **Hardware Watchdog** and no verified
active state. This second capture contained 39 successful samples and one
observed restart. Recovery again showed the timer open, lock drive inhibited,
test inactive and GPIO9 mux `0x10`. No accessory press was requested during
either attempt, and the second capture was stopped after recovery.

These attempts failed to establish GPIO9 input operation during normal
execution. The failure following the mux-change request is consistent
with interference with flash access, but the watchdog report alone does
not prove the electrical mechanism. The earlier ROM-mode input response
still stands. Do not copy this configuration into normal firmware or treat
either experiment as qualification for aux transmission or a direct cable.

A subsequent comparison with manufacturer V1 `0.3.0` found identical
bootloader code/data segments and matching DOUT, 4 MB, 40 MHz image headers.
The identified manufacturer startup uses the framework's ESP8266 flash-pin
exclusions; its application explicitly configures inputs GPIO5 and GPIO10,
with no identified GPIO9 aux handler. The owner confirmed normal operation
and emergency release under manufacturer firmware, but had not confirmed an
antenna-side aux action. Thus the manufacturer firmware's normal operation
does not establish that it supports the GPIO9 reconfiguration tested here.

## V2 jack inputs

On 20 October 2026, one unloaded V2 ran a temporary web-installed input
monitor (build `204a43c5e107d77ef1ba2750`). Timer starts and lock-output drive
were inhibited. GPIO0 and GPIO1 remained inputs without internal pulls;
the monitor counted level changes sampled by the main loop. Both were LOW
with both jacks empty and with a free-ended cable in the spare jack.

The owner then reported using a metal pin inside each socket, the same
method used for their emergency-input tests. Read-only HTTP diagnostics
recorded:

| Owner-identified socket stimulated | Sampled response |
| --- | --- |
| Nearest magnet/lock-output barrel connector | GPIO0 change counter rose from 0 to 4; GPIO1 remained at 0 |
| Nearest USB-C | GPIO1 change counter rose from 0 to 18; GPIO0 remained at 4 |

Both inputs were LOW afterward. During these initial tests, uptime continued
without an observed reset and commanded lock output remained off. Transition counts can include
contact bounce; they are not press counts. The observed maximum sampling
gap was 248 ms, so short pulses could be missed.

**A subsequent held-contact attempt coincided with a device reset.** A live
capture lost HTTP contact, then uptime restarted and the input counters
reinitialized. The device reported `Power-on reset`; the owner confirmed
only the USB-C-side jack contact was operated, without touching reset or
power. No sustained HIGH was captured. The exact reset cause and which
internal contacts the metal pin bridged remain unknown. Further powered
characterization requires a defined plug/contact connection and current
limiting; the brief responses above do not establish a safe sustained closure.

A later capture during two owner-reported holds of roughly three seconds
recorded two GPIO1 HIGH intervals followed by LOW. Consecutive HIGH samples
spanned 1.26 s and 0.73 s, respectively; these are sampled observation spans,
not exact contact durations or confirmation of uninterrupted three-second
activation. GPIO0 remained LOW and uptime continued without a new reset.
Extra transitions around the first activation were consistent with contact
bounce. The earlier reset remains unexplained.

This supports the socket-to-input roles on this unit. The stimulus did not
identify a plug tip/ring/sleeve pair or trace the intervening components.
It did not establish output capability, current limiting, isolation between
power supplies or safe timer-to-timer interconnection. No mechanical release
or output-voltage measurement was made. These are bench observations, not
an instruction to probe a powered socket with metal or a qualification of
an auxiliary communication feature.

In a later unpowered check, the owner reported a continuity beep from the
auxiliary-jack sleeve to the module's top-left pad, with the antenna pointing
up and pads running down its left and right edges. In that orientation,
the ESP32-C3-WROOM-02 top-left pad is **pad 1, 3V3**; the pad immediately
below is **pad 2, EN** ([module pin definitions](https://www.espressif.com/sites/default/files/documentation/esp32-c3-wroom-02_datasheet_en.pdf)).
The sleeve-to-EN reading was approximately **3.35 kΩ**, without a beep.
The owner also reported emergency-jack continuity to the same 3V3 pad.
All power sources were confirmed disconnected. The first path was described
as having no resistance, but an exact numeric resistance was not supplied.
This supports a shared supply-contact interpretation and is consistent
with the earlier owner-reported auxiliary sleeve-to-UART-ground voltage of
approximately **+3.27 V**. It does not establish a GPIO signal path or the
EN-path resistor topology. A direct cable could join the timers' supply
rails; these observations do not qualify such an interconnection.

With all power again confirmed disconnected, the owner reported no beep
from the auxiliary tip to individual module pads. Touching the fourth pad
down on the left lit the blue LED; touching the fifth lit the amber lock
indicator. With the antenna up, those pads are GPIO5 and GPIO6, matching
the firmware's indicator assignments. Meter test current is a possible
source of this illumination; it does not establish lock-output energization
or a direct tip-to-ground connection. A beep while the probe touched both
the third and fourth right-side pads was ambiguous and is not a net
assignment. No numeric tip-to-GPIO1 resistance was supplied with this report;
absence of a beep alone does not exclude a resistive path.

The owner then supplied numeric resistance readings, continuing the
unpowered check with the antenna up:

| Auxiliary tip to module pad | Reported display |
| --- | --- |
| Second down on right: pad 17, GPIO1 | **0.00 kΩ** |
| First down on right: pad 18, GPIO0 | **1.98 kΩ** |

The GPIO1 reading supports a low-resistance connection from the auxiliary
tip to GPIO1; the display's precision does not establish an exact zero-ohm
path. Combined with the sleeve-to-3V3 observation, this supports a
**tip = GPIO1 signal, sleeve = 3V3 supply** interpretation on this unit,
rather than a verified tip-ground bond. The earlier tip-to-UART-ground
reading of approximately **0.99 kΩ** is consistent with a pull-down path.
The 1.98 kΩ to GPIO0 could include separate bias paths through ground;
it does not establish a discrete 1.98 kΩ resistor or a direct GPIO0 link.
No ground contact, externally observed transmission, current limiting or
safe peer-cable design has yet been established.

The owner reported the complementary result on the emergency jack:
its tip read **0.00 kΩ to GPIO0**, and the resistance to GPIO1 was almost
identical to the auxiliary tip's 1.98 kΩ cross-reading (no separate exact
number supplied). Together, these readings support distinct low-resistance
signal paths: **auxiliary tip → GPIO1; emergency tip → GPIO0**. Separate
pull-down paths through ground are a plausible explanation for the
approximately 1.98 kΩ between signals, consistent with the earlier
approximately 0.99 kΩ auxiliary-tip-to-ground reading. This remains an
in-circuit interpretation, not a traced resistor schematic or proof of
transmit capability. The emergency input retains its existing release role.
