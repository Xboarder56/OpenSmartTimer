#!/usr/bin/env python3
"""Exercise bench-suite aggregation with fake commands, never real hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BenchGate(unittest.TestCase):
    def exercise(self, failing, metadata_failure=False):
        with tempfile.TemporaryDirectory(prefix='ost-bench-gate-') as directory:
            root = Path(directory)
            hardware = root / 'relay/tools/hardware'
            hardware.mkdir(parents=True)
            shutil.copy2(ROOT / 'relay/tools/hardware/qualify.sh', hardware)
            commands = root / 'commands'
            commands.mkdir()
            # Both metadata readers and the six suites use this fake Python.
            python = commands / 'python3'
            python.write_text('''#!/bin/sh
if [ "$1" = "-c" ]; then
  if [ "$FAKE_METADATA_FAIL" = yes ]; then exit 9; fi
  if [ "$#" -eq 2 ]; then cat >/dev/null; fi
  echo 0123456789abcdef01234567
  exit 0
fi
echo '{"summary":"fake suite"}'
[ "$FAKE_FAIL" = no ]
''')
            node = commands / 'node'
            node.write_text('''#!/bin/sh
echo '{"summary":"fake suite"}'
[ "$FAKE_FAIL" = no ]
''')
            curl = commands / 'curl'
            curl.write_text('''#!/bin/sh
echo '{"build_id":"0123456789abcdef01234567"}'
''')
            for path in (python, node, curl):
                path.chmod(0o755)
            env = {**os.environ, 'PATH': str(commands) + os.pathsep + os.environ['PATH'],
                   'OST_TIMER': 'http://unused.invalid', 'OST_CANDIDATE': str(root),
                   'OST_BENCH_OUT': str(root / 'out'), 'FAKE_FAIL': 'yes' if failing else 'no',
                   'FAKE_METADATA_FAIL': 'yes' if metadata_failure else 'no'}
            result = subprocess.run(['bash', str(hardware / 'qualify.sh')], env=env,
                                    text=True, capture_output=True, timeout=10)
            logs = list((root / 'out/qual-0123456789abcdef01234567').glob('*.log'))
            self.assertEqual(len(logs), 0 if metadata_failure else 6, result.stdout + result.stderr)
            for log in logs:
                self.assertIn('fake suite', log.read_text())
            return result

    def test_successful_suites_pass(self):
        result = self.exercise(False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_suites_fail_the_gate_without_skipping_other_suites(self):
        result = self.exercise(True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('Qualification suites failed: 6', result.stdout)

    def test_missing_candidate_metadata_stops_before_any_hardware_suite(self):
        result = self.exercise(False, metadata_failure=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
