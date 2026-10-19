"""The relay enforces the pre-public wire shape; endpoints verify identity."""
import pytest

from ostrelay.e2e import validate_frame
from ostrelay.wire import b64encode


def accepted():
    return dict(type='e2e_accept', upstream_seq=1, fp_hash=b64encode(bytes(16)),
        client_id=b64encode(bytes(16)), server_id=b64encode(bytes(16)),
        server_key=b64encode(b'\x04' + bytes(64)), tag=b64encode(bytes(32)),
        sig=b64encode(bytes(64)))


def test_accept_requires_identity_proof_without_claiming_to_verify_it():
    frame = accepted()
    # A structurally valid proof is opaque to the routing service. The endpoint
    # tests reject this zero signature using the capability's identity anchor.
    assert validate_frame(frame, upstream=True) is frame
    del frame['sig']
    with pytest.raises(ValueError): validate_frame(frame, upstream=True)


@pytest.mark.parametrize('signature', [None, '', b64encode(bytes(63)),
    b64encode(bytes(65)), b64encode(bytes(64)) + '='])
def test_accept_rejects_missing_or_noncanonical_signature_bytes(signature):
    with pytest.raises(ValueError):
        validate_frame({**accepted(), 'sig': signature}, upstream=True)


def test_accept_cannot_add_a_replacement_verify_key_or_come_from_the_browser():
    with pytest.raises(ValueError):
        validate_frame({**accepted(), 'vk': b64encode(bytes(33))}, upstream=True)
    with pytest.raises(ValueError): validate_frame(accepted(), upstream=False)
