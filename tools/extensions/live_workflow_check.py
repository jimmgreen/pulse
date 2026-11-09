from live_dev_check import child_process
"""Exercise declared workflows in an isolated real Pulse host, including native per-step transfer confirmation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--page', choices=('workflow-navigation', 'transfer-workflow', 'all'), default='all')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    pages = ('workflow-navigation', 'transfer-workflow') if args.page == 'all' else (args.page,)
    results = {}
    try:
        for page in pages:
            run = output / page
            profile = run / 'profile'
            folder = profile / 'fixture' / 'child'
            folder.mkdir(parents=True)
            (folder / 'unchanged.txt').write_text('isolated workflow fixture\n', encoding='utf-8')
            manifest = json.loads((root / 'examples/extensions' / page / 'manifest.json').read_text(encoding='utf-8'))
            if page == 'transfer-workflow':
                source, target = profile / 'source', profile / 'target'
                source.mkdir()
                target.mkdir()
                for name in ('success-first.bin', 'success-second.bin', 'cancel.bin', 'revoke.bin', 'fail.bin',
                             'refuse.bin', 'close.bin', 'close-confirm.bin'):
                    with (source / name).open('wb') as stream:
                        stream.truncate(8 * 1024 * 1024)
                # Two declared copy steps use distinct real selections; each still confirms natively.
                manifest['workflows'][0]['steps'] = ['copy', 'copy', 'refresh']
            manifest_path = run / 'manifest.json'
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
            env = {key: value for key, value in os.environ.items() if not key.startswith('PULSE_')}
            env.update(LOCALAPPDATA=str(run / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
                       PULSE_TEST_EXTENSION_CAPTURE=str(run / 'capture'))
            validation = subprocess.run([str(args.pulse.resolve()), '--extension-validate-json', str(manifest_path)],
                                        env=env, capture_output=True, timeout=30)
            (run / 'validation.json').write_bytes(validation.stdout)
            if validation.returncode or not json.loads(validation.stdout).get('valid'):
                raise RuntimeError('Native workflow validation failed: ' + page)
            with (run / 'process.log').open('wb') as log:
                process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', '--extension-manifest',
                                            str(manifest_path), str(folder)], env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    code = process.wait(timeout=240)
                except BaseException:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=15)
                    raise
            if child_process(process.pid, ('pulse.index.exe', 'pulse.content.exe')) is not None:
                raise RuntimeError('Isolated probe left a per-user search helper running')
            report = (run / 'capture/report.txt').read_text(encoding='utf-8')
            images = sorted((run / 'capture').glob('*.png'))
            expected = 3 if page == 'workflow-navigation' else 15
            valid_images = len(images) == expected and all(image.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for image in images)
            unchanged = (folder / 'unchanged.txt').read_text(encoding='utf-8') == 'isolated workflow fixture\n'
            results[page] = {'noSearchHelpers': True, 'exitCode': code, 'passes': report.count('[PASS]'), 'failures': report.count('[FAIL]'),
                             'screenshots': len(images), 'validScreenshotInventory': valid_images, 'fixtureUnchanged': unchanged}
            if code or '[FAIL]' in report or 'Result: PASS' not in report or not valid_images or not unchanged:
                raise RuntimeError('Workflow acceptance failed: ' + page + '; inspect capture/report.txt')
            if page == 'transfer-workflow':
                for name in ('success-first.bin', 'success-second.bin'):
                    if not (target / name).is_file() or digest(source / name) != digest(target / name):
                        raise RuntimeError('Confirmed workflow output differs from isolated source: ' + name)
                for name in ('fail.bin', 'refuse.bin', 'close.bin', 'close-confirm.bin'):
                    if (target / name).exists():
                        raise RuntimeError('Failed, refused or closed workflow produced destination: ' + name)
            print(page + ': ' + str(results[page]['passes']) + ' real-host checks, ' + str(len(images)) + ' screenshots')
    finally:
        (output / 'checks.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
