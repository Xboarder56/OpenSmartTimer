# `ost-relay/1` protocol

The first public contract between the relay server (`relay/src`), partner
page (`relay/spa`), mock timer and V2 firmware (`src/relay`, off by default).
See [firmware integration](RELAY_FIRMWARE.md), [relay overview](../relay/README.md)
and [security boundaries](../SECURITY.md). Hardware V1/V2 and protocol revision
numbers are independent.

Firmware, server and trusted page must implement the same contract. There is
no plaintext transport fallback. Incompatible public contract changes require
a new protocol revision.

## 1. Transport and identifiers

| Item | Value |
| --- | --- |
| JSON protocol tag / capability audience | `ost-relay/1` |
| WebSocket subprotocol header | `ost-relay.v1` |
| Capability prefix | `ost-cap.v1.` |
| Relay public routes | `/api/v1/*`, `/setup`, optional `/owner` |
| Trusted page routes (independent HTTPS origin) | `/s/<public-id>`, `/s/assets/*` |
| Device connection | One outbound WSS to `/api/v1/timer` |
| Browser connection | Public handshake, encrypted requests and SSE |

The header uses a dot because a WebSocket subprotocol must be an HTTP token;
`/` is not allowed. [RFC 6455, client requirements](https://www.rfc-editor.org/rfc/rfc6455#section-4.1)

Messages are UTF-8 JSON objects in text frames. Base64url is unpadded and
canonical. Integers used for time and sequences are nonnegative and no larger
than `2^53-1`; sequences begin at one. The current C++ uptime storage is
32-bit and disables relay operation on wrap; it refuses a mint whose expiry
would cross that boundary. Boot IDs are 1–16 lowercase hex characters.
Timer IDs use URL-safe ASCII; enrollment currently generates 16 characters.

The backend rejects duplicate object keys, excessive nesting, nonfinite
numbers, binary frames and oversize bodies. Signed bodies have their own
exact-byte rule in section 7. The C++ parser is fixed-buffer and covered by host rejection/interop tests;
platform-level fuzzing remains additional validation.

## 2. Roles and authoritative state

The creator selects a trusted operator, enrolls locally, and chooses a time limit for a partner link. Current creator flows grant every supported control; scope fields remain internal for future extensions.
A partner may exercise only the link's scopes. An owner can also use a locally
minted link remotely; optional OIDC owner accounts manage setup codes and enrollments, not partner
authority or cross-boot links.

The timer owns the boot/session IDs, state machine, expiry, local emergency
input, controls lock and duration limits. The adapter applies relay commands through the shared timer authority and
maintains its own bounded operation ledger, following REST/BLE retry semantics.
A relay request must never bypass those checks. An ordinary remote `release`
is still subject to controls-lock policy; owner disable/revoke is a separate
local action and does not itself command a physical release.

## 3. Trust and keys

| Key/material | Held by | Purpose |
| --- | --- | --- |
| Enrollment code | Operator/owner until consumed | One-time enrollment |
| Enrollment secret, 32 bytes | Timer and relay database | Hello and acknowledgement HMACs; mutual authentication of every WSS connection |
| Capability key, 32 bytes | Timer only | Mint/verify link HMAC |
| ECDSA P-256 private scalar, 32 bytes | Timer only | Authenticate the full ECDH accept transcript |
| ECDSA P-256 public key, 33-byte compressed point | Token, browser | Verify the timer identity before using session keys |
| Owner cookie secret, 32 bytes represented as 64 hex chars | Relay operator | Owner/OIDC browser sessions only |
| Grant fingerprint, 16 random bytes | Timer and trusted partner browser | Prove possession of the link during ECDH |
| Ephemeral P-256 private key | Each endpoint during handshake | Fresh session traffic keys |
| Directional AES-256-GCM keys | Timer/browser RAM only | Protect command, status and outcome bytes |
| TLS SPKI pin, SHA-256 | Setup code | Authenticate the enrollment HTTPS server |
| TLS leaf SPKI digest, SHA-256 | Observed by the timer on each connection | Bind the hello to the TLS key in use |

The relay is an untrusted transport. The complete link token and raw fingerprint
never enter relay HTTP, WSS, logs or its database. The owner reviews and physically
approves the independent trusted-page origin locally during setup. The routing
relay cannot replace it through enrollment responses or reconnects. Production
requires a host/account whose deployment credentials,
assets and TLS key the relay cannot access. A second container on the relay host
does not isolate the trusted page from compromise of that host.

Authenticated ephemeral P-256 ECDH derives fresh direction keys. Erasing ephemeral
private keys and retired traffic keys protects recorded past sessions against
later disclosure of the grant fingerprint. Browser keys are nonextractable
WebCrypto objects; deleting references does not promise physical erasure by JS.
The relay still sees timer/grant IDs, expiry/scopes, traffic size/timing and public
handshakes, and can deny service. Trusted-page compromise, a stolen live link or
a compromised timer remains outside this protection. Enrollment secrets in the
relay database do not grant timer capability or traffic keys.

Signatures are ECDSA P-256 over SHA-256 using the V2 mbedTLS provider. Every signer uses RFC 6979 deterministic nonces and emits the
low-S form, as raw `r || s` (64 bytes). Verifiers reject high-S signatures,
including the malleable twin of a low-S signature. `vk` is the SEC1 compressed
public point (33 bytes, 44 base64url characters); browsers expand it before
WebCrypto import. The mock, browser, C++ host (OpenSSL, RFC 6979 vectors) and
ESP provider (mbedTLS) implement these rules. See
[Testing and verification](VALIDATION.md) for automated coverage and
physical-device verification limits.

## 4. Capability tokens

Wire representation:

```text
ost-cap.v1.BASE64URL(payload_json_utf8).BASE64URL(HMAC_SHA256(capability_key, payload_json_utf8)[:16])
```

The payload has exactly these keys:

```json
{"aud":"ost-relay/1","tid":"timer_id","fp":"base64url_16_bytes","sc":["view","pause_resume","adjust","release","start","lock","identify"],"boot":"aabbccdd","exp_up_ms":14400000,"mint_up_ms":42159,"vk":"base64url_33_byte_compressed_P256_point"}
```

`fp` is a cryptographically random per-link fingerprint. `exp_up_ms` is timer
uptime in the named boot. Every scope list includes `view`, contains no unknown
or duplicate scopes, and grants no implied mutation privileges.

| Scope | Allowed operation |
| --- | --- |
| `view` | Watch authenticated status/events |
| `pause_resume` | `pause`, `resume` |
| `adjust` | Positive or negative `adjust` |
| `release` | `release` |
| `start` | Fixed-duration `start` |
| `lock` | `lock_controls` |
| `identify` | `identify` |

The creator UI defaults to 60 minutes and accepts 1–1,440 minutes. Local APIs
accept an explicit integer `ttl_seconds` in 1–86,400. The lower-level C++ default
constant remains four hours, but creator flows always supply a lifetime.
The sync queue holds at most 32 unexpired grants, including revoked tombstones. It limits mint attempts to 20/hour. It refuses to evict a live
grant silently. Public routing handles expire after two hours; refreshing a
handle cannot extend the device grant.

The timer verifies HMAC, audience, timer/boot, expiry, live table entry and
required scope for every mutation. Boot changes invalidate old links. This
link lasts for its **boot and TTL**, not only one active timer
session. `if_session_id` on a command protects that click; it does not make the
grant itself session-bound. Creators select the lifetime with all supported
controls across timer sessions. No scope selector,
separate add/subtract permissions or cumulative grant limits are included now.
Expiry starts at generation, not first use, and retries never extend it.
Expiry removes partner access. If the link started, extended, paused or locked
the current session, its device-enforced hold limit opens the timer when the
link expires, even during a relay outage. Expiry of an unused link does not
change an independently started local session.
Owner accounts and enrollment do not give a partner an identity or additional
control permissions. Partner-specific scope selection remains outside this UI.

### 4.1 Registering and revoking a link

After a local mint, the authenticated device connection announces:

```json
{"type":"token_mint","upstream_seq":2,"fp_hash":"base64url_16_bytes","token_digest":"base64url_32_bytes","scopes":["view","pause_resume","adjust","release","start","lock","identify"],"boot_id":"aabbccdd","exp_up_ms":14400000,"label":"partner"}
```

- `fp_hash = BASE64URL(SHA256(raw_fp)[:16])`.
- `token_digest = BASE64URL(SHA256(ASCII(full_wire_token)))`.
- The digest registers the exact token, including its HMAC and public key.
  This is public registration metadata. Device capability verification, rather
  than a relay digest comparison, authorizes every encrypted request.
- A repeated mint must match the original timer, boot, digest and scopes, and
  can never undo a revocation. Its `exp_up_ms` may differ: the **timer owns a
  grant's expiry** and re-announces the same grant when the owner extends it
  (same URL), and the relay stores the new value. Only the authenticated
  timer's connection can do this; partners and the relay cannot create,
  extend or revoke grants.
- The expiry inside the wire token is the grant's **initial** value. The timer
  and relay treat their current grant record as authoritative, which only
  moves later; a token whose embedded expiry is later than the record is
  refused. Public browser routing admissions last two hours and can be refreshed.
  Only a fresh AEAD-authenticated device status can establish a changed expiry.
  Its `share_link_expires_up_ms` lets an open partner page follow an extension
  without a new URL. After expiry, reactivation uses a new authenticated ECDH
  session and a fresh device sync challenge. The browser verifies the new
  expiry before restarting its countdown or revealing controls.
- `token_revoke` carries the same grant metadata as `token_mint`, with a fresh
  upstream sequence. It creates a revoked tombstone even when the original mint
  was lost or a relay restore omitted it. A later mint cannot revive that row.
- Revocation deletes affected browser sessions in the same transaction. An
  active stream rechecks authorization at most one second later and before
  each delivered event. Device authorization stops immediately on local revoke.

After the transaction commits, the relay replies:

```json
{"type":"token_ack","action":"token_mint","upstream_seq":2,"fp_hash":"base64url_16_bytes","boot_id":"aabbccdd","status":"ready"}
```

`action` identifies the request type. Sequence, fingerprint, boot and action
must all match the currently outstanding operation. `ready` confirms a mint;
`revoked` confirms revocation (including an attempted replay of a revoked mint).
`expired` ends the grant; `capacity` is retryable; `conflict` and `unknown` expose
an error. Malformed frames still close the connection. The old fingerprint-only
revoke helper can update a known row; an unknown row returns `unknown`, never a
false success. All current clients send full revoke metadata.

Registration frames and their ACKs are authenticated by the challenged WSS
connection, not signed envelopes. Generic `acked_upstream_seq` receipt ACKs
cannot mark a link ready. The mock and C++ retain up to 32 grant metadata entries
in RAM until expiry, retry pending operations every two seconds while connected,
and replay live registrations AND revocation tombstones after a same-boot
reconnect. Replay preserves the original digest, rights and expiry. C++ retains
metadata only; the local creator is responsible for retaining its wire token.
A boot change invalidates links; no pending journal across boots is promised.

New link creation requires a live authenticated relay connection. This is
stronger than a LAN address alone: the relay must actually be reachable. The
creator API returns 503 before minting when unavailable. Its UI starts disabled,
grays out the form when `can_share` is false, and disables sharing if availability
checks fail or become stale. The C++ handler uses `sharingAvailable()` before
minting. The V2 local interface exposes this flow in Settings → Remote sharing;
Bluetooth connections and pairing pause relay availability.
Already accepted mints may finish/retry after a drop; revocation still stops
local authorization offline and queues relay synchronization. No new grant or
expiry extension is created by those retries.

The mock's loopback creator page shows `pending`, `ready`, `revoking`, `revoked`,
`expired` or `error`; it exposes the share URL only in `ready`. Its POST contains
`ttl_seconds`, `label` (up to 23 characters), and a bounded `request_id` used to
make uncertain retries idempotent while the grant record is retained. Scope
input is rejected. `maintain_connection()` reconnects with a bounded 1–30 second
backoff. The C++ `announce*` return values mean queued; poll `grantState()` for
confirmation. The V2 firmware exposes the same flow as `/api/v1/relay/links`
(see [API.md](API.md#experimental-remote-relay-v2-only)).

## 5. Enrollment and local lifecycle

`POST /api/v1/enroll` accepts:

```json
{"enrollment_code":"single_use_code","hardware_model":"MagBound Smart Timer V2","chip_family":"ESP32-C3","build_id":"aabbccddeeff00112233aabb","device_name":"Kitchen timer"}
```

`device_name` is optional, at most 32 printable ASCII characters, and initializes
the timer's label in the owner/admin list. Without it, the list falls back to the
hardware model. It is display metadata, carries no control authority and is
visible to the relay operator. An operator may edit the label independently;
renaming a device does not change an existing enrollment's label.

It returns `timer_id`, `enrollment_secret`, `wss_endpoint` and
`spki_sha256_pin`. The code is a single-use operator code (expires after 15
minutes), a reusable owner code (`o1.<owner_id>.<secret>`) or the public
`open-enrollment` code, as the relay's `OSTRELAY_ENROLLMENT` allows; each
reusable-code enrollment gets a new random `timer_id`. Refusals: `403
enrollment_denied`, `410 code_consumed|code_expired`, `409
relay_full|owner_timer_limit`, `429 rate_limited_enrollment`, and `503
tls_identity_unavailable` while the relay has not discovered its TLS key. The
503 response includes `Retry-After: 15` and consumes neither the code nor
enrollment capacity. The device must authenticate
the enrollment HTTPS server before trusting that response; the returned pin
cannot authenticate an already-intercepted enrollment exchange.

Server startup requires an HTTPS origin. The relay must know which TLS key
timers are shown: `OSTRELAY_SPKI_PIN` names it (several pins may be listed), or,
when that is empty, the relay reads the leaf key from its own public URL with
ordinary certificate validation and repeats that every ten minutes and whenever
a hello arrives bound to an unknown key. Setup codes carry the current pin.
Plain HTTP is an explicit loopback-only development exception, in which the
key digest is 32 zero bytes. The reference mock verifies HTTPS and WSS
certificates, binds the WSS peer's key into its hello and checks the
acknowledgement; `--ca` adds a local test CA.

The V2 timer enrolls itself: the owner calls `POST /api/v1/relay/enroll` with
the relay origin, enrollment code, operator's SPKI pin, independent control-page
origin and current `if_boot_id`. This stages an immutable 60-second request;
the timer displays its public addresses through local status. A brief Reset
press while unlocked approves the request captured on button-down, then
enrollment begins. Another setup cannot replace it; exact retries do not extend
its deadline. Expiry, cancellation, emergency input, timer closure or an OTA,
network or Bluetooth transition invalidates pending approval. There is no HTTP
approval endpoint or pre-armed setup window. The timer
refuses the enrollment TLS session unless the leaf key matches that pin, and
refuses a response whose `spki_sha256_pin` differs from it. The timer carries no
public CA bundle (about 60 KB of flash), so certificate chain, expiry and
hostname are never checked. **The pin authenticates enrollment only.** Every
later WSS connection accepts whatever leaf certificate is presented and is then
authenticated in both directions with the enrollment secret, bound to that
certificate's key (section 6). A renewed or replaced relay certificate
therefore needs no action from the owner.

The firmware lifecycle is opt-in and default-off: enrollment enables it,
`enable`/`disable` persist the choice, `disable` revokes every link and works
during a session, `forget` erases credentials and keys, factory reset erases
all relay storage, and OTA/restart/network transitions pause the connection
and refuse relay commands. After a reconnect the timer sends pending link
registrations and revocations **before** its first status frame, so a link
revoked while offline never receives fresh state.

## 6. WSS challenge and hello

Offer `Sec-WebSocket-Protocol: ost-relay.v1`. On acceptance, the relay sends:

```json
{"type":"challenge","server_nonce":"base64url_16_random_bytes"}
```

The timer replies within five seconds:

```json
{"type":"hello","protocol":"ost-relay/1","timer_id":"timer_id","boot_id":"aabbccdd","uptime_ms":1234,"resume_from_upstream_seq":0,"resume_from_downstream_seq":0,"hello_tag":"base64url_16_bytes"}
```

Compute the tag over concatenated bytes, with no separators or terminators:

```text
HMAC-SHA256(enrollment_secret,
    UTF8("OST-RELAY-HELLO-v2")
    || raw_server_nonce_16
    || UTF8(timer_id) || UTF8(boot_id)
    || BE64(uptime_ms)
    || BE64(resume_from_upstream_seq)
    || BE64(resume_from_downstream_seq)
    || tls_leaf_spki_sha256_32)[:16]
```

`tls_leaf_spki_sha256_32` is the SHA-256 of the DER SubjectPublicKeyInfo of the
leaf certificate the server presented on this TLS connection, as the timer saw
it. It is not sent in the frame. The relay recomputes the tag with each key it
knows to be its own and refuses the hello (`4003`) when none matches. A hello
forwarded by something that terminates TLS in between names that party's key,
so it fails at the real relay. Without a TLS key the timer sends no hello.

The fresh random challenge prevents replaying a captured hello on another
connection. The relay verifies the tag before registering the timer. A newly
authenticated connection displaces the old connection for that timer.

Reply:

```json
{"type":"hello_ack","session_id":"transport_session_id","resume_ok":false,"server_time_unix":1789560000,"ack_tag":"base64url_16_bytes"}
```

```text
ack_tag = HMAC-SHA256(enrollment_secret,
    UTF8("OST-RELAY-ACK-v1") || raw_server_nonce_16 || raw_hello_tag_16)[:16]
```

Only a relay holding this timer's enrollment secret can produce `ack_tag`, and
it does so only after accepting a hello bound to its own TLS key. The timer
checks it in constant time before treating the connection as live and before
sending anything else; a missing or wrong tag closes the connection with
`relay_auth_failed`. Together the two tags authenticate both ends and the TLS
session between them without a certificate authority, a stored certificate pin
or a clock.

`server_time_unix` is diagnostic, never device authority. The current relay
always advertises `resume_ok:false`; no durable server replay negotiation is
implemented. The timer replays its unacknowledged in-memory results and sends
fresh status. Status heartbeats are every five seconds in the C++ scaffold,
every two seconds in the mock by default. Relay JSON `ping` is every 30 seconds;
`pong` is `{ "type": "pong" }`. Idle peers are disconnected after the configured
90-second timeout at a heartbeat check. C++ separately bounds connect (10s),
challenge/hello (5s) and live idle time (90s).

## 7. Authenticated ephemeral channel

Let `fp` be the raw 16-byte fingerprint, `h = SHA256(fp)[:16]`, and let all
base64url values be canonical and unpadded. Concatenations below are binary;
`BE64` is an eight-byte big-endian unsigned integer. Labels and IDs are ASCII.

```text
auth = HKDF-SHA256(ikm=fp, salt="OST-RELAY-E2E-AUTH-v1", info="", len=32)
open = "OST-RELAY-E2E-OPEN-v1" || timer_id || NUL || boot_id || NUL
       || h || client_id[16] || client_public_key[65]
```

The browser generates a new random client ID and ephemeral P-256 key pair for
every page load. `POST /api/v1/session` contains `fp_hash`, `client_id`, `boot_id`,
`client_key`, and `tag=HMAC-SHA256(auth,open)`; no token or cookies. The relay
forwards those fields as `type:"e2e_open"`. Public points use SEC1 uncompressed
encoding (65 bytes). The timer checks the live grant, boot and MAC before ECDH.
It generates a random 16-byte server ID and fresh ephemeral P-256 key pair:

```text
accept = "OST-RELAY-E2E-ACCEPT-v1" || open || server_id[16] || server_public_key[65]
accept_tag = HMAC-SHA256(auth,accept)
sig = ECDSA-P256-SHA256(timer_identity_private_key, accept)
keys = HKDF-SHA256(ikm=ECDH_shared_secret[32], salt=auth,
                  info="OST-RELAY-E2E-KEYS-v1" || SHA256(accept), len=64)
```

The timer's `e2e_accept` has `upstream_seq`, `fp_hash`, `client_id`, `server_id`,
`server_key`, `tag` and mandatory `sig` (64-byte low-S raw `r || s`). HTTP returns
`{session_handle,timer_id,boot_id,frame}`. The browser verifies the full accept
signature using only the identity key `vk` from its locally retained capability,
and verifies the grant MAC before using keys. The relay cannot supply a replacement
identity key. The signature binds the protocol labels, timer, boot, grant hash,
both IDs and both ephemeral public keys. Even a link holder who can compute
the grant MAC cannot substitute a fake timer handshake. The first 32 bytes protect browser→timer, the last
32 timer→browser.
Ephemeral private material is discarded after derivation. Repeating an identical
open retransmits its cached accept, including its signature; it neither signs
again nor resets counters. A new channel requires a fresh signed handshake. The server retries only identical opens
within the five-second handshake
wait to accommodate device admission throttling; mutations are never retried.

### 7.1 Encrypted frames and replay rejection

Both directions use this outer shape (timer frames additionally have the global
`upstream_seq` used for WSS receipt acknowledgments):

```json
{"type":"e2e","fp_hash":"base64url_16_bytes","client_id":"base64url_16_bytes","server_id":"base64url_16_bytes","seq":1,"request_id":"unique-request-id","ciphertext":"base64url_ciphertext_and_16_byte_tag"}
```

```text
nonce = zero[4] || BE64(seq)
AAD = "OST-RELAY-E2E-DATA-v1" || direction[1] || h[16] || client_id[16]
      || server_id[16] || BE64(seq) || ASCII(request_id)
```

Direction is byte 1 for browser→timer, byte 2 for timer→browser. AES-256-GCM
uses a full 16-byte tag. Plaintext is at most 1,800 bytes; request IDs contain
0–64 printable nonspace ASCII bytes (HTTP requests require a nonempty ID).
Transmit sequences begin at one and increase for **every encryption**, including
result retransmission. Receive sequences must exceed the session high-water
mark. Update that mark only after successful AEAD authentication, but before
JSON, token or command validation. This remains true after operation-history
eviction and WSS reconnect. Old packets, reflected direction, altered routing,
request ID, nonce, ciphertext and cross-session packets cannot apply controls.
The server sees and validates only outer shapes/routing; it holds no crypto keys.

There are eight device channels, at most two pending. A new authenticated open
is admitted at most once/second. Pending channels expire after ten seconds;
confirmed channels after 120 seconds without authenticated requests. Revoke,
boot change, forget and emergency shutdown erase affected traffic keys.
Same-boot WSS reconnect retains keys/counters in RAM; browser reload creates a
new channel. Closed tabs/reloads may occupy old slots until idle expiry, so rapid
repeated reloads can temporarily exhaust admission. No plaintext fallback exists.
Firmware, relay and trusted page must implement matching channel formats.
Hosting addresses are runtime configuration; deployments do not require a
per-host firmware build.

### 7.2 Encrypted requests and authenticated responses

`POST /api/v1/command` accepts only the encrypted outer frame and public
`X-OST-Session` handle. Inside encryption, requests contain the complete
`cap_token` and either `type:"sync"`, `type:"lookup",op_id`, or this command:

```json
{"type":"command","downstream_seq":42,"cap_token":"ost-cap.v1.payload.tag","op":"adjust","op_id":"unique-request-id","if_boot_id":"aabbccdd","if_session_id":"aabbccdd-1","expiry_uptime_ms":6234,"params":{"delta_ms":60000}}
```

Command `downstream_seq` must match the authenticated outer sequence and
`op_id` must match `request_id`. Capture preconditions and observed uptime at
click time; expiry is that observed uptime plus 5,000 ms. The device verifies
capability HMAC, audience, live grant, scope, boot, expiry, command deadline and
shared timer authority independently. Encryption cannot override emergency
input, controls lock, runtime maintenance, state or duration limits.
`start` uses positive uint32 `duration_ms`, `adjust` nonzero int32 `delta_ms`;
other operations use `{}`. Configuration and OTA have no relay opcodes.

Responses encrypt an inner status/event/result envelope with exactly three keys:

```json
{"type":"status","upstream_seq":3,"body_json":"{\"boot_id\":\"aabbccdd\",\"uptime_ms\":1234}"}
```

Routine responses have no ECDSA signature. The identity-signed handshake
establishes the timer-to-browser traffic key; AES-GCM authenticates the entire
inner envelope and its outer request/session/sequence context. The browser
checks AEAD before parsing the strict envelope, then checks boot, sequence and
freshness. `body_json` remains the timer's original serialized JSON string.
These responses provide session authentication, not independently verifiable
receipts for third parties. Missing handshake signatures and legacy signed
response envelopes fail closed; there is no downgrade or plaintext fallback.
Status contains state, remaining/session/max duration, lock/emergency flags,
boot/session IDs, revision, uptime and build ID. Results additionally bind opcode,
operation ID, attempt sequence, mutation and outcome; HTTP 200 alone is no proof
of application. The relay cannot parse or cache these plaintext bodies.

The device reserves one of eight RAM result records before mutation. Unacknowledged
results retain their original outcome, status and correlation in RAM and are
re-encoded for retransmission with a **new GCM sequence**. A full unacknowledged ring refuses new application.
WSS ACKs confirm
relay receipt only, not durable storage. A bounded operation ledger handles intent
dedup separately. Encrypted lookup asks the timer for retained history; no server
plaintext result endpoint exists. Timeout/absent history means unknown outcome,
not proof of no application. No exactly-once or power-loss durability is promised.

## 8. Trusted browser, routing and freshness

A share URL is generated locally as:

```text
https://trusted-origin/s/<fp_hash>#relay=BASE64URL(ASCII(https://relay-origin))&cap=<wire-token>
```

The timer generates this link using its owner-approved runtime origin; it must
differ from the relay. `<fp_hash>` is a public routing identifier, not a control
credential. It is checked against the capability before any session request.
The page clears the fragment immediately. The token, its signing-key/boot anchors,
public routing handle and unresolved intents persist in **trusted-origin
sessionStorage** for reload/reconciliation. Traffic keys/counters never persist.
The relay has neither `/s/` nor partner assets; they return 404 there.
The URL parser also accepts `/s/?relay=<encoded>#<wire-token>` while the local
grant is valid. Both formats use the encrypted channel. Stored enrollments
without an approved page origin stay offline until physical setup approval.

Routing handles contain no secret authorization. `GET /api/v1/session` refreshes
public routing admission only; header `X-OST-Session` or query `session_handle`
is required. The database stores only the handle, fingerprint hash, client ID
and wall-clock lifetime. DELETE/session or POST/logout drops routing admission;
it is not device grant revocation. New firmware requires new grants; migration
drops the old token-bearing session table but does not erase historical backups.

SSE `/api/v1/watch?session_handle=...` forwards only matching opaque `e2e` frames.
The page enables controls only after an AEAD-authenticated `sync` reply matches its
fresh unpredictable request ID within five seconds. A captured old broadcast,
result, reconnect or relay-claimed uptime cannot establish freshness. Sync runs
on connect, after results, before each new mutation and every ten seconds; controls go stale after 15
seconds. Countdown between authenticated snapshots is an estimate. Terminal
relay availability events may disable access but never authorize a mutation.

The first encrypted status challenge starts without waiting for the stream's
`open` event; read-only heartbeats continue during stream admission/reconnect.
Some browsers or proxies wait for stream data before reporting it open. HTTP
and SSE can deliver the identical response: a duplicate does not undo successful
verification of that exact challenge. Success still requires its unpredictable
nonce, five-second receive window, authenticated encryption under the
identity-verified channel,
fresh sequence and valid snapshot. A stale, cached or invalid frame cannot
complete the check or enable controls.

If fresh status remains unavailable after 15 seconds, read-only polling can
replace this tab's encrypted channel using its retained capability. This also
recovers a suspended tab whose device traffic keys expired after 120 seconds.
The page releases its old routing admission, verifies a new identity-signed
ECDH handshake and requires a fresh nonce-bound status before restoring
controls. Failed attempts back off from 15 seconds to at most 120 seconds;
polling continues through temporary outages. Late events and responses from
the replaced channel cannot alter the recovered tab. Duration drafts,
unresolved intents and the upstream sequence watermark are retained. Recovery
never replays a mutation or renews an intent's deadline.

The browser serializes inbound verification and outbound encryption/delivery so
async completion cannot reorder receive guards. It stores an intent before send.
Before a new mutation, the page obtains a fresh authenticated status and uses its
uptime for the five-second deadline. The boot and timer-session preconditions
remain those reviewed at the click (or before lock confirmation); a changed
session cancels the action. An existing retry never receives a fresh deadline.
Lost replies or reload never auto-resend a mutation. Encrypted lookup may resolve
the matching unknown intent without rewinding a newer dial. Unknown outcomes
trigger read-only lookups with a bounded backoff; normal in-flight requests show
“Sending” rather than an unknown-outcome warning. Explicit retries
retain original ID/preconditions/deadline and require fresh status. CORS permits
only the configured trusted HTTPS page origin with credentials omitted.

The partner page's random-duration option draws once with WebCrypto, in whole
seconds within the inclusive minute range, and sends the existing fixed-duration
`start` command. The selected value is persisted with the intent; retries do not
draw again. The local HTTP and Bluetooth random-start operations continue to
choose on the timer. No v1 wire operation changes are needed.

## 9. Firmware boundary and persistence

The separate scaffold has narrow clock, entropy, storage, timer and transport
interfaces. Enabled preference/credential hooks fail closed until provided;
local shutdown always stops the in-memory client and reports failed persistence.
Token table/key storage is designed for NVS. Names fit the NVS namespace limit,
but real flash layout, schema migration, failed-write recovery and wear still
need hardware validation. The result ring (`ResultRecord`) is RAM only.

Implemented for V2: pinned HTTPS
enrollment, WSS/TLS bound to the presented key and authenticated by the relay's
acknowledgement, mbedTLS ECDSA and hardware RNG, NVS
storage, the shared timer-authority adapter, local owner API,
OTA/factory-reset/transition hooks. The relay runs in its own FreeRTOS task;
timer commands are applied on the Arduino loop. The owner UI is in the timer
page (Settings → Remote sharing); relay management is unavailable in the Bluetooth app.
Event bodies carry the timer's own event name (`started`, `paused`,
`share_link_expired`, …; `snapshot_changed` only if none is known), its
`value_ms` and `via_share_link`. Status bodies carry `pause_allowed` and
`control_lock_allowed`, the owner's saved settings; a pause or lock-controls
command for a control that is off returns `control_disabled`, and the trusted
page hides it. Those settings cannot be changed through the relay. Status
bodies also add `share_link_limit_ms`
(`null`, or ms until the timer opens by itself because the share link holding
the session expires; a link can never start or extend a session past its own
expiry, reported as `maximum_duration_reached`). A Bluetooth app connection or
pairing window pauses the relay (see docs/RELAY_FIRMWARE.md).

## 10. Resource bounds

Default host configuration; these are limits, not a demonstrated scale rating:

| Resource | Default |
| --- | --- |
| Connected timers / pending hello connections | 100 / 32 |
| Hello authentication deadline | 5 seconds |
| HTTP body / WSS text frame | 8 KiB / 16 KiB |
| JSON maximum nesting | 16 |
| Browser sessions total / per token | 1,000 / 8 |
| Stored token rows per timer | 128 |
| SSE subscribers per timer / queued frames each | 16 / 64 |
| Pending commands per timer | 32 |
| Command wait for HTTP response | 5 seconds |
| Session / timer encrypted requests per five minutes | 300 / 3,000 |
| Session-create attempts per ASGI peer per minute | 10 |
| Owner sessions total / per owner | 1,000 / 8 |
| Owner inventory | 1,000 |
| Initial handshake deadline (accept + challenge + hello) | 5 seconds |
| Rate-limit keys per map | 4,096 |
| Cleanup cadence | 60 seconds |
| Device secure channels / pending / idle expiry | 8 / 2 / 120 seconds |
| C++ plaintext payload bound | 1,800 bytes |
| C++ cooperative minimum between new commands | 250ms |

The relay disconnects a slow SSE consumer rather than growing its queue.
Rate maps and transient routing expire. A global session capacity prevents
unbounded session rows. Reconnection and persistent timer/grant inventory
still require operator monitoring. Public ingress flood/slow-body mitigation
and measured larger-scale limits are outside the example's current evidence.

## 11. Validation and change policy

Run `./relay/tests/check.sh` for source checks and
`./relay/tests/relay_e2e.sh --browser` for the isolated HTTPS/WSS/SSE/browser/
restore flow. Reports stay under ignored `tmp/relay-tests/`.
[Testing and verification](VALIDATION.md) describes coverage and physical-device
verification limits.
The [operator runbook](../relay/OPERATOR.md) covers deployment and secrets.
Keep the firmware, server, clients, contract and rejection tests aligned.
