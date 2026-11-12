"""Schema-3 companion tests use synthetic PEs and never execute binaries."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest

from test_pack_installer_dual import dialog, image, installer, sources, stage


class ThinDialogPayloadTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pulse-thin-pack-')
        self.base = Path(self.temporary.name)
        self.repo, self.builds = sources(self.base)
        self.output = self.base / 'installed'

    def tearDown(self):
        self.temporary.cleanup()

    def package(self):
        self.primary = stage(self.repo, self.builds, self.output, thin=True)
        self.pointer = json.loads((self.output / 'dialog-x86-current.json').read_text())
        self.bundle = self.output / self.pointer['directory']
        return dialog.verify(self.bundle)

    def rewrite(self, mutation):
        path = self.bundle / 'manifest.json'
        manifest = json.loads(path.read_text())
        mutation(manifest)
        path.write_text(json.dumps(manifest))
        (self.bundle / 'VERIFIED.json').write_text(json.dumps({
            'manifest_sha256': dialog.digest(path), 'integrity_only': True}))

    def test_default_dual_build_is_thin_and_primary_stays_complete(self):
        manifest = self.package()
        self.assertEqual(manifest['schema'], 3)
        self.assertEqual(manifest['architecture'], 'x86')
        self.assertEqual(manifest['dynamic_helpers'], {})
        self.assertEqual(manifest['host_bundle'], {'manifest_sha256': self.primary['manifest_sha256']})
        self.assertEqual({f['path'] for f in manifest['files']}, set(dialog.thin_roots()) | set(dialog.THIN_LICENSES.values()))
        main = dialog.verify(self.output / self.primary['directory'])
        self.assertEqual(main['schema'], 1)
        self.assertTrue(all((self.output / self.primary['directory'] / name).is_file() for name in dialog.roots('x64', True)))
        self.assertEqual(installer.verify_dialog_payload(self.output), self.primary)

    def test_only_four_companion_sources_and_two_licenses_are_needed(self):
        build = self.builds['x86']
        for f in build.iterdir():
            if f.is_dir(): shutil.rmtree(f)
            elif f.name not in dialog.thin_roots(): f.unlink()
        self.package()

    def test_pe_and_delayed_dependencies_are_closed(self):
        image(self.builds['x86'] / 'PulseDialogAuto32.dll', machine=332, normal=['helper.dll'], delay=['late.dll'])
        image(self.builds['x86'] / 'helper.dll', machine=332)
        image(self.builds['x86'] / 'late.dll', machine=332, normal=['kernel32.dll'])
        manifest = self.package()
        self.assertTrue({'helper.dll', 'late.dll'}.issubset({f['path'] for f in manifest['files']}))

    def test_missing_root_rejected_before_output_creation(self):
        (self.builds['x86'] / 'PulseDialogAuto32.dll').unlink()
        with self.assertRaisesRegex(dialog.PackageError, 'missing dependency'):
            self.package()
        self.assertFalse(self.output.exists())

    def test_wrong_pe_architecture_rejected_before_output_creation(self):
        image(self.builds['x86'] / 'PulseDialogAuto32.dll', machine=34404)
        with self.assertRaisesRegex(dialog.PackageError, 'wrong architecture'):
            self.package()
        self.assertFalse(self.output.exists())

    def test_tampered_root_rejected(self):
        self.package()
        f = self.bundle / 'PulseDialogAuto32.dll'
        f.write_bytes(f.read_bytes() + b'tamper')
        with self.assertRaisesRegex(dialog.PackageError, 'metadata/hash/import'):
            dialog.verify(self.bundle)

    def test_missing_host_rejected(self):
        self.package()
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256='a' * 64))
        with self.assertRaises(OSError): dialog.verify(self.bundle)

    def test_host_path_and_extra_binding_fields_rejected(self):
        self.package()
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256='../escape'))
        with self.assertRaisesRegex(dialog.PackageError, 'host manifest hash'): dialog.verify(self.bundle)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=self.primary['manifest_sha256'], directory='../escape'))
        with self.assertRaisesRegex(dialog.PackageError, 'host binding'): dialog.verify(self.bundle)

    def test_host_manifest_tamper_rejected(self):
        self.package()
        f = self.output / self.primary['directory'] / 'manifest.json'
        f.write_bytes(f.read_bytes() + b' ')
        with self.assertRaisesRegex(dialog.PackageError, 'host manifest hash mismatch'): dialog.verify(self.bundle)

    def test_host_binary_tamper_rejected(self):
        self.package()
        f = self.output / self.primary['directory'] / 'pulse.exe'
        f.write_bytes(f.read_bytes() + b'tamper')
        with self.assertRaisesRegex(dialog.PackageError, 'metadata/hash/import'): dialog.verify(self.bundle)

    def test_x86_host_rejected(self):
        self.package()
        pointer = dialog.package_addressed(self.builds['x86'], self.bundle.parent, 'dual-test', 'x86', self.repo)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=pointer['manifest_sha256']))
        with self.assertRaisesRegex(dialog.PackageError, 'full x64 schema 1'): dialog.verify(self.bundle)

    def test_nonautomatic_host_rejected(self):
        self.package()
        host = self.bundle.parent / 'new-host'
        dialog.package(self.builds['x64'], host, 'dual-test', 'x64', self.repo)
        digest = dialog.digest(host / 'manifest.json')
        host.rename(host.parent / digest)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=digest))
        with self.assertRaisesRegex(dialog.PackageError, 'complete automatic'): dialog.verify(self.bundle)

    def test_wrong_host_version_rejected(self):
        self.package()
        pointer = dialog.package_addressed(self.builds['x64'], self.bundle.parent, 'other', 'x64', self.repo)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=pointer['manifest_sha256']))
        with self.assertRaisesRegex(dialog.PackageError, 'version'): dialog.verify(self.bundle)

    def test_thin_host_chain_rejected_before_recursing(self):
        self.package()
        image(self.builds['x86'] / 'PulseDialogAuto32.dll', machine=332, normal=['user32.dll'])
        second = dialog.package_addressed(self.builds['x86'], self.bundle.parent, 'dual-test', 'x86', self.repo,
                                         thin=True, host_digest=self.primary['manifest_sha256'])
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=second['manifest_sha256']))
        with self.assertRaisesRegex(dialog.PackageError, 'host must be full schema 1'):
            dialog.verify(self.bundle)

    def test_schema2_host_rejected(self):
        self.package()
        original = self.output / self.primary['directory']
        host = self.bundle.parent / 'schema2-host'
        shutil.copytree(original, host)
        path = host / 'manifest.json'
        manifest = json.loads(path.read_text())
        manifest['schema'] = 2
        path.write_text(json.dumps(manifest))
        digest = dialog.digest(path)
        (host / 'VERIFIED.json').write_text(json.dumps({'manifest_sha256': digest, 'integrity_only': True}))
        host.rename(host.parent / digest)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=digest))
        with self.assertRaisesRegex(dialog.PackageError, 'full x64 schema 1'): dialog.verify(self.bundle)

    def test_unrelated_binary_rejected(self):
        self.package()
        extra = self.bundle / 'extra.dll'
        image(extra, machine=332)
        self.rewrite(lambda m: m['files'].append(dialog.record(extra, 'extra.dll', 'entry', 'x86')))
        with self.assertRaisesRegex(dialog.PackageError, 'unrelated binary'): dialog.verify(self.bundle)

    def test_thin_cannot_be_primary_pointer(self):
        self.package()
        (self.output / 'dialog-current.json').write_text(json.dumps(self.pointer))
        with self.assertRaisesRegex(dialog.PackageError, 'requires schema 1'): dialog.verify_installed(self.output)

    def test_wrong_thin_manifest_architecture_rejected(self):
        self.package()
        self.rewrite(lambda m: m.update(architecture='x64'))
        with self.assertRaisesRegex(dialog.PackageError, 'component contract'): dialog.verify(self.bundle)

    def test_missing_root_even_with_recomputed_manifest_rejected(self):
        self.package()
        (self.bundle / 'PulseDialogAuto32.dll').unlink()
        self.rewrite(lambda m: m.update(files=[f for f in m['files'] if f['path'] != 'PulseDialogAuto32.dll']))
        with self.assertRaisesRegex(dialog.PackageError, 'mandatory component'): dialog.verify(self.bundle)

    def test_current_primary_binding_is_exact_even_for_same_version(self):
        self.package()
        image(self.builds['x64'] / 'pulse.exe', normal=['user32.dll'])
        other = dialog.package_addressed(self.builds['x64'], self.bundle.parent, 'dual-test', 'x64', self.repo)
        self.rewrite(lambda m: m['host_bundle'].update(manifest_sha256=other['manifest_sha256']))
        digest = dialog.digest(self.bundle / 'manifest.json')
        renamed = self.bundle.parent / digest
        self.bundle.rename(renamed)
        self.pointer.update(directory='DialogBridge/' + digest, manifest_sha256=digest)
        (self.output / 'dialog-x86-current.json').write_text(json.dumps(self.pointer))
        dialog.verify(renamed)
        with self.assertRaisesRegex(dialog.PackageError, 'current primary host'): installer.verify_dialog_payload(self.output)


if __name__ == '__main__':
    unittest.main()
