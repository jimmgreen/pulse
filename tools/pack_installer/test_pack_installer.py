"""Run with: python tools/pack_installer/test_pack_installer.py"""
import hashlib
import json
import lzma
from pathlib import Path
import struct
import tempfile
import unittest
import sys

import pack_installer as pack
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'pack_dialog_experiment'))
import pack as dialog_pack
import test_pack as dialog_fixtures


class PayloadDictionaryTest(unittest.TestCase):
    def test_installer_contains_separate_verified_dialog_bundle(self):
        fixture = dialog_fixtures.PackageTests()
        fixture.setUp()
        try:
            pointer = fixture.installed()
            items = [(p, p.relative_to(fixture.output).as_posix())
                     for p in sorted(fixture.output.rglob('*')) if p.is_file()]
            xz, _ = pack.build_payload(items, 'same-version')
            raw = lzma.decompress(xz)
            length, = struct.unpack_from('<I', raw, 4)
            manifest = json.loads(raw[8:8 + length])
            files = {item['path']: item for item in manifest['files']}
            self.assertIn('pulse.exe', files)
            self.assertIn(pointer['directory'] + '/pulse.exe', files)
            self.assertIn(pointer['directory'] + '/PulseDialogAuto64.dll', files)
            self.assertNotIn('PulseDialogAuto64.dll', files)
            self.assertNotIn('manifest.json', files)
            extracted = fixture.base / 'extracted'
            extracted.mkdir()
            cursor = 8 + length
            for item in manifest['files']:
                path = extracted / item['path']
                path.parent.mkdir(parents=True, exist_ok=True)
                data = raw[cursor:cursor + item['size']]
                self.assertEqual(hashlib.sha256(data).hexdigest(), item['sha256'])
                path.write_bytes(data)
                cursor += item['size']
            self.assertEqual(cursor, len(raw))
            self.assertEqual(dialog_pack.verify_installed(extracted), pointer)
        finally:
            fixture.tearDown()

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
