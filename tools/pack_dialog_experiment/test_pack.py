"""Pure-file synthetic PE fixtures: never execute or load an image."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import pack
from pe_dependencies import PE, PEError


def image(path, machine=0x8664, normal=(), delay=()):
    data = bytearray(0x1200)
    def u16(at, n): struct.pack_into('<H', data, at, n)
    def u32(at, n): struct.pack_into('<I', data, at, n)
    data[:2] = b'MZ'; u32(0x3c, 0x80); data[0x80:0x84] = b'PE\0\0'
    opt_size = 240 if machine == 0x8664 else 224
    u16(0x84, machine); u16(0x86, 1); u16(0x94, opt_size)
    opt = 0x98; u16(opt, 0x20b if machine == 0x8664 else 0x10b)
    offset = 112 if machine == 0x8664 else 96
    if machine == 0x8664: struct.pack_into('<Q', data, opt + 24, 0x140000000)
    else: u32(opt + 28, 0x400000)
    u32(opt + 60, 0x200); u32(opt + offset - 4, 16)
    section = opt + opt_size
    u32(section + 8, 0x1000); u32(section + 12, 0x1000); u32(section + 16, 0x1000); u32(section + 20, 0x200)
    name_rva = 0x1400
    for delayed, names in ((False, normal), (True, delay)):
        if not names: continue
        rva, stride, index = (0x1200, 32, 13) if delayed else (0x1000, 20, 1)
        u32(opt + offset + index * 8, rva); u32(opt + offset + index * 8 + 4, (len(names) + 1) * stride)
        for i, name in enumerate(names):
            at = 0x200 + rva - 0x1000 + i * stride
            if delayed: u32(at, 1)
            u32(at + (4 if delayed else 12), name_rva)
            raw = 0x200 + name_rva - 0x1000
            encoded = name.encode('ascii') + b'\0'; data[raw:raw + len(encoded)] = encoded
            name_rva += 128
    path.write_bytes(data)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pulse-dialog-pack-synthetic-')
        self.base = Path(self.temp.name).resolve()
        self.repo = self.base / 'repo'; self.build = self.base / 'build'; self.repo.mkdir(); self.build.mkdir()
        self.output = self.base / 'new-experiment'
        for relative in pack.REPO_LICENSES:
            path = self.repo / relative; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('fixture license', encoding='utf-8')
        for group in ('LumaText', 'PDFium'):
            folder = self.build / 'licenses' / group; folder.mkdir(parents=True); (folder / 'LICENSE').write_text('fixture license', encoding='utf-8')
        for name in pack.roots('x64') + list(pack.DYNAMIC): image(self.build / name, normal=['kernel32.dll'])
        image(self.build / pack.roots('x64')[1], normal=['normal.dll'], delay=['delayed.dll'])
        image(self.build / 'normal.dll'); image(self.build / 'delayed.dll', delay=['kernel32.dll'])

    def tearDown(self): self.temp.cleanup()
    def plan(self): return pack.plan(self.build, 'experiment-1', 'x64', self.repo)
    def package(self): return pack.package(self.build, self.output, 'experiment-1', 'x64', self.repo)

    def test_recursive_normal_delay_and_licenses(self):
        manifest = self.package()
        self.assertEqual(manifest, pack.verify(self.output))
        names = {f['path'].lower() for f in manifest['files']}
        self.assertIn('normal.dll', names); self.assertIn('delayed.dll', names)
        self.assertFalse(manifest['contract']['capability_certification'])
        self.assertFalse((self.output / 'kernel32.dll').exists())

    def test_automatic_architectures(self):
        for arch, machine in pack.MACHINES.items():
            with self.subTest(arch=arch):
                for name in pack.roots(arch, True) + list(pack.DYNAMIC): image(self.build / name, machine)
                destination = self.base / arch
                pack.package(self.build, destination, 'experiment-1', arch, self.repo, True)
                pack.verify(destination)
                auto = pack.roots(arch, True)[-2]
                image(self.build / auto, 0x14c if machine == 0x8664 else 0x8664)
                with self.assertRaisesRegex(pack.PackageError, 'wrong architecture'):
                    pack.plan(self.build, 'experiment-1', arch, self.repo, True)

    def test_missing_dependencies_aggregated(self):
        (self.build / 'normal.dll').unlink(); (self.build / 'delayed.dll').unlink()
        with self.assertRaises(pack.PackageError) as caught: self.plan()
        self.assertIn('normal.dll', str(caught.exception)); self.assertIn('delayed.dll', str(caught.exception))
        self.assertFalse(self.output.exists())

    def test_unknown_dependency_fails(self):
        image(self.build / 'normal.dll', normal=['unknown-vendor.dll'])
        with self.assertRaisesRegex(pack.PackageError, 'unknown-vendor'): self.plan()

    def test_wrong_bitness(self):
        image(self.build / 'normal.dll', machine=0x14c)
        with self.assertRaisesRegex(pack.PackageError, 'wrong architecture'): self.plan()

    def test_x86_and_delay_va(self):
        path = self.base / 'x86.dll'; image(path, machine=0x14c, normal=['kernel32.dll'], delay=['user32.dll'])
        data = bytearray(path.read_bytes()); struct.pack_into('<II', data, 0x400, 0, 0x401480); path.write_bytes(data)
        pe = PE(path); self.assertEqual(pe.machine, 0x14c); self.assertEqual(pe.delay, ['user32.dll'])

    def test_machine_magic_mismatch(self):
        path = self.build / 'normal.dll'; data = bytearray(path.read_bytes()); struct.pack_into('<H', data, 0x84, 0x14c); path.write_bytes(data)
        with self.assertRaisesRegex(PEError, 'machine/magic'): PE(path)

    def test_tampered_file(self):
        self.package(); path = self.output / 'normal.dll'; path.write_bytes(path.read_bytes() + b'tamper')
        with self.assertRaisesRegex(pack.PackageError, 'mismatch'): pack.verify(self.output)

    def test_manifest_path_escape(self):
        self.package(); path = self.output / 'manifest.json'; manifest = json.loads(path.read_text(encoding='utf-8'))
        manifest['files'][0]['path'] = '../escaped.dll'; path.write_text(json.dumps(manifest), encoding='utf-8')
        with self.assertRaisesRegex(pack.PackageError, 'unsafe relative'): pack.verify(self.output)
        self.assertFalse((self.base / 'escaped.dll').exists())

    def test_dependency_path_escape(self):
        image(self.build / 'normal.dll', normal=['../escape.dll'])
        with self.assertRaisesRegex(pack.PackageError, 'unsafe DLL'): self.plan()

    def test_duplicate_output_untouched(self):
        self.output.mkdir(); sentinel = self.output / 'keep'; sentinel.write_text('unchanged')
        with self.assertRaisesRegex(pack.PackageError, 'already exists'): self.package()
        self.assertEqual(sentinel.read_text(), 'unchanged'); self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_extra_file_rejected(self):
        self.package(); (self.output / 'unlisted.txt').write_text('x')
        with self.assertRaisesRegex(pack.PackageError, 'unlisted'): pack.verify(self.output)

    def test_missing_marker_rejected(self):
        self.package(); (self.output / 'VERIFIED.json').unlink()
        with self.assertRaises(OSError): pack.verify(self.output)

    def test_license_required(self):
        (self.build / 'licenses' / 'LumaText' / 'LICENSE').unlink()
        with self.assertRaisesRegex(pack.PackageError, 'empty license'): self.plan()

    def test_system_shadow_rejected(self):
        image(self.build / 'kernel32.dll')
        with self.assertRaisesRegex(pack.PackageError, 'shadows system'): self.plan()

    def test_bounded_headers_and_import_terminator(self):
        path = self.base / 'broken.dll'; full = (self.build / 'normal.dll').read_bytes()
        for size in (0, 2, 63, 127, 128, 151, 300, 511, len(full) - 1):
            path.write_bytes(full[:size])
            with self.assertRaises(PEError): PE(path)
        image(path, normal=['kernel32.dll']); data = bytearray(path.read_bytes())
        struct.pack_into('<I', data, 0x98 + 112 + 8 + 4, 20); path.write_bytes(data)
        with self.assertRaisesRegex(PEError, 'terminator'): PE(path)

    def test_manifest_duplicate_and_unknown_contract(self):
        self.package(); path = self.output / 'manifest.json'; manifest = json.loads(path.read_text(encoding='utf-8'))
        manifest['files'].append(manifest['files'][0]); path.write_text(json.dumps(manifest), encoding='utf-8')
        with self.assertRaisesRegex(pack.PackageError, 'duplicate'): pack.verify(self.output)
        manifest['files'].pop(); manifest['contract']['picker_ui_abi'] = 99; path.write_text(json.dumps(manifest), encoding='utf-8')
        with self.assertRaisesRegex(pack.PackageError, 'contract'): pack.verify(self.output)

if __name__ == '__main__': unittest.main(verbosity=2)
