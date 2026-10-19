"""Real firmware pin callback contract; OpenSSL-backed host SDK doubles.

Hardware validation remains responsible for ESP-IDF/mBed TLS parsing/handshakes.
The large RSA case regresses the former 400-byte reserialization limit.
"""
import datetime
import hashlib
import subprocess

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import NameOID
import pytest

from host_build import compile_host


@pytest.fixture(scope='module')
def verifier(tmp_path_factory):
    binary = tmp_path_factory.mktemp('tls-pin')/'esp_tls_pin_test'
    compile_host('esp_tls_pin_test', ['../src/relay/EspTls.cpp'], binary,
        ['-DESP_PLATFORM', '-Itests/firmware/tls_stubs', '-Itests/firmware/esp_stubs'])
    return binary


@pytest.mark.parametrize('kind', ['p256', 'rsa2048', 'rsa4096'])
def test_certificate_leaf_pin_with_supported_key_sizes(verifier, tmp_path, kind):
    key = (ec.generate_private_key(ec.SECP256R1()) if kind == 'p256' else
        rsa.generate_private_key(public_exponent=65537, key_size=int(kind[3:])))
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'pin-test.invalid')])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
        .public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now-datetime.timedelta(minutes=1))
        .not_valid_after(now+datetime.timedelta(hours=1)).sign(key, hashes.SHA256()))
    public = key.public_key().public_bytes(serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    if kind == 'rsa4096':
        assert len(public) > 400
    path = tmp_path/'certificate.der'
    path.write_bytes(cert.public_bytes(serialization.Encoding.DER))
    subprocess.run([str(verifier), str(path), hashlib.sha256(public).hexdigest()], check=True)
