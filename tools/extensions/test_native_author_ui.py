"""Exercise the independently authored Number Lens package in a test-hook host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    host, package, output = (p.resolve() for p in (args.pulse, args.package, args.output))
    output.mkdir(parents=True, exist_ok=False)
    fixture = output/'fixture'
    fixture.mkdir()
    cli = [sys.executable, str(Path(__file__).with_name('pulse_ext.py')), '--pulse', str(host), '--json']
    store = output/'extensions'
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(output), PULSE_TEST_NATIVE_CASE='number-lens',
        PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_EXTENSION_CAPTURE=str(output/'capture'))
    report = dict(hostSha256=hashlib.sha256(host.read_bytes()).hexdigest(),
        packageSha256=hashlib.sha256(package.read_bytes()).hexdigest(), commands=[])
    try:
        for command in (cli+['install', str(package), '--store', str(store), '--allow-native'],
                        cli+['enable', 'independent.number-lens', '--store', str(store), '--allow-native'],
                        [str(host), '--test-instance', '--extension-store', str(store), str(fixture)]):
            result = subprocess.run(command, env=env, capture_output=True, timeout=120)
            report['commands'].append(dict(command=command, exit=result.returncode,
                stdout=result.stdout.decode('utf-8', errors='replace'), stderr=result.stderr.decode('utf-8', errors='replace')))
            if result.returncode:
                raise RuntimeError('command failed: '+str(command))
        text = (output/'capture/report.txt').read_text(encoding='utf-8')
        images = sorted((output/'capture').glob('author-*.png'))
        report.update(report=text, screenshots=[str(p) for p in images])
        report['passed'] = '[FAIL]' not in text and 'Result: PASS' in text and len(images) == 3
    except Exception as error:
        report.update(passed=False, error=str(error))
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
