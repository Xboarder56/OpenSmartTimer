Import("env")

"""Keep the build machine's paths out of the firmware and its symbols.

GCC records the compile directory and its own header directories in the debug
information, and expands __FILE__ to the path it was given. The published
.elf symbol files would carry the builder's home and checkout paths, and the
V2 image embeds the ELF's SHA-256, so the image bytes would depend on where it
was built. Each prefix below is rewritten to a fixed name instead.
GCC uses the last matching map, so the most specific prefix comes last.
package_firmware.py refuses to stage an image or symbol file that still
contains one of these paths.
"""

import os

prefixes = [
    (os.path.expanduser("~"), "~"),
    (env.subst("$PROJECT_DIR"), "."),
    (env.subst("$PROJECT_CORE_DIR"), "/platformio"),
    (env.subst("$PROJECT_PACKAGES_DIR"), "/platformio/packages"),
]
flags = []
for path, name in prefixes:
    if path and os.path.isabs(path):
        flags.append(f"-ffile-prefix-map={os.path.realpath(path)}={name}")
        if os.path.realpath(path) != os.path.normpath(path):
            flags.append(f"-ffile-prefix-map={os.path.normpath(path)}={name}")
# ASPPFLAGS: preprocessed .S files go through the GCC driver; plain .s files
# go straight to the assembler, which takes no such flag.
env.Append(CCFLAGS=flags, ASPPFLAGS=flags)
