"""Verify AI authority rejection and real native confirmation in an isolated Pulse window."""
import argparse
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
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    profile = output / 'profile'
    fixture = profile / 'fixture'
    fixture.mkdir(parents=True)
    guarded = fixture / 'guard.txt'
    original = b'isolated AI authorization fixture\n'
    guarded.write_bytes(original)
    env = {key: value for key, value in os.environ.items() if not key.startswith('PULSE_')}
    env.update(LOCALAPPDATA=str(output / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
               PULSE_TEST_EXTENSION_CAPTURE=str(output / 'capture'), PULSE_TEST_AI_AUTHORIZATION='1')
    root = Path(__file__).resolve().parents[2]
    manifest = root / 'examples/extensions/workflow-navigation/manifest.json'
    with (output / 'process.log').open('wb') as log:
        process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', '--extension-manifest',
                                    str(manifest), str(fixture)], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            code = process.wait(timeout=120)
        except BaseException:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
            raise
    if child_process(process.pid, ('pulse.index.exe', 'pulse.content.exe')) is not None:
        raise RuntimeError('Isolated AI probe left a per-user search helper running')
    report = (output / 'capture/report.txt').read_text(encoding='utf-8')
    images = sorted((output / 'capture').glob('*.png'))
    result = {'noSearchHelpers': True, 'exitCode': code, 'passes': report.count('[PASS]'), 'failures': report.count('[FAIL]'),
              'screenshots': len(images), 'unchangedFixture': guarded.read_bytes() == original,
              'validImages': len(images) == 2 and all(p.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for p in images)}
    (output / 'checks.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    if code or '[FAIL]' in report or 'Result: PASS' not in report or not result['unchangedFixture'] or not result['validImages']:
        raise RuntimeError('AI authority acceptance failed; inspect capture/report.txt')
    print(str(result['passes']) + ' real-host authorization checks, two native confirmation captures')


if __name__ == '__main__':
    main()
