"""Opt-in application-internal UI tree acceptance using an isolated profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--catalog', action='store_true', help='Exercise the full component catalog')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    pulse = args.pulse.resolve()
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    profile = output / 'profile'
    folder = output / 'fixture'
    profile.mkdir()
    folder.mkdir()
    (folder / 'sample.txt').write_text('isolated UI tree fixture\n', encoding='utf-8')
    manifest = root / 'examples/extensions' / ('ui-catalog' if args.catalog else 'ui-tree') / 'manifest.json'
    env = {key: value for key, value in os.environ.items() if not key.startswith('PULSE_')}
    env.update(LOCALAPPDATA=str(output / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
               PULSE_TEST_EXTENSION_CAPTURE=str(output / 'capture'))
    completed = False
    try:
        validation = subprocess.run([str(pulse), '--extension-validate-json', str(manifest)],
                                    env=env, capture_output=True, timeout=30)
        (output / 'validation.json').write_bytes(validation.stdout)
        if validation.returncode or not json.loads(validation.stdout).get('valid'):
            raise RuntimeError('Native host rejected the UI tree template: ' + validation.stdout.decode('utf-8', errors='replace'))
        with (output / 'process.log').open('wb') as log:
            process = subprocess.Popen([str(pulse), '--test-instance', '--extension-manifest', str(manifest), str(folder)],
                                       env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=240 if args.catalog else 120)
            except BaseException:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=15)
                raise
        report = (output / 'capture/report.txt').read_text(encoding='utf-8')
        if code or '[FAIL]' in report or 'Result: PASS' not in report:
            raise RuntimeError('Application tree acceptance failed; inspect capture/report.txt')
        images = sorted((output / 'capture').glob('*.png'))
        if args.catalog:
            schema = json.loads((root / 'schemas/extensions/manifest.schema.json').read_text(encoding='utf-8'))
            expected_images = len(schema['$defs']['uiNode']['oneOf']) + 7
        else:
            expected_images = 8
        if len(images) != expected_images or any(image.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n' for image in images):
            raise RuntimeError('Unexpected application-internal PNG capture inventory')
        if (folder / 'sample.txt').read_text(encoding='utf-8') != 'isolated UI tree fixture\n':
            raise RuntimeError('Read-only UI probe changed its fixture')
        completed = True
        print('{} real-host checks passed; {} internal screenshots'.format(report.count('[PASS]'), len(images)))
    finally:
        (output / 'checks.json').write_text(json.dumps({'completed': completed}, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
