"""Test-only bridge to a real C++ RelayClient with deterministic host doubles."""
import json
import select
import subprocess


class CppClient:
    def __init__(self, binary):
        self.process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1)

    def call(self, action, **fields):
        self.process.stdin.write(json.dumps({'action': action, **fields}, ensure_ascii=False) + '\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 10)[0], 'C++ response timeout'
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError('C++ bridge stopped: ' + self.process.stderr.read())
        return json.loads(line)

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait()
        self.process.stdout.close(); self.process.stderr.close()


def write_browser_fixture(binary, path):
    """Generate synthetic C++ envelopes for Chromium; temp file never deployed."""
    client = CppClient(binary)
    try:
        info = client.call('inspect')
        client.call('connect')
        client.call('frame', frame={'type':'challenge','server_nonce':'AAECAwQFBgcICQoLDA0ODw'})
        client.call('ack')
        from secure import BrowserOpen
        opened=BrowserOpen(info['wire'])
        accepted=client.call('frame',frame=opened.frame)['frames'][0]
        channel=opened.accept(accepted)
        frames=client.call('frame',frame=channel.encrypt({'type':'sync','cap_token':info['wire']},'fixture-status'))['frames']
        frames += client.call('event')['frames']
        frames += client.call('frame',frame=channel.encrypt({
            'type':'command','cap_token':info['wire'],'op':'adjust','op_id':'browser-cpp',
            'if_boot_id':'aabbccdd','if_session_id':'session-1','expiry_uptime_ms':6000,'params':{'delta_ms':1000}},'browser-cpp'))['frames']
        frames=[channel.decrypt(f) for f in frames if f['type']=='e2e']
        path.write_text(json.dumps({'public_key':info['public_key'],'envelopes':frames}))
    finally:
        client.close()
