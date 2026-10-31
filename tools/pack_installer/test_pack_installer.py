"""Run with: python tools/pack_installer/test_pack_installer.py"""
import hashlib
import json
import lzma
from pathlib import Path
import struct
import tempfile
import unittest

import pack_installer as pack


class PayloadDictionaryTest(unittest.TestCase):
    def test_dictionary_boundaries(self):
        for size, expected in ((0, 4096), (4096, 4096), (4097, 8192),
                               (16 << 20, 16 << 20), ((16 << 20) + 1, 32 << 20),
                               (32 << 20, 32 << 20), ((32 << 20) + 1, 64 << 20),
                               (64 << 20, 64 << 20), (100 << 20, 64 << 20)):
            with self.subTest(size=size):
                self.assertEqual(pack.payload_dictionary_size(size), expected)

    def test_manifest_and_contents_round_trip(self):
        with tempfile.TemporaryDirectory(prefix='pulse_pack_dictionary_') as directory:
            root = Path(directory)
            contents = [b'MZ' + bytes(range(256)) * 64, b'license\n', b'']
            items = []
            for i, data in enumerate(contents):
                source = root / str(i)
                source.write_bytes(data)
                items.append((source, f'licenses/test-{i}.bin'))
            for channel, minimum in pack.CHANNELS.items():
                with self.subTest(channel=channel):
                    xz, stats = pack.build_payload(items, '1.2.3', channel)
                    raw = lzma.decompress(xz)
                    self.assertEqual(raw[:4], b'PPKM')
                    length, = struct.unpack_from('<I', raw, 4)
                    manifest = json.loads(raw[8:8 + length])
                    self.assertEqual(manifest['channel'], channel)
                    self.assertEqual(manifest['min_windows_build'], minimum)
                    self.assertEqual(manifest['version'], '1.2.3')
                    self.assertEqual(stats, {'raw': len(raw), 'xz': len(xz), 'files': 3})
                    cursor = 8 + length
                    for entry, (_, destination), content in zip(manifest['files'], items, contents):
                        self.assertEqual(entry['path'], destination)
                        self.assertEqual(entry['size'], len(content))
                        self.assertEqual(entry['sha256'], hashlib.sha256(content).hexdigest())
                        self.assertEqual(raw[cursor:cursor + len(content)], content)
                        cursor += len(content)
                    self.assertEqual(cursor, len(raw))
                    # The stream must actually advertise the reduced dictionary:
                    # a decoder capped at 2 MiB cannot accept the former 64 MiB.
                    self.assertEqual(lzma.decompress(xz, memlimit=2 << 20), raw)


if __name__ == '__main__':
    unittest.main()