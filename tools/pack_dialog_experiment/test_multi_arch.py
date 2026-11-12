"""Separate PE closures and fake-native transactional registration regression tests."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import multi_arch
import pack
from test_pack import image


def state(enabled=False, path='', conflict=False, error=0):
    return {'error': error, 'enabled': enabled, 'conflict': conflict,
            'status_error': 0, 'dll_path': path}


class FakeControl:
    def __init__(self, directory):
        self.directory = directory
        self.states = {arch: state() for arch in multi_arch.ARCHITECTURES}
        self.calls = []
        self.fail = None
        self.rollback_fail = False

    def __call__(self, directory, arch, command):
        self.calls.append((arch, command))
        if command == 'status':
            return copy.deepcopy(self.states[arch])
        if self.fail == (arch, command):
            self.fail = None
            self.states[arch] = state(command == 'disable', multi_arch.expected_dll(directory, arch), True, 5)
        elif self.rollback_fail and command == 'disable':
            self.states[arch] = state(True, multi_arch.expected_dll(directory, arch), True, 183)
        else:
            self.states[arch] = state(command == 'enable', multi_arch.expected_dll(directory, arch))
        return copy.deepcopy(self.states[arch])


class MultiArchitectureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pulse-dialog-multiarch-synthetic-')
        self.base = Path(self.temp.name).resolve()
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        for relative in pack.REPO_LICENSES:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture license', encoding='utf-8')
        self.builds = {}
        for arch, machine in pack.MACHINES.items():
            build = self.base / ('build-' + arch)
            build.mkdir()
            self.builds[arch] = build
            for group in ('LumaText', 'PDFium'):
                folder = build / 'licenses' / group
                folder.mkdir(parents=True)
                (folder / 'LICENSE').write_text('fixture license', encoding='utf-8')
            for name in pack.roots(arch, automatic=True) + list(pack.DYNAMIC):
                image(build / name, machine, normal=['kernel32.dll'])
        self.output = self.base / 'dual'
        self.index = multi_arch.package(self.builds, self.output, 'dual-1', self.repo)
        self.control = FakeControl(self.output)

    def tearDown(self):
        self.temp.cleanup()

    def rewrite_index(self):
        (self.output / multi_arch.INDEX_NAME).write_text(json.dumps(self.index), encoding='utf-8')

    def operate(self, command, architectures=multi_arch.ARCHITECTURES):
        return multi_arch.operate(self.output, command, architectures, self.control)

    def test_two_valid_separate_closures(self):
        self.assertEqual(self.index, multi_arch.verify(self.output))
        for arch in multi_arch.ARCHITECTURES:
            manifest = pack.verify(self.output / arch)
            self.assertEqual(manifest['architecture'], arch)
            self.assertEqual({item['machine'] for item in manifest['files'] if item['role'] != 'license'}, {pack.MACHINES[arch]})
            self.assertEqual(self.index['architectures'][arch]['manifest_sha256'], pack.digest(self.output / arch / 'manifest.json'))

    def test_mixed_pe_still_rejected(self):
        image(self.builds['x86'] / multi_arch.CONTROL_NAME, pack.MACHINES['x64'])
        with self.assertRaisesRegex(pack.PackageError, 'wrong architecture'):
            multi_arch.package(self.builds, self.base / 'invalid', 'dual-1', self.repo)
        self.assertFalse((self.base / 'invalid').exists())

    def test_missing_build_and_existing_output_rejected(self):
        with self.assertRaisesRegex(pack.PackageError, 'both x64 and x86'):
            multi_arch.package({'x64': self.builds['x64']}, self.base / 'other', 'dual-1', self.repo)
        with self.assertRaisesRegex(pack.PackageError, 'already exists'):
            multi_arch.package(self.builds, self.output, 'dual-1', self.repo)

    def test_bound_manifest_bytes_and_extra_root_entries(self):
        path = self.output / 'x86' / 'manifest.json'
        original = path.read_bytes()
        path.write_bytes(original + b'\n')
        with self.assertRaisesRegex(pack.PackageError, 'manifest hash mismatch'):
            multi_arch.verify(self.output)
        path.write_bytes(original)
        (self.output / 'unexpected.txt').write_text('unexpected')
        with self.assertRaisesRegex(pack.PackageError, 'root entries'):
            multi_arch.verify(self.output)

    def test_index_requires_both_known_architectures_and_canonical_paths(self):
        original = copy.deepcopy(self.index)
        for mutation in ('missing', 'path', 'hash', 'schema', 'version'):
            self.index = copy.deepcopy(original)
            if mutation == 'missing': del self.index['architectures']['x86']
            if mutation == 'path': self.index['architectures']['x86']['directory'] = '../x86'
            if mutation == 'hash': self.index['architectures']['x86']['manifest_sha256'] = 'A' * 64
            if mutation == 'schema': self.index['schema'] = True
            if mutation == 'version': self.index['version'] = 'different'
            self.rewrite_index()
            with self.subTest(mutation=mutation), self.assertRaises(pack.PackageError):
                multi_arch.verify(self.output)

    def test_duplicate_json_and_swapped_architecture_rejected(self):
        path = self.output / multi_arch.INDEX_NAME
        path.write_text('{"schema":1,"schema":1}', encoding='utf-8')
        with self.assertRaisesRegex(pack.PackageError, 'duplicate JSON'):
            multi_arch.verify(self.output)
        self.rewrite_index()
        shutil.rmtree(self.output / 'x86')
        shutil.copytree(self.output / 'x64', self.output / 'x86')
        self.index['architectures']['x86']['manifest_sha256'] = pack.digest(self.output / 'x86' / 'manifest.json')
        self.rewrite_index()
        with self.assertRaisesRegex(pack.PackageError, 'architecture/version mismatch'):
            multi_arch.verify(self.output)

    def test_unselected_architecture_tamper_blocks_all_execution(self):
        path = self.output / 'x86' / multi_arch.CONTROL_NAME
        path.write_bytes(path.read_bytes() + b'tamper')
        with self.assertRaisesRegex(pack.PackageError, 'metadata/hash/import mismatch'):
            self.operate('enable', ('x64',))
        self.assertEqual(self.control.calls, [])

    def test_enable_disable_and_architecture_selection(self):
        self.assertEqual(self.operate('enable', ('x86',))['result'], 'ok')
        self.assertFalse(self.control.states['x64']['enabled'])
        self.assertTrue(self.control.states['x86']['enabled'])
        self.assertEqual(self.operate('disable', ('x86',))['result'], 'ok')
        self.assertFalse(self.control.states['x86']['enabled'])
        self.assertNotIn(('x64', 'enable'), self.control.calls)
        self.assertNotIn(('x64', 'disable'), self.control.calls)

    def test_conflict_preflight_does_not_mutate_either_view(self):
        self.control.states['x86'] = state(conflict=True)
        report = self.operate('enable')
        self.assertEqual(report['result'], 'failed')
        self.assertEqual(self.control.calls, [('x64', 'status'), ('x86', 'status')])

    def test_other_installation_is_not_overwritten_or_disabled(self):
        self.control.states['x64'] = state(True, str(self.base / 'another' / 'PulseDialogAuto64.dll'))
        for command in ('enable', 'disable'):
            self.control.calls.clear()
            self.assertEqual(self.operate(command)['result'], 'failed')
            self.assertEqual(self.control.calls, [('x64', 'status'), ('x86', 'status')])

    def test_failed_enable_rolls_back_current_partial_and_prior_view(self):
        self.control.fail = ('x86', 'enable')
        report = self.operate('enable')
        self.assertEqual(report['result'], 'rolled-back')
        self.assertFalse(any(value['enabled'] for value in self.control.states.values()))
        self.assertEqual([(item['architecture'], item['command']) for item in report['rollback']],
                         [('x86', 'disable'), ('x64', 'disable')])

    def test_enable_rollback_preserves_preexisting_enabled_view(self):
        self.control.states['x64'] = state(True, multi_arch.expected_dll(self.output, 'x64'))
        self.control.fail = ('x86', 'enable')
        report = self.operate('enable')
        self.assertEqual(report['result'], 'rolled-back')
        self.assertTrue(self.control.states['x64']['enabled'])
        self.assertEqual([(item['architecture'], item['command']) for item in report['rollback']], [('x86', 'disable')])

    def test_failed_disable_reenables_previously_enabled_views(self):
        self.operate('enable')
        self.control.fail = ('x86', 'disable')
        report = self.operate('disable')
        self.assertEqual(report['result'], 'rolled-back')
        self.assertTrue(all(value['enabled'] for value in self.control.states.values()))
        self.assertEqual([(item['architecture'], item['command']) for item in report['rollback']],
                         [('x86', 'enable'), ('x64', 'enable')])

    def test_rollback_failure_is_explicit(self):
        self.control.fail = ('x86', 'enable')
        self.control.rollback_fail = True
        report = self.operate('enable')
        self.assertEqual(report['result'], 'rollback-failed')
        self.assertEqual(len(report['rollback']), 2)

    def test_status_reports_both_views_without_mutation(self):
        report = self.operate('status')
        self.assertEqual(report['result'], 'ok')
        self.assertEqual(set(report['architectures']), {'x64', 'x86'})
        self.assertEqual(self.control.calls, [('x64', 'status'), ('x86', 'status')])

    def test_status_error_and_exception_are_visible(self):
        self.control.states['x86']['status_error'] = 5
        self.assertEqual(self.operate('status')['result'], 'failed')
        def failing(directory, arch, command):
            raise subprocess.TimeoutExpired(command, 45)
        report = multi_arch.operate(self.output, 'enable', invoke=failing)
        self.assertEqual(report['result'], 'failed')
        self.assertTrue(all('failure' in entry for entry in report['operations']))

    def test_native_json_protocol_scoped_disable_and_exit_validation(self):
        response = subprocess.CompletedProcess([], 0, json.dumps(state()), '')
        with mock.patch.object(multi_arch.subprocess, 'run', return_value=response) as run:
            multi_arch.invoke_helper(self.output, 'x86', 'disable')
            self.assertEqual(run.call_args.args[0], [str(self.output / 'x86' / multi_arch.CONTROL_NAME), '--disable-for-json', str(self.output / 'x86')])
            self.assertEqual(run.call_args.kwargs['cwd'], self.output / 'x86')
            response.returncode = 1
            with self.assertRaisesRegex(pack.PackageError, 'exit/result mismatch'):
                multi_arch.invoke_helper(self.output, 'x86', 'disable')


if __name__ == '__main__':
    unittest.main()
