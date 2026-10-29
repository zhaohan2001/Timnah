#!/usr/bin/env python3
"""Check exact key identity and operation mapping during trace conversion."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class ConversionTest(unittest.TestCase):
    def test_distinct_key_prefix(self):
        with tempfile.TemporaryDirectory(prefix='trace-prefix-test-') as directory:
            root = Path(directory)
            executable = root / 'extract_key_prefix'
            subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG',
                            '-Wall', '-Wextra', '-Werror',
                            str(Path(__file__).with_name('extract_key_prefix.cpp')),
                            '-o', str(executable)], check=True)
            source = ('0,deleted,7,8,1,delete,0\n'
                      '1,alpha,beta,10,8,1,get,0\n'
                      '2,alpha,beta,10,8,1,set,0\n'
                      '3,plain,5,8,1,get,0\n'
                      '4,later,5,8,1,get,0\n')
            output, statistics = root / 'prefix.csv', root / 'statistics.json'
            subprocess.run([str(executable), str(output), str(statistics), '2'],
                           input=source, text=True, check=True)
            observed = json.loads(statistics.read_text())
            self.assertEqual(output.read_text(), ''.join(source.splitlines(True)[:4]))
            self.assertTrue(observed['key_limit_reached'])
            self.assertEqual((observed['unique_keys'], observed['raw_records'],
                              observed['records'], observed['reads'], observed['updates']),
                             (2, 4, 3, 2, 1))
            subprocess.run([str(executable), str(output), str(statistics), '10'],
                           input=source + '5,incomplete', text=True, check=True)
            observed = json.loads(statistics.read_text())
            self.assertFalse(observed['key_limit_reached'])
            self.assertEqual(observed['unique_keys'], 3)
            self.assertEqual(output.read_text(), source)

    def test_compact_key_dictionary(self):
        with tempfile.TemporaryDirectory(prefix='key-dictionary-test-') as directory:
            executable = Path(directory) / 'test_key_dictionary'
            subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG',
                            '-Wall', '-Wextra', '-Werror',
                            str(Path(__file__).with_name('test_key_dictionary.cpp')),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_embedded_key_commas_and_delete_only_keys(self):
        with tempfile.TemporaryDirectory(prefix='trace-conversion-test-') as directory:
            root = Path(directory)
            source = root / 'trace.csv'
            source.write_text(
                '0,alpha,beta,10,8,1,get,0\n'
                '1,plain,5,8,1,set,0\n'
                '2,alpha,beta,10,8,1,gets,0\n'
                '3,,leading,8,8,1,add,0\n'
                '4,alpha,beta,10,8,1,delete,0\n'
                '5,delete,only,11,8,1,delete,0\n'
                '6,plain,5,8,1,incr,0\n'
            )
            binary = root / 'tc.bin'
            subprocess.run([
                'python3', str(Path(__file__).with_name('prepare_traces.py')),
                '--input', str(source), '--workload', 'tc', '--output', str(binary),
                '--source-url', 'diagnostic-fixture', '--scope', 'diagnostic_fixture',
            ], check=True, capture_output=True, text=True)
            manifest = json.loads(binary.with_suffix('.json').read_text())
            self.assertEqual((manifest['raw_records'], manifest['records']), (7, 5))
            self.assertEqual((manifest['raw_unique_keys'], manifest['unique_keys']), (4, 3))
            self.assertEqual((manifest['reads'], manifest['updates'], manifest['excluded_delete']), (2, 3, 2))
            expected = [(1, 0), (2, 1), (1, 0), (3, 1), (2, 1)]
            raw = binary.read_bytes()
            self.assertEqual(list(struct.iter_unpack('<QQ', raw[112:])), expected)
            checksum = 0
            for key, write in expected:
                checksum = ((checksum ^ ((key << 1) | write)) * 1099511628211) & ((1 << 64) - 1)
            self.assertEqual(struct.unpack('<8sQQQQQ64s', raw[:112])[1:6], (5, 3, 2, 3, checksum))
            window = root / 'window.bin'
            subprocess.run([
                'python3', '-B', str(Path(__file__).with_name('select_trace_window.py')),
                '--input', str(binary), '--output', str(window), '--max-keys', '2',
            ], check=True, capture_output=True, text=True)
            self.assertEqual(list(struct.iter_unpack('<QQ', window.read_bytes()[112:])), expected[:3])
            selected = json.loads(window.with_suffix('.json').read_text())
            self.assertEqual((selected['records'], selected['unique_keys']), (3, 2))
            self.assertEqual((selected['source_records'], selected['source_unique_keys']), (5, 3))
            self.assertEqual(selected['selection']['excluded_records'], 2)


if __name__ == '__main__':
    unittest.main()
