# GitHub firmware updates

Firmware updates use **browser-assisted checks on both V1 and V2**. The timer
never checks for updates itself, never installs automatically and writes no
flash for a check. The browser downloads the feed and application image over
HTTPS, then sends the image through the existing local HTTP or authenticated
Bluetooth OTA transport. The relay companion has no firmware update authority.

By default, the timer page and the Bluetooth app also check the feed when they
open: at most once a day per browser, only while the timer is open, and
silently on any error. A newer release shows a dismissible **Update available**
notice. **View update** runs the normal check below; **Later** hides that
version until a newer one appears. The **Check when this page opens** switch in
the Firmware section turns this off; the choice is stored in the browser only.

In **Device settings → Firmware**, choose **Check for updates**. The section also
shows the installed version and whether the Reset-button allowance is active
(**Updates are locked** or **Ready to update**). If no newer published version exists,
the page shows **No updates available**. Otherwise the existing confirmation
dialog shows the version and release notes with **Cancel** and **Install**.
A version with a prerelease part, such as `1.1.0-rc.1`, is headed **Pre-release
update** and says it may be less tested than a stable release.
Cancel downloads no image. Install rechecks the timer before downloading and
uses local HTTP or authenticated Bluetooth OTA with device-side signature
verification. Installation needs
an open timer and its five-minute physical Reset-button arm. Keep the browser
open and the timer powered during upload. The browser needs Internet access,
including in AP or Bluetooth-only mode. The local `.bin` uploader remains.

## Public configuration and migration

`include/ReleaseUpdateConfig.h` is the single public configuration location:
repository, derived GitHub Pages URL, and P-256 public signing key. There is
no repository literal in updater logic. To change it:

```sh
python3 scripts/release/update_feed.py configure \
  --repository xboarder56/OpenSmartTimer --public-key <130-character-public-key>
```

Keep the signing key when simply moving repositories. For installed timers,
keep the old Pages feed and image mirror available long enough to offer a
signed bridge firmware containing the new URL. Timers cannot discover an
arbitrary new repository if their old feed disappears. Key rotation similarly
needs a bridge signed by the old key that embeds the new public key. Offline
local upload provides physical recovery if hosting or the key is lost.
The update source is build configuration, not a remotely writable setting.
The firmware and Pages builds also bind their browser connection security
policies to that configured Pages path. The source HTML's GitHub Pages wildcard
is a development preview marker, replaced in both shipped interfaces.

## Release signing setup

Install `requirements-dev.txt` into the release/test Python environment. Host
signature tests also use `pkg-config` and OpenSSL development headers.

Generate a key only for a new publisher, not for each release:

```sh
python3 scripts/release/update_feed.py generate-key --private-key /private/path/release-key.pem
```

The command refuses an existing file and writes mode `0600`. Back up the
private key outside the repository. Configure the printed public key in the
header, then set the private PEM as the repository's `UPDATE_SIGNING_KEY`
GitHub Actions secret. Never put it in source, issues or public artifacts.

Keep a durable private backup outside the worktree before publishing the
initial release. Ignored local files and artifact allowlists do not replace
that backup or a review of the actual Git staging area and history.

Set GitHub Pages to deploy through GitHub Actions. A `v*` tag validates version,
changelog, public update configuration and exact CI application images. The
draft workflow signs their descriptors with the matching Actions secret and
attaches `updates.json` beside images/checksums/metadata. Missing/different keys,
wrong repositories or changed images fail the job. Drafts never enter feeds.

Publishing a release stages signed images and bounded feeds:

```text
updates/magbound_v1_esp8266.json
updates/magbound_v2_esp32c3.json
updates/images/OpenSmartTimer-<version>-<model>.bin
```

There is one feed per timer model. Published GitHub releases with a valid
`v<SemVer>` tag participate in the same version ordering, including tagged
prereleases; there are no RC/beta channels or opt-in selectors. Drafts and
standalone Git tags are excluded. The newest version supplies both signed
images and the matching companion source. An installed equal/newer version
reports no updates. For a missing initial feed (HTTP 404 or GitHub's CORS-blocked
Pages response), the public GitHub release API must confirm an empty release
list or return 404 (no public repository/releases) before reporting no updates.
Checks are anonymous; private GitHub credentials are never requested or sent.
Published public releases with an unavailable feed,
unconfirmed network failures, malformed feeds and signature failures remain
errors. Release notes are bounded to 8 KiB and shown with simple formatting
(headings, bullets, bold, code and `https://` links) built from text, never as
HTML. They are informational rather than signed firmware metadata. Feeds and images are excluded from the PWA offline shell cache.

## Integrity and device behavior

Each descriptor signs this exact ASCII payload, including its final newline:

```text
OpenSmartTimer-Update-v1
<target>
<version>
<decimal image size>
<8 lowercase CRC32 hex characters>
<64 lowercase SHA-256 hex characters>
```

ECDSA P-256/SHA-256 signatures use 64 raw bytes (`r || s`), encoded as 128
lowercase hex characters. Targets are exact PlatformIO names; versions have
at most 47 characters. URLs derive from the configured Pages source and
validated model-specific filename.

The timer verifies the signature before `Update.begin()` and streaming SHA-256
before writing the held-back final chunk and committing. HTTP preparation
binds the next upload to the signed hash during its preparation window. Size,
CRC32, chip header, product marker, physical arm, open timer, scan, relay
maintenance and competing-upload guards remain. The unverified recovery
override does not bypass a supplied signature or hash.

V1 borrows the SDK's 6 KiB crypto stack only for verification, with a heap/block
guard, and releases it before starting the writer. It allocates no TLS client.
V2 uses the pinned SDK's mbedTLS verifier. Neither buffers an image in device
RAM. HTTPS browsers additionally verify signature and SHA-256 with WebCrypto;
plain HTTP timer pages rely on the device verifier.

## Verification

Software checks cover signature mutations, wrong models, SemVer, oversized,
incomplete/corrupt downloads, and the actual OTA writer refusing to commit a
changed hash or incomplete/aborted image. Final images must pass both build
budgets and the V1/V2 relay boundary.

Source and image checks do not establish behavior during interrupted transfer,
power loss or transport contention on physical hardware. Verify the exact
image and applicable HTTP/Bluetooth transport on the actual device; see
[Testing and verification](VALIDATION.md).
