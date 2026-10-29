#!/usr/bin/env python3
"""Prepare an observed request interval using complete-source key identities."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from prepare_traces import convert, digest


def prepare(source, parent, destination, end_seconds, preload_keys):
    source, parent, destination = Path(source), Path(parent), Path(destination)
    if end_seconds <= 0 or preload_keys <= 0:
        raise ValueError('Interval and preload cardinality must be positive')
    if source.resolve() == destination.resolve() or parent.resolve() == destination.resolve():
        raise ValueError('Output must differ from source and parent')
    provenance = json.loads(parent.with_suffix('.json').read_text())
    if digest(source) != provenance['source_sha256'] or digest(parent) != provenance['binary_sha256']:
        raise ValueError('Source or complete-parent checksum mismatch')
    if provenance['unique_keys'] < preload_keys:
        raise ValueError('The complete parent does not contain enough distinct real keys')
    with tempfile.TemporaryDirectory(prefix='trace-time-window-') as directory:
        directory = Path(directory)
        executable = directory / 'extract_time_prefix'
        subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG',
                        str(Path(__file__).with_name('extract_time_prefix.cpp')),
                        '-o', str(executable)], check=True)
        csv, statistics = directory / 'source.csv', directory / 'window.json'
        command = [str(executable), str(csv), str(statistics), str(end_seconds)]
        if source.suffix == '.zst':
            producer = subprocess.Popen(['zstd', '-dc', str(source)], stdout=subprocess.PIPE)
            try:
                result = subprocess.run(command, stdin=producer.stdout)
                producer.stdout.close()
                result.check_returncode()
            finally:
                if producer.poll() is None:
                    producer.terminate()
                producer.wait()
        else:
            with source.open('rb') as stream:
                subprocess.run(command, stdin=stream, check=True)
        observed = json.loads(statistics.read_text())
        scope = 'Twitter_observed_0_' + str(end_seconds) + '_seconds'
        manifest = convert(csv, destination, provenance['workload'], provenance['source_url'], scope)
        if manifest['unique_keys'] > preload_keys:
            raise ValueError('Observed interval exceeds the requested preload domain')
        if (manifest['raw_records'], manifest['first_timestamp'], manifest['last_timestamp']) != (
                observed['raw_records'], observed['first_timestamp'], observed['last_timestamp']):
            raise ValueError('Interval boundaries do not match conversion statistics')
        with destination.open('rb') as left, parent.open('rb') as right:
            left.seek(112)
            right.seek(112)
            for block in iter(lambda: left.read(8 * 1024 * 1024), b''):
                if block != right.read(len(block)):
                    raise ValueError('Observed key identities do not match the complete-source prefix')
        manifest.update(interval_seconds=dict(start=0, end_exclusive=end_seconds),
                        preload_provenance=dict(
                            parent_source_sha256=provenance['source_sha256'],
                            parent_binary_sha256=provenance['binary_sha256'],
                            parent_unique_keys=provenance['unique_keys'],
                            preload_keys=preload_keys,
                            active_observed_keys=manifest['unique_keys'],
                            window_cold_real_keys=preload_keys-manifest['unique_keys'],
                            mapping='First-seen IDs match the complete-source prefix; IDs through preload_keys identify real keys observed in the complete parent.'))
        for name in ['cluster', 'category']:
            if name in provenance:
                manifest[name] = provenance[name]
        destination.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Prepared observed interval with verified complete-source preload identities.', flush=True)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--parent', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--end-seconds', type=int, required=True)
    parser.add_argument('--preload-keys', type=int, default=100000000)
    args = parser.parse_args()
    prepare(args.input, args.parent, args.output, args.end_seconds, args.preload_keys)
