"""Verify focused selection/sort capabilities in an isolated actual Pulse instance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from live_dev_check import child_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    profile = output / 'profile'
    folder = profile / 'fixture'
    folder.mkdir(parents=True)
    (profile / 'empty').mkdir()
    (profile / 'other').mkdir()
    (profile / 'other' / 'unchanged.txt').write_text('other pane fixture', encoding='utf-8')
    for index, (name, size) in enumerate((('alpha.txt', 50), ('bravo.bin', 400), ('charlie.txt', 120), ('delta.log', 8))):
        file = folder / name
        file.write_bytes(bytes([index + 1]) * size)
        os.utime(file, (1600000000 + index * 500, 1600000000 + (3 - index) * 900))
    files = list(folder.iterdir()) + [profile / 'other' / 'unchanged.txt']
    before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    manifest = root / 'examples/extensions/selection-sort-panel/manifest.json'
    env = {key: value for key, value in os.environ.items() if not key.startswith('PULSE_')}
    env.update(LOCALAPPDATA=str(output / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
               PULSE_TEST_EXTENSION_CAPTURE=str(output / 'capture'))
    validation = subprocess.run([str(args.pulse.resolve()), '--extension-validate-json', str(manifest)],
                                env=env, capture_output=True, timeout=30)
    (output / 'validation.json').write_bytes(validation.stdout)
    if validation.returncode or not json.loads(validation.stdout).get('valid'):
        raise RuntimeError('Native template validation failed')
    with (output / 'process.log').open('wb') as log:
        process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', '--extension-manifest',
                                    str(manifest), str(folder)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=240)
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
            raise
    report = (output / 'capture/report.txt').read_text(encoding='utf-8')
    images = sorted((output / 'capture').glob('*.png'))
    image_ok = len(images) == 6 and all(image.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for image in images)
    unchanged = all(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == before[str(path)] for path in files)
    no_helpers = child_process(process.pid, ('pulse.index.exe', 'pulse.content.exe')) is None
    result = dict(exitCode=code, passes=report.count('[PASS]'), failures=report.count('[FAIL]'),
                  screenshots=len(images), validScreenshotInventory=image_ok, fixtureUnchanged=unchanged,
                  noSearchHelpers=no_helpers)
    (output / 'checks.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if code or '[FAIL]' in report or 'Result: PASS' not in report or not image_ok or not unchanged or not no_helpers:
        raise RuntimeError('Focused directory acceptance failed; inspect capture/report.txt')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
