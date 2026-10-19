import importlib.util
from pathlib import Path
import tempfile
import re

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pages_build', ROOT / 'scripts/build/build_pages.py')
pages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pages)
with tempfile.TemporaryDirectory() as directory:
    output = Path(directory)
    pages.stage(output, 'example/smart-timer', 'a' * 40)
    files = {str(p.relative_to(output)) for p in output.rglob('*') if p.is_file()}
    assert files == set(pages.ASSETS) | {'index.html', 'home.css', 'home.js', 'bluetooth-app/service-worker.js'}
    home = (output / 'index.html').read_text()
    # The home page links to this exact commit and carries the safety notice.
    assert 'OWNER/REPOSITORY' not in home and 'REVISION' not in home
    assert 'https://github.com/example/smart-timer/blob/' + 'a' * 40 + '/SAFETY.md' in home
    assert 'https://github.com/example/smart-timer/releases' in home
    assert 'provided as is, without warranty' in home and 'battery only' in home
    assert 'serious injury or death' in home and 'self-restraint while alone' in home
    assert 'href="bluetooth-app/"' in home
    html = (output / 'bluetooth-app/index.html').read_text()
    assert '/tree/' + 'a' * 40 in html
    assert 'href="https://github.com/example/smart-timer/blob/' + 'a' * 40 + '/SAFETY.md" id="safetyLink"' in html
    assert 'id="appBuild" data-i18n-literal>' in html
    assert 'title="Commit ' + 'a' * 40 + '">' + 'a' * 12 + '</a>' in html
    assert 'Development checkout' not in html
    base = re.search(r'^#define SMART_TIMER_UPDATE_BASE_URL "([^"]+)"$',
                     (ROOT / 'include/ReleaseUpdateConfig.h').read_text(), re.MULTILINE)[1]
    assert f"connect-src 'self' {base} https://api.github.com;" in html
    assert '*.github.io' not in html
    worker = (output / 'bluetooth-app/service-worker.js').read_text()
    assert 'development-v' not in worker
    assert re.search(r"const APP_VERSION = '[0-9a-f]{24}';", worker)
    assert (ROOT / 'LICENSE').is_file()
    assert 'LICENSE' not in files
    for page in ('index.html', 'bluetooth-app/index.html'):
        html = (output / page).read_text()
        assert 'AGPLv3 license' not in html
        assert 'href="LICENSE"' not in html and 'href="../LICENSE"' not in html
    try:
        pages.stage(output, 'example/smart-timer', 'a' * 40)
    except ValueError:
        pass
    else:
        raise AssertionError('nonempty output must not be overwritten')
    with tempfile.TemporaryDirectory() as other:
        pages.stage(Path(other), 'example/smart-timer', 'b' * 40)
        assert worker != (Path(other) / 'bluetooth-app/service-worker.js').read_text()
    with tempfile.TemporaryDirectory() as identical:
        pages.stage(Path(identical), 'example/smart-timer', 'a' * 40)
        assert worker == (Path(identical) / 'bluetooth-app/service-worker.js').read_text()
for repository, revision in [('owner/repo', 'main'), ('owner/repo', 'a' * 39),
                             ('owner/repo\" onclick=\"x', 'a' * 40)]:
    with tempfile.TemporaryDirectory() as directory:
        try:
            pages.stage(Path(directory), repository, revision)
        except ValueError:
            pass
        else:
            raise AssertionError('untrusted Pages build identity was accepted')
print('Pages allowlist, visible commit, coherent cache revision and output safety passed')
