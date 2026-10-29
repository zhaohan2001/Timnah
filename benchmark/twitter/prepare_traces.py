#!/usr/bin/env python3
"""Convert observed Twitter/FIU requests to fixed-width index replay records."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def convert(source, destination, code, source_url, scope):
    destination.parent.mkdir(parents=True, exist_ok=True)
    original = digest(source)
    temporary = destination.with_suffix('.part')
    with tempfile.TemporaryDirectory(prefix='trace-conversion-') as workspace:
        workspace = Path(workspace)
        executable = workspace / 'convert_trace'
        statistics = workspace / 'statistics.json'
        subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG',
                        str(Path(__file__).with_name('convert_trace.cpp')),
                        '-o', str(executable)], check=True)
        command = [str(executable), code, str(temporary), str(statistics), original]
        if source.name.endswith(('.tgz', '.tar.gz')):
            with tarfile.open(source, 'r:gz') as archive:
                members = [item for item in archive.getmembers() if item.isfile()]
                if len(members) != 1:
                    raise ValueError('Archive must contain exactly one trace file')
                with archive.extractfile(members[0]) as stream:
                    process = subprocess.Popen(command, stdin=subprocess.PIPE)
                    try:
                        shutil.copyfileobj(stream, process.stdin, 8 * 1024 * 1024)
                        process.stdin.close()
                        if process.wait():
                            raise RuntimeError('Trace conversion failed')
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
        elif source.suffix == '.zst':
            producer = subprocess.Popen(['zstd', '-dc', str(source)], stdout=subprocess.PIPE)
            try:
                result = subprocess.run(command, stdin=producer.stdout)
                producer.stdout.close()
                if result.returncode or producer.wait():
                    raise RuntimeError('Trace decompression or conversion failed')
            finally:
                if producer.poll() is None:
                    producer.kill()
                    producer.wait()
        else:
            with source.open('rb') as stream:
                subprocess.run(command, stdin=stream, check=True)
        counts = json.loads(statistics.read_text())
    manifest = dict(format='TTRACE01', workload=code, source_url=source_url,
                    scope=scope, source_sha256=original, binary_sha256=digest(temporary),
                    **counts, key_bytes=8, value_bytes=8,
                    adaptation='Collision-free first-seen key IDs; bijective 64-bit key mixing; reads map to Search and writes to Update; deterministic 64-bit surrogate values; delete records excluded; TTL and cache conditional-write semantics not modeled.',
                    replay='Observed input order retained; concurrent workers may reorder execution; finite traces wrap without resampling.')
    temporary.replace(destination)
    destination.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest), flush=True)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--workload', choices=['ts', 'tt', 'tc', 'w'], required=True)
    parser.add_argument('--source-url', required=True)
    parser.add_argument('--scope', required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    convert(args.input, args.output or Path(__file__).resolve().parent / 'data' / (args.workload + '.bin'),
            args.workload, args.source_url, args.scope)
