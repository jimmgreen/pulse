"""Pure-file synthetic PE fixtures: never execute or load an image."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import ctypes
import os
import threading
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

    def installed(self):
        image(self.build / 'pulse_launcher.exe', normal=['kernel32.dll'])
        for name in pack.roots('x64', True)[-2:]: image(self.build / name)
        return pack.package_installed(self.build, self.output, 'same-version', 'x64', self.repo,
                                      [(self.build / 'pulse.exe', 'pulse.exe'),
                                       (self.build / 'lumatext.dll', 'lumatext.dll')])

    def test_installed_private_schema1_closure(self):
        pointer = self.installed()
        self.assertEqual(pointer, pack.verify_installed(self.output))
        self.assertEqual(set(pointer), {'schema', 'directory', 'manifest_sha256'})
        bundle = self.output / pointer['directory']
        self.assertEqual(pointer['directory'], 'DialogBridge/' + pack.digest(bundle / 'manifest.json'))
        manifest = pack.verify(bundle)
        self.assertEqual(manifest['schema'], 1)
        self.assertTrue((bundle / 'pulse.exe').is_file())
        self.assertTrue((bundle / 'normal.dll').is_file())
        self.assertTrue((bundle / 'delayed.dll').is_file())
        self.assertFalse((self.output / 'manifest.json').exists())
        self.assertFalse((self.output / 'PulseDialogAuto64.dll').exists())
        # Neither the private host nor its dependency resolution uses root files.
        image(self.output / 'pulse.exe', normal=['user32.dll'])
        self.assertFalse((self.output / 'lumatext.dll').exists())
        self.assertEqual(pointer, pack.verify_installed(self.output))

    def test_same_content_reused_same_version_new_build_separated(self):
        first = self.installed()
        bridge = self.output / 'DialogBridge'
        old_manifest = self.output / first['directory'] / 'manifest.json'
        before = old_manifest.stat().st_mtime_ns
        reused = pack.package_addressed(self.build, bridge, 'same-version', 'x64', self.repo)
        self.assertEqual(first, reused)
        self.assertEqual(old_manifest.stat().st_mtime_ns, before)
        source = self.build / 'pulse.exe'
        source.write_bytes(source.read_bytes() + b'new build same version')
        second = pack.package_addressed(self.build, bridge, 'same-version', 'x64', self.repo)
        self.assertNotEqual(first['directory'], second['directory'])
        self.assertEqual(len(list(bridge.iterdir())), 2)
        self.assertEqual(pack.verify(old_manifest.parent)['version'], 'same-version')
        self.assertEqual(pack.verify(self.output / second['directory'])['version'], 'same-version')

    def test_all_runtime_files_share_private_closure(self):
        for name in pack.roots('x64', True)[-2:]: image(self.build / name)
        image(self.build / 'pulse_launcher.exe', normal=['kernel32.dll'])
        image(self.build / 'Pulse.AI.exe', normal=['runtime-only.dll'])
        image(self.build / 'runtime-only.dll', normal=['kernel32.dll'])
        pointer = pack.package_installed(self.build, self.output, 'same-version', 'x64', self.repo,
            [(self.build / name, name) for name in ('pulse.exe', 'lumatext.dll', 'Pulse.AI.exe')])
        private = self.output / pointer['directory']
        self.assertEqual({p.name for p in self.output.iterdir()}, {'pulse.exe', 'dialog-current.json', 'DialogBridge'})
        self.assertEqual((self.output / 'pulse.exe').read_bytes(), (self.build / 'pulse_launcher.exe').read_bytes())
        self.assertTrue((private / 'Pulse.AI.exe').is_file())
        self.assertTrue((private / 'runtime-only.dll').is_file())
        self.assertEqual((private / 'pulse.exe').read_bytes(), (self.build / 'pulse.exe').read_bytes())
        pack.verify_installed(self.output)

    def test_installed_pointer_rejects_hash_paths_schema_and_extra_keys(self):
        pointer = self.installed()
        path = self.output / 'dialog-current.json'
        invalid = [dict(pointer, schema=True), dict(pointer, schema=2), dict(pointer, extra=1),
                   dict(pointer, directory='../bundle'), dict(pointer, directory='DialogBridge/' + '0' * 64),
                   dict(pointer, manifest_sha256=pointer['manifest_sha256'].upper()),
                   dict(pointer, directory='DialogBridge/' + '0' * 64, manifest_sha256='0' * 64)]
        for bad in invalid:
            with self.subTest(bad=bad):
                path.write_text(json.dumps(bad), encoding='utf-8')
                with self.assertRaises((pack.PackageError, OSError)): pack.verify_installed(self.output)
        path.write_text('{"schema":1,"schema":1}', encoding='utf-8')
        with self.assertRaisesRegex(pack.PackageError, 'duplicate'): pack.verify_installed(self.output)

    def test_private_missing_dependency_never_resolves_from_root(self):
        pointer = self.installed()
        bundle = self.output / pointer['directory']
        (self.output / 'normal.dll').write_bytes((bundle / 'normal.dll').read_bytes())
        (bundle / 'normal.dll').unlink()
        with self.assertRaises(OSError): pack.verify_installed(self.output)

    def test_pointer_binds_raw_manifest_bytes(self):
        pointer = self.installed()
        manifest = self.output / pointer['directory'] / 'manifest.json'
        manifest.write_bytes(manifest.read_bytes() + b'\n')
        with self.assertRaisesRegex(pack.PackageError, 'manifest hash mismatch'): pack.verify_installed(self.output)

    def test_no_flat_bridge_components_or_corrupt_reuse(self):
        pointer = self.installed()
        flat = self.output / 'PulsePickerUiProof.dll'
        image(flat)
        with self.assertRaisesRegex(pack.PackageError, 'flat dialog'): pack.verify_installed(self.output)
        flat.unlink()
        private = self.output / pointer['directory'] / 'pulse.exe'
        private.write_bytes(private.read_bytes() + b'tamper')
        with self.assertRaisesRegex(pack.PackageError, 'mismatch'):
            pack.package_addressed(self.build, self.output / 'DialogBridge', 'same-version', 'x64', self.repo)

    def test_publication_retries_only_bounded_windows_sharing_failures(self):
        original = Path.rename
        attempts = []
        def temporarily_busy(source, destination):
            attempts.append(1)
            if len(attempts) <= 2:
                error = PermissionError('temporary sharing denial'); error.winerror = 5
                raise error
            return original(source, destination)
        with mock.patch.object(Path, 'rename', temporarily_busy), mock.patch.object(pack.time, 'sleep') as sleep:
            pointer = self.installed()
            self.assertEqual(pointer, pack.verify_installed(self.output))
            self.assertEqual(sleep.call_args_list, [mock.call(0.05), mock.call(0.1)])
            self.assertEqual(len(attempts), 3)

    def test_permanent_publication_denial_never_publishes_pointer(self):
        error = PermissionError('persistent access denial'); error.winerror = 5
        with mock.patch.object(Path, 'rename', side_effect=error) as rename, mock.patch.object(pack.time, 'sleep') as sleep:
            with self.assertRaises(PermissionError): self.installed()
            self.assertEqual(rename.call_count, 6)
            self.assertEqual(sleep.call_count, 5)
        self.assertFalse((self.output / 'dialog-current.json').exists())
        self.assertEqual(list((self.output / 'DialogBridge').iterdir()), [])

    def test_publication_other_error_is_not_retried(self):
        error = OSError('disk failure'); error.winerror = 112
        with mock.patch.object(Path, 'rename', side_effect=error) as rename, mock.patch.object(pack.time, 'sleep') as sleep:
            with self.assertRaises(OSError): self.installed()
            self.assertEqual(rename.call_count, 1)
            sleep.assert_not_called()
        self.assertFalse((self.output / 'dialog-current.json').exists())

    def test_post_publication_verification_still_rejects_mutation(self):
        original = pack.publish_directory
        def changed(source, destination):
            original(source, destination)
            image = destination / 'pulse.exe'
            image.write_bytes(image.read_bytes() + b'changed after publish')
        with mock.patch.object(pack, 'publish_directory', changed):
            with self.assertRaisesRegex(pack.PackageError, 'mismatch'): self.installed()
        self.assertFalse((self.output / 'dialog-current.json').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows directory sharing semantics')
    def test_real_windows_directory_lease_released_during_retry(self):
        source, destination = self.base / 'held-directory', self.base / 'published-directory'
        source.mkdir(); (source / 'fixture').write_bytes(b'preserved')
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(source), 0x80000000, 3, None, 3, 0x02000000, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        try:
            with self.assertRaises(OSError) as denied: source.rename(destination)
            self.assertIn(denied.exception.winerror, (5, 32, 33))
            # Release the real no-delete-sharing lease while publication retries.
            release = threading.Timer(0.1, lambda: kernel.CloseHandle(handle))
            release.start()
            try: pack.publish_directory(source, destination)
            finally: release.join()
        except BaseException:
            # If the precondition failed before the timer, release our own lease.
            if 'release' not in locals(): kernel.CloseHandle(handle)
            raise
        self.assertEqual((destination / 'fixture').read_bytes(), b'preserved')

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

    def test_system_dependencies_match_native_verifier(self):
        source = (Path(__file__).resolve().parents[2] / 'src/common/dialog_bundle.cpp').read_text(encoding='utf-8')
        native = source.split('static const std::string names = R"(', 1)[1].split(')";', 1)[0]
        self.assertEqual(pack.SYSTEM, set(native.split()))

    def test_accessibility_uses_windows_runtime(self):
        image(self.build / 'normal.dll', normal=['oleacc.dll'])
        manifest, sources = self.plan()
        self.assertIn('oleacc.dll', manifest['system_dependencies'])
        self.assertNotIn('oleacc.dll', sources)

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
