# Security

This policy describes OpenSmartTimer's control and deployment trust boundaries.
Read [Safety](SAFETY.md) for physical safety limits and
[Testing and verification](docs/VALIDATION.md) for the scope of automated and
physical-device checks.

## Trust boundaries

- **Local HTTP:** no application login, TLS listener or API token. Anyone
  with network access can control the timer. Use a trusted or isolated LAN;
  an AP password is not authentication on the router LAN. To keep websites
  out (DNS rebinding), the API answers only requests addressed to an IP
  address, a single-label name or a private-suffix name such as `.local`,
  `.lan` or `.home.arpa`; other names get `403 host_rejected`. Requests over
  the timer's own setup access point are exempt so setup keeps working.
- **Bluetooth:** V2 requires encrypted bonding, application authentication
  and physical approval for a new controller. Any approved app can administer
  the other approvals. Keep its stored controller key private.
- **Remote sharing (experimental):** off by default, and neither the protocol
  nor its implementations have had an independent security review. The relay
  carries opaque authenticated encryption and
  holds no link token or traffic key. The trusted partner page is hosted at an
  independently controlled HTTPS origin saved after local owner review and
  physical confirmation. Relay responses cannot change it. Authenticated
  ephemeral ECDH and strict sequence guards are designed to prevent a
  compromised relay from forging controls or replaying authenticated packets. The trusted page/device,
  live link custody, metadata and relay availability remain trust boundaries.
- **Timer authority:** each transport uses the shared controller rules.
  Remote commands are designed not to override emergency input, sealed
  controls, duration limits or the current boot/session checks. Grant expiry is enforced locally,
  including when the relay is unreachable.
- **Relay TLS:** the timer does not validate certificates against a certificate
  authority, hostname or expiry date. Enrollment is authenticated by the server
  key pin in the setup code, so obtain that code from a trusted operator
  channel. After enrollment every connection is authenticated in both
  directions with the timer's enrollment secret and bound to the TLS key the
  timer was shown; a server that lacks the secret, or that presents another
  key, is refused. Certificate renewals need nothing from owners. The relay
  learns the key it serves from `OSTRELAY_SPKI_PIN` or, when that is empty, by
  reading its own public certificate with ordinary certificate validation.
- **Updates:** GitHub installation requires a physical one-use OTA arm and
  signed release metadata plus image SHA-256 verification. Protect the release
  signing key and verify the exact model and published artifact. When a V2
  downloads a release itself, HTTPS does not validate the server certificate:
  the signature and SHA-256 are the only trust, as for a browser download.

## Relay deployment

Follow [the operator guide](relay/OPERATOR.md). Use HTTPS, a specific trusted
proxy list, one worker/replica and the localhost-only admin surface. Keep
the database, backups, TLS keys, owner setup codes, browser credentials and
operator secrets private. The database contains enrollment secrets in
plaintext; partner sessions store only public routing metadata. The cookie
secret protects owner/OIDC sessions, not partner traffic. Production trusted-page
hosting credentials, files and TLS keys must be inaccessible to the relay host;
a second container on the same host does not protect against host compromise.
Treat all database backups as credential-bearing material; schema migration
does not sanitize existing backups.

HTTP bodies, connection/handshake slots, sessions, queues and command history
are bounded. Initial WebSocket authentication has one deadline covering
acceptance, challenge writing and hello reading. Browser security headers are
enforced by the application as well as the recommended proxy. These controls
and the regression tests are defense in depth, not an independent security
audit or a guarantee against all denial of service.

## Reporting

Use GitHub private vulnerability reporting on the project repository when
enabled. Do not put live share links, controller/enrollment keys, private
signing keys, database copies or personal captures in public issues. Provide
the affected version/model and a minimal reproduction using disposable test
credentials. Ordinary reproducible bugs can use the public issue tracker.
