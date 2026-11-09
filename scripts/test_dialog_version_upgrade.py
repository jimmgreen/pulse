"""Exercise real old/new dialog providers across isolated installation transactions.

Never installs into Program Files or changes the real COM registration. The setup
test's --apply-staged uses its file-backed test registration inside output/app.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def package_digest(package):
    pointer = json.loads((package / 'dialog-current.json').read_text(encoding='utf-8'),
                         object_pairs_hook=unique_object)
    if not isinstance(pointer, dict) or set(pointer) != {'schema', 'directory', 'manifest_sha256'}:
        raise ValueError(f'invalid pointer: {package}')
    digest = pointer['manifest_sha256']
    if type(pointer['schema']) is not int or pointer['schema'] != 1 or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError(f'invalid pointer schema/hash: {package}')
    if pointer['directory'] != f'DialogBridge/{digest}':
        raise ValueError(f'pointer path/hash mismatch: {package}')
    raw = (package / pointer['directory'] / 'manifest.json').read_bytes()
    if hashlib.sha256(raw).hexdigest() != digest:
        raise ValueError(f'manifest hash mismatch: {package}')
    if json.loads(raw, object_pairs_hook=unique_object)['schema'] != 1:
        raise ValueError(f'expected schema-1 bundle: {package}')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--packages', nargs=3, type=Path, required=True, metavar=('A', 'B', 'C'))
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, required=True, help='new absolute isolated output directory')
    args = parser.parse_args()
    children = []
    streams = []
    summary = None
    try:
        if not args.output.is_absolute() or args.output.exists():
            raise ValueError('--output must be a NEW absolute directory')
        packages = [path.resolve(strict=True) for path in args.packages]
        digests = [package_digest(path) for path in packages]
        if len(set(digests)) != 3:
            raise ValueError('A/B/C must have distinct manifest digests to prove version switching')
        build = args.build.resolve(strict=True)
        setup = build / 'pulse_setup_dialog_versions_test.exe'
        host = build / 'pulse_dialog_upgrade_host_test.exe'
        if not setup.is_file() or not host.is_file():
            raise ValueError('build both test executables before running')
        output = args.output
        output.mkdir(parents=False)
        controls = [output / ('control-' + name) for name in 'ABC']
        for control in controls:
            control.mkdir()
        installed = output / 'app'
        summary = (output / 'upgrade.log').open('x', encoding='utf-8')

        def report(message):
            print(message, flush=True)
            summary.write(message + '\n')
            summary.flush()

        def remaining(deadline):
            value = deadline - time.monotonic()
            if value <= 0:
                raise TimeoutError('upgrade sequence exceeded host watchdog budget')
            return value

        def spawn(command, name, control=None):
            log = (output / (name + '.stdout.log')).open('xb')
            streams.append(log)
            env = os.environ.copy()
            if control is not None:
                env['PULSE_TEST_DATA_DIR'] = str(control)
            process = subprocess.Popen([str(arg) for arg in command], stdout=log, stderr=subprocess.STDOUT,
                                       env=env, cwd=output)
            children.append(process)
            return process

        def wait_exit(process, deadline, name):
            code = process.wait(timeout=remaining(deadline))
            if code:
                raise RuntimeError(f'{name} exited {code}; see {output}')

        def apply(index, deadline):
            name = 'apply-' + 'ABC'[index]
            process = spawn([setup, '--apply-staged', installed, packages[index]], name)
            wait_exit(process, deadline, name)
            if package_digest(installed) != digests[index]:
                raise RuntimeError(f'{name}: installed pointer does not select expected package')
            report(f'[PASS] {name}: staged transaction selected {digests[index]}')

        def wait_ready(process, control, index, expected, deadline, name):
            marker = control / f'ready-{index}'
            while not marker.is_file():
                code = process.poll()
                if code is not None:
                    raise RuntimeError(f'{name} exited {code} before ready-{index}; see {control / "host.log"}')
                log = control / 'host.log'
                try:
                    log_text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
                except PermissionError:
                    # The log is diagnostic, not the completion signal. A brief
                    # writer/scanner sharing denial must not abort a live host;
                    # exit status, atomic ready marker and deadline stay binding.
                    log_text = ''
                if (control / 'watchdog-failed').exists() or '[FAIL]' in log_text:
                    raise RuntimeError(f'{name} reported failure; see {log}')
                time.sleep(min(0.025, remaining(deadline)))
            marker_data = json.loads(marker.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
            if marker_data != {'pid': process.pid, 'round': index, 'manifest_sha256': expected}:
                raise RuntimeError(f'{name} ready marker identity/digest mismatch: {marker_data}')
            report(f'[PASS] {name} ready-{index}: actual Show/Close, owner restoration, digest={expected}')

        # Prepare all paths/inputs and install A before the old host's 120s clock.
        apply(0, time.monotonic() + 120)
        deadline = time.monotonic() + 115
        old = spawn([host, '--hold', installed, controls[0]], 'host-A', controls[0])
        wait_ready(old, controls[0], 0, digests[0], deadline, 'old A')
        for index in (1, 2):
            if old.poll() is not None:
                raise RuntimeError('old A host exited before upgrade')
            apply(index, deadline)
            fresh = spawn([host, '--once', installed, controls[index]], 'host-' + 'ABC'[index], controls[index])
            wait_ready(fresh, controls[index], 0, digests[index], deadline, 'new ' + 'ABC'[index])
            wait_exit(fresh, deadline, 'new ' + 'ABC'[index])
            report(f'[PASS] new {"ABC"[index]} host exited normally')
            if old.poll() is not None:
                raise RuntimeError('old A host was closed during upgrade')
            (controls[0] / f'next-{index}').write_text('show again\n', encoding='ascii')
            wait_ready(old, controls[0], index, digests[0], deadline, 'old A')
        (controls[0] / 'release').write_text('release\n', encoding='ascii')
        wait_exit(old, deadline, 'old A')
        report('[PASS] old A survived B/C upgrades and exited normally after release')
        report('[PASS] complete: isolated real GUI upgrade sequence')
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        message = f'[FAIL] {error}'
        print(message, file=sys.stderr, flush=True)
        if summary:
            summary.write(message + '\n')
            summary.flush()
        return 1
    finally:
        # Only handles returned by this invocation are touched; no image-name or
        # process-tree killing, no user application shutdown, no fixture removal.
        for process in children:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for stream in streams:
            stream.close()
        if summary:
            summary.close()


if __name__ == '__main__':
    sys.exit(main())
