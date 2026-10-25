#!/usr/bin/env python3
"""Package reviewed project source and the pinned firmware dependency materials."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hxx', '.s', '.S',
                   '.ld', '.py', '.sh', '.json', '.txt', '.md', '.rst', '.yml',
                   '.yaml', '.ini', '.csv', '.inc', '.in', '.tcl', '.cfg',
                   '.properties', '.xml', '.cmake', '.ino', '.def', '.patch', '.mk', '.conf'}
SOURCE_NAMES = {'LICENSE', 'COPYING', 'COPYING.LESSER', 'NOTICE', 'Kconfig',
                'Kconfig.projbuild', 'Makefile'}


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify_archive(path: Path, record: dict) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing or symlinked source input: {record['file']}")
    if path.stat().st_size != record['size_bytes'] or sha256(path) != record['sha256']:
        raise ValueError(f"source input checksum mismatch: {record['file']}")


def source_input(record: dict, cache: Path, offline: bool) -> Path:
    filename = record['file']
    if not re.fullmatch(r'[A-Za-z0-9_.-]+\.(tar\.gz|zip)', filename):
        raise ValueError('unsafe source input filename')
    if not re.fullmatch(r'[0-9a-f]{64}', record['sha256']):
        raise ValueError('invalid source input checksum')
    url = urlsplit(record['url'])
    if url.scheme != 'https' or url.hostname not in {'codeload.github.com', 'components-file.espressif.com', 'files.pythonhosted.org'} or url.username or url.password:
        raise ValueError('unexpected source input download origin')
    path = cache / filename
    if not path.exists() and not path.is_symlink():
        if offline:
            raise ValueError(f'missing cached source input: {filename}')
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(record['url'], timeout=90) as response:
                    while block := response.read(1024 * 1024):
                        stream.write(block)
                stream.close()
                verify_archive(temporary, record)
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
    verify_archive(path, record)
    return path


def project_archive(root: Path, destination: Path) -> str:
    status = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=normal'], cwd=root)
    if status:
        raise ValueError('commit the reviewed source before packaging; working tree is dirty')
    # git archive exports tracked HEAD only. Ignored bench state never enters it.
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    subprocess.run(['git', 'archive', '--format=tar', '--output', str(destination), 'HEAD'], cwd=root, check=True)
    with tarfile.open(destination) as archive:
        for entry in archive:
            if entry.issym() or entry.islnk() or entry.name.startswith('/') or '..' in Path(entry.name).parts:
                raise ValueError(f'unsafe project archive member: {entry.name}')
    return revision


def core_files(root: Path, expected_version: str):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('missing or symlinked Arduino package')
    metadata = json.loads((root / 'package.json').read_text())
    if metadata['version'] != expected_version:
        raise ValueError('Arduino package differs from the pinned version')
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part.startswith('.') or part in {'node_modules', '__pycache__'} for part in relative.parts):
            continue
        if path.is_symlink():
            raise ValueError(f'symlinked Arduino source: {relative}')
        if path.is_file() and (path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES or path.name.startswith(('LICENSE', 'COPYING', 'NOTICE'))):
            yield relative, path


def verify_notices(root: Path) -> None:
    inventory = json.loads((root / 'third-party/inventory.json').read_text())
    for record in inventory['notice_files']:
        relative = Path(record['file'])
        if relative.is_absolute() or '..' in relative.parts or relative.parts[:2] != ('third-party', 'notices'):
            raise ValueError('unsafe notice inventory path')
        path = root / relative
        if path.is_symlink() or not path.is_file() or sha256(path) != record['sha256']:
            raise ValueError(f'notice checksum mismatch: {relative}')


def add_file(archive: tarfile.TarFile, path: Path, name: str, epoch: int) -> dict:
    info = archive.gettarinfo(str(path), arcname=name)
    info.uid = info.gid = 0
    info.uname = info.gname = ''
    info.mtime = epoch
    info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
    with path.open('rb') as stream:
        archive.addfile(info, stream)
    return {'file': name, 'size_bytes': path.stat().st_size, 'sha256': sha256(path)}


def package(root: Path, output: Path, cache: Path, offline: bool = False) -> Path:
    verify_notices(root)
    version = re.search(r'(?m)^firmware_version\s*=\s*([0-9A-Za-z.-]+)\s*$', (root / 'platformio.ini').read_text()).group(1)
    lock = json.loads((root / 'framework/esp32c3/lock.json').read_text())
    inputs = json.loads((root / 'third-party/source-inputs.json').read_text())['archives']
    cached = [(record, source_input(record, cache, offline)) for record in inputs]
    framework = root / 'dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz'
    if framework.is_symlink() or not framework.is_file() or sha256(framework) != lock['archive_sha256']:
        raise ValueError('build or install the verified V2 framework archive first')
    cores = [('arduino-esp8266-3.1.2', root / '.pio-core/v1/packages/framework-arduinoespressif8266', '3.30102.0'),
             ('arduino-esp32-3.3.0', root / '.pio-core/v2/packages/framework-arduinoespressif32', '3.3.0')]
    # Validate all dependency snapshots before producing an output.
    snapshots = [(name, list(core_files(path, version))) for name, path, version in cores]
    output.mkdir(parents=True, exist_ok=True)
    prefix = f'OpenSmartTimer-{version}-Source-Materials'
    target = output / (prefix + '.tar.gz')
    # Stage beside the output so the final rename stays on one filesystem;
    # runners may mount the system temporary directory separately.
    with tempfile.TemporaryDirectory(dir=output) as directory:
        project = Path(directory) / 'project.tar'
        revision = project_archive(root, project)
        manifest = {'schema': 1, 'firmware_version': version, 'source_revision': revision,
                    'purpose': 'Project source, Arduino sources, pinned SDK sources and V2 relinking inputs; not flashable.',
                    'files': []}
        temporary = Path(directory) / 'bundle.tar.gz'
        with tarfile.open(temporary, 'w:gz') as archive:
            def add(path, name):
                manifest['files'].append(add_file(archive, path, prefix + '/' + name, lock['source_date_epoch']))
            add(project, 'project.tar')
            for record, path in cached:
                add(path, 'upstream/' + record['file'])
            add(framework, 'vendor-sdk/' + framework.name)
            for name, files in snapshots:
                for relative, path in files:
                    add(path, 'arduino/' + name + '/' + relative.as_posix())
            instructions = ("OpenSmartTimer source and relinking materials\n\n"
                            "Extract project.tar into a new directory and read docs/BUILDING.md,\n"
                            "docs/INSTALLATION.md and THIRD_PARTY_NOTICES.md there.\n"
                            "arduino/ contains the actual source packages used by both builds.\n"
                            "upstream/ contains hash-pinned SDK source archives; vendor-sdk/\n"
                            "contains the verified C3 framework compiler input. Neither is flashable.\n"
                            "Modify the supplied core sources and relink with the project using\n"
                            "the pinned build recipe. Manual local-file upload does not require\n"
                            "the project's release-signing private key.\n"
                            "MATERIALS.json records the source commit and material hashes.\n").encode()
            info = tarfile.TarInfo(prefix + '/README.txt')
            info.size, info.mode, info.mtime = len(instructions), 0o644, lock['source_date_epoch']
            archive.addfile(info, io.BytesIO(instructions))
            payload = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
            info = tarfile.TarInfo(prefix + '/MATERIALS.json')
            info.size, info.mode, info.mtime = len(payload), 0o644, lock['source_date_epoch']
            archive.addfile(info, io.BytesIO(payload))
        temporary.replace(target)
    (output / 'SOURCE-SHA256SUMS').write_text(f'{sha256(target)}  {target.name}\n')
    print(f'Packaged {target.name}: revision={revision}, {len(manifest["files"])} materials, {target.stat().st_size} bytes')
    return target


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist/source-materials')
    parser.add_argument('--cache', type=Path, default=ROOT / 'dist/source-cache')
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    package(ROOT, args.output.resolve(), args.cache.resolve(), args.offline)


if __name__ == '__main__':
    main()
