# Hardware qualification records

The JSON records in this directory describe reported checks for exact firmware images. Each record identifies the version, source, hardware target, embedded build ID, image size, CRC32 and SHA-256. It contains no firmware binaries, device credentials, network identities, live share links or raw captures.

A record is the maintainer's own report of bench checks on one unit of each model, with no person or load attached. It is not a certification, an independent test, or evidence that any other unit, lock or installation will release, and it is not proof that CI observed physical hardware. Pending and failed checks remain explicit. Firmware images and accompanying manifests are distributed as release assets rather than committed into this directory.

## Which images to qualify

Qualify the images CI built from the release source: push it, download the `opensmarttimer-application-images` artifact from that CI run, install those exact files and record their hashes. A local V1 build matches the CI image; a local V2 build matches only on Linux x86-64 (see [Reproducibility](../BUILDING.md#reproducibility)), so compare SHA-256 values before relying on a local build. Records are not build inputs, so committing one does not change the images.

## Validate a record

To validate a partial record's structure without claiming completed qualification:

```sh
python3 scripts/release/verify_qualification.py --allow-pending
```

To require completed qualification and match a downloaded bundle:

```sh
python3 scripts/release/verify_qualification.py \
  --record /path/to/qualification.json \
  --bundle /path/to/firmware-bundle
```

Strict validation rejects pending/failed checks, insufficient soak duration and mismatched source or artifact metadata. `scripts/check_release.sh` applies it to stable versions; a prerelease (a version with a `-` part, such as `1.0.0-rc.2`) may be published with pending checks, but its record must still name the exact images, its physical emergency check (`physical_safety`) must have passed on both models, no check may be marked failed, and its changelog must say the checks are partial. The release workflow enforces this when the tag is pushed, and its draft job refuses CI images that differ in any byte from the recorded ones (`--images`). An earlier passed record cannot qualify a changed image. Bundles with `source: null` do not establish release source provenance.

See [Testing and verification](../VALIDATION.md) for the distinction between source checks, image validation and physical-device checks, and [Building](../BUILDING.md#release-artifacts) for distributed artifact formats.
