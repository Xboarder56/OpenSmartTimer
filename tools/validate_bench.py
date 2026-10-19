#!/usr/bin/env python3
"""Opt-in unloaded-bench REST/SSE exercise. Never changes Wi-Fi, keys or firmware.

Starts short timers, pauses/resumes, adjusts, releases, and seals a 3-second
session. Network/physical failure can defeat cleanup; never run on a person.
"""
import argparse
import http.client
import json
import socket
import statistics
import threading
import time
import uuid
from pathlib import Path
from urllib.parse import urlencode, urlsplit


class Stream:
    def __init__(self, origin):
        self.connection = http.client.HTTPConnection(origin.hostname, origin.port or 80, timeout=20)
        self.connection.request('GET', '/api/v1/events/stream')
        self.response = self.connection.getresponse()
        assert self.response.status == 200, f'SSE HTTP {self.response.status}'
        self.stopping = threading.Event()
        self.frames = []
        self.error = None
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            block = bytearray()
            while not self.stopping.is_set():
                byte = self.response.read(1)
                if not byte:
                    raise RuntimeError('SSE ended unexpectedly')
                block += byte
                assert len(block) <= 8192, 'Oversized SSE frame'
                if block.endswith(b'\n\n'):
                    lines = block.decode().splitlines()
                    block.clear()
                    if 'event: status' in lines:
                        data = json.loads(next(line[6:] for line in lines if line.startswith('data: ')))
                        self.frames.append((time.monotonic(), data))
                        if len(self.frames) > 4096:
                            self.frames.pop(0)
        except Exception as error:
            if not self.stopping.is_set():
                self.error = str(error)

    def close(self):
        self.stopping.set()
        if self.connection.sock:
            try:
                self.connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(2)
        self.response.close()
        self.connection.close()


class Bench:
    def __init__(self, url, output):
        self.origin = urlsplit(url)
        self.output = output
        self.report = {'url': url, 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                       'checks': [], 'diagnostics': [], 'requests': [], 'failures': [],
                       'scope': 'short unloaded REST/SSE bench; not full hardware qualification'}
        self.identity = None
        self.streams = []
        self.last_request = 0
        self.run_id = uuid.uuid4().hex[:12]
        self.sequence = 0
        self.control_started = False

    def save(self):
        self.output.write_text(json.dumps(self.report, indent=2) + '\n')

    def check(self, name, condition=True):
        assert condition, name
        self.report['checks'].append(name)
        print('PASS:', name, flush=True)
        self.save()

    def request(self, path, fields=None, *, method=None, expected=200, raw=None, headers=None, form=False):
        # Bound offered traffic to at most five requests/second per board.
        time.sleep(max(0, self.last_request + 0.2 - time.monotonic()))
        headers = dict(headers or {})
        if fields is not None:
            raw = urlencode(fields) if form else json.dumps(fields)
            headers['Content-Type'] = ('application/x-www-form-urlencoded;charset=UTF-8'
                                       if form else 'application/json')
        connection = http.client.HTTPConnection(self.origin.hostname, self.origin.port or 80, timeout=5)
        start = time.monotonic()
        try:
            connection.request(method or ('POST' if raw is not None else 'GET'), path, body=raw, headers=headers)
            response = connection.getresponse()
            data = response.read(65537)
            assert len(data) <= 65536, 'Oversized HTTP response'
            try:
                declared_length = response.getheader('Content-Length')
                if declared_length is not None:
                    assert len(data) == int(declared_length), f'Incomplete HTTP body: {len(data)}/{declared_length}'
                value = json.loads(data) if 'json' in response.getheader('Content-Type', '') else data.decode()
            except (ValueError, AssertionError):
                self.report.setdefault('invalid_responses', []).append({
                    'path': path, 'http': response.status, 'headers': dict(response.getheaders()),
                    'received_bytes': len(data), 'body': data.decode('utf-8', errors='replace')})
                self.save()
                raise
            self.report['requests'].append({'path': path, 'http': response.status,
                                            'elapsed_ms': round((time.monotonic() - start) * 1000, 2)})
            assert response.status == expected, f'{path}: expected HTTP {expected}, got {response.status}: {value}'
            return value
        finally:
            self.last_request = time.monotonic()
            connection.close()

    def status(self):
        value = self.request('/api/v1/status')
        if self.identity:
            assert (value['device_id'], value['boot_id'], value['build_id']) == self.identity, 'Unexpected device/reboot/build'
        return value

    def mutate(self, action, fields=None, **kwargs):
        data = dict(fields or {})
        data.setdefault('if_boot_id', self.identity[1])
        self.sequence += 1
        data.setdefault('operation_id', f'bench-{self.run_id}-{self.sequence}')
        value = self.request('/api/v1/timer/' + action, data, **kwargs)
        if kwargs.get('expected', 200) == 200:
            assert {'state', 'remaining_ms', 'boot_id', 'session_id', 'mutation'} <= value.keys(), 'Incomplete mutation state'
        return value

    def sample(self):
        value = self.request('/api/v1/system/diagnostics')
        value['sample_elapsed_ms'] = round((time.monotonic() - self.started) * 1000)
        # Keep resource/transport evidence; omit network identities from diagnostics.
        value.pop('station_mac', None)
        self.report['diagnostics'].append(value)
        self.save()
        return value

    def wait_open(self, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = self.status()
            if value['state'] == 'open':
                return value
            time.sleep(0.1)
        raise AssertionError('Timer did not expire within the bounded test window')

    def controls(self):
        initial = self.status()
        self.identity = (initial['device_id'], initial['boot_id'], initial['build_id'])
        self.report['identity'] = dict(zip(('device_id', 'boot_id', 'build_id'), self.identity))
        self.report['hardware_model'] = initial['hardware_model']
        self.check('Initial timer open and emergency inactive', initial['state'] == 'open' and not initial['emergency_input_active'])
        self.control_started = True
        for path in ('/api/v1/capabilities', '/api/v1/device/name', '/api/v1/wifi/status',
                     '/api/v1/wifi/ap', '/api/v1/events', '/api/v1/wifi/scan'):
            self.request(path)
        self.check('Status/capabilities/name/network/events/scan reads')
        for duration in (0, -1, 86400001):
            result = self.mutate('start', {'duration_ms': duration}, expected=400)
            assert result['code'] == 'invalid_duration'
        self.check('Out-of-range durations rejected without starting')
        result = self.mutate('start', {'duration_ms': 10000, 'if_boot_id': 'bench-stale'}, expected=409)
        self.check('Stale boot rejected', result['code'] == 'stale_boot')
        for raw, code in (('{"duration_ms":1000,"duration_ms":2000}', 'duplicate_field'),
                          ('{"duration_ms":1000.1}', 'invalid_json')):
            result = self.request('/api/v1/timer/start', raw=raw, headers={'Content-Type': 'application/json'}, expected=400)
            assert result['code'] == code
        self.check('Duplicate JSON and fractional duration rejected')
        result = self.request('/api/v1/timer/start?duration_ms=2000', {'duration_ms': 1000}, expected=400)
        self.check('JSON/query ambiguity rejected', result['code'] == 'ambiguous_request')
        result = self.request('/api/v1/timer/start', {'duration_ms': 1000},
                              headers={'Origin': 'http://untrusted.invalid'}, expected=403)
        self.check('Cross-origin mutation rejected')
        operation = f'bench-{self.run_id}-start'
        running = self.mutate('start', {'duration_ms': 15000, 'operation_id': operation}, form=True)
        self.check('Form-encoded Start returns authoritative state', running['state'] == 'running')
        duplicate = self.mutate('start', {'duration_ms': 15000, 'operation_id': operation})
        self.check('Start retry deduplicated', duplicate['mutation']['duplicate'] and duplicate['session_id'] == running['session_id'])
        result = self.mutate('start', {'duration_ms': 16000, 'operation_id': operation}, expected=409)
        self.check('Conflicting operation ID rejected', result['code'] == 'operation_id_conflict')
        paused = self.mutate('pause', {'if_session_id': running['session_id']})
        assert paused['state'] == 'paused'
        time.sleep(0.35)
        self.check('Pause holds remaining time', self.status()['remaining_ms'] == paused['remaining_ms'])
        operation = f'bench-{self.run_id}-adjust'
        adjusted = self.mutate('adjust', {'delta_ms': 2000, 'operation_id': operation})
        self.check('Atomic positive adjustment', adjusted['remaining_ms'] == paused['remaining_ms'] + 2000)
        duplicate = self.mutate('adjust', {'delta_ms': 2000, 'operation_id': operation})
        self.check('Adjustment retry does not apply twice', duplicate['mutation']['duplicate'] and duplicate['remaining_ms'] == adjusted['remaining_ms'])
        result = self.mutate('adjust', {'delta_ms': 86400000}, expected=409)
        self.check('Maximum-duration overflow rejected', result['code'] == 'maximum_duration_reached')
        result = self.mutate('resume', {'if_session_id': 'stale-session'}, expected=409)
        self.check('Stale session rejected', result['code'] == 'stale_session')
        self.check('Resume returns running state', self.mutate('resume')['state'] == 'running')
        self.check('Release returns open state', self.mutate('release')['state'] == 'open')
        self.check('Idempotent release while open', self.mutate('release')['state'] == 'open')
        self.mutate('start', {'min_ms': 1000, 'max_ms': 2000})
        self.check('Random short timer expires', self.wait_open()['last_release_reason'] == 'expired')
        self.mutate('start', {'duration_ms': 3000})
        self.check('Controls lock only on short running timer', self.mutate('lock-controls')['sealed'])
        self.mutate('release', expected=409)
        self.check('Locked controls reject release and still expire', self.wait_open()['state'] == 'open')
        self.mutate('start', {'duration_ms': 5000})
        self.check('Negative adjustment to zero releases', self.mutate('adjust', {'delta_ms': -5000})['state'] == 'open')
        self.check('Legacy start text preserved', self.request('/setMagnetLock?t_state=1&i_state=5000') == 'closed')
        self.request('/pause')
        self.check('Legacy paused triplet preserved', self.request('/getTimer').endswith(';closed;1'))
        self.request('/resume')
        self.check('Legacy release text preserved', self.request('/setMagnetLock?t_state=0') == 'open')
        self.check('Legacy open triplet preserved', self.request('/getTimer') == '0;open;0')
        self.sample()

    def load(self, seconds, existing_sse=0):
        baseline = self.sample()
        # An owner's open UI tab can hold a slot; it must be declared, not assumed.
        assert baseline['sse_clients'] == existing_sse, (
            f'Expected {existing_sse} existing SSE clients, found {baseline["sse_clients"]}; '
            'close other UI/SSE clients or pass --existing-sse-clients')
        self.report['existing_sse_clients'] = existing_sse
        limit = baseline.get('sse_client_limit', 3)  # V1 2, V2 3
        for _ in range(limit - existing_sse):
            self.streams.append(Stream(self.origin))
        start = time.monotonic()
        next_cycle = start
        next_sample = start
        cycles = 0
        while time.monotonic() - start < seconds:
            self.status()
            for stream in self.streams:
                assert stream.error is None, stream.error
                if stream.frames:
                    assert time.monotonic() - stream.frames[-1][0] < 25, 'SSE freshness lost'
            if time.monotonic() >= next_cycle:
                self.mutate('start', {'duration_ms': 5000})
                self.mutate('pause')
                self.mutate('adjust', {'delta_ms': 1000})
                self.mutate('resume')
                self.mutate('release')
                cycles += 1
                next_cycle = time.monotonic() + 9
            if time.monotonic() >= next_sample:
                current = self.sample()
                assert current['sse_clients'] == limit, 'SSE slot lost under load'
                print(f'LOAD {round(time.monotonic() - start)}s: heap={current["free_heap_bytes"]}, '
                      f'largest={current["max_free_block_bytes"]}, cycles={cycles}', flush=True)
                next_sample = time.monotonic() + 15
        self.report['load_cycles'] = cycles
        self.report['load_seconds'] = round(time.monotonic() - start, 1)
        self.report['sse'] = [{'frames': len(stream.frames), 'states': sorted({frame[1]['state'] for frame in stream.frames}),
                               'error': stream.error} for stream in self.streams]
        self.check(f'{len(self.streams)} bench SSE streams (all {limit} slots in use) remain responsive during control/load',
                   all(len(s.frames) > 3 for s in self.streams))
        for stream in self.streams:
            stream.close()
        self.streams.clear()
        time.sleep(2)
        final = self.sample()
        self.check('All bench SSE slots recovered', final['sse_clients'] == existing_sse)
        self.check('No unexpected Wi-Fi disconnects during load',
                   final['wifi_unexpected_disconnect_count'] == baseline['wifi_unexpected_disconnect_count'])

    def run(self, seconds, existing_sse=0):
        self.started = time.monotonic()
        try:
            self.controls()
            self.load(seconds, existing_sse)
        except Exception as error:
            self.report['failures'].append(str(error))
            # Keep the evidence before cleanup changes it: the event history
            # holds only the most recent entries.
            try:
                self.report['failure_status'] = self.request('/api/v1/status')
                self.report['failure_events'] = self.request('/api/v1/events')
            except Exception as capture_error:
                self.report['failure_capture_error'] = str(capture_error)
            raise
        finally:
            for stream in self.streams:
                stream.close()
            if self.control_started:
                try:
                    # Do not stop an unknown/rebooted device; require the original boot.
                    state = self.status()
                    if state['state'] != 'open' and not state['sealed']:
                        self.mutate('release')
                    state = self.wait_open()
                    self.report['final_state'] = state['state']
                except Exception as error:
                    self.report['failures'].append('CLEANUP: ' + str(error))
                    print('CHECK DEVICE MANUALLY:', error, flush=True)
            timings = [row['elapsed_ms'] for row in self.report['requests']]
            if timings:
                self.report['latency_ms'] = {'median': statistics.median(timings),
                    'p95': sorted(timings)[int((len(timings) - 1) * .95)], 'maximum': max(timings)}
            self.report['elapsed_seconds'] = round(time.monotonic() - self.started, 1)
            self.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--allow-timer-control', action='store_true')
    parser.add_argument('--existing-sse-clients', type=int, default=0, choices=(0, 1, 2),
                        help='SSE clients already connected (e.g. an open UI tab) that the bench must leave alone')
    args = parser.parse_args()
    origin = urlsplit(args.url)
    if origin.scheme != 'http' or not origin.hostname or origin.path not in ('', '/') or origin.query or origin.fragment:
        parser.error('use a plain HTTP timer origin')
    if not args.allow_timer_control:
        parser.error('requires --allow-timer-control on an unloaded bench; never on a person')
    if not 30 <= args.seconds <= 3600:
        parser.error('--seconds must be between 30 and 3600')
    if args.output.exists():
        parser.error('choose a new output file; existing evidence is never overwritten')
    Bench(args.url, args.output).run(args.seconds, args.existing_sse_clients)


if __name__ == '__main__':
    main()
