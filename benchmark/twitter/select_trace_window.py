#!/usr/bin/env python3
"""Select a contiguous observed-request prefix bounded by distinct-key count."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import sys

sys.dont_write_bytecode = True
from prepare_traces import digest


def select(source, destination, maximum_keys):
    if source.resolve() == destination.resolve():
        raise ValueError('Input and output must differ')
    manifest = json.loads(source.with_suffix('.json').read_text())
    if digest(source) != manifest['binary_sha256']:
        raise ValueError('Input binary checksum mismatch')
    with source.open('rb') as stream:
        magic, records, keys, reads, updates, checksum, sha = struct.unpack('<8sQQQQQ64s', stream.read(112))
    if (magic != b'TTRACE01' or source.stat().st_size != 112 + 16 * records or
            (records, keys, reads, updates) != tuple(manifest[k] for k in ('records', 'unique_keys', 'reads', 'updates')) or
            sha.decode() != manifest['source_sha256']):
        raise ValueError('Input manifest does not match its binary')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.part')
    with tempfile.TemporaryDirectory(prefix='trace-window-') as directory:
        directory = Path(directory)
        executable, statistics = directory / 'select_window', directory / 'statistics.json'
        subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG',
                        str(Path(__file__).with_suffix('.cpp')), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(source), str(temporary), str(statistics), str(maximum_keys)], check=True)
        counts = json.loads(statistics.read_text())
    result = dict(manifest)
    result['source_scope'] = manifest['scope']
    for field in ('records', 'unique_keys', 'reads', 'updates', 'first_timestamp',
                  'last_timestamp', 'timestamp_regressions', 'binary_sha256'):
        if field in manifest:
            result['source_' + field] = manifest[field]
    for field in ('first_timestamp', 'last_timestamp', 'timestamp_regressions'):
        result.pop(field, None)
    result.update(counts)
    result['scope'] = 'observed_' + manifest['workload'] + '_contiguous_prefix_' + str(maximum_keys) + '_keys'
    result['selection'] = dict(kind='contiguous_request_prefix', start_record=0,
                               end_record_exclusive=counts['records'], maximum_keys=maximum_keys,
                               excluded_records=records-counts['records'],
                               rule='Stop before the first request introducing a key beyond the distinct-key bound.')
    result['binary_sha256'] = digest(temporary)
    result['replay'] = 'Selected contiguous request window retained; concurrent workers may reorder execution; the finite window wraps without resampling.'
    temporary.replace(destination)
    destination.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-keys', type=int, required=True)
    args = parser.parse_args()
    if args.max_keys <= 0:
        parser.error('--max-keys must be positive')
    select(args.input, args.output, args.max_keys)
