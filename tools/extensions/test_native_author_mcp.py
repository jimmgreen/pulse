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
        self.catalog = self.sdk/'sdk/native/api-catalog.json'
        self.catalog.parent.mkdir(parents=True)
        self.catalog.write_text(json.dumps({'interfaces': [{'name': 'pulse.test', 'version': 1}]}), encoding='utf-8')
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
        template = self.examples/'basic'
        template.mkdir()
        (template/'CMakeLists.txt').write_text('project(sample)', encoding='utf-8')
        (template/'plugin.cpp').write_text('int main() {}', encoding='utf-8')
        created = self.call('native_project_create', {'name': 'sample', 'template': 'basic'})
        self.assertTrue(created['ok'], created)
        self.assertEqual(Path(created['result']['project']).joinpath('plugin.cpp').read_text(), 'int main() {}')
        self.assertFalse(self.call('native_project_create', {'name': 'sample', 'template': 'basic'})['ok'])
        self.assertFalse(self.call('native_project_create', {'name': '../outside', 'template': 'basic'})['ok'])
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
        template = self.examples/'basic'
        template.mkdir()
        (template/'CMakeLists.txt').write_text('project(sample)', encoding='utf-8')
        requests = [
            {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
            {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': {'name': 'native_project_create', 'arguments': {'name': 'sample', 'template': 'basic'}}},
        ]
        source, output = io.BytesIO(b''.join(json.dumps(item).encode()+b'\n' for item in requests)), io.StringIO()
        mcp.serve(self.author, source, output)
        replies = [json.loads(line) for line in output.getvalue().splitlines()]
        by_id = {reply['id']: reply['result'] for reply in replies}
        self.assertEqual(by_id[1]['protocolVersion'], '2025-06-18')
        self.assertEqual([tool['name'] for tool in by_id[2]['tools']], [tool['name'] for tool in mcp.TOOLS])
        self.assertTrue(by_id[3]['structuredContent']['ok'])
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


if __name__ == '__main__':
    unittest.main()
