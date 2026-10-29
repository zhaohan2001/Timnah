#!/usr/bin/env python3
"""Check observed-trace startup settings and independent YCSB presets."""
from pathlib import Path
import json
import math
import os
import re
import resource
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
if os.environ.get('TIMNAH_TRACE_DIR'):
    data = Path(os.environ['TIMNAH_TRACE_DIR'])
else:
    data = next(parent / 'benchmark/twitter/data' for parent in binary.parents
                if (parent / 'benchmark/twitter/data').is_dir())

for code in ('ts', 'tt', 'tc', 'w'):
    manifest = json.loads((data / (code + '.json')).read_text())
    result = subprocess.run([str(binary), '-workload=' + code],
                            capture_output=True, text=True, check=True)
    match = re.search(r'RESOLVED \S+ (\S+) (\S+) (\S+) (\S+) (\S+)', result.stdout)
    assert match is not None
    expected = (manifest['reads'] / manifest['records'],
                manifest['updates'] / manifest['records'], 0., 0., 0.)
    assert all(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12)
               for a, b in zip(map(float, match.groups()), expected))
    assert 'rounds=10 ' in result.stdout and 'distribution=trace' in result.stdout
    assert "preload=100000000 operations_per_round=100000000" in result.stdout
    assert 'scope=' + manifest['scope'] in result.stdout
    for extra in (['-read_ratio=.5'], ['-skewness=.9'], ['-keySize=16'],
                  ['-valueSize=16'], ['-num_load=0']):
        rejected = subprocess.run([str(binary), '-workload=' + code, *extra],
                                  capture_output=True, text=True)
        assert rejected.returncode != 0, (code, extra)
    explicit = subprocess.run([str(binary), '-workload=' + code, '-num_rounds=3',
                               '-num_load=100', '-real_epoch_ops=0', '-real_drift_stride=100'],
                              capture_output=True, text=True, check=True)
    assert 'rounds=3 ' in explicit.stdout and 'preload=100 ' in explicit.stdout
    print('PASS observed-trace parameters:', code)

with tempfile.TemporaryDirectory(prefix='missing-trace-test-') as empty:
    environment = dict(os.environ, TIMNAH_TRACE_DIR=empty)
    for code in 'abcd':
        result = subprocess.run([str(binary), '-workload=' + code], env=environment,
                                capture_output=True, text=True, check=True)
        assert 'YCSB workload=' + code in result.stdout
        assert 'Real dataset' not in result.stdout and 'Real profile' not in result.stdout
    missing = subprocess.run([str(binary), '-workload=ts'], env=environment,
                             capture_output=True, text=True)
    assert missing.returncode != 0 and 'Trace file missing' in missing.stderr
print('PASS YCSB startup independence and observed-trace parameter validation')
