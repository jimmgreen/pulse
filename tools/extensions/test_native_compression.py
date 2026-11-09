"""Focused real compression publication probe with an isolated store and profile."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pulse', required=True, type=Path)
    parser.add_argument('--sample-build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    pulse, source, output = args.pulse.resolve(), args.sample_build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    profile = output / 'profile'
    fixture = profile / 'fixture'
    fixture.mkdir(parents=True)
    (fixture / 'notes.txt').write_bytes(b'Compression source bytes: hello archive!\r\n' * 100)
    before = hashlib.sha256((fixture / 'notes.txt').read_bytes()).digest()
    temporary = profile / 'temporary'
    temporary.mkdir()
    package_dir = output / 'package'
    package_dir.mkdir()
    for name in ('pulse_archive.dll', '7z.dll', 'License.txt', 'NOTICE.txt'):
        shutil.copy2(source / name, package_dir / name)
    cli = [sys.executable, str(ROOT / 'tools/extensions/pulse_ext.py'), '--pulse', str(pulse), '--json']
    def run(command, env=None):
        result = subprocess.run([str(x) for x in command], capture_output=True, timeout=180, env=env)
        if result.returncode:
            report = profile / 'capture/report.txt'
            evidence = report.read_text(encoding='utf-8') if report.exists() else ''
            raise RuntimeError(evidence + result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
    manifest = package_dir / 'manifest.json'
    package = package_dir / 'archive.pulse-ext'
    store = profile / 'extensions'
    run(cli + ['native-manifest', ROOT / 'sdk/native/samples/archive/manifest.json', '--library', package_dir / 'pulse_archive.dll',
               '--file', package_dir / '7z.dll', '--file', package_dir / 'License.txt', '--file', package_dir / 'NOTICE.txt', '--output', manifest])
    run(cli + ['pack', manifest, '--output', package])
    run(cli + ['install', package, '--store', store, '--allow-native'])
    run(cli + ['enable', 'sample.native-archive', '--store', store, '--allow-native'])
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(profile / 'capture'),
               PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='archive', PULSE_TEST_NATIVE_COMPRESSION='1',
               LOCALAPPDATA=str(profile / 'local-app-data'), TEMP=str(temporary), TMP=str(temporary))
    run([pulse, '--test-instance', fixture], env=env)
    report = (profile / 'capture/report.txt').read_text(encoding='utf-8')
    if '[FAIL]' in report or 'Result: PASS' not in report:
        raise RuntimeError(report)
    if hashlib.sha256((fixture / 'notes.txt').read_bytes()).digest() != before:
        raise RuntimeError('Source file was modified')
    with zipfile.ZipFile(fixture / 'notes.txt.zip') as archive:
        if archive.namelist() != ['notes.txt'] or archive.read('notes.txt') != (fixture / 'notes.txt').read_bytes():
            raise RuntimeError('Published ZIP member names or content differ from the source')
    if list(fixture.glob('.pulse-stage-*')):
        raise RuntimeError('Private staging remains after publication')
    if not (profile / 'capture/staging-confirm.png').is_file():
        raise RuntimeError('Missing application-rendered confirmation evidence')
    print(report)
    print('Evidence:', profile / 'capture')


if __name__ == '__main__':
    main()
