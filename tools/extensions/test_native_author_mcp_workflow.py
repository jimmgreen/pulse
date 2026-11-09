"""Exercise the complete native author tool flow through MCP stdio."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


def stop_and_confirm_host(pid):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.TerminateProcess.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:
            return
        raise ctypes.WinError(error)
    try:
        user = ctypes.WinDLL('user32', use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        @callback_type
        def close_window(window, unused):
            owner = wintypes.DWORD()
            user.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == pid:
                user.PostMessageW(window, 0x0010, 0, 0)
            return True
        user.EnumWindows(close_window, 0)
        if kernel.WaitForSingleObject(handle, 10000) == 258:
            subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True, timeout=15)
        if kernel.WaitForSingleObject(handle, 10000) != 0:
            raise RuntimeError(f'isolated Pulse host PID {pid} did not signal exit')
    finally:
        kernel.CloseHandle(handle)


def short_path(path):
    buffer = ctypes.create_unicode_buffer(32768)
    size = ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer))
    if not size or size >= len(buffer):
        raise OSError(f'could not resolve an unquoted Windows path: {path}')
    return buffer.value


class Client:
    def __init__(self, process):
        self.process = process
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.identity = 0

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def request(self, method, params=None, timeout=360):
        self.identity += 1
        identity = self.identity
        request = dict(jsonrpc='2.0', id=identity, method=method, params=params or {})
        self.process.stdin.write(json.dumps(request)+'\n')
        self.process.stdin.flush()
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            line = self.lines.get(timeout=max(0.01, deadline-time.monotonic()))
            if line is None:
                raise RuntimeError('MCP server closed stdout')
            value = json.loads(line)
            if value.get('id') == identity:
                if 'error' in value:
                    raise RuntimeError(value['error'])
                return value['result']
        raise TimeoutError(f'MCP request {identity} timed out')

    def tool(self, name, arguments):
        result = self.request('tools/call', {'name': name, 'arguments': arguments})
        value = result.get('structuredContent')
        if not isinstance(value, dict):
            raise RuntimeError(f'{name} omitted structured result: {result}')
        return value

    def notify(self, method, params=None):
        self.process.stdin.write(json.dumps(dict(jsonrpc='2.0', method=method, params=params or {}))+'\n')
        self.process.stdin.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--sdk-archive', type=Path, required=True)
    parser.add_argument('--vsvars', type=Path, default=Path(r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.pulse, args.sdk_archive, args.vsvars, args.output = (p.resolve() for p in (args.pulse, args.sdk_archive, args.vsvars, args.output))
    if args.output.exists():
        parser.error('output must be a new isolated directory')
    from test_native_sdk import extract
    args.output.mkdir(parents=True)
    sdk = extract(args.sdk_archive, args.output/'source-sdk')
    workspace = args.output/'author-workspace'
    workspace.mkdir(parents=True)
    server = sdk/'tools/extensions/native_author_mcp.py'
    command = ('set VSCMD_SKIP_SENDTELEMETRY=1&& call '+short_path(args.vsvars)+
        ' >nul && '+short_path(Path(sys.executable))+' -u '+short_path(server)+' --workspace '+short_path(workspace)+
        ' --sdk-root '+short_path(sdk)+' --pulse '+short_path(args.pulse))
    process = subprocess.Popen(['cmd.exe', '/d', '/c', command], cwd=workspace, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8', bufsize=1)
    client = Client(process)
    report = dict(hostSha256=hashlib.sha256(args.pulse.read_bytes()).hexdigest(), sdkArchiveSha256=hashlib.sha256(args.sdk_archive.read_bytes()).hexdigest(), steps=[])
    host_pid = None
    try:
        initialized = client.request('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'pulse-native-author-acceptance', 'version': '1'}})
        report['protocolVersion'] = initialized['protocolVersion']
        client.notify('notifications/initialized')
        available = client.request('tools/list')['tools']
        expected = ('native_sdk_describe', 'native_project_create', 'native_project_build', 'native_package_stage',
            'native_package_install', 'native_host_diagnose', 'native_host_run')
        assert tuple(item['name'] for item in available) == expected
        described = client.tool('native_sdk_describe', {})
        assert any(item['name'] == 'pulse.ui.mount' for item in described['result']['interfaces'])
        project_result = client.tool('native_project_create', {'name': 'directory-workbench', 'template': 'directory-workbench'})
        assert project_result['ok'], project_result
        report['steps'].append(dict(name='describe-and-create', ok=True))
        build_result = client.tool('native_project_build', {'project': project_result['result']['project']})
        assert build_result['ok'], build_result
        libraries = [Path(path) for path in build_result['result']['libraries'] if Path(path).name.lower() == 'pulse_directory_workbench.dll']
        assert len(libraries) == 1 and libraries[0].is_file(), build_result
        report['steps'].append(dict(name='native-project-build', ok=True, librarySha256=hashlib.sha256(libraries[0].read_bytes()).hexdigest()))
        staged = client.tool('native_package_stage', {'project': project_result['result']['project'], 'library': str(libraries[0])})
        assert staged['ok'] and staged['result']['verified']['verification']['integrityVerified'], staged
        report['steps'].append(dict(name='package-stage-verify', ok=True))
        installed = client.tool('native_package_install', {'package': staged['result']['package'], 'allowNative': True, 'enable': True})
        assert installed['ok'] and installed['result']['enabled'], installed
        report['steps'].append(dict(name='isolated-native-install-enable', ok=True))
        diagnosis = client.tool('native_host_diagnose', {})
        assert diagnosis['ok'] and diagnosis['result']['catalogMatches'], diagnosis
        report['steps'].append(dict(name='host-sdk-catalog-match', ok=True))
        launched = client.tool('native_host_run', {'allowNative': True})
        run_info = launched.get('result', {}).get('run', {})
        assert launched['ok'] and run_info.get('launched'), launched
        host_pid = run_info.get('processId')
        report['steps'].append(dict(name='isolated-host-launch', ok=True, processId=host_pid,
            launchOnly=True, processLog=run_info.get('processLog')))
        report['passed'] = True
    except Exception as error:
        report.update(passed=False, error=str(error))
    finally:
        if host_pid:
            try:
                stop_and_confirm_host(host_pid)
                report['steps'].append(dict(name='isolated-host-cleanup', ok=True, processId=host_pid,
                    exitConfirmed=True))
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                report.update(passed=False, cleanupError=str(error))
        try:
            process.stdin.close()
            process.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            process.kill()
            process.wait(timeout=10)
    report['serverExit'] = process.returncode
    report['serverStderr'] = process.stderr.read()[-8192:]
    report_path = args.output/'report.json'
    report_path.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] and process.returncode == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
