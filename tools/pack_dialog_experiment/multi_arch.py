"""Package and control two separately verified x64/x86 dialog experiment bundles.

This explicit deployment tool does not change the production installer or the
application's single-architecture dialog-current.json contract.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import pack


ARCHITECTURES = ('x64', 'x86')
INDEX_NAME = 'multiarch.json'
CONTROL_NAME = 'PulseDialogAutoControl.exe'


def package(builds, output, version, repository):
    output = Path(output)
    if set(builds) != set(ARCHITECTURES):
        raise pack.PackageError('both x64 and x86 builds required')
    if not output.is_absolute():
        raise pack.PackageError('absolute NEW output required')
    pack.no_links(output.parent)
    if output.exists() or output.is_symlink():
        raise pack.PackageError('output already exists; never overwrite or clean')
    for arch in ARCHITECTURES:
        if output.is_relative_to(Path(builds[arch]).absolute()):
            raise pack.PackageError('output must be outside both build directories')
        # Check both complete closures before creating any destination.
        pack.plan(builds[arch], version, arch, repository, automatic=True)
    output.mkdir()
    records = {}
    for arch in ARCHITECTURES:
        child = output / arch
        pack.package(builds[arch], child, version, arch, repository, automatic=True)
        records[arch] = {'directory': arch,
                         'manifest_sha256': pack.digest(child / 'manifest.json')}
    index = {'schema': 1, 'kind': 'pulse-dialog-multiarch-experiment',
             'version': version, 'architectures': records}
    with (output / INDEX_NAME).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(index, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    return verify(output)


def verify(directory):
    directory = pack.no_links(directory)
    index_path = pack.no_links(directory / INDEX_NAME)
    if index_path.stat().st_size > 4096:
        raise pack.PackageError('multiarch index bound')
    index = json.loads(index_path.read_text(encoding='utf-8'), object_pairs_hook=pack.unique_object)
    if not isinstance(index, dict) or set(index) != {'schema', 'kind', 'version', 'architectures'}:
        raise pack.PackageError('multiarch index schema')
    if type(index['schema']) is not int or index['schema'] != 1 or index['kind'] != 'pulse-dialog-multiarch-experiment':
        raise pack.PackageError('multiarch index schema')
    if not isinstance(index['version'], str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}', index['version']):
        raise pack.PackageError('multiarch version label')
    records = index['architectures']
    if not isinstance(records, dict) or set(records) != set(ARCHITECTURES):
        raise pack.PackageError('both x64 and x86 bundles required')
    if {path.name for path in directory.iterdir()} != {INDEX_NAME, *ARCHITECTURES}:
        raise pack.PackageError('unlisted/missing multiarch root entries')
    for arch in ARCHITECTURES:
        record = records[arch]
        if not isinstance(record, dict) or set(record) != {'directory', 'manifest_sha256'}:
            raise pack.PackageError('multiarch child record')
        if record['directory'] != arch:
            raise pack.PackageError('multiarch child directory/architecture mismatch')
        digest = record['manifest_sha256']
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise pack.PackageError('multiarch child hash')
        child = pack.no_links(directory / arch)
        if pack.digest(pack.no_links(child / 'manifest.json')) != digest:
            raise pack.PackageError(f'{arch}: multiarch child manifest hash mismatch')
        manifest = pack.verify(child)
        if manifest['schema'] != 1 or manifest['architecture'] != arch or manifest['version'] != index['version']:
            raise pack.PackageError(f'{arch}: child schema/architecture/version mismatch')
        names = {item['path'].lower() for item in manifest['files']}
        if not all(name.lower() in names for name in pack.roots(arch, automatic=True)):
            raise pack.PackageError(f'{arch}: automatic components required')
    return index


def expected_dll(directory, arch):
    return str(Path(directory) / arch / f'PulseDialogAuto{64 if arch == "x64" else 32}.dll')


def same_path(left, right):
    # Native control returns canonical absolute Windows paths, not URI paths.
    return os.path.normcase(os.path.abspath(left)) == os.path.normcase(os.path.abspath(right))


def invoke_helper(directory, arch, command):
    # Recheck BOTH closures before EVERY executable launch, even for one view.
    verify(directory)
    child = Path(directory) / arch
    arguments = [str(child / CONTROL_NAME)]
    arguments += ['--status-json'] if command == 'status' else [
        '--enable-json' if command == 'enable' else '--disable-for-json', str(child)]
    process = subprocess.run(arguments, cwd=child, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             encoding='utf-8', errors='strict', timeout=45,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if len(process.stdout) > 16384 or len(process.stderr) > 16384:
        raise pack.PackageError(f'{arch}: control response bound')
    try:
        status = json.loads(process.stdout, object_pairs_hook=pack.unique_object)
    except ValueError as error:
        raise pack.PackageError(f'{arch}: native control requires JSON protocol: {process.stdout[:256]!r}') from error
    fields = {'error', 'enabled', 'conflict', 'status_error', 'dll_path'}
    if not isinstance(status, dict) or set(status) != fields:
        raise pack.PackageError(f'{arch}: native control response schema')
    if any(type(status[name]) is not int or not 0 <= status[name] <= 0xffffffff for name in ('error', 'status_error')):
        raise pack.PackageError(f'{arch}: native control error type')
    if any(type(status[name]) is not bool for name in ('enabled', 'conflict')) or not isinstance(status['dll_path'], str):
        raise pack.PackageError(f'{arch}: native control status type')
    if process.returncode not in (0, 1) or bool(process.returncode) != bool(status['error']):
        raise pack.PackageError(f'{arch}: native control exit/result mismatch')
    return status


def good(status):
    return not status['error'] and not status['status_error'] and not status['conflict']


def own_status(status, directory, arch):
    path = status['dll_path']
    return not path or same_path(path, expected_dll(directory, arch))


def operate(directory, command, architectures=ARCHITECTURES, invoke=invoke_helper):
    if command not in ('status', 'enable', 'disable'):
        raise pack.PackageError('unknown control command')
    selected = tuple(architectures)
    if not selected or len(set(selected)) != len(selected) or any(arch not in ARCHITECTURES for arch in selected):
        raise pack.PackageError('invalid architecture selection')
    directory = pack.no_links(directory)
    verify(directory)
    report = {'command': command, 'result': 'failed', 'architectures': {}, 'operations': [], 'rollback': []}

    def call(arch, action, destination):
        try:
            status = invoke(directory, arch, action)
            destination.append({'architecture': arch, 'command': action, 'status': status})
            report['architectures'][arch] = status
            return status
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            destination.append({'architecture': arch, 'command': action, 'failure': str(error)})
            return None

    before = {}
    for arch in ARCHITECTURES:
        before[arch] = call(arch, 'status', report['operations'])
    if command == 'status':
        report['result'] = 'ok' if all(status is not None and good(status) for status in before.values()) else 'failed'
        return report
    # Never mutate one view until all selected views pass preflight. Existing
    # registrations from another installation remain untouched, even if idle.
    if any(before[arch] is None or not good(before[arch]) or not own_status(before[arch], directory, arch) for arch in selected):
        report['failure'] = 'preflight: conflict, status error, or another bundle registration'
        return report
    changed = []
    for arch in selected:
        wanted = command == 'enable'
        if before[arch]['enabled'] == wanted:
            continue
        # Include the attempted view: a helper failure can leave its own journal
        # partially changed. Scoped native rollback checks the DLL again.
        changed.append(arch)
        status = call(arch, command, report['operations'])
        success = status is not None and good(status) and status['enabled'] == wanted and own_status(status, directory, arch)
        if wanted:
            success = success and bool(status and status['dll_path'])
        if not success:
            report['failure'] = f'{arch}: {command} failed'
            restored = True
            for previous in reversed(changed):
                action = 'enable' if before[previous]['enabled'] else 'disable'
                rollback = call(previous, action, report['rollback'])
                restored = restored and rollback is not None and good(rollback) and rollback['enabled'] == before[previous]['enabled'] and own_status(rollback, directory, previous)
            report['result'] = 'rolled-back' if restored else 'rollback-failed'
            return report
    report['result'] = 'ok'
    return report


@contextmanager
def control_lock():
    if os.name != 'nt':
        raise pack.PackageError('native registration control requires Windows')
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.ReleaseMutex.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    # Separate from the native per-call registration mutex: the child must be
    # able to acquire that mutex. This lock serializes multiarch invocations.
    handle = kernel.CreateMutexW(None, False, 'Global\\Pulse.DialogAuto.MultiArch.v1')
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    acquired = False
    try:
        acquired = kernel.WaitForSingleObject(handle, 30000) in (0, 0x80)
        if not acquired:
            raise pack.PackageError('multiarch control mutex timeout')
        yield
    finally:
        if acquired:
            kernel.ReleaseMutex(handle)
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pack')
    p.add_argument('--build-x64', type=Path, required=True)
    p.add_argument('--build-x86', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[2])
    for command in ('verify', 'status', 'enable', 'disable'):
        p = sub.add_parser(command)
        p.add_argument('--directory', type=Path, required=True)
        if command != 'verify':
            p.add_argument('--arch', choices=ARCHITECTURES, action='append', help='default: both registry views')
    args = parser.parse_args()
    try:
        if args.command == 'pack':
            result = package({'x64': args.build_x64, 'x86': args.build_x86}, args.output, args.version, args.repository)
        elif args.command == 'verify':
            result = verify(args.directory)
        else:
            with control_lock():
                result = operate(args.directory, args.command, tuple(args.arch or ARCHITECTURES))
            print(json.dumps(result, ensure_ascii=False))
            return 0 if result['result'] == 'ok' else 1
        print(json.dumps({'result': 'verified-files', 'architectures': list(ARCHITECTURES), 'version': result['version'],
                          'capability_certification': False}, ensure_ascii=False))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
