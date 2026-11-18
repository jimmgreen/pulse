"""Install the public C++ fileops example and verify recycle behavior in an isolated host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    fixture = args.output/'profile/fixture'
    fixture.mkdir(parents=True)
    (fixture/'folder').mkdir()
    for name in ('keep.txt', 'cancel.txt', 'cancel-confirm.txt', 'recycle.txt', 'disabled.txt'):
        (fixture/name).write_bytes(('isolated native fileops fixture: '+name+'\n').encode())
    before = {path.name: sha(path) for path in fixture.iterdir() if path.is_file()}
    author = args.output/'author'
    author.mkdir()
    copied = author/args.plugin.name
    shutil.copyfile(args.plugin, copied)
    manifest, package = author/'manifest.json', author/'file-operations.pulse-ext'
    profile = fixture.parent
    cli = [os.sys.executable, str(root/'tools/extensions/pulse_ext.py'), '--pulse', str(args.pulse), '--json']
    env = {key:value for key,value in os.environ.items() if not key.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(profile/'capture'),
        PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='file-operations', LOCALAPPDATA=str(args.output/'local-app-data'))
    result = dict(hostSha256=sha(args.pulse), pluginSha256=sha(args.plugin), fixtureSha256Before=before, commands=[])
    try:
        for command in [
            cli+['native-manifest', str(root/'examples/native/file-operations/manifest.json'), '--library', str(copied), '--output', str(manifest)],
            cli+['pack', str(manifest), '--output', str(package)],
            cli+['verify', str(package)],
            cli+['install', str(package), '--store', str(profile/'extensions'), '--allow-native', '--accept-capabilities'],
            cli+['enable', 'sample.native-file-operations', '--store', str(profile/'extensions'), '--allow-native'],
            [str(args.pulse), '--test-instance', str(fixture)]]:
            run = subprocess.run(command, env=env, capture_output=True, timeout=120)
            result['commands'].append(dict(command=command, exit=run.returncode,
                stdout=run.stdout.decode('utf-8', errors='replace'), stderr=run.stderr.decode('utf-8', errors='replace')))
            if run.returncode:
                raise AssertionError('command failed: '+str(command))
        text = (profile/'capture/report.txt').read_text(encoding='utf-8')
        startup = (profile/'capture/package-startup.txt').read_text(encoding='utf-8')
        images = list((profile/'capture').glob('*.png'))
        after = {name: sha(fixture/name) if (fixture/name).is_file() else None for name in before}
        result.update(passCount=text.count('[PASS]'), failCount=text.count('[FAIL]'), startupChecks=startup.count('[PASS]'),
            fixtureRestored=after==before, fixtureSha256After=after,
            screenshots=[str(path) for path in images], report=text, startupReport=startup)
        assert '[FAIL]' not in text+startup and 'Result: PASS' in text, 'host behavioral assertions failed'
        assert result['fixtureRestored'] and result['startupChecks']==2, 'fixture restoration or package startup failed'
        assert len(images)==5 and all(path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n') and path.stat().st_size>4096 for path in images), 'five application screenshots required'
        result['passed'] = True
    except Exception as error:
        result['passed'] = False
        result['error'] = str(error)
    (args.output/'report.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({key:value for key,value in result.items() if key not in ('commands', 'report', 'startupReport', 'fixtureSha256Before', 'fixtureSha256After')}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
