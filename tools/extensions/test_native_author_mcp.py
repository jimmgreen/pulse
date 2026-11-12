"""Unit and stdio protocol tests for the native SDK author MCP server."""
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
import importlib.util
import subprocess
import sys
from unittest import mock

MODULE_PATH = Path(__file__).with_name('native_author_mcp.py')
spec = importlib.util.spec_from_file_location('native_author_mcp', MODULE_PATH)
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)
workflow_spec = importlib.util.spec_from_file_location('native_author_workflow', Path(__file__).with_name('test_native_author_mcp_workflow.py'))
workflow = importlib.util.module_from_spec(workflow_spec)
workflow_spec.loader.exec_module(workflow)


class NativeAuthorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sdk = self.root/'sdk'
        self.examples = self.sdk/'examples/native'
        self.examples.mkdir(parents=True)
        (self.examples/'catalog.json').write_text(json.dumps({'formatVersion': 1, 'templates': ['inspector', 'rename-panel']}), encoding='utf-8')
        basic = self.examples/'basic'
        basic.mkdir()
        (basic/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\nproject(pulse_basic_plugin LANGUAGES CXX)\nadd_library(pulse_basic SHARED plugin.cpp)\n', encoding='utf-8')
        (basic/'plugin.cpp').write_text('int main() {}', encoding='utf-8')
        for tpl_name, sample_id, cmake_target in (
            ('inspector', 'sample.native-inspector', 'pulse_inspector'),
            ('rename-panel', 'sample.native-rename', 'pulse_rename_panel'),
        ):
            tpl = self.examples/tpl_name
            tpl.mkdir()
            (tpl/'CMakeLists.txt').write_text(
                f'cmake_minimum_required(VERSION 3.25)\nproject({cmake_target}_plugin LANGUAGES CXX)\nadd_library({cmake_target} SHARED plugin.cpp)\n',
                encoding='utf-8',
            )
            (tpl/'manifest.json').write_text(
                json.dumps({'apiVersion': 1, 'id': sample_id, 'name': tpl_name, 'resources': ['context.read']}),
                encoding='utf-8',
            )
            (tpl/'plugin.cpp').write_text('int main() {}', encoding='utf-8')
            (tpl/'README.md').write_text(f'# {sample_id}\nBuild {cmake_target}.dll\n', encoding='utf-8')
        self.catalog = self.sdk/'sdk/native/api-catalog.json'
        self.catalog.parent.mkdir(parents=True)
        self.catalog.write_text(json.dumps({
            'sdkPreview': 3,
            'abiVersion': 1,
            'interfaces': [{'name': 'pulse.test', 'version': 1, 'header': 'include/pulse/test.h'}],
        }), encoding='utf-8')
        self.pulse = self.root/'pulse.exe'
        self.pulse.write_bytes(b'test host')
        self.workspace = self.root/'author'
        self.author = mcp.Author(self.workspace, self.sdk, self.pulse)

    def tearDown(self):
        self.temp.cleanup()

    def call(self, name, arguments):
        return self.author.execute(name, arguments, threading.Event())

    def test_tool_arguments_are_strict_and_native_consent_is_required(self):
        self.assertFalse(self.call('native_project_create', {'name': 'sample', 'template': 'basic', 'overwrite': True})['ok'])
        self.assertFalse(self.call('native_package_install', {'package': 'x.pulse-ext', 'allowNative': False})['ok'])
        self.assertFalse(self.call('native_package_install', {'package': 'x.pulse-ext', 'allowNative': 'true'})['ok'])

    def test_additional_file_argument_validation(self):
        for files in ('license.txt', [None], [''], ['x'] * 64):
            with self.subTest(files=files):
                result = self.call('native_package_stage', {'project': 'project', 'library': 'plugin.dll', 'files': files})
                self.assertFalse(result['ok'])
                self.assertIn('array', result['error'])

    @unittest.skipUnless(sys.platform == 'win32', 'Windows host process cleanup')
    def test_host_cleanup_waits_for_exit_when_taskkill_is_denied(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], stdin=subprocess.DEVNULL)
        try:
            with mock.patch.object(workflow.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1,
                    stdout=b'', stderr=b'ERROR: Access denied')):
                workflow.stop_and_confirm_host(process.pid)
            self.assertEqual(process.wait(timeout=2), 1)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)

    def test_create_copies_only_named_template_and_never_overwrites(self):
        self.assertFalse(self.call('native_project_create', {'name': 'sample', 'template': 'basic'})['ok'])
        created = self.call('native_project_create', {'name': 'sample', 'template': 'inspector'})
        self.assertTrue(created['ok'], created)
        self.assertEqual(created['result']['packageId'], 'local.sample')
        self.assertEqual(Path(created['result']['project']).joinpath('plugin.cpp').read_text(), 'int main() {}')
        self.assertFalse(self.call('native_project_create', {'name': 'sample', 'template': 'inspector'})['ok'])
        self.assertFalse(self.call('native_project_create', {'name': '../outside', 'template': 'inspector'})['ok'])
        self.assertFalse((self.root/'outside').exists())

    def test_workspace_paths_reject_traversal_and_symlink_escape(self):
        self.assertFalse(self.call('native_project_build', {'project': '../sdk'})['ok'])
        external = self.root/'outside'
        external.mkdir()
        link = self.workspace/'escape'
        self.workspace.mkdir(parents=True, exist_ok=True)
        try:
            link.symlink_to(external, target_is_directory=True)
        except OSError as error:
            self.skipTest('directory symlink creation unavailable: '+str(error))
        with self.assertRaises(mcp.AuthorError):
            self.author.inside('escape')

    def test_stdio_initialize_tool_list_and_tool_call(self):
        requests = [
            {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
            {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': {'name': 'native_project_create', 'arguments': {'name': 'sample', 'template': 'inspector'}}},
        ]
        source, output = io.BytesIO(b''.join(json.dumps(item).encode()+b'\n' for item in requests)), io.StringIO()
        mcp.serve(self.author, source, output)
        replies = [json.loads(line) for line in output.getvalue().splitlines()]
        by_id = {reply['id']: reply['result'] for reply in replies}
        self.assertEqual(by_id[1]['protocolVersion'], '2025-06-18')
        self.assertEqual([tool['name'] for tool in by_id[2]['tools']], [tool['name'] for tool in mcp.TOOLS])
        self.assertTrue(by_id[3]['structuredContent']['ok'], by_id[3])
        self.assertEqual(len(replies), 3)

    def test_process_diagnostics_keep_exit_status_and_output_tail(self):
        commands = []
        event = threading.Event()
        with self.assertRaises(mcp.AuthorError):
            self.author.run(['cmd.exe', '/d', '/c', 'echo mcp-test-diagnostic 1>&2 & exit /b 7'], event, commands, timeout=5)
        self.assertEqual(commands[0]['exit'], 7)
        self.assertIn('mcp-test-diagnostic', commands[0]['output'])

    def test_package_stage_copies_library_beside_manifest_before_hash_binding(self):
        cli = self.sdk/'tools/extensions/pulse_ext.py'
        cli.parent.mkdir(parents=True)
        cli.write_text('''import json, pathlib, sys
args=sys.argv
command=next(x for x in args if x in ("native-manifest", "pack", "verify"))
if command == "native-manifest":
    library=pathlib.Path(args[args.index("--library")+1])
    output=pathlib.Path(args[args.index("--output")+1])
    if library.parent != output.parent or not library.is_file(): sys.exit(9)
elif command == "pack":
    output=pathlib.Path(args[args.index("--output")+1]); output.write_bytes(b"package")
print(json.dumps({"ok": True, "verification": {"integrityVerified": True}}))
''', encoding='utf-8')
        project = self.workspace/'projects/workbench'
        project.mkdir(parents=True)
        (project/'manifest.json').write_text('{}', encoding='utf-8')
        build = self.workspace/'build'
        build.mkdir()
        library = build/'workbench.dll'
        library.write_bytes(b'compiled-library')
        result = self.call('native_package_stage', {'project': str(project), 'library': str(library)})
        self.assertTrue(result['ok'], result)
        staged = Path(result['result']['manifest']).parent
        self.assertEqual((staged/'workbench.dll').read_bytes(), b'compiled-library')
        self.assertEqual((staged/'workbench.pulse-ext').read_bytes(), b'package')

    def test_sdk_describe_separates_abi_samples_and_project_create_avoids_id_collision(self):
        described = self.call('native_sdk_describe', {})
        self.assertTrue(described['ok'], described)
        self.assertEqual(described['result']['templates'], ['inspector', 'rename-panel'])
        self.assertEqual(described['result']['abiSamples'], ['basic'])
        self.assertEqual(described['result']['sdkPreview'], 3)

        first = self.call('native_project_create', {'name': 'alpha-renamer', 'template': 'rename-panel'})
        second = self.call('native_project_create', {'name': 'beta-renamer', 'template': 'rename-panel'})
        self.assertTrue(first['ok'] and second['ok'])
        self.assertEqual(first['result']['packageId'], 'local.alpha-renamer')
        self.assertEqual(second['result']['packageId'], 'local.beta-renamer')
        self.assertNotEqual(first['result']['primaryLibrary'], second['result']['primaryLibrary'])
        manifest_a = json.loads(Path(first['result']['manifest']).read_text(encoding='utf-8'))
        self.assertEqual(manifest_a['id'], 'local.alpha-renamer')
        self.assertEqual(manifest_a['contributes'], {'commands': []})

        custom = self.call('native_project_create', {
            'name': 'custom-proj',
            'template': 'inspector',
            'id': 'com.acme.inspector',
            'displayName': 'Acme Inspector',
        })
        self.assertTrue(custom['ok'], custom)
        self.assertEqual(custom['result']['packageId'], 'com.acme.inspector')
        self.assertEqual(custom['result']['displayName'], 'Acme Inspector')
        self.assertEqual(custom['result']['primaryLibrary'], 'com_acme_inspector.dll')

        reserved = self.call('native_project_create', {
            'name': 'bad-id',
            'template': 'rename-panel',
            'id': 'sample.native-rename',
        })
        self.assertFalse(reserved['ok'])
        self.assertIn('reserved_id', reserved['error'])

    def test_project_build_supports_configs_symbols_and_attach_guidance(self):
        created = self.call('native_project_create', {'name': 'debug-plugin', 'template': 'rename-panel'})
        self.assertTrue(created['ok'], created)
        proj = created['result']['project']
        self.assertFalse(self.call('native_project_build', {'project': proj, 'config': 'MinSizeRel'})['ok'])

        def fake_run(command, cancelled, commands, timeout=300, json_reply=False):
            commands.append({'command': command, 'exit': 0, 'output': '', 'outputTruncated': False, 'elapsedMs': 1})
            if command[:2] == ['cmake', '--build']:
                build_dir = Path(command[2])
                build_dir.mkdir(parents=True, exist_ok=True)
                (build_dir / 'local_debug_plugin.dll').write_bytes(b'dll')
                (build_dir / 'local_debug_plugin.pdb').write_bytes(b'pdb')
            return ''

        with mock.patch.object(self.author, 'run', side_effect=fake_run):
            for cfg, expected_dir in (('Debug', 'build-debug'), ('RelWithDebInfo', 'build-relwithdebinfo'), ('Release', 'build')):
                built = self.call('native_project_build', {'project': proj, 'config': cfg})
                self.assertTrue(built['ok'], built)
                res = built['result']
                self.assertEqual(res['config'], cfg)
                self.assertTrue(res['build'].endswith(expected_dir))
                self.assertEqual(len(res['libraries']), 1)
                self.assertEqual(len(res['symbols']), 1)
                self.assertTrue(res['symbols'][0].endswith('.pdb'))
                self.assertEqual(res['debuggerAttach']['vsCodeLaunchConfig']['type'], 'cppvsdbg')

    def test_host_diagnose_reports_sdk_preview_interface_diffs_and_toolchain(self):
        fake_toolchain = {'ready': True, 'targetArchitecture': 'windows-x64', 'missingReasons': []}

        def host_reply(host_catalog):
            payload = {'nativeSdk': {'catalog': host_catalog}}
            return lambda command, cancelled, commands, timeout=40, json_reply=False: json.dumps(payload)

        expected = json.loads(self.catalog.read_text(encoding='utf-8'))
        with mock.patch.object(mcp.pulse_ext, 'inspect_toolchain', return_value=fake_toolchain):
            with mock.patch.object(self.author, 'run', side_effect=host_reply(expected)):
                matched = self.call('native_host_diagnose', {})
            self.assertTrue(matched['ok'], matched)
            self.assertEqual(matched['result']['diagnosis'], 'matched')
            self.assertTrue(matched['result']['abiCompatible'])
            self.assertEqual(matched['result']['sdkPreview'], {'sdk': 3, 'host': 3, 'matches': True})

            preview_mismatch = dict(expected, sdkPreview=2)
            with mock.patch.object(self.author, 'run', side_effect=host_reply(preview_mismatch)):
                res_prev = self.call('native_host_diagnose', {})
            self.assertEqual(res_prev['result']['diagnosis'], 'sdk_preview_mismatch')
            self.assertFalse(res_prev['result']['abiCompatible'])

            iface_mismatch = {
                'sdkPreview': 3,
                'abiVersion': 1,
                'interfaces': [{'name': 'pulse.test', 'version': 2}],
            }
            with mock.patch.object(self.author, 'run', side_effect=host_reply(iface_mismatch)):
                res_iface = self.call('native_host_diagnose', {})
            self.assertEqual(res_iface['result']['diagnosis'], 'interface_version_mismatch')
            self.assertEqual(len(res_iface['result']['interfaceComparison']['versionMismatches']), 1)

    def test_host_stop_validates_workspace_profile_record_and_invokes_cli(self):
        self.assertFalse(self.call('native_host_stop', {'pid': 9999})['ok'])
        profile_dir = self.workspace / 'profiles/run-100'
        profile_dir.mkdir(parents=True)
        (profile_dir / 'extension-host-instance.json').write_text(
            json.dumps({'formatVersion': 1, 'processId': 4242}),
            encoding='utf-8',
        )
        self.assertFalse(self.call('native_host_stop', {'pid': 9999, 'profile': 'profiles/run-100'})['ok'])

        def fake_stop(command, cancelled, commands, timeout=30, json_reply=False):
            self.assertIn('stop-host', command)
            self.assertIn('4242', command)
            return json.dumps({'ok': True, 'command': 'stop-host', 'stop': {'stopped': True, 'processId': 4242, 'forceKilled': False}})

        with mock.patch.object(self.author, 'run', side_effect=fake_stop):
            stopped = self.call('native_host_stop', {'pid': 4242})
        self.assertTrue(stopped['ok'], stopped)
        self.assertTrue(stopped['result']['stop']['stopped'])
        self.assertEqual(Path(stopped['result']['profile']), profile_dir.resolve())

    def test_sdk_describe_surfaces_reference_samples_and_supports_offline_author(self):
        sw_dir = self.sdk / 'sdk/native/samples/sandboxed-worker'
        sw_dir.mkdir(parents=True)
        (sw_dir / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\n', encoding='utf-8')
        (sw_dir / 'plugin.cpp').write_text('int x = 0;\n', encoding='utf-8')
        (sw_dir / 'manifest.json').write_text('{"apiVersion":1,"id":"com.example.sw","name":"SW"}', encoding='utf-8')
        (sw_dir / 'README.md').write_text('# Sandboxed Worker\n', encoding='utf-8')

        described = self.call('native_sdk_describe', {})
        self.assertTrue(described['ok'], described)
        self.assertEqual(described['result']['referenceSamples'], [{
            'name': 'sandboxed-worker',
            'path': 'sdk/native/samples/sandboxed-worker',
            'hasManifest': True,
            'hasReadme': True,
        }])

        offline_author = mcp.Author(self.workspace, self.sdk, pulse=None)
        with mock.patch.object(mcp.pulse_ext, 'inspect_toolchain', return_value={'ready': True, 'missingReasons': []}):
            diag = offline_author.execute('native_host_diagnose', {}, threading.Event())
        self.assertTrue(diag['ok'], diag)
        self.assertTrue(diag['result']['offlineMode'])
        self.assertEqual(diag['result']['diagnosis'], 'offline_toolchain_only')

        run_without_host = offline_author.execute('native_host_run', {'allowNative': True}, threading.Event())
        self.assertFalse(run_without_host['ok'])

    def test_native_plugin_test_enforces_workspace_bounds_and_structured_diagnostics(self):
        self.assertFalse(self.call('native_plugin_test', {'target': '../sdk'})['ok'])
        proj = self.workspace / 'projects/smoke'
        proj.mkdir(parents=True)
        (proj / 'manifest.json').write_text(
            json.dumps({'apiVersion': 1, 'id': 'com.acme.smoke', 'name': 'Smoke', 'resources': ['context.read']}),
            encoding='utf-8',
        )

        with mock.patch.object(mcp.pulse_ext, 'test_package', return_value={
            'valid': True,
            'contractCompatible': True,
            'harnessExecuted': True,
            'unloadClean': True,
            'commandsExecuted': 1,
            'checks': ['offlineValidation', 'pe64Exports', 'headlessHarnessLifecycle'],
        }):
            ok_res = self.call('native_plugin_test', {'target': 'projects/smoke', 'smokeCommands': True, 'strictResources': True})
        self.assertTrue(ok_res['ok'], ok_res)
        self.assertTrue(ok_res['result']['unloadClean'])
        self.assertEqual(ok_res['result']['commandsExecuted'], 1)

        with mock.patch.object(
            mcp.pulse_ext,
            'test_package',
            side_effect=mcp.pulse_ext.Failure('plugin_test_failed', 'Missing resource declaration', 2, harness={'ok': False, 'unloadClean': True}),
        ):
            fail_res = self.call('native_plugin_test', {'target': 'projects/smoke', 'strictResources': True})
        self.assertFalse(fail_res['ok'])
        self.assertEqual(fail_res['diagnostics']['code'], 'plugin_test_failed')
        self.assertFalse(fail_res['diagnostics']['harness']['ok'])

    def test_native_host_logs_filters_by_package_and_redacts_external_paths_and_env(self):
        store_dir = self.workspace / '.pulse-author/extensions'
        store_dir.mkdir(parents=True)
        (store_dir.parent / 'extension-startup-status.json').write_text(json.dumps({
            'hostPath': r'C:\Users\SecretAdmin\AppData\Local\Programs\Pulse\pulse.exe',
            'storePath': str(store_dir),
            'environment': {'LOCALAPPDATA': r'C:\Users\SecretAdmin\AppData\Local'},
            'packages': [
                {'id': 'com.acme.alpha', 'status': 'active', 'dll': str(store_dir / 'alpha.dll')},
                {'id': 'com.acme.beta', 'status': 'disabled', 'dll': r'D:\SecretExternal\beta.dll'},
            ],
        }), encoding='utf-8')
        (store_dir / 'native-crashed.txt').write_text(
            'com.acme.alpha\tdeadbeef\tC:\\Users\\SecretAdmin\\secret.dll\n'
            'com.acme.beta\tcafebabe\tD:\\Other\\beta.dll\n',
            encoding='utf-8',
        )

        profile = self.workspace / 'profiles/run-200'
        audit_dir = profile / 'extension-audit'
        crashes_dir = profile / 'Diagnostics/Crashes'
        audit_dir.mkdir(parents=True)
        crashes_dir.mkdir(parents=True)
        (audit_dir / '0001.jsonl').write_text(
            json.dumps({'packageId': 'com.acme.alpha', 'operation': 'files.stat', 'outcome': 'ok', 'utcMs': 10, 'sessionId': 's1',
                        'detail': r'C:\Users\SecretAdmin\Documents\private.txt'}) + '\n' +
            json.dumps({'packageId': 'com.acme.beta', 'operation': 'settings.set', 'outcome': 'ok', 'utcMs': 11, 'sessionId': 's1'}) + '\n',
            encoding='utf-8',
        )
        (crashes_dir / 'crash-01.json').write_text(json.dumps({
            'packageId': 'com.acme.alpha',
            'modulePath': str(store_dir / 'alpha.dll'),
            'symbolSearch': r'C:\Users\SecretAdmin\Symbols',
        }), encoding='utf-8')
        (profile / 'extension-store-process.log').write_text(
            'LOCALAPPDATA=C:\\Users\\SecretAdmin\\AppData\\Local\n'
            f'Loaded store from {store_dir}\n'
            'External probe C:\\Users\\SecretAdmin\\probe.txt\n',
            encoding='utf-8',
        )

        filtered = self.call('native_host_logs', {'packageId': 'com.acme.alpha', 'maxEvents': 10})
        self.assertTrue(filtered['ok'], filtered)
        serialized = json.dumps(filtered['result'])
        self.assertNotIn('SecretAdmin', serialized)
        self.assertNotIn('SecretExternal', serialized)
        self.assertIn('<redacted-path>', serialized)
        self.assertIn('<redacted-env>', serialized)
        self.assertIn('<workspace>/.pulse-author/extensions', serialized)
        self.assertEqual(len(filtered['result']['auditEvents']), 1)
        self.assertEqual(filtered['result']['auditEvents'][0]['packageId'], 'com.acme.alpha')
        self.assertEqual(len(filtered['result']['startupStatus']['packages']), 1)
        self.assertEqual(len(filtered['result']['crashAttribution']['crashed']), 1)
        self.assertEqual(len(filtered['result']['crashAttribution']['reports']), 1)


if __name__ == '__main__':
    unittest.main()
