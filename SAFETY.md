# Safety, warranty, and liability notice

> [!WARNING]
> This is unofficial, experimental replacement firmware for a device that can physically restrain a person, including in self-bondage use. A defect, wiring difference, damaged component, exhausted battery, inaccessible power source, network failure, unexpected restart, or mechanical failure can delay or prevent release and can cause serious injury or death.

Read this entire notice before downloading, installing, distributing, or using the firmware.

## Battery power only

> [!CAUTION]
> **Never power the timer from a wall outlet while it controls a lock.** No mains adapter, no USB charger, no power bank that is itself plugged in to charge. Run it from a battery only: a 12 V battery pack for the V1, a USB power bank for the V2. Follow the hardware seller's power instructions.

Why this matters:

- **A battery runs out.** That puts an upper limit on how long a failed timer can keep the lock energized. A wall supply never runs out. The larger the battery, the weaker that limit.
- **A battery can be unplugged.** Keep it within reach of the person and of whoever is attending.
- **A battery keeps mains voltage away from a person who cannot move.** A faulty or counterfeit mains adapter can put dangerous voltage on its output.

Battery power lowers these risks. It does not remove them, and it is not a release plan on its own. The firmware cannot tell what is powering the timer and will hold the lock on wall power for as long as a session runs. Use mains power only on the bench, with the lock disconnected and no one attached, for example while installing firmware.

## No affiliation or certification

This independent project is not produced, approved, endorsed, supported, or warranted by MagBound or the original device manufacturer. Product names are used only to identify the hardware for which the firmware was developed.

The firmware has not been certified as a safety system, life-safety system, medical device, emergency-release system, or protective device. Automated tests verify particular software behavior under test conditions; they do not certify the assembled hardware, external lock, power source, accessory switch, wiring, installation, or suitability for any use.

Pin assignments and connector roles were worked out by inspecting one unit of each model. Other production runs may differ, and several circuits remain untraced (see the [hardware guide](docs/HARDWARE.md)).

Terms such as “fail-open,” “emergency input,” “recovery,” and “safety check” describe intended software behavior. They are not promises that the hardware will open, that every failure mode has been identified, or that injury cannot occur. API state and indicator LEDs report commanded or observed electronic state and are not independent confirmation that a mechanical lock has released.

## Human-restraint warning

Do not rely on this firmware, its timer, Wi-Fi, web interface, API, LEDs, accessory input, or automatic power-loss behavior as the only way to release a person.

At minimum:

- Power the timer from a battery only, never from a wall outlet or charger (see [Battery power only](#battery-power-only)).
- Never use the system for self-restraint while alone.
- A sober, consenting adult capable of immediate intervention must remain present and have an independent physical way to release the restraint.
- Keep at least two independently tested release methods immediately reachable. At least one must not depend on this firmware, Wi-Fi, a phone, the timer controller, or the timer's normal power path.
- Remote control through the experimental relay is not a person present and not a release method. The Internet connection, relay operator or partner's device can fail at any moment, and the timer then keeps running on its own.
- Do not use the system where delayed release could restrict breathing or circulation, involve the neck or chest, cause suspension or a fall, or expose anyone to water, traffic, machinery, fire, extreme temperature, or another time-critical hazard.
- Do not use it on a minor, an incapacitated person, a sleeping or intoxicated person, or anyone who cannot give informed and continuing consent.
- Test the complete assembled system without a person attached before every use and after every firmware, wiring, battery, accessory, or configuration change.
- Stop using the device after any unexpected behavior, reset, intermittent connection, damaged cable, low or failing battery, or failed release test.

Removing battery power may de-energize the lock in the tested configuration, but that behavior is not a sufficient release plan by itself. The battery can become inaccessible, disconnected wiring can behave differently, and the external mechanism can remain mechanically bound. Verify power-loss behavior on the exact assembled unit and retain an independent mechanical release.

## What this firmware does that you must plan for

- **Pause and control locking can be switched off.** Device settings has a saved switch for each; when one is off the timer refuses that control from the web page, Bluetooth, the local API and share links. Both are on until you change them. Turn off what you do not intend to use.
- **Locked controls cannot be undone in software.** Once a session's controls are locked, pause, time changes and release are refused by the web page, the Bluetooth app, the API and remote sharing until the timer expires. Only expiry, the physical emergency input or removing power ends that session; a session started through a share link also ends when that link expires.
- **There is no fixed upper limit on how long the lock stays energized.** The countdown holds at most 24 hours at a time, but time can be added again and again, and a paused session keeps the lock energized until someone resumes or ends it. A random duration can be anywhere in the range that was entered.
- **Anyone holding a share link can start a session, change its time and lock its controls** within the firmware's limits until the link expires or is revoked. They cannot see whether the person is safe. Share a link only with someone the restrained person has agreed to; that agreement cannot be withdrawn through the software once controls are locked.
- **Anyone on the same network can control the timer.** The local page and API have no login.
- **The firmware cannot tell whether the lock actually released.** It commands the output and reports what it commanded.
- **Release means the output is switched off.** That frees only a lock that needs power to hold. Anything that stays locked without power will not open.
- **The emergency input is read by this firmware; it is not an independent cut-off.** The firmware cannot tell whether a switch is connected: an unplugged, broken or wrong-jack switch looks the same as one that is not pressed. Verify the correct jack on the actual unit with the controlled load disconnected, and test the accessory before every use. The owner identifies V1 emergency by the 12 V power input and V2 emergency by the magnet/lock output; use these model-specific landmarks rather than a shared left/right rule ([hardware guide](docs/HARDWARE.md#accessory-jacks)).
- **The lock can also release early and without warning,** for example after a power dip, a restart or contact noise. Do not use it where an unexpected release is itself dangerous.
- **The firmware does not monitor current, voltage or temperature.** A coil energized for hours, and whatever powers it, can get hot. Use only the lock and power source the hardware was built for, and do not cover them.
- **An update, restart or crash is intended to leave the output off,** which is not the same as a tested mechanical release on your unit.

## Assumption of risk

Installation and use are voluntary. The installer and operator are solely responsible for evaluating the hardware, wiring, controlled mechanism, power source, environment, applicable law, and every release method. They assume the entire risk arising from installation, modification, distribution, and use, including the risk of hardware damage, loss of access, unintended energization, delayed release, bodily injury, and death.

The project does not recommend or instruct any use of this firmware to restrain a person. Whether and how to use it is the installer's and operator's own decision and risk.

## No warranty

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THE SOFTWARE, FIRMWARE IMAGES, DOCUMENTATION, AND RELATED MATERIALS ARE PROVIDED “AS IS” AND “WITH ALL FAULTS,” WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE, NON-INFRINGEMENT, ACCURACY, RELIABILITY, SAFETY, OR THAT THE SOFTWARE WILL BE ERROR-FREE OR OPERATE WITHOUT INTERRUPTION.

NO AUTHOR, COPYRIGHT HOLDER, CONTRIBUTOR, MAINTAINER, OR DISTRIBUTOR MAKES A PROMISE THAT THE DEVICE WILL RELEASE, REMAIN ACCESSIBLE, PRESERVE SETTINGS, RECOVER FROM AN UPDATE, OR BE SUITABLE FOR RESTRAINT OR ANY OTHER PARTICULAR PURPOSE.

## Limitation of liability

TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, IN NO EVENT SHALL ANY AUTHOR, COPYRIGHT HOLDER, CONTRIBUTOR, MAINTAINER, OR DISTRIBUTOR BE LIABLE FOR ANY CLAIM, LOSS, INJURY, DEATH, PROPERTY DAMAGE, DEVICE DAMAGE, LOSS OF ACCESS, OR OTHER DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, CONSEQUENTIAL, OR PUNITIVE DAMAGES ARISING FROM OR RELATED TO THE SOFTWARE, FIRMWARE, DOCUMENTATION, INSTALLATION, MODIFICATION, DISTRIBUTION, OR USE, WHETHER ASSERTED IN CONTRACT, TORT (INCLUDING NEGLIGENCE), STRICT LIABILITY, STATUTE, OR ANY OTHER LEGAL THEORY, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH HARM.

Nothing in this notice excludes or limits liability that applicable law does not permit to be excluded or limited.

## Distribution

When redistributing the firmware, please include the matching source version, release hash, [installation guide](docs/INSTALLATION.md) and this notice. Please do not present a modified build as official, certified, tested by this project, or safe for human restraint.

Copying, modification and distribution permissions and conditions are defined
by the repository's [AGPL-3.0-only license](LICENSE). The **Assumption of risk**,
**No warranty** and **Limitation of liability** sections above are additional
terms under section 7(a) of that license: they supplement its sections 15 and
16 and do not restrict the permissions it grants. Everything else in this
notice is guidance and information, not an additional restriction on those
permissions.

This notice is general project information, not legal advice. Anyone who redistributes this firmware, or offers hardware or services that use it, is responsible for complying with the laws that apply to them.
