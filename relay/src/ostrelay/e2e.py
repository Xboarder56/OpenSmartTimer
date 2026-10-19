"""Wire validation and opaque routing only. No grant or traffic keys live here."""
from .wire import b64decode, integer
import re


def validate_frame(frame, *, upstream=False):
    kind=frame.get('type')
    fields={'type','fp_hash','client_id'}
    if kind=='e2e_open':
        fields|={'boot_id','client_key','tag'}
        b64decode(frame.get('client_key'),65); b64decode(frame.get('tag'),32)
        if not re.fullmatch(r'[0-9a-f]{1,16}',frame.get('boot_id','')): raise ValueError('bad boot')
    elif kind=='e2e_accept' and upstream:
        fields|={'server_id','server_key','tag','sig'}
        b64decode(frame.get('server_id'),16); b64decode(frame.get('server_key'),65);b64decode(frame.get('tag'),32);b64decode(frame.get('sig'),64)
    elif kind=='e2e':
        fields|={'server_id','seq','request_id','ciphertext'}
        b64decode(frame.get('server_id'),16)
        if not integer(frame.get('seq'),1): raise ValueError('bad sequence')
        if not re.fullmatch(r'[!-~]{0,64}',frame.get('request_id','')): raise ValueError('bad request')
        cipher=b64decode(frame.get('ciphertext'))
        if not 16<=len(cipher)<=1816: raise ValueError('bad ciphertext size')
    else: raise ValueError('encrypted frame required')
    if upstream:
        fields.add('upstream_seq')
        if not integer(frame.get('upstream_seq'),1): raise ValueError('bad upstream')
    if set(frame)!=fields: raise ValueError('unexpected fields')
    b64decode(frame.get('fp_hash'),16);b64decode(frame.get('client_id'),16)
    return frame
