"""Bluetooth bench for V2 (ost-ble/1), run with the relay enrolled.

Private local test, never prints keys. No Wi-Fi, boot or firmware writes;
existing application approvals are retained for repeatable validation.
"""
import asyncio
import hashlib
import hmac
import json
import secrets
import statistics
import struct
import sys
import time
import urllib.request
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlsplit

import importlib.util
_spec = importlib.util.spec_from_file_location('bleak_control', Path(__file__).resolve().parents[3] / 'examples/bleak_control.py')
ble = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ble)
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools'))
from validate_bench import Stream

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_config import OUT as OUT_DIR, need, timer
URL = timer()
REPORT = OUT_DIR / 'bluetooth-bench.json'
KEY = Path(need('OST_BLE_KEY'))
MGMT_REQ = '4b546245-44a0-4cd0-9eed-8be6c51195ba'
MGMT_RSP = '93b52e61-25f4-46a6-a597-5724013f60f9'


class Bench:
    def __init__(self):
        self.report = {'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                       'checks': [], 'commands': [], 'management': [], 'diagnostics': [],
                       'events': [], 'failures': [], 'status_notifications': 0}
        self.counter = 0
        self.request_id = secrets.randbelow(40000) + 1000
        self.results = asyncio.Queue()
        self.ready = asyncio.Queue()
        self.streams = []
        self.own_session = None

    def save(self):
        REPORT.write_text(json.dumps(self.report, indent=2) + '\n')

    def check(self, name, condition=True):
        assert condition, name
        self.report['checks'].append(name)
        self.save()
        print('PASS:', name, flush=True)

    def next_id(self):
        self.request_id = self.request_id % 65535 + 1
        return self.request_id

    async def http(self, path, body=None):
        def fetch():
            request = urllib.request.Request(URL + path,
                data=json.dumps(body).encode() if body is not None else None,
                headers={'Content-Type': 'application/json'} if body is not None else {})
            with urllib.request.urlopen(request, timeout=5) as response:
                return json.load(response)
        return await asyncio.to_thread(fetch)

    async def live(self):
        return ble.decode_live(bytes(await self.client.read_gatt_char(ble.LIVE_STATUS_UUID)))

    async def command(self, action, expected='applied', request_id=None,
                      session=None, boot=None, **values):
        live = await self.live()
        self.counter += 1
        request_id = request_id or self.next_id()
        frame = ble.build_command(action, request_id, self.boot if boot is None else boot,
            live['session_sequence'] if session is None else session, self.counter,
            SimpleNamespace(**values), self.session_key)
        start = time.monotonic()
        await self.client.write_gatt_char(ble.CONTROL_COMMAND_UUID, frame, response=True)
        while True:
            result = await asyncio.wait_for(self.results.get(), timeout=4)
            if result['request_id'] == request_id:
                break
        assert result['opcode'] == ble.OPCODES[action] and result['boot_id'] == self.boot
        self.report['commands'].append({**result, 'action': action,
                                       'latency_ms': round((time.monotonic()-start)*1000, 2)})
        self.save()
        assert result['result'] == expected, result
        if action in ('start', 'random') and expected == 'applied':
            self.own_session = result['session_sequence']
        return result

    async def management(self, opcode, body=None, expected=200):
        original_id = self.next_id()
        current_op, current_id, current_body = opcode, original_id, body or {}
        accumulated = bytearray()
        began = time.monotonic()
        segments = 0
        while True:
            self.counter += 1
            encoded = json.dumps(current_body, separators=(',', ':')).encode()
            prefix = struct.pack('<BBHIIHB', 1, current_op, current_id, self.boot,
                                 self.counter, len(encoded), 0) + encoded
            frame = prefix + hmac.new(self.session_key, b'OST-MGMT-v1' + prefix,
                                     hashlib.sha256).digest()[:16]
            # Exercise fragmentation even on the negotiated larger MTU.
            for index, offset in enumerate(range(0, len(frame), 18)):
                fragment = bytes((index, int(offset + 18 >= len(frame)))) + frame[offset:offset+18]
                await self.client.write_gatt_char(MGMT_REQ, fragment, response=True)
            while True:
                ready = await asyncio.wait_for(self.ready.get(), timeout=5)
                if ready[2] == original_id:
                    break
            raw = bytes(await self.client.read_gatt_char(MGMT_RSP))
            assert len(raw) >= 28
            version, response_op, rid, code, total, offset, size = struct.unpack_from('<BBHHHHH', raw)
            assert version == 1 and response_op == opcode and rid == original_id
            assert total <= 8192 and offset == len(accumulated), (hex(opcode), code, total, offset)
            assert len(raw) == 12 + size + 16 and size <= 484
            mac = hmac.new(self.session_key, b'OST-MGMT-RSP-v1' +
                           struct.pack('<I', self.counter) + raw[:-16], hashlib.sha256).digest()[:16]
            assert hmac.compare_digest(mac, raw[-16:]), 'Management response MAC mismatch'
            assert ready == (1, opcode, original_id, code, total)
            accumulated += raw[12:-16]
            segments += 1
            if len(accumulated) == total:
                break
            assert size > 0 and len(accumulated) < total
            current_op, current_id = 0x3f, self.next_id()
            current_body = {'request_id': original_id, 'offset': len(accumulated)}
        result = json.loads(accumulated)
        assert code == expected, (hex(opcode), code, expected, result)
        self.report['management'].append({'opcode': hex(opcode), 'status': code,
            'bytes': total, 'segments': segments, 'latency_ms': round((time.monotonic()-began)*1000, 2)})
        self.save()
        return result

    async def run(self):
        initial = await self.http('/api/v1/status')
        assert initial['state'] == 'open' and not initial['emergency_input_active']
        self.report['identity'] = {k: initial[k] for k in ('device_id', 'boot_id', 'build_id')}
        target = await ble.find_timer('OST-' + initial['device_id'][-6:], None)
        key = ble.read_key_file(KEY)
        async with ble.BleakClient(target) as self.client:
            try:
                info = json.loads(bytes(await self.client.read_gatt_char(ble.PROTOCOL_INFO_UUID)))
                assert info['protocol'] == 'ost-ble/1' and info['boot_id'] == initial['boot_id']
                self.check('Public protocol identity matches REST')
                # Encrypted/bonded but not application-authenticated: management
                # response must be refused promptly (ATT 0x83), without data.
                await ble.request_link_pairing(self.client)
                began = time.monotonic()
                try:
                    data = await asyncio.wait_for(self.client.read_gatt_char(MGMT_RSP), timeout=8)
                    self.report['unauthenticated_read'] = {'returned_bytes': len(data)}
                    raise AssertionError('management response readable before app authentication')
                except asyncio.TimeoutError:
                    self.report['unauthenticated_read'] = {'result': 'timeout', 'seconds': 8}
                    raise AssertionError('unauthenticated management read stalled instead of rejecting')
                except AssertionError:
                    raise
                except Exception as error:
                    self.report['unauthenticated_read'] = {'rejected': type(error).__name__, 'detail': str(error)[:200],
                                                          'seconds': round(time.monotonic() - began, 3)}
                self.check('Unauthenticated management read rejected promptly without data')
                self.boot, self.session_key = await ble.authenticate(self.client, key)
                self.check('Stored bench key authenticates on new BLE connection')
                await self.client.start_notify(ble.CONTROL_RESULT_UUID,
                    lambda _, data: self.results.put_nowait(ble.decode_result(bytes(data))))
                await self.client.start_notify(MGMT_RSP,
                    lambda _, data: self.ready.put_nowait(struct.unpack('<BBHHH', bytes(data))))
                def on_status(_, data):
                    ble.decode_live(bytes(data))
                    self.report['status_notifications'] += 1
                def on_event(_, data):
                    self.report['events'].append(ble.decode_event(bytes(data)))
                await self.client.start_notify(ble.LIVE_STATUS_UUID, on_status)
                await self.client.start_notify(ble.EVENT_UUID, on_event)
                await asyncio.sleep(2)
                for opcode, path in [(0x40, '/api/v1/wifi/status'), (0x41, '/api/v1/wifi/ap'),
                      (0x42, '/api/v1/device/name'), (0x43, '/api/v1/bluetooth'),
                      (0x44, '/api/v1/system/diagnostics'), (0x45, '/api/v1/events'),
                      (0x46, '/api/v1/capabilities'), (0x47, '/api/v1/status')]:
                    result = await self.management(opcode)
                    rest = await self.http(path)
                    assert set(result) == set(rest), (hex(opcode), set(result) ^ set(rest))
                    if opcode == 0x43:
                        assert result['active_app_id'] > 0
                        assert result['active_app_id'] == rest['active_app_id']
                        assert result['approved_app_count'] == rest['approved_app_count']
                    if opcode == 0x44:
                        self.report['diagnostics'].append(result)
                    self.check(f'BLE management {path}: authenticated segmented JSON, REST field parity')
                await self.management(0x53, {'unknown_field': True}, expected=400)
                self.check('Invalid management settings rejected without configuration change')
                assert not (await self.http('/api/v1/status'))['ota_armed']
                await self.management(0x70, {'image_size': 4096, 'image_crc32': '00000000'}, expected=403)
                self.check('Unarmed BLE OTA begin rejected; no firmware written')
                await self.command('start', duration_ms=0, expected='invalid_duration')
                await self.command('start', duration_ms=86400001, expected='invalid_duration')
                await self.command('start', duration_ms=5000, boot=self.boot ^ 1, expected='stale_boot')
                self.check('Invalid durations and stale boot rejected over BLE')
                start = await self.command('start', duration_ms=15000)
                dup = await self.command('start', duration_ms=15000, request_id=start['request_id'])
                assert dup['duplicate'] and dup['session_sequence'] == start['session_sequence']
                await self.command('start', duration_ms=14000, request_id=start['request_id'], expected='operation_conflict')
                self.check('BLE start retry deduplicated and conflicting request ID rejected')
                paused = await self.command('pause')
                await asyncio.sleep(0.5)
                assert (await self.live())['remaining_ms'] == paused['remaining_ms']
                self.check('BLE pause holds remaining time')
                adjusted = await self.command('adjust', delta_ms=1000)
                assert adjusted['remaining_ms'] == paused['remaining_ms'] + 1000
                retry = await self.command('adjust', delta_ms=1000, request_id=adjusted['request_id'])
                assert retry['duplicate'] and retry['remaining_ms'] == adjusted['remaining_ms']
                await self.command('adjust', delta_ms=86400000, expected='maximum_duration_reached')
                await self.command('resume', session=start['session_sequence'] + 1, expected='stale_session')
                await self.command('resume')
                await self.command('release')
                self.check('Atomic BLE adjustment, retry, duration/session guards, resume and release')
                await self.command('identify')
                await self.command('random', min_ms=500, max_ms=900)
                await asyncio.sleep(1.2)
                assert (await self.live())['state'] == 'open'
                self.check('BLE identify and random-duration natural expiry')
                await self.command('start', duration_ms=3000)
                await self.command('lock-controls')
                await self.command('release', expected='controls_locked')
                await asyncio.sleep(3.3)
                assert (await self.live())['state'] == 'open'
                self.check('Sealed BLE session rejects release and still expires')
                for _ in range(2):
                    self.streams.append(await asyncio.to_thread(Stream, urlsplit(URL)))
                for cycle in range(12):
                    await self.command('start', duration_ms=5000)
                    await self.command('pause')
                    await self.command('adjust', delta_ms=500)
                    await self.command('resume')
                    await self.command('release')
                    status = await self.http('/api/v1/status')
                    assert status['state'] == 'open' and status['boot_id'] == initial['boot_id']
                    self.report['diagnostics'].append(await self.http('/api/v1/system/diagnostics'))
                    await asyncio.sleep(1)
                self.check('12 BLE control cycles coexist with two SSE streams and REST while the relay yields to Bluetooth')
                assert self.report['status_notifications'] > 20 and self.report['events']
                self.check('BLE status/event notifications delivered during mutations')
                self.report['sse'] = [{'frames': len(s.frames), 'error': s.error} for s in self.streams]
                assert all(len(s.frames) > 10 and not s.error for s in self.streams)
                self.check('Both SSE streams received BLE-driven state changes')
            finally:
                for stream in self.streams:
                    await asyncio.to_thread(stream.close)
                status = await self.http('/api/v1/status')
                if status['boot_id'] == initial['boot_id'] and status['state'] != 'open':
                    if not status['sealed'] and status['session_id'].endswith('-' + str(self.own_session)):
                        await self.http('/api/v1/timer/release', {'if_boot_id': initial['boot_id'],
                                                              'if_session_id': status['session_id']})
                    else:
                        await asyncio.sleep(4)
                final = await self.http('/api/v1/status')
                self.report['final_state'] = final['state']
                self.save()
                assert final['state'] == 'open', 'Manual bench release required'
        await asyncio.sleep(2)
        final_diag = await self.http('/api/v1/system/diagnostics')
        self.report['diagnostics'].append(final_diag)
        self.check('SSE and BLE connections recover after disconnect',
                   final_diag['sse_clients'] <= 1 and final_diag['bluetooth_connections'] == 0)
        pass  # coexistence run: keep the bench approval for the final RC bench
        times = [v['latency_ms'] for v in self.report['commands']]
        self.report['command_latency_ms'] = {'median': statistics.median(times), 'max': max(times)}
        self.save()


async def main():
    bench = Bench()
    try:
        await bench.run()
    except Exception as error:
        bench.report['failures'].append(str(error))
        bench.save()
        raise


if __name__ == '__main__':
    asyncio.run(main())
