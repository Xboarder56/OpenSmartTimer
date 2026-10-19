"""Shipped CSP permits the configured feed and public release API."""
from pathlib import Path
from runpy import run_path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
policy = run_path(str(ROOT / 'scripts/build/update_source.py'))
patch = policy['patch_update_policy']
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'include').mkdir()
    config = root / 'include/ReleaseUpdateConfig.h'
    for base in ['https://example.github.io/timer/', 'https://moved.github.io/new_timer/', '']:
        config.write_text(f'#define SMART_TIMER_UPDATE_BASE_URL "{base}"\n')
        for page in ['web/index.html', 'bluetooth-app/index.html']:
            result = patch((ROOT / page).read_bytes(), root).decode()
            assert '*.github.io' not in result
            expected = "connect-src 'self'" + (' ' + base if base else '') + ' https://api.github.com;'
            assert expected in result
    for base in ['https://evil.example/timer/', 'https://example.github.io/timer/; connect-src *',
                 'http://example.github.io/timer/', 'https://example.github.io/timer/?x=1']:
        config.write_text(f'#define SMART_TIMER_UPDATE_BASE_URL "{base}"\n')
        try:
            patch((ROOT / 'web/index.html').read_bytes(), root)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid update source entered the shipped CSP')
print('Both shipped HTML policies bind to the configured update path; migrations and disabled/invalid sources covered')
