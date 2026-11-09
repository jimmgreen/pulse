"""Measure fresh-process startup with isolated profiles; never flush OS caches."""
import argparse
import csv
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def modules(pid):
    class Module(ctypes.Structure):
        _fields_ = [(name, wintypes.DWORD) for name in
                    ('dwSize', 'th32ModuleID', 'th32ProcessID', 'GlblcntUsage', 'ProccntUsage')] + [
                    ('modBaseAddr', ctypes.c_void_p), ('modBaseSize', wintypes.DWORD),
                    ('hModule', wintypes.HMODULE), ('szModule', wintypes.WCHAR * 256),
                    ('szExePath', wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Module)]
    kernel.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Module)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(0x18, pid)
    if handle == ctypes.c_void_p(-1).value:
        return {'error': ctypes.get_last_error()}
    result = []
    try:
        item = Module()
        item.dwSize = ctypes.sizeof(item)
        ok = kernel.Module32FirstW(handle, ctypes.byref(item))
        if not ok:
            return {'error': ctypes.get_last_error()}
        while ok:
            result.append(item.szExePath)
            ok = kernel.Module32NextW(handle, ctypes.byref(item))
    finally:
        kernel.CloseHandle(handle)
    return {'paths': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', required=True, type=Path)
    parser.add_argument('--archive-dir', required=True, type=Path)
    parser.add_argument('--workbench-dll', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if os.name != 'nt' or args.repeats < 3:
        parser.error('Windows and at least three repetitions are required')
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    pulse = args.pulse.resolve()
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
    evidence = {'environment': {'platform': platform.platform(), 'processor': platform.processor(),
                'python': sys.version, 'cpu_count': os.cpu_count()}, 'host': str(pulse),
                'host_sha256': digest(pulse), 'runs': [], 'cli': [],
                'definitions': {
                    'cold': 'New process and new isolated profile each run; OS file cache was NOT flushed; no machine reboot.',
                    'startup': 'Probe process_ms: GetSystemTimePreciseAsFileTime minus GetProcessTimes creation time; first_present is first probe-observed renderer presentation, initial_listing_presented is first presented ready snapshot.',
                    'memory': 'GetProcessMemoryInfo WorkingSetSize and PrivateUsage at each CSV event; complete is roughly 5 s after return listing, after two navigation legs.',
                    'modules': 'Toolhelp snapshot after initial listing CSV event and again after return listing; no extension panels or commands invoked. Absence at sampled points does not prove never transiently loaded.',
                    'scope': 'PULSE_TEST_P1_SCOPE=selection; isolated test mode, not normal user-profile startup.'}}
    def save():
        (output / 'results.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
    def cli(*arguments):
        command = [sys.executable, str(root / 'tools/extensions/pulse_ext.py'), '--pulse', str(pulse), '--json', *map(str, arguments)]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=45)
        evidence['cli'].append({'command': command, 'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        save()
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    store = output / 'store'
    for name, manifest, library, extras in (
            ('archive', root / 'sdk/native/samples/archive/manifest.json', args.archive_dir / 'pulse_archive.dll', ['7z.dll', 'License.txt', 'NOTICE.txt']),
            ('workbench', root / 'examples/native/directory-workbench/manifest.json', args.workbench_dll, [])):
        package = output / name
        package.mkdir()
        shutil.copy2(library, package / library.name)
        flags = []
        for extra in extras:
            shutil.copy2(args.archive_dir / extra, package / extra)
            flags.extend(['--file', package / extra])
        cli('native-manifest', manifest, '--library', package / library.name, '--output', package / 'manifest.json', *flags)
        archive = output / (name + '.pulseext')
        cli('pack', package / 'manifest.json', '--output', archive)
        cli('install', archive, '--store', store, '--accept-capabilities', '--allow-native')
        identity = json.loads(manifest.read_text(encoding='utf-8'))['id']
        cli('enable', identity, '--store', store, '--allow-native')
    cli('list', '--store', store)
    evidence['artifacts'] = {str(p.relative_to(output)): digest(p) for p in output.glob('*/*') if p.is_file() and p.suffix.lower() == '.dll'}
    initial, target = output / 'fixture-initial', output / 'fixture-target'
    for folder in (initial, target):
        folder.mkdir()
        for i in range(100):
            (folder / f'entry-{i:03}.txt').write_text('startup fixture\n', encoding='utf-8')
    for repeat in range(args.repeats):
        for condition in ('none', 'archive-workbench'):
            run_dir = output / f'{repeat + 1}-{condition}'
            run_dir.mkdir()
            profile = run_dir / 'profile'
            profile.mkdir()
            report = run_dir / 'startup.csv'
            child_env = dict(env, PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_STARTUP_PERF=str(report),
                             PULSE_TEST_STARTUP_TARGET=str(target), PULSE_TEST_P1_SCOPE='selection', PULSE_TEST_STARTUP_ONLY='1')
            command = [str(pulse), '--test-instance']
            if condition != 'none':
                command += ['--extension-store', str(store)]
            command.append(str(initial))
            run = {'condition': condition, 'repeat': repeat + 1, 'command': command, 'modules': {}, 'environment': {k: v for k, v in child_env.items() if k.startswith('PULSE_')}}
            evidence['runs'].append(run)
            with (run_dir / 'process.log').open('wb') as log:
                process = subprocess.Popen(command, env=child_env, stdout=log, stderr=subprocess.STDOUT)
                run['pid'] = process.pid
                started = time.monotonic()
                while process.poll() is None and time.monotonic() - started < 135:
                    rows = list(csv.DictReader(report.read_text().splitlines())) if report.exists() else []
                    for event in ('initial_listing_presented', 'return_listing_presented'):
                        if event not in run['modules'] and any(row['event'] == event for row in rows):
                            run['modules'][event] = modules(process.pid)
                    time.sleep(0.05)
                if process.poll() is None:
                    process.kill()
                    run['harness_timeout'] = True
                run['exit_code'] = process.wait()
            run['events'] = list(csv.DictReader(report.read_text().splitlines())) if report.exists() else []
            run['valid'] = run['exit_code'] == 0 and any(r['event'] == 'complete' for r in run['events'])
            save()
            print(f'{repeat + 1} {condition}: valid={run["valid"]}', flush=True)
    evidence['summary'] = {}
    for condition in ('none', 'archive-workbench'):
        summary = evidence['summary'][condition] = {}
        for event in ('first_present', 'initial_listing_presented', 'complete'):
            summary[event] = {}
            for metric in ('process_ms', 'working_set_bytes', 'private_commit_bytes'):
                values = [float(row[metric]) for run in evidence['runs'] if run['valid'] and run['condition'] == condition for row in run['events'] if row['event'] == event]
                summary[event][metric] = {'n': len(values), 'median': statistics.median(values) if values else None, 'min': min(values) if values else None, 'max': max(values) if values else None}
    save()
    return 0 if all(run['valid'] for run in evidence['runs']) else 1


if __name__ == '__main__':
    sys.exit(main())
