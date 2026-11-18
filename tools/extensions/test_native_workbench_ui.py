"""Install the public directory-workbench DLL and exercise it in Pulse."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.pulse, args.plugin, args.output = (p.resolve() for p in (args.pulse, args.plugin, args.output))
    if args.output.exists():
        parser.error('output must be a new isolated directory')
    root = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True)
    profile, author = args.output/'profile', args.output/'author'
    fixture = profile/'fixture'
    author.mkdir()
    fixture.mkdir(parents=True)
    for name in ('Folders', 'Images', 'Documents', 'Other'):
        (fixture/name).mkdir()
    for name, content in (('photo.png', b'image fixture'), ('summary.txt', b'document fixture'),
        ('packed.zip', b'other fixture')):
        (fixture/name).write_bytes(content)
    for path, content in ((fixture/'Folders'/'child', None),
        (fixture/'Images'/'image.png', b'image fixture'),
        (fixture/'Documents'/'note.txt', b'document fixture'),
        (fixture/'Other'/'archive.zip', b'other fixture'),
        ):
        if content is None:
            path.mkdir()
        else:
            path.write_bytes(content)
    before = {p.relative_to(fixture).as_posix(): sha(p) for p in fixture.rglob('*') if p.is_file()}
    copied = author/args.plugin.name
    shutil.copyfile(args.plugin, copied)
    store = profile/'extensions'
    manifest, package = author/'manifest.json', author/'directory-workbench.pulse-ext'
    cli = [sys.executable, str(root/'tools/extensions/pulse_ext.py'), '--pulse', str(args.pulse), '--json']
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(profile/'capture'),
        PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='directory-workbench',
        LOCALAPPDATA=str(args.output/'local-app-data'))
    result = dict(hostSha256=sha(args.pulse), pluginSha256=sha(args.plugin), commands=[])
    try:
        commands = [
            cli+['native-manifest', str(root/'examples/native/directory-workbench/manifest.json'), '--library', str(copied), '--output', str(manifest)],
            cli+['pack', str(manifest), '--output', str(package)],
            cli+['verify', str(package)],
            cli+['install', str(package), '--store', str(store), '--allow-native', '--accept-capabilities'],
            cli+['enable', 'sample.native-directory-workbench', '--store', str(store), '--allow-native'],
            [str(args.pulse), '--test-instance', str(fixture)],
        ]
        for command in commands:
            completed = subprocess.run(command, env=env, capture_output=True, timeout=120)
            result['commands'].append(dict(command=command, exit=completed.returncode,
                stdout=completed.stdout.decode('utf-8', errors='replace'), stderr=completed.stderr.decode('utf-8', errors='replace')))
            if completed.returncode:
                raise AssertionError('command failed: '+str(command))
        capture = profile/'capture'
        report = (capture/'report.txt').read_text(encoding='utf-8')
        startup = (capture/'package-startup.txt').read_text(encoding='utf-8')
        images = sorted(capture.glob('workbench-*.png'))
        unchanged = all(sha(fixture/name) == digest for name, digest in before.items())
        added = fixture/'workbench-probe-watch.txt'
        result.update(report=report, startupReport=startup, passCount=report.count('[PASS]'),
            failCount=report.count('[FAIL]'), screenshots=[str(p) for p in images],
            originalFixtureFilesUnchanged=unchanged,
            plannedWatchMutationVerified=added.is_file() and added.read_bytes() == b'isolated watch fixture\n')
        assert '[FAIL]' not in report+startup and 'Result: PASS' in report
        assert startup.count('[PASS]') == 2 and len(images) == 5
        assert all(p.read_bytes().startswith(b'\x89PNG\r\n\x1a\n') and p.stat().st_size > 4096 for p in images)
        assert unchanged and result['plannedWatchMutationVerified']
        result['passed'] = True
    except Exception as error:
        result.update(passed=False, error=str(error))
    (args.output/'report.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key not in ('commands', 'report', 'startupReport')}, indent=2))
    return 0 if result.get('passed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
