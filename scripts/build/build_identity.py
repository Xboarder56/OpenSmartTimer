Import("env")

import hashlib
import os


project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
environment = env.subst("$PIOENV")
output_path = os.path.join(build_dir, "generated_build_identity.h")

# This is a deterministic fingerprint of the source and target configuration
# that produced the image. The complete artifact SHA-256 remains in BUILD.json;
# embedding that digest in the image itself would be self-referential.
input_paths = [
    "platformio.ini",
    "scripts/build/build_identity.py",
    "scripts/build/build_paths.py",
    "scripts/build/embed_web_assets.py",
    "scripts/build/update_source.py",
    "scripts/build/relay_option.py",
]
for directory in ("include", "src", "web"):
    for root, _directories, files in os.walk(os.path.join(project_dir, directory)):
        for name in sorted(files):
            if name.endswith((".h", ".cpp", ".html", ".css", ".js")):
                input_paths.append(os.path.relpath(os.path.join(root, name), project_dir))

# The V2 SDK is a locally pinned build product. Its manifest and configuration
# affect the binary even when application source is unchanged.
if environment == "magbound_v2_esp32c3":
    for relative_path in (
        ".framework/esp32c3-libs/esp32c3/sdkconfig",
        "scripts/framework/verify_framework_esp32c3.py",
    ):
        if os.path.isfile(os.path.join(project_dir, relative_path)):
            input_paths.append(relative_path)

digest = hashlib.sha256()
digest.update(b"OpenSmartTimer-build-id-v1\0")
digest.update(environment.encode("utf-8") + b"\0")
# The relay switch can come from the environment, not only platformio.ini.
relay_option = env.get("OPENSMARTTIMER_RELAY", "")
if relay_option:
    digest.update(b"relay=" + relay_option.encode("utf-8") + b"\0")
for relative_path in sorted(set(input_paths)):
    digest.update(relative_path.replace(os.sep, "/").encode("utf-8") + b"\0")
    with open(os.path.join(project_dir, relative_path), "rb") as source:
        digest.update(source.read())
    digest.update(b"\0")
build_id = digest.hexdigest()[:24]

contents = (
    "#pragma once\n\n"
    f'#define SMART_TIMER_BUILD_ID "{build_id}"\n'
)
os.makedirs(build_dir, exist_ok=True)
existing = None
if os.path.isfile(output_path):
    with open(output_path, "r", encoding="utf-8") as current:
        existing = current.read()
if existing != contents:
    with open(output_path, "w", encoding="utf-8", newline="\n") as generated:
        generated.write(contents)

env.Append(CPPPATH=[build_dir])
env.Depends(
    "$BUILD_DIR/${PROGNAME}.elf",
    [os.path.join(project_dir, path) for path in sorted(set(input_paths))],
)
print(f"OpenSmartTimer build ID ({environment}): {build_id}")
