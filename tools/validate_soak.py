#!/usr/bin/env python3
"""Opt-in, unloaded V1/V2 soak. No firmware, network settings or key changes.

Exercises short controls, every SSE viewer slot, scans, BLE management and bench
relay restarts. Share URLs stay in private output/stdin, never on stdout.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import threading
import time
import uuid
from urllib.parse import urlsplit
import zlib

ROOT = Path(__file__).resolve().parents[1]


def utc():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.new')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    os.chmod(temporary, 0o600)
    temporary.replace(path)


class Viewer:
    def __init__(self, origin, identity):
        self.stop = threading.Event()
        self.error = None
        self.frames = 0
        self.last_frame = time.monotonic()
        self.states = set()
        self.identity = identity
        self.connection = http.client.HTTPConnection(origin.hostname, origin.port or 80, timeout=25)
        self.connection.request('GET', '/api/v1/events/stream')
        self.response = self.connection.getresponse()
        if self.response.status != 200:
            self.response.close()
            self.connection.close()
            raise AssertionError(f'SSE admission HTTP {self.response.status}')
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            frame = bytearray()
            while not self.stop.is_set():
                byte = self.response.read(1)
                if not byte:
                    raise AssertionError('SSE ended unexpectedly')
                frame += byte
                assert len(frame) <= 8192, 'SSE frame exceeds bound'
                if frame.endswith(b'\n\n'):
                    lines = frame.decode().splitlines()
                    frame.clear()
                    if 'event: status' in lines:
                        value = json.loads(next(s[6:] for s in lines if s.startswith('data: ')))
                        assert tuple(value[k] for k in ('device_id', 'boot_id', 'build_id')) == self.identity, 'SSE identity/reboot changed'
                        self.frames += 1
                        self.last_frame = time.monotonic()
                        self.states.add(value['state'])
        except Exception as error:
            if not self.stop.is_set():
                self.error = str(error)

    def close(self):
        self.stop.set()
        if self.connection.sock:
            try:
                self.connection.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(2)
        self.response.close()
        self.connection.close()

    def summary(self):
        return {'frames': self.frames, 'states': sorted(self.states), 'error': self.error}


class Board:
    def __init__(self, name, url, image, output, stop):
        self.name, self.origin, self.image, self.output, self.stop = name, urlsplit(url), image, output, stop
        assert self.origin.scheme == 'http' and self.origin.hostname, 'Bench timer must use a local HTTP origin'
        self.request_lock = threading.Lock()
        self.control_lock = threading.Lock()
        self.last_request = 0
        self.identity = None
        self.started = time.monotonic()
        self.run_id = uuid.uuid4().hex[:10]
        self.sequence = 0
        self.viewers = []
        self.sse_limit = 3  # replaced by the timer's sse_client_limit in preflight()
        self.report = {'scope': 'unloaded mixed-transport soak; not full physical qualification',
                       'started_utc': utc(), 'artifact': image,
                       'requests': 0, 'latencies_ms': [], 'diagnostics': [], 'relay_samples': [],
                       'control_cycles': 0, 'scan_requests': 0, 'viewer_rotations': 0,
                       'scans': [],
                       'sse_closed': [], 'request_timeouts': [], 'failures': [], 'completed': False}

    def request(self, path, body=None, expected=200):
        # Under soak load a rare request outlasts the 5 s limit and the next
        # is answered at once. Record it and ask once more; a timer that misses
        # the retry too (about 11 s unresponsive) fails the soak. Timer
        # mutations carry an operation_id, so a retry cannot apply twice; a
        # retried relay job or scan POST at worst fails the soak, never doubles.
        try:
            return self._request_once(path, body, expected)
        except TimeoutError:
            record = {'utc': utc(), 'path': path.split('?')[0]}
            self.report['request_timeouts'].append(record)
            time.sleep(1)
            value = self._request_once(path, body, expected)
            # The timer's own view right after: did its loop pause, and where?
            try:
                diag = self._request_once('/api/v1/system/diagnostics')
                record['after'] = {key: diag.get(key) for key in (
                    'uptime_ms', 'loop_stall', 'free_heap_bytes', 'minimum_observed_free_heap_bytes',
                    'max_free_block_bytes', 'sse_clients', 'rssi_dbm', 'wifi_disconnect_count')}
            except Exception as error:  # evidence only; the retry already succeeded
                record['after'] = {'error': str(error)}
            return value

    def _request_once(self, path, body=None, expected=200):
        with self.request_lock:
            time.sleep(max(0, self.last_request + .25 - time.monotonic()))
            connection = http.client.HTTPConnection(self.origin.hostname, self.origin.port or 80, timeout=5)
            started = time.monotonic()
            try:
                encoded = json.dumps(body) if body is not None else None
                connection.request('POST' if body is not None else 'GET', path, encoded,
                                   {'Content-Type': 'application/json'} if body is not None else {})
                response = connection.getresponse()
                raw = response.read(65537)
                assert len(raw) <= 65536, 'HTTP response exceeds bound'
                length = response.getheader('Content-Length')
                assert length is None or len(raw) == int(length), 'Truncated HTTP response'
                allowed = expected if isinstance(expected, tuple) else (expected,)
                assert response.status in allowed, f'{path.split("?")[0]}: HTTP {response.status}, expected {expected}'
                value = json.loads(raw)
                self.report['requests'] += 1
                self.report['latencies_ms'].append(round((time.monotonic() - started) * 1000, 2))
                return value
            finally:
                self.last_request = time.monotonic()
                connection.close()

    def status(self):
        value = self.request('/api/v1/status')
        if self.identity is not None:
            assert tuple(value[k] for k in ('device_id', 'boot_id', 'build_id')) == self.identity, 'Unexpected identity/reboot/build'
        assert not value['emergency_input_active'], 'Emergency input changed during unattended bench run'
        return value

    def mutate(self, action, **fields):
        self.sequence += 1
        fields.update(if_boot_id=self.identity[1], operation_id=f'soak-{self.run_id}-{self.sequence}')
        return self.request('/api/v1/timer/' + action, fields)

    def sample(self):
        value = self.request('/api/v1/system/diagnostics')
        value.pop('station_mac', None)
        value['elapsed_s'] = round(time.monotonic() - self.started, 2)
        assert value['build_id'] == self.image['build_id'], 'Diagnostics build changed'
        assert value['wifi_unexpected_disconnect_count'] == self.initial_diag['wifi_unexpected_disconnect_count'], 'Unexpected Wi-Fi disconnect'
        assert value['status_json_failure_count'] == self.initial_diag['status_json_failure_count'], 'Status allocation/JSON failure'
        assert value['sse_clients'] == self.sse_limit, f'SSE slots lost: {value["sse_clients"]}'
        assert value['free_heap_bytes'] >= 4096 and value['max_free_block_bytes'] >= 2048, 'Memory reserve below soak floor'
        stack = value.get('loop_stack_min_free_bytes', 0)
        assert stack == 0 or stack >= 512, 'Loop stack reserve below soak floor'
        if self.name == 'v1':
            assert value['http_response_failed_count'] == self.initial_diag['http_response_failed_count'], 'Unexpected HTTP sender abort'
        self.report['diagnostics'].append(value)
        if self.name == 'v2':
            relay = self.request('/api/v1/relay')
            self.report['relay_samples'].append({k: relay[k] for k in (
                'state', 'paused_for_bluetooth', 'maintenance', 'reconnects', 'live_links',
                'free_heap_bytes', 'largest_free_block_bytes', 'task_stack_min_free_bytes')})
        self.persist()

    def preflight(self):
        value = self.status()
        self.identity = tuple(value[k] for k in ('device_id', 'boot_id', 'build_id'))
        assert value['build_id'] == self.image['build_id'], 'Installed image is not the frozen candidate'
        assert value['state'] == 'open' and not value['sealed'] and not value['ota_armed'], 'Timer must start open, unsealed and OTA-unarmed'
        self.report['identity'] = dict(zip(('device_id', 'boot_id', 'build_id'), self.identity))
        self.initial_diag = self.request('/api/v1/system/diagnostics')
        assert self.initial_diag['sse_clients'] == 0, 'Close other timer/SSE pages before the soak'
        self.sse_limit = self.initial_diag.get('sse_client_limit', 3)  # V1 2, V2 3
        assert not self.initial_diag.get('bluetooth_connections', 0), 'Close other Bluetooth controllers before the soak'
        self.initial_diag.pop('station_mac', None)
        self.report['initial_diagnostics'] = self.initial_diag

    def persist(self):
        record = dict(self.report)
        timings = record.pop('latencies_ms')
        if timings:
            ordered = sorted(timings)
            record['http_latency_ms'] = {'median': round(statistics.median(timings), 2),
                'p99': ordered[min(len(ordered) - 1, int(len(ordered) * .99))], 'max': max(timings)}
        save(self.output / (self.name + '.json'), record)

    def controls(self):
        assert self.status()['state'] == 'open', 'Unexpected active timer before soak cycle'
        cycle = self.report['control_cycles']
        if cycle % 4 == 0:
            self.mutate('start', duration_ms=2000)
            if cycle % 8 == 0:
                self.mutate('lock-controls')
            deadline = time.monotonic() + 6
            while self.status()['state'] != 'open':
                assert time.monotonic() < deadline, 'Natural/locked expiry missed bounded window'
                self.stop.wait(.2)
        else:
            started = self.mutate('start', duration_ms=5000)
            assert started['state'] == 'running'
            paused = self.mutate('pause')
            adjusted = self.mutate('adjust', delta_ms=500)
            assert adjusted['remaining_ms'] == paused['remaining_ms'] + 500
            assert self.mutate('resume')['state'] == 'running'
            assert self.mutate('release')['state'] == 'open'
        self.report['control_cycles'] += 1

    def scan(self):
        assert self.status()['state'] == 'open', 'Scan requires an open timer'
        value = self.request('/api/v1/wifi/scan', {}, expected=202)
        deadline = time.monotonic() + 25
        while value['state'] == 'scanning':
            assert time.monotonic() < deadline, 'Asynchronous Wi-Fi scan timed out'
            self.status()
            self.stop.wait(.5)
            value = self.request('/api/v1/wifi/scan')
        assert value['state'] == 'ready', 'Wi-Fi scan failed'
        self.report['scan_requests'] += 1
        self.report['scans'].append({'elapsed_s': round(time.monotonic() - self.started, 1),
                                    'state': value['state'], 'networks_count': len(value['networks'])})

    def run(self, deadline):
        try:
            self.viewers = [Viewer(self.origin, self.identity) for _ in range(self.sse_limit)]
            next_control = next_sample = time.monotonic()
            next_scan = time.monotonic() + 60
            next_rotation = time.monotonic() + 600
            paths = ('/api/v1/capabilities', '/api/v1/events', '/api/v1/device/name', '/api/v1/wifi/ap')
            index = 0
            while not self.stop.is_set() and time.monotonic() < deadline:
                self.status()
                for viewer in self.viewers:
                    assert viewer.error is None, viewer.error
                    assert time.monotonic() - viewer.last_frame < 25, 'SSE freshness lost'
                if time.monotonic() >= next_control and self.control_lock.acquire(blocking=False):
                    try:
                        self.controls()
                        next_control = time.monotonic() + 20
                    finally:
                        self.control_lock.release()
                if time.monotonic() >= next_sample:
                    self.sample()
                    next_sample = time.monotonic() + 30
                if time.monotonic() >= next_scan and self.control_lock.acquire(blocking=False):
                    try:
                        self.scan()
                        next_scan = time.monotonic() + 300
                    finally:
                        self.control_lock.release()
                if time.monotonic() >= next_rotation:
                    previous = self.viewers.pop(0)
                    previous.close()
                    self.report['sse_closed'].append(previous.summary())
                    self.stop.wait(1)
                    self.viewers.append(Viewer(self.origin, self.identity))
                    self.report['viewer_rotations'] += 1
                    next_rotation = time.monotonic() + 600
                self.request(paths[index % len(paths)])
                index += 1
                self.stop.wait(.1)
            self.report['completed'] = not self.stop.is_set() and time.monotonic() >= deadline
        except Exception as error:
            self.report['failures'].append(str(error))
            self.stop.set()
        finally:
            for viewer in self.viewers:
                viewer.close()
                self.report['sse_closed'].append(viewer.summary())
            self.viewers.clear()
            try:
                value = self.status()
                if value['state'] != 'open' and not value['sealed']:
                    self.mutate('release', if_session_id=value['session_id'])
                elif value['state'] != 'open':
                    time.sleep(3)
                self.report['final_state'] = self.status()['state']
                self.report['final_diagnostics'] = self.request('/api/v1/system/diagnostics')
                self.report['final_diagnostics'].pop('station_mac', None)
                assert self.report['final_state'] == 'open', 'Final timer is not open'
                assert self.report['final_diagnostics']['sse_clients'] == 0, 'Soak SSE viewers did not recover'
            except Exception as error:
                self.report['failures'].append('Cleanup: ' + str(error))
                self.stop.set()
            self.report['finished_utc'] = utc()
            self.report['elapsed_s'] = round(time.monotonic() - self.started, 1)
            self.persist()


def relay_job(board, path, fields):
    relay = board.request('/api/v1/relay')
    job_id = relay['next_job_id']
    board.request(path, dict(fields, job_id=job_id, if_boot_id=relay['boot_id']), expected=202)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        relay = board.request('/api/v1/relay')
        if relay['job']['id'] == job_id and relay['job']['state'] == 'done':
            assert relay['job']['result'] == 'ok', 'Relay owner job failed'
            return relay['job']
        time.sleep(1)
    raise AssertionError('Relay owner job timed out')


def wait_relay(board, predicate, seconds=90):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = board.request('/api/v1/relay')
        if predicate(value):
            return value
        time.sleep(1)
    raise AssertionError('Relay recovery timed out')


def wait_partner(path, previous_online=0):
    deadline = time.monotonic() + 75
    while time.monotonic() < deadline:
        try:
            value = json.loads(path.read_text())
            assert not value['errors'], 'Partner page error'
            if value.get('current_state') == 'online' and value['online_samples'] > previous_online:
                return value
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(1)
    raise AssertionError('Same partner page did not regain verified online status')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--v1-url', required=True)
    parser.add_argument('--v2-url', required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=7200)
    parser.add_argument('--ble-python', required=True)
    parser.add_argument('--ble-key', type=Path, required=True)
    parser.add_argument('--relay-container', required=True)
    parser.add_argument('--unloaded-bench', action='store_true')
    args = parser.parse_args()
    assert args.unloaded_bench, 'Explicit unloaded-bench acknowledgement required'
    assert args.seconds >= 120, 'Use at least 120 seconds; release soak requires 7200 or more'
    build = json.loads((args.candidate / 'BUILD.json').read_text())
    images = build['artifacts']
    if isinstance(images, dict):
        images = list(images.values())
    by_chip = {image['chip_family']: image for image in images}
    stop = threading.Event()
    boards = [Board('v1', args.v1_url, by_chip['ESP8266'], args.output, stop),
              Board('v2', args.v2_url, by_chip['ESP32-C3'], args.output, stop)]
    for board in boards:
        image = board.image
        filename = image.get('file', image.get('filename'))
        raw = (args.candidate / filename).read_bytes()
        assert len(raw) == image['size_bytes'] and hashlib.sha256(raw).hexdigest() == image['sha256'], 'Frozen candidate checksum mismatch'
        assert f'{zlib.crc32(raw) & 0xffffffff:08x}' == image['crc32'], 'Frozen candidate CRC mismatch'
        board.preflight()
    v2 = boards[1]
    relay = v2.request('/api/v1/relay')
    assert relay['sharing_available'] and relay['state'] == 'connected', 'V2 relay must be enrolled and ready'
    assert relay['live_links'] == 0, 'Refusing to interfere with pre-existing share links'
    args.output.mkdir(parents=True, exist_ok=False)
    os.chmod(args.output, 0o700)
    overview = {'started_utc': utc(), 'planned_seconds': args.seconds, 'finished': False,
                'ble_cycles': [], 'relay_restarts': [], 'failures': [],
                'unavailable_metrics': ['allocation counts are not exposed by the firmware']}
    fp = None
    partner = None
    threads = []
    begin = time.monotonic()
    deadline = begin + args.seconds
    try:
        job = relay_job(v2, '/api/v1/relay/links', {'label': 'RC soak', 'ttl_s': min(86400, args.seconds + 600)})
        fp = job['fp_hash']
        share_deadline = time.monotonic() + 30
        while True:
            share = v2.request('/api/v1/relay/links/share?fp_hash=' + fp, expected=(200, 409))
            if share.get('share_url'):
                break
            assert time.monotonic() < share_deadline, 'New grant did not become available for sharing'
            time.sleep(1)
        partner = subprocess.Popen(['node', str(ROOT / 'relay/tools/hardware/soak_partner.cjs'), str(args.output / 'partner.json')],
                                   stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
        partner.stdin.write(json.dumps({'url': share['share_url']}) + '\n')
        partner.stdin.flush()
        for board in boards:
            thread = threading.Thread(target=board.run, args=(deadline,), daemon=True)
            thread.start()
            threads.append(thread)
        next_ble = begin + 120
        next_restart = begin + 300
        next_progress = begin
        while not stop.is_set() and time.monotonic() < deadline:
            now = time.monotonic()
            assert partner.poll() is None, 'Partner browser exited during soak'
            if now >= next_ble:
                partner_before = wait_partner(args.output / 'partner.json')['online_samples']
                with v2.control_lock:
                    assert v2.status()['state'] == 'open', 'V2 must be open for BLE phase'
                    cycle_dir = args.output / f'ble-{len(overview["ble_cycles"]) + 1}'
                    environment = dict(os.environ, OST_TIMER=args.v2_url, OST_BLE_KEY=str(args.ble_key.resolve()), OST_BENCH_OUT=str(cycle_dir.resolve()))
                    cycle_dir.mkdir()
                    with (cycle_dir / 'runner.log').open('w') as log:
                        result = subprocess.run([args.ble_python, str(ROOT / 'relay/tools/hardware/bluetooth_soak.py')],
                                                env=environment, stdout=log, stderr=log, timeout=180)
                    assert result.returncode == 0, 'BLE soak phase failed; see private report'
                    evidence = json.loads((cycle_dir / 'bluetooth-soak.json').read_text())
                    assert not evidence['failures'], 'BLE soak checks failed'
                    assert any(s['paused_for_bluetooth'] for s in v2.report['relay_samples']), 'Relay did not yield during Bluetooth'
                    resumed = wait_relay(v2, lambda r: r['state'] == 'connected' and not r['paused_for_bluetooth'] and not r['maintenance'])
                    assert resumed['live_links'] == 1, 'Live grant lost after Bluetooth'
                    wait_partner(args.output / 'partner.json', partner_before)
                    overview['ble_cycles'].append({'elapsed_s': round(time.monotonic() - begin, 1),
                                                  'checks': len(evidence['checks']), 'commands': len(evidence['commands']),
                                                  'management': len(evidence['management']), 'same_page_recovered': True})
                next_ble = time.monotonic() + 1200
            if time.monotonic() >= next_restart:
                partner_before = wait_partner(args.output / 'partner.json')['online_samples']
                before = v2.request('/api/v1/relay')['reconnects']
                subprocess.run(['docker', 'restart', args.relay_container], check=True, stdout=subprocess.DEVNULL, timeout=45)
                resumed = wait_relay(v2, lambda r: r['state'] == 'connected' and r['reconnects'] > before)
                assert resumed['live_links'] == 1 and not resumed['maintenance'], 'Relay restart lost grant or left storage lease'
                wait_partner(args.output / 'partner.json', partner_before)
                overview['relay_restarts'].append({'elapsed_s': round(time.monotonic() - begin, 1), 'reconnects': resumed['reconnects'], 'same_page_recovered': True})
                next_restart = time.monotonic() + 1800
            if time.monotonic() >= next_progress:
                save(args.output / 'RESULT.json', overview)
                summary = {board.name: {'requests': board.report['requests'], 'cycles': board.report['control_cycles'],
                                        'failures': len(board.report['failures'])} for board in boards}
                print(json.dumps({'elapsed_s': round(time.monotonic() - begin), 'planned_s': args.seconds,
                                  'boards': summary, 'ble_cycles': len(overview['ble_cycles']),
                                  'relay_restarts': len(overview['relay_restarts'])}), flush=True)
                next_progress = time.monotonic() + 45
            stop.wait(1)
        for thread in threads:
            thread.join(15)
        assert all(not t.is_alive() for t in threads), 'Board workers failed to complete'
        assert not stop.is_set(), 'A board failed the soak; inspect private reports'
        assert all(b.report['completed'] and not b.report['failures'] for b in boards), 'Soak incomplete'
        assert overview['ble_cycles'] and overview['relay_restarts'], 'Mixed transport phases missing'
        overview['finished'] = True
    except Exception as error:
        overview['failures'].append(str(error))
        stop.set()
    finally:
        for thread in threads:
            thread.join(20)
        if partner:
            try:
                partner.stdin.write('stop\n')
                partner.stdin.flush()
                partner.wait(15)
            except (OSError, subprocess.TimeoutExpired):
                partner.terminate()
                partner.wait(10)
            if (args.output / 'partner.json').exists():
                evidence = json.loads((args.output / 'partner.json').read_text())
                overview['partner'] = evidence
                if evidence.get('errors') or not evidence.get('online_samples') or evidence.get('current_state') != 'online':
                    overview['failures'].append('Partner browser failed; inspect private report')
        if fp:
            try:
                wait_relay(v2, lambda r: r['state'] == 'connected' and r['sharing_available'])
                relay_job(v2, '/api/v1/relay/links/revoke', {'fp_hash': fp})
                assert v2.request('/api/v1/relay')['live_links'] == 0, 'Soak grant did not revoke'
                overview['temporary_link_revoked'] = True
            except Exception as error:
                overview['failures'].append('Relay cleanup: ' + str(error))
        overview['finished_utc'] = utc()
        overview['elapsed_s'] = round(time.monotonic() - begin, 1)
        save(args.output / 'RESULT.json', overview)
    print(json.dumps({'finished': overview['finished'], 'elapsed_s': overview['elapsed_s'],
                      'failures': overview['failures']}), flush=True)
    return 0 if overview['finished'] and not overview['failures'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
