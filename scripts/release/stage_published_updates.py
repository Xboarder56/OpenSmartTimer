#!/usr/bin/env python3
"""Stage the latest signed tagged release and matching companion app source.

Runs in Pages CI with gh. No private key is required. Never selects a draft.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
from update_feed import config, stage


def version_key(tag):
    if not isinstance(tag, str) or len(tag) >= 49: return None
    match = re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?", tag)
    if not match: return None
    core = tuple(int(n) for n in match.groups()[:3])
    if any(n > 0xffffffff for n in core): return None
    pre = match[4]
    if pre is None: return (*core, 1, ())
    parts = pre.split('.')
    if any(not p or (p.isdigit() and len(p) > 1 and p[0] == '0') for p in parts): return None
    return (*core, 0, tuple((0, int(p)) if p.isdigit() else (1, p) for p in parts))


def select(releases):
    candidates = [(version_key(r.get('tagName')), r) for r in releases
                  if not r.get('isDraft', False) and version_key(r.get('tagName'))]
    latest = max(candidates, default=(None, None), key=lambda item: item[0])[1]
    if latest is None: raise ValueError('no published release with a valid version')
    return latest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--app-tag-file', type=Path, required=True)
    args = parser.parse_args()
    repo, _, public = config()
    # Paginate so selection sees every published version tag.
    data = subprocess.check_output(['gh', 'api', '--paginate', '--slurp',
                                   f'repos/{repo}/releases?per_page=100'], text=True)
    releases = [{'tagName': r['tag_name'], 'isPrerelease': r['prerelease'],
                 'isDraft': r['draft'],'notes':r.get('body') or ''} for page in json.loads(data) for r in page]
    release=select(releases)
    tag=release['tagName']
    bundle=args.downloads/tag
    bundle.mkdir(parents=True)
    subprocess.run(['gh','release','download',tag,'--repo',repo,'--dir',str(bundle),
        '--pattern','updates.json','--pattern','OpenSmartTimer-*.bin'],check=True)
    stage(bundle,args.output,public,release['notes'],expected_version=tag[1:])
    args.app_tag_file.write_text(tag+'\n')
    print('Signed public release staged; not deployed.')


if __name__ == '__main__':
    main()
