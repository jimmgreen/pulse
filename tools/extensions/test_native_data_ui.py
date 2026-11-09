"""Install an independently compiled provider DLL and verify real in-app data updates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


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
    profile = args.output/'profile'
    fixture = profile/'fixture'
    fixture.mkdir(parents=True)
    (fixture/'untouched.txt').write_text('Native provider fixture: unchanged\n', encoding='utf-8')
    before = {p.name:sha(p) for p in fixture.iterdir()}
    author = args.output/'author'
    author.mkdir()
    import shutil
    copied = author/args.plugin.name
    shutil.copyfile(args.plugin, copied)
    store = profile/'extensions'
    manifest, package = author/'manifest.json', author/'live-data.pulse-ext'
    cli = [os.sys.executable, str(root/'tools/extensions/pulse_ext.py'), '--pulse', str(args.pulse), '--json']
    env = {k:v for k,v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(profile/'capture'),
        PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='live-data', LOCALAPPDATA=str(args.output/'local-app-data'))
    result = dict(hostSha256=sha(args.pulse), pluginSha256=sha(args.plugin), commands=[])
    try:
        for command in [
            cli+['native-manifest', str(root/'examples/native/live-data/manifest.json'), '--library', str(copied), '--output', str(manifest)],
            cli+['pack', str(manifest), '--output', str(package)],
            cli+['verify', str(package)],
            cli+['install', str(package), '--store', str(store), '--allow-native', '--accept-capabilities'],
            cli+['enable', 'sample.native-live-data', '--store', str(store), '--allow-native'],
            [str(args.pulse), '--test-instance', str(fixture)]]:
            run = subprocess.run(command, env=env, capture_output=True, timeout=120)
            result['commands'].append(dict(command=command, exit=run.returncode,
                stdout=run.stdout.decode('utf-8',errors='replace'), stderr=run.stderr.decode('utf-8',errors='replace')))
            if run.returncode:
                raise AssertionError('command failed: '+str(command))
        text = (profile/'capture/report.txt').read_text(encoding='utf-8')
        startup = (profile/'capture/package-startup.txt').read_text(encoding='utf-8')
        images = list((profile/'capture').glob('*.png'))
        result.update(passCount=text.count('[PASS]'), failCount=text.count('[FAIL]'),
            startupChecks=startup.count('[PASS]'), filesUnchanged=before=={p.name:sha(p) for p in fixture.iterdir()},
            screenshots=[str(p) for p in images], report=text, startupReport=startup)
        assert '[FAIL]' not in text+startup and 'Result: PASS' in text and result['filesUnchanged']
        assert len(images)==5 and all(p.read_bytes().startswith(b'\x89PNG\r\n\x1a\n') and p.stat().st_size>4096 for p in images)
        assert result['startupChecks']==2
        result['passed'] = True
    except Exception as error:
        result['passed'] = False
        result['error'] = str(error)
    (args.output/'report.json').write_text(json.dumps(result,indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('commands','report','startupReport')},indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
