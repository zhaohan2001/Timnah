#!/usr/bin/env python3
"""Install the published observed workloads and verify their exact contents."""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
import urllib.request
from pathlib import Path


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def matches(path, size, checksum):
    return path.is_file() and path.stat().st_size == size and digest(path) == checksum


def download(url, destination, size, checksum):
    if matches(destination, size, checksum):
        return
    pending = destination.with_name(destination.name + '.partial')
    for attempt in range(5):
        try:
            offset = pending.stat().st_size if pending.exists() else 0
            if offset >= size:
                pending.unlink()
                offset = 0
            headers = {'User-Agent': 'Timnah-trace-installer'}
            if offset:
                headers['Range'] = 'bytes={}-'.format(offset)
            request = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(request, timeout=60) as response:
                append = bool(offset and response.status == 206)
                if append and not response.headers.get('Content-Range', '').startswith('bytes {}-'.format(offset)):
                    raise RuntimeError('Unexpected download range')
                with pending.open('ab' if append else 'wb') as stream:
                    shutil.copyfileobj(response, stream, 8 * 1024 * 1024)
            if not matches(pending, size, checksum):
                pending.unlink(missing_ok=True)
                raise RuntimeError('Downloaded asset checksum mismatch')
            pending.replace(destination)
            return
        except Exception:
            if attempt == 4:
                raise
            print('Download interrupted; retrying {}.'.format(destination.name), flush=True)
            time.sleep(min(2 ** attempt, 8))


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workloads', nargs='+', choices=['ts', 'tt', 'tc', 'w'], default=['ts', 'tt', 'tc', 'w'])
    parser.add_argument('--output-dir', type=Path, default=here / 'data')
    parser.add_argument('--cache-dir', type=Path, default=here / '.downloads')
    parser.add_argument('--asset-dir', type=Path, help='Use an existing local copy of the release assets')
    args = parser.parse_args()
    if not shutil.which('zstd'):
        raise RuntimeError('zstd is required; install it before preparing the workloads')
    specification = json.loads((here / 'datasets.json').read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    base = 'https://github.com/{}/releases/download/{}/'.format(specification['repository'], specification['release'])
    for code in dict.fromkeys(args.workloads):
        item = specification['datasets'][code]
        destination = args.output_dir / (code + '.bin')
        manifest = destination.with_suffix('.json')
        if matches(destination, item['binary_bytes'], item['binary_sha256']):
            manifest.write_text(json.dumps(item['manifest'], indent=2) + '\n')
            print('{}: installed data checksum verified.'.format(code), flush=True)
            continue
        parts = []
        for part in item['parts']:
            source = (args.asset_dir or args.cache_dir) / part['name']
            if args.asset_dir:
                if not matches(source, part['bytes'], part['sha256']):
                    raise RuntimeError('Local release asset is missing or corrupt: ' + str(source))
            else:
                print('{}: downloading {}.'.format(code, part['name']), flush=True)
                download(base + part['name'], source, part['bytes'], part['sha256'])
            parts.append(source)
        compressed_hash = hashlib.sha256()
        pending = destination.with_suffix('.bin.partial')
        try:
            with pending.open('wb') as output:
                process = subprocess.Popen(['zstd', '-dc'], stdin=subprocess.PIPE, stdout=output)
                try:
                    for source in parts:
                        with source.open('rb') as stream:
                            for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
                                compressed_hash.update(block)
                                process.stdin.write(block)
                    process.stdin.close()
                    if process.wait():
                        raise RuntimeError('Trace decompression failed')
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
            if compressed_hash.hexdigest() != item['compressed_sha256'] or not matches(pending, item['binary_bytes'], item['binary_sha256']):
                raise RuntimeError('Installed trace checksum mismatch: ' + code)
            pending.replace(destination)
            manifest.write_text(json.dumps(item['manifest'], indent=2) + '\n')
            print('{}: installed and verified {} records.'.format(code, item['manifest']['records']), flush=True)
        except BaseException:
            pending.unlink(missing_ok=True)
            raise
    print('Observed workloads are ready.', flush=True)


if __name__ == '__main__':
    main()
