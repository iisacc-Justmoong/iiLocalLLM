#!/usr/bin/env python3
"""Download checksum-locked official declarations for offline catalog regeneration.

No provider package is installed or imported. Downloads and caches stay under build/.
The checked-in lock is the input; updating provider revisions is a separate reviewed edit.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import urllib.request


def fetch(root, source):
    relative = Path(source['provider']) / source['path']
    if relative.is_absolute() or '..' in relative.parts:
        raise ValueError('Invalid source path')
    if not re.fullmatch('[a-f0-9]{40}', source['revision']):
        raise ValueError('Source revision must be an immutable commit SHA')
    expected = f'https://raw.githubusercontent.com/{source["repository"]}/{source["revision"]}/{source["path"]}'
    if source['url'] != expected:
        raise ValueError('Source URL does not match its locked repository and revision')
    destination = root / relative
    if destination.is_file() and hashlib.sha256(destination.read_bytes()).hexdigest() == source['sha256']:
        return
    request = urllib.request.Request(expected, headers={'User-Agent':'iiLocalLLM-parameter-catalog'})
    with urllib.request.urlopen(request, timeout=45) as response:
        data = response.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != source['sha256']:
        raise ValueError(f'Source checksum mismatch: {relative}')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock',type=Path,default=Path('catalog/parameter-sources.json'))
    parser.add_argument('--directory',type=Path,default=Path('build/parameter-sources'))
    args = parser.parse_args()
    if not args.directory.resolve().is_relative_to(Path('build').resolve()):
        parser.error('source cache must be under build/')
    sources = json.loads(args.lock.read_text())
    with ThreadPoolExecutor(max_workers=8) as workers:
        list(workers.map(lambda s: fetch(args.directory,s),sources))
    (args.directory/'sources.json').write_text(json.dumps(sources,indent=2)+'\n')
    print(f'Verified {len(sources)} pinned source files')


if __name__ == '__main__':
    main()
