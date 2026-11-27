"""Focused Explorer bridge payload checks; synthetic PE files are never executed."""
from pathlib import Path
import unittest
import tempfile
import struct

from test_pack_installer_dual import dialog, installer, sources
from test_pack import image


class ExplorerBridgeLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pulse-explorer-layout-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.repo, self.builds = sources(self.base)
        self.output = self.base / 'installed'

    def bridge_stage(self):
        for arch, entries in [('x64', installer.OPTIONAL_DIALOG_BRIDGE_STAGE_X64),
                              ('x86', installer.OPTIONAL_DIALOG_BRIDGE_STAGE_X86)]:
            for name, _ in entries:
                image(self.builds[arch] / name, machine=dialog.MACHINES[arch])
            self.helper(arch, dialog.MACHINES[arch])

    def helper(self, arch, machine, characteristics=0x0002):
        helper = self.builds[arch] / 'pulse_explorer_bridge.exe'
        image(helper, machine=machine)
        data = bytearray(helper.read_bytes())
        header, = struct.unpack_from('<I', data, 0x3c)
        struct.pack_into('<H', data, header + 22, characteristics)
        helper.write_bytes(data)
        return helper

    def test_explorer_bridge_is_inside_verified_app_runtime(self):
        pointer = installer.package_app_runtime_only(
            self.repo, self.builds['x64'], self.output, 'explorer-test',
            installer.collect(self.repo, self.builds['x64']))
        manifest = dialog.verify_app_pointer(self.output)
        names = {entry['path'] for entry in manifest['files']}
        self.assertIn('pulse_explorer_bridge.exe', names)
        runtime = self.output / pointer['directory']
        self.assertEqual((runtime / 'pulse_explorer_bridge.exe').read_bytes(),
                         (self.builds['x64'] / 'pulse_explorer_bridge.exe').read_bytes())
        self.assertFalse((self.output / 'pulse_explorer_bridge.exe').exists())

    def test_explorer_bridge_missing_fails_collect(self):
        bridge = self.builds['x64'] / 'pulse_explorer_bridge.exe'
        self.assertTrue(bridge.is_file())
        bridge.unlink()
        with self.assertRaisesRegex(SystemExit, 'missing file: .*pulse_explorer_bridge.exe'):
            installer.collect(self.repo, self.builds['x64'])

    def test_explorer_bridge_is_in_release_checks(self):
        root = Path(__file__).resolve().parents[2]
        self.assertIn("'pulse_explorer_bridge.exe'",
                      (root / 'scripts/release_payload.ps1').read_text(encoding='utf-8-sig'))
        script = (root / 'scripts/pack_setup.bat').read_text(encoding='utf-8-sig')
        self.assertIn('pulse_explorer_bridge ', script)
        self.assertIn('pulse_explorer_bridge.exe ', script)

    def test_dual_stage_binds_x86_helper_from_its_own_build(self):
        self.bridge_stage()
        stage = installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])
        helpers = [(source, destination) for source, destination in stage if 'explorer_bridge' in destination]
        self.assertEqual(helpers, [(self.builds['x86'] / 'pulse_explorer_bridge.exe',
                                    'DialogBridgeStage/pulse_explorer_bridge32.exe')])
        self.assertEqual(dialog.PE(helpers[0][0]).machine, dialog.MACHINES['x86'])

    def test_single_arch_stage_does_not_claim_x86_helper(self):
        self.bridge_stage()
        stage = installer.collect_dialog_bridge_stage(self.builds['x64'])
        self.assertFalse(any('explorer_bridge32' in destination for _, destination in stage))

    def test_new_bridge_requires_complete_primary_stage(self):
        with self.assertRaisesRegex(dialog.PackageError, 'complete x64'):
            installer.collect_dialog_bridge_stage(self.builds['x64'])

    def test_missing_x86_helper_is_not_silently_omitted(self):
        self.bridge_stage()
        (self.builds['x86'] / 'pulse_explorer_bridge.exe').unlink()
        with self.assertRaisesRegex(dialog.PackageError, 'missing Explorer bridge component'):
            installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])

    def test_missing_x86_core_is_not_replaced_from_previous_install(self):
        self.bridge_stage()
        (self.builds['x86'] / 'PulseDialogCore32.dll').unlink()
        with self.assertRaisesRegex(dialog.PackageError, 'missing Explorer bridge component'):
            installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])

    def test_x64_helper_cannot_be_labeled_x86(self):
        self.bridge_stage()
        self.helper('x86', dialog.MACHINES['x64'])
        with self.assertRaisesRegex(dialog.PackageError, 'wrong Explorer bridge architecture'):
            installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])

    def test_dll_cannot_be_labeled_helper_executable(self):
        self.bridge_stage()
        self.helper('x86', dialog.MACHINES['x86'], 0x2002)
        with self.assertRaisesRegex(dialog.PackageError, 'must be an executable'):
            installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])

    def test_legacy_helper_free_stage_remains_compatible(self):
        self.bridge_stage()
        for arch in self.builds:
            (self.builds[arch] / 'pulse_explorer_bridge.exe').unlink()
        stage = installer.collect_dialog_bridge_stage(self.builds['x64'], self.builds['x86'])
        self.assertTrue(installer.has_decoupled_dialog_bridge_stage(stage))
        self.assertFalse(any('explorer_bridge' in destination for _, destination in stage))


if __name__ == '__main__':
    unittest.main()
