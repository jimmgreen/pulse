"""Pure-file installer dual-architecture payload tests; synthetic PEs are never run."""
import hashlib
import json
import lzma
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

import pack_installer as installer
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'pack_dialog_experiment'))
import pack as dialog
from test_pack import image


def sources(base):
    repo = base / 'repo'
    repo.mkdir()
    for relative in dialog.REPO_LICENSES:
        path = repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('isolated fixture license\n')
    builds = {}
    for architecture, machine in dialog.MACHINES.items():
        build = base / ('build-' + architecture)
        build.mkdir()
        names = set(dialog.roots(architecture, True)) | set(dialog.DYNAMIC) | {'pulse_launcher.exe'}
        names.update(Path(pattern).name for pattern, _ in installer.DEFAULT_LAYOUT
                     if pattern.startswith('{build}/') and not pattern.endswith('/*'))
        for name in names:
            image(build / name, machine=machine, normal=['kernel32.dll'])
        for group in ('LumaText', 'PDFium'):
            folder = build / 'licenses' / group
            folder.mkdir(parents=True)
            (folder / 'LICENSE').write_text('isolated fixture license\n')
        builds[architecture] = build
    return repo, builds


def stage(repo, builds, output, version='dual-test', companion=True, thin=False):
    if thin or not companion:
        return installer.package_installed_dual(repo, builds['x64'], output, version,
            installer.collect(repo, builds['x64']), builds['x86'] if companion else None)
    # Retain legacy full schema-1 fixtures and their existing validation cases.
    items = installer.collect(repo, builds['x86'])
    dialog.plan(builds['x86'], version, 'x86', repo, automatic=True, runtime_items=items)
    installer.package_installed_dual(repo, builds['x64'], output, version,
        installer.collect(repo, builds['x64']))
    pointer = dialog.package_addressed(builds['x86'], output / 'DialogBridge', version, 'x86', repo, items)
    (output / 'dialog-x86-current.json').write_text(json.dumps(pointer))
    return installer.verify_dialog_payload(output)


def write_native_fixtures(output):
    output = output.absolute()
    if output.exists():
        raise ValueError('NEW fixture directory required')
    output.mkdir()
    repo, builds = sources(output)
    result = {}
    for name in ('A', 'B', 'C'):
        for dual in (False, True):
            destination = output / (('dual-' if dual else 'single-') + name)
            stage(repo, builds, destination, 'version-' + name, dual)
            if not dual and name == 'A':
                for filename in ('lumatext.dll', 'pdfium.dll'):
                    shutil.copyfile(builds['x64'] / filename, destination / filename)
            result[destination.name] = str(destination)
    (output / 'fixtures.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


class DualDialogPayloadTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pulse-installer-dual-')
        self.base = Path(self.temporary.name)
        self.repo, self.builds = sources(self.base)
        self.output = self.base / 'installed'
    def tearDown(self):
        self.temporary.cleanup()
    def test_single_pointer_remains_unchanged(self):
        primary = stage(self.repo, self.builds, self.output, companion=False)
        self.assertEqual(set(primary), {'schema', 'directory', 'manifest_sha256'})
        self.assertFalse((self.output / 'dialog-x86-current.json').exists())
        self.assertEqual(installer.verify_dialog_payload(self.output), primary)
    def test_complete_architecture_pure_companion(self):
        primary = stage(self.repo, self.builds, self.output)
        secondary = json.loads((self.output / 'dialog-x86-current.json').read_text())
        self.assertEqual(set(primary), set(secondary))
        self.assertNotEqual(primary['directory'], secondary['directory'])
        for architecture, pointer in [('x64', primary), ('x86', secondary)]:
            manifest = dialog.verify(self.output / pointer['directory'])
            self.assertEqual(manifest['schema'], 1)
            self.assertEqual(manifest['architecture'], architecture)
            self.assertEqual(pointer['manifest_sha256'], dialog.digest(self.output / pointer['directory'] / 'manifest.json'))
            self.assertTrue(all((self.output / pointer['directory'] / name).is_file() for name in dialog.roots(architecture, True)))
        self.assertEqual({p.name for p in self.output.iterdir()}, {'pulse.exe', 'dialog-current.json', 'dialog-x86-current.json', 'DialogBridge'})
    def test_wrong_architecture_fails_before_destination_creation(self):
        image(self.builds['x86'] / 'pulse.exe', machine=dialog.MACHINES['x64'])
        with self.assertRaisesRegex(dialog.PackageError, 'wrong architecture'):
            stage(self.repo, self.builds, self.output)
        self.assertFalse(self.output.exists())
    def test_x86_main_cannot_claim_x86_companion(self):
        with self.assertRaisesRegex(dialog.PackageError, 'x64 main'):
            installer.package_installed_dual(self.repo, self.builds['x86'], self.output, 'same',
                installer.collect(self.repo, self.builds['x86']), self.builds['x86'])
        self.assertFalse(self.output.exists())
    def test_missing_companion_closure_never_borrows_main_binary(self):
        (self.builds['x86'] / 'PulseDialogAuto32.dll').unlink()
        with self.assertRaisesRegex(dialog.PackageError, 'missing dependency'):
            stage(self.repo, self.builds, self.output)
        self.assertFalse(self.output.exists())
    def test_pointer_cannot_select_primary_or_escape_hash_directory(self):
        primary = stage(self.repo, self.builds, self.output)
        path = self.output / 'dialog-x86-current.json'
        path.write_text(json.dumps(primary))
        with self.assertRaisesRegex(dialog.PackageError, 'x86'):
            installer.verify_dialog_payload(self.output)
        primary['directory'] = '../outside'
        path.write_text(json.dumps(primary))
        with self.assertRaisesRegex(dialog.PackageError, 'directory/hash'):
            installer.verify_dialog_payload(self.output)
    def test_tampered_companion_and_duplicate_pointer_keys_fail(self):
        stage(self.repo, self.builds, self.output)
        path = self.output / 'dialog-x86-current.json'
        pointer = json.loads(path.read_text())
        binary = self.output / pointer['directory'] / 'PulseDialogAuto32.dll'
        binary.write_bytes(binary.read_bytes() + b'changed')
        with self.assertRaises(dialog.PackageError):
            installer.verify_dialog_payload(self.output)
        path.write_text('{"schema":1,"schema":1,"directory":"x","manifest_sha256":"x"}')
        with self.assertRaises(ValueError):
            installer.verify_dialog_payload(self.output)
    def test_valid_old_companion_cannot_mix_with_new_primary(self):
        old = self.base / 'old'
        stage(self.repo, self.builds, old, version='older')
        stage(self.repo, self.builds, self.output, version='newer')
        pointer = json.loads((old / 'dialog-x86-current.json').read_text())
        shutil.copytree(old / pointer['directory'], self.output / pointer['directory'])
        (self.output / 'dialog-x86-current.json').write_text(json.dumps(pointer))
        with self.assertRaisesRegex(dialog.PackageError, 'version'):
            installer.verify_dialog_payload(self.output)
    def test_installer_stream_roundtrip_preserves_both_bound_pointers(self):
        stage(self.repo, self.builds, self.output)
        items = [(p, p.relative_to(self.output).as_posix()) for p in sorted(self.output.rglob('*')) if p.is_file()]
        compressed, _ = installer.build_payload(items, 'release-test')
        raw = lzma.decompress(compressed)
        length, = struct.unpack_from('<I', raw, 4)
        manifest = json.loads(raw[8:8 + length])
        extracted = self.base / 'extracted'
        extracted.mkdir()
        cursor = 8 + length
        for entry in manifest['files']:
            data = raw[cursor:cursor + entry['size']]
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry['sha256'])
            target = extracted / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            cursor += entry['size']
        self.assertEqual(cursor, len(raw))
        self.assertEqual(installer.verify_dialog_payload(extracted), installer.verify_dialog_payload(self.output))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--write-fixtures':
        write_native_fixtures(Path(sys.argv[2]))
    else:
        unittest.main()
