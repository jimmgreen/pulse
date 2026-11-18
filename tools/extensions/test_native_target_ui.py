"""Install the public directory-transfer DLL and exercise V2/V3/V4 on isolated files."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def files(root):
    return {str(path.relative_to(root)).replace('\\', '/'): sha(path)
            for path in root.rglob('*') if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.pulse, args.plugin, args.output = (path.resolve() for path in (args.pulse, args.plugin, args.output))
    if args.output.exists():
        parser.error('output must be a new isolated directory')
    root = Path(__file__).resolve().parents[2]
    profile = args.output/'profile'
    fixture, target = profile/'fixture', profile/'target'
    fixture.mkdir(parents=True)
    target.mkdir()
    (fixture/'folder').mkdir()
    (fixture/'folder'/'nested').mkdir()
    (fixture/'folder'/'empty').mkdir()
    (fixture/'folder'/'nested'/'tile.txt').write_bytes(b'isolated nested desktop tile data\n')
    for name in ('keep.txt', 'cancel.txt', 'copy.txt', 'move.txt', 'close.txt',
                 'close-confirm.txt', 'collision.txt', 'replace.txt', 'disable.txt',
                 'v3-copy.txt', 'v3-move.txt', 'v3-metadata.txt', 'v3-lease.txt'):
        (fixture/name).write_bytes(('isolated V3 transfer fixture: '+name+'\n').encode())
    (target/'collision.txt').write_bytes(b'Original destination sentinel; must never be overwritten.\n')
    before_source, before_target = files(fixture), files(target)
    expected_source = dict(before_source)
    del expected_source['move.txt']
    del expected_source['v3-move.txt']
    expected_target = {**before_target, 'copy.txt': before_source['copy.txt'], 'move.txt': before_source['move.txt'],
                       'v3-copy.txt': before_source['v3-copy.txt'], 'v3-move.txt': before_source['v3-move.txt'],
                       'folder/nested/tile.txt': before_source['folder/nested/tile.txt']}
    author = args.output/'author'
    author.mkdir()
    copied = author/args.plugin.name
    shutil.copyfile(args.plugin, copied)
    manifest, package = author/'manifest.json', author/'directory-transfer.pulse-ext'
    cli = [os.sys.executable, str(root/'tools/extensions/pulse_ext.py'), '--pulse', str(args.pulse), '--json']
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(profile/'capture'),
               PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='directory-transfer',
               LOCALAPPDATA=str(args.output/'local-app-data'))
    result = dict(hostSha256=sha(args.pulse), pluginSha256=sha(args.plugin), commands=[],
                  fixtureSha256Before=before_source, targetSha256Before=before_target,
                  fixtureSha256Expected=expected_source, targetSha256Expected=expected_target)
    try:
        for command in [
            cli+['native-manifest', str(root/'examples/native/directory-transfer/manifest.json'), '--library', str(copied), '--output', str(manifest)],
            cli+['pack', str(manifest), '--output', str(package)],
            cli+['verify', str(package)],
            cli+['install', str(package), '--store', str(profile/'extensions'), '--allow-native', '--accept-capabilities'],
            cli+['enable', 'sample.native-directory-transfer', '--store', str(profile/'extensions'), '--allow-native'],
            [str(args.pulse), '--test-instance', str(fixture)]]:
            run = subprocess.run(command, env=env, capture_output=True, timeout=150)
            result['commands'].append(dict(command=command, exit=run.returncode,
                stdout=run.stdout.decode('utf-8', errors='replace'), stderr=run.stderr.decode('utf-8', errors='replace')))
            if run.returncode:
                raise AssertionError('command failed: '+str(command))
        report = (profile/'capture/report.txt').read_text(encoding='utf-8')
        startup = (profile/'capture/package-startup.txt').read_text(encoding='utf-8')
        images = list((profile/'capture').glob('*.png'))
        result.update(passCount=report.count('[PASS]'), failCount=report.count('[FAIL]'), startupChecks=startup.count('[PASS]'),
                      fixtureSha256After=files(fixture), targetSha256After=files(target),
                      screenshots=[str(path) for path in images], report=report, startupReport=startup)
        result['fixtureMatchesExpected'] = result['fixtureSha256After'] == expected_source and result['targetSha256After'] == expected_target
        assert '[FAIL]' not in report+startup and 'Result: PASS' in report, 'host behavioral assertions failed'
        assert result['fixtureMatchesExpected'] and result['startupChecks'] == 2, 'file outcome or package startup mismatch'
        assert not (profile/'target-old').exists(), 'target identity replacement fixture was not restored'
        assert len(images) == 11 and all(path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n') and path.stat().st_size > 4096 for path in images), 'eleven application screenshots required'
        result['passed'] = True
    except Exception as error:
        result['passed'] = False
        result['error'] = str(error)
    (args.output/'report.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key in (
        'hostSha256', 'pluginSha256', 'passCount', 'failCount', 'startupChecks',
        'fixtureMatchesExpected', 'screenshots', 'passed', 'error')}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
