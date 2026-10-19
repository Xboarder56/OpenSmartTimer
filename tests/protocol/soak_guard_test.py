#!/usr/bin/env python3
"""Soak admission, failures and cleanup with injected transport; no hardware."""
import importlib.util
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('soak', ROOT / 'tools/validate_soak.py')
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class SoakGuardTests(unittest.TestCase):
    def board(self, directory):
        return soak.Board('v1', 'http://synthetic.invalid', {'build_id': 'a' * 24}, Path(directory), threading.Event())

    def status(self, **changes):
        return dict({'device_id': 'synthetic', 'boot_id': 'boot', 'build_id': 'a' * 24,
                     'state': 'open', 'sealed': False, 'ota_armed': False, 'emergency_input_active': False}, **changes)

    def test_preflight_refuses_wrong_image_armed_active_or_occupied_viewers(self):
        with tempfile.TemporaryDirectory() as directory:
            for status, count in [(self.status(build_id='b' * 24), 0), (self.status(ota_armed=True), 0),
                                  (self.status(state='running'), 0), (self.status(), 1)]:
                board = self.board(directory)
                board.request = lambda path, *_: status if path.endswith('/status') else {'sse_clients': count}
                with self.assertRaises(AssertionError):
                    board.preflight()
                self.assertEqual(board.viewers, [], 'admission failure must not start viewers or controls')

    def test_one_timeout_is_recorded_and_retried_but_two_fail(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(soak.time, 'sleep'):
            board = self.board(directory)
            answers = [TimeoutError('timed out'), {'state': 'open'}]
            def once(*_):
                answer = answers.pop(0)
                if isinstance(answer, Exception):
                    raise answer
                return answer
            board._request_once = once
            answers.append({'loop_stall': {'longest_ms': 2200, 'phase': 'web_request'}, 'uptime_ms': 5})
            self.assertEqual(board.request('/api/v1/status'), {'state': 'open'})
            self.assertEqual([t['path'] for t in board.report['request_timeouts']], ['/api/v1/status'])
            self.assertEqual(board.report['request_timeouts'][0]['after']['loop_stall']['longest_ms'], 2200)
            answers[:] = [TimeoutError('timed out'), TimeoutError('timed out')]
            with self.assertRaises(TimeoutError):
                board.request('/api/v1/timer/start', {'operation_id': 'same'})
            self.assertEqual(len(board.report['request_timeouts']), 2)

    def test_unexpected_reboot_is_not_accepted_as_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            board = self.board(directory)
            board.identity = ('synthetic', 'boot', 'a' * 24)
            board.request = lambda *_: self.status(boot_id='another-boot')
            with self.assertRaisesRegex(AssertionError, 'reboot'):
                board.status()

    def test_worker_failure_closes_every_viewer_and_preserves_failed_evidence(self):
        closed = []
        class Viewer:
            error = 'injected stream loss'
            last_frame = time.monotonic()
            def __init__(self, *_): pass
            def close(self): closed.append(self)
            def summary(self): return {'frames': 1, 'states': ['open'], 'error': self.error}
        with tempfile.TemporaryDirectory() as directory:
            board = self.board(directory)
            board.identity = ('synthetic', 'boot', 'a' * 24)
            board.request = lambda path, *_: self.status() if path.endswith('/status') else {'sse_clients': 0}
            with patch.object(soak, 'Viewer', Viewer):
                board.run(time.monotonic() + 5)
            saved = json.loads((Path(directory) / 'v1.json').read_text())
            self.assertEqual(len(closed), 3)
            self.assertEqual(saved['failures'], ['injected stream loss'])
            self.assertFalse(saved['completed'])
            self.assertEqual(saved['final_state'], 'open')
            self.assertTrue(board.stop.is_set())

    def test_scan_starts_a_real_scan_and_records_no_network_names(self):
        with tempfile.TemporaryDirectory() as directory:
            board = self.board(directory)
            calls = []
            def request(path, body=None, expected=200):
                calls.append((path, body, expected))
                if path.endswith('/status'): return self.status()
                return {'state': 'ready', 'networks': [{'ssid': 'private-network'}]}
            board.request = request
            board.scan()
            self.assertIn(('/api/v1/wifi/scan', {}, 202), calls)
            self.assertEqual(board.report['scans'][0]['networks_count'], 1)
            self.assertNotIn('private-network', json.dumps(board.report))


if __name__ == '__main__':
    unittest.main()
