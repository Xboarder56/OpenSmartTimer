# Remote sharing (experimental, V2)

Remote sharing lets one partner control your **V2** timer over the internet
from a time-limited link. It is **off by default** and experimental. Read
[SAFETY.md](../SAFETY.md) first: remote control never replaces the physical
emergency release, removing power, or the local controls.

## What you need

- A V2 timer on Wi-Fi (not the setup access point).
- A **relay server** and an independently hosted trusted partner page. The relay
  forwards encrypted traffic; the timer and trusted browser authenticate each
  other using the link. The design is that a compromised relay can interrupt
  access but not authorize controls; it has had no independent security review.
  Protect the partner page's hosting credentials and share
  links. Operators set these up with [relay/OPERATOR.md](../relay/OPERATOR.md).

## 1. Connect the timer to a relay

1. Get a **setup code** (it starts with `ost-relay-setup:`):
   - **Your own relay:** it prints one in its log on first start.
   - **A friend's relay:** ask them; your code works for all your timers. If
     their relay has sign-in, open its `/owner` page instead, sign in and
     select **Create setup code**.
   - **A public relay:** open its `/setup` page, for example
     `https://relay.example.com/setup`, and copy the code.
2. Unlock the timer, open its page, then **Device settings → Remote sharing**.
3. Paste the setup code. Review both the relay address and independently hosted
   control-page address, including any port.
4. Select **Connect relay**, then briefly press **Reset** on the unlocked timer
   within the 60-second review window to approve those addresses. Wait for
   **Connected** before creating a link.

Given separate details instead? Open **Enter details manually** and type the
relay address, the enrollment code, the server key pin and the trusted
control-page address.

## 2. Share a link

1. Optionally name the link (for example the partner's name).
2. Choose how long it is valid: 15 minutes to 24 hours, or **Custom**.
3. Select **Create share link**, then **Copy** and send it privately. Anyone
   with the link has full partner control (start, pause, add time, lock the
   controls) until it ends; there is no separate partner login.

One link can be active at a time. While it is active:

- A **SHARED** badge shows at the top of the timer page, and the page tucks the
  local controls away. **Show local controls** brings them back; **Unlock now**
  stays visible unless controls are locked.
- Recent activity marks actions that came **via share link**.

The partner page has the same English/Simplified Chinese and appearance controls
as the local page. It supports fixed or random duration; random start chooses
once within the selected minimum and maximum. **Lock controls** is available
only while running and asks for confirmation before disabling software controls
until the timer opens. The emergency input and removing power still command
release; neither is guaranteed to open the lock (see [SAFETY.md](../SAFETY.md)).

Commands refresh the verified timer status before sending. If a response is lost,
the page checks for the saved result automatically and keeps the outcome marked
unknown until confirmed. It never resends an action automatically. **Check
results** remains available for an unresolved action.

## How a link ends

- **It expires.** Your partner cannot start or extend a session beyond the
  link's end. If the link started, extended, paused or locked the session, the
  firmware is designed to command release when the link expires, without
  needing the relay or the internet. That is software behaviour, not a way to
  release.
- **You revoke it** (works during a session). Only that link stops; the timer
  is not unlocked or changed.
- **You turn remote sharing off**, or **forget** the relay.
- **The timer restarts.** Links never survive a restart.
- **The emergency input is triggered.** Remote sharing turns off and the link
  ends, so a partner cannot simply restart the timer. Turn sharing on again
  when you are ready.

**Extend** (+15 min, +1 hr, +4 hr on the active link) keeps the **same link**:
your partner does not need a new URL, and an open partner page simply shows the
later end time. It also works during a session. After a link expires, the
**Ended** card offers **Reactivate** (same link again, until the timer
restarts) and **New link** (a fresh URL; use this if the old link may have
been shared further). **Renew** on an active link does the same: the old URL
stops working and you send the new one.

Links are extended, renewed and revoked from the timer's own page, which has no
login: anyone on the timer's local network can do it. Your partner and the
relay operator cannot.

## Bluetooth comes first

While a Bluetooth app is connected, or the pairing window is open, remote
sharing pauses (the timer does not have memory for both). Your partner sees the
timer as offline; the link stays valid and sharing resumes a few seconds after
Bluetooth disconnects. Leaving Bluetooth switched on with no app connected does
not pause sharing.

## Troubleshooting

| Message | What to do |
| --- | --- |
| The relay server key does not match the pin | Shown during setup. The setup code is old or mistyped, or the relay's certificate changed after the code was made; ask the operator for a new setup code. |
| The relay could not prove it is the server this timer was set up with | The relay address now leads to a different server, or the relay lost its data. Check the address; if the relay was rebuilt, set sharing up again with a new setup code. A renewed certificate does not cause this. |
| The relay refused this setup code | The code was used or expired (one-time codes last 15 minutes), was replaced or revoked, or the relay is full. Get a new code. |
| The timer could not reach the relay | Check the timer's Wi-Fi and the relay address. |
| Waiting for free memory before connecting | Close other open timer pages or Bluetooth apps; the relay retries automatically. |
| Unlock the timer first | Setup, turning sharing on, creating and renewing links need an unlocked timer. |

Developers: the owner API is in [API.md](API.md#experimental-remote-relay-v2-only)
and the protocol in [RELAY_PROTOCOL.md](RELAY_PROTOCOL.md).
