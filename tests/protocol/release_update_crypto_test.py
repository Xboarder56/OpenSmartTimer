"""Run real release signatures and streaming SHA-256 against the C++ updater."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('update_feed', ROOT / 'scripts/release/update_feed.py')
feed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feed)


def openssl_flags():
    return subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True).split()


def compile_run(source, directory, name='release-crypto'):
    cpp = Path(directory) / (name + '.cpp')
    cpp.write_text(source)
    binary = cpp.with_suffix('')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                    '-I' + str(ROOT / 'include'), str(ROOT / 'src/ReleaseUpdate.cpp'), str(cpp), '-o', str(binary), *openssl_flags()], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    key = ec.derive_private_key(1, ec.SECP256R1())  # Public test fixture, never a release key.
    record = {'target': 'magbound_v1_esp8266', 'release_version': '1.0.0-rc.10', 'image_size': 24,
              'image_crc32': '10203040', 'image_sha256': __import__('hashlib').sha256(b'abc').hexdigest()}
    r, s = utils.decode_dss_signature(key.sign(feed.canonical(record), ec.ECDSA(hashes.SHA256())))
    signature = (r.to_bytes(32, 'big') + s.to_bytes(32, 'big')).hex()
    source = r'''
#include "ReleaseUpdate.h"
#include <cassert>
#include <cstring>
int main() {
  using namespace ReleaseUpdate;
  Descriptor d;
  strcpy(d.version, "1.0.0-rc.10"); d.size = 24; d.crc32 = 0x10203040;
  assert(decodeHex("SHA", d.sha256, 32));
  assert(decodeHex("SIGNATURE", d.signature, 64));
  assert(verify(d, "magbound_v1_esp8266", "KEY"));
  assert(!verify(d, "magbound_v2_esp32c3", "KEY"));
  assert(!verify(d, "not-a-target", "KEY"));
  d.signature[0] ^= 1; assert(!verify(d, "magbound_v1_esp8266", "KEY")); d.signature[0] ^= 1;
  d.sha256[0] ^= 1; assert(!verify(d, "magbound_v1_esp8266", "KEY")); d.sha256[0] ^= 1;
  ++d.size; assert(!verify(d, "magbound_v1_esp8266", "KEY")); --d.size;
  ++d.crc32; assert(!verify(d, "magbound_v1_esp8266", "KEY")); --d.crc32;
  strcpy(d.version, "1.0.0-rc.11"); assert(!verify(d, "magbound_v1_esp8266", "KEY"));
  assert(!verify(d, "magbound_v1_esp8266", ""));
  ImageHash hash;
  hash.begin(); hash.append(reinterpret_cast<const uint8_t*>("a"), 1); hash.append(reinterpret_cast<const uint8_t*>("bc"), 2);
  assert(hash.matches(d.sha256));
  hash.begin(); hash.append(reinterpret_cast<const uint8_t*>("abd"), 3); assert(!hash.matches(d.sha256));
  assert(newer("1.0.0-rc.10", "1.0.0-rc.2"));
  assert(newer("1.0.0", "1.0.0-rc.10"));
  assert(!newer("1.0.0-rc.10", "1.0.0"));
  assert(!newer("1.0.0", "1.0.0"));
  assert(newer("1.1.0-alpha", "1.0.0"));
  assert(newer("1.0.0-rc.1.more", "1.0.0-rc.1"));
  assert(!validVersion("1.0.0-rc.01"));
  assert(!validVersion("4294967296.0.0"));
  assert(!validVersion("1.0.0\n"));
  assert(!validVersion("1.0.0-"));
  assert(!validVersion("1.0.0-a..b"));
}
'''.replace('SIGNATURE', signature).replace('SHA', record['image_sha256']).replace('KEY', feed.public_hex(key.public_key()))
    with tempfile.TemporaryDirectory() as directory:
        compile_run(source, directory)
    print('Real signed release verification, mutation rejection, SHA-256 streaming and SemVer passed')
