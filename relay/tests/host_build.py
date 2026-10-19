"""Shared host compiler settings; no dependency download or ESP build."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# The timer-side relay code lives in the firmware tree (src/relay,
# include/relay); these host tests build it with test doubles and OpenSSL.
COMMON = ['tests/firmware/HostCrypto.cpp', 'tests/firmware/OpenSslEcdsaP256.cpp',
          '../src/relay/CapabilityToken.cpp', '../src/relay/RelayJson.cpp', '../src/relay/RelayEnvelope.cpp',
          '../src/relay/RelayEncoding.cpp', '../src/relay/RelaySecure.cpp']


def openssl_flags():
    try:
        subprocess.run(['pkg-config', '--atleast-version=3.0', 'openssl'], check=True, capture_output=True)
        return shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    except (FileNotFoundError, subprocess.CalledProcessError):
        raise SystemExit('Host relay tests require OpenSSL 3 development headers and pkg-config. See relay/tests/README.md.') from None


# Root firmware headers/sources plus the root host Arduino/GPIO stubs, for
# adapters that must compile against the real TimerController (V2 target).
ROOT_FIRMWARE_FLAGS = ['-I../include', '-I../tests/host', '-DSMART_TIMER_HARDWARE_V2=1']
ROOT_FIRMWARE_SOURCES = ['../src/TimerController.cpp', '../src/TimerCommandAuthority.cpp']


def compile_host(name, sources, output, extra_flags=()):
    command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-I../include/relay', '-Itests/firmware',
               *extra_flags, f'tests/firmware/{name}.cpp', *sources,
               *openssl_flags(), '-o', str(output)]
    subprocess.run(command, cwd=ROOT, check=True)
