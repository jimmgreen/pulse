"""Check the shared installed layout and its real stable-entry launch.

Uses a new fixture directory and the application's isolated test entry. It does
not install into Program Files or change the user's preferences/registrations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    output = args.output
    if not output.is_absolute() or output.exists():
        raise ValueError('--output must be a new absolute directory')
    pointer = json.loads((package / 'dialog-current.json').read_text(encoding='utf-8'))
    digest = pointer['manifest_sha256']
    if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise ValueError('invalid digest')
    if pointer['directory'] != 'DialogBridge/' + digest:
        raise ValueError('invalid version directory')
    runtime = package / pointer['directory']
    manifest = runtime / 'manifest.json'
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != digest:
        raise ValueError('manifest digest mismatch')
    if {p.name for p in package.iterdir()} != {'pulse.exe', 'dialog-current.json', 'DialogBridge'}:
        raise ValueError('package contains unexpected root runtime copies')
    shared = ('pulse.exe', 'lumatext.dll', 'pdfium.dll', 'Pulse.Preview.exe',
              'pulse_shell.exe', 'pulse_elevated.exe')
    for name in shared:
        if not (runtime / name).is_file():
            raise ValueError('missing shared component: ' + name)
        if name != 'pulse.exe' and (package / name).exists():
            raise ValueError('duplicate root component: ' + name)
    if (package / 'pulse.exe').stat().st_size >= (runtime / 'pulse.exe').stat().st_size:
        raise ValueError('root entry is not a smaller launcher')
    output.mkdir()
    local = output / 'local'
    local.mkdir()
    report = output / 'stable-entry.log'
    env = os.environ.copy()
    env.update(LOCALAPPDATA=str(local), PULSE_TEST_DATA_DIR=str(output),
               PULSE_TEST_SETTINGS_FLOW=str(report), PULSE_TEST_STABLE_ENTRY_REGISTRATION='1')
    with (output / 'process.log').open('xb') as log:
        child = subprocess.Popen([str(package / 'pulse.exe'), '--launcher-wait', '--test-instance',
                                  '--shot', str(output / 'stable-entry.png')],
                                 cwd=output, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = child.wait(timeout=60)
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
    if code:
        raise RuntimeError(f'stable launcher or child exited {code}')
    text = report.read_text(encoding='utf-8')
    if '[FAIL]' in text or 'failures=0' not in text or 'test runs inside versioned runtime' not in text:
        raise RuntimeError('missing or failed stable-entry registration assertions')
    print('[PASS] one shared runtime; root has only launcher and pointer')
    print('[PASS] root launcher started actual UI and forwarded successful exit')
    print('[PASS] versioned application registered stable root entry in isolated registry')
    print(f'launcher_bytes={(package / "pulse.exe").stat().st_size}')
    print(f'shared_component_bytes={sum((runtime / name).stat().st_size for name in shared)}')
    print(text, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
