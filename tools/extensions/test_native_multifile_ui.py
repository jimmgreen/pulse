"""Real archive sample install/upgrade/rollback with an isolated host profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--sample-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--preview-fixtures', type=Path)
    parser.add_argument('--task-fixtures', type=Path)
    parser.add_argument('--settings-fixtures', action='store_true')
    args = parser.parse_args()
    pulse, source, output = args.pulse.resolve(), args.sample_build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cli = [sys.executable, str(ROOT/'tools/extensions/pulse_ext.py'), '--pulse', str(pulse), '--json']
    store = output/'profile/extensions'
    def run(arguments, expected=0, env=None):
        result = subprocess.run([str(x) for x in arguments], capture_output=True, timeout=120, env=env)
        if result.returncode != expected:
            raise RuntimeError(str(arguments)+'\n'+result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
        text = result.stdout.decode('utf-8')
        return json.loads(text) if text.startswith('{') else text
    def author(label, update=False):
        folder = output/label; folder.mkdir()
        for name in ('pulse_archive.dll', '7z.dll', 'License.txt', 'NOTICE.txt'): shutil.copy2(source/name, folder/name)
        if update:
            with (folder/'NOTICE.txt').open('ab') as stream: stream.write(b'\nPackage revision test.\n')
        manifest = folder/'manifest.json'
        run(cli+['native-manifest', ROOT/'sdk/native/samples/archive/manifest.json', '--library', folder/'pulse_archive.dll',
                 '--file', folder/'7z.dll', '--file', folder/'License.txt', '--file', folder/'NOTICE.txt', '--output', manifest])
        package = folder/'archive.pulse-ext'
        run(cli+['pack', manifest, '--output', package]); run(cli+['verify', package])
        installed = run(cli+['install', package, '--store', store, '--allow-native'])
        return installed['installation']['package']['activeRevision']
    def launch(label):
        profile = output/'profile'; capture = profile/label; fixture = profile/'fixture'; fixture.mkdir(exist_ok=True)
        if args.preview_fixtures:
            for name in ('sample.7z', 'sample.rar'):
                shutil.copy2(args.preview_fixtures/name, fixture/name)
        else:
            (fixture/'sample.7z').write_bytes(b'command selection fixture')
        if args.task_fixtures:
            if not args.preview_fixtures:
                raise ValueError('--task-fixtures requires --preview-fixtures for the real small 7z fixture')
            for name in ('progress.7z', 'ordinary.rar'):
                shutil.copy2(args.task_fixtures/name, fixture/name)
            (fixture/'corrupt.7z').write_bytes(b'7z\xbc\xaf\x27\x1c\x00\x04corrupt archive fixture')
        temporary = profile/'temporary'; temporary.mkdir(exist_ok=True)
        (fixture/'notes.txt').write_text('ordinary file')
        env = {k:v for k,v in os.environ.items() if not k.upper().startswith('PULSE_')}
        env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(capture), PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE='archive',
                   LOCALAPPDATA=str(profile/'local-app-data'), TEMP=str(temporary), TMP=str(temporary))
        if args.settings_fixtures:
            env['PULSE_TEST_NATIVE_SETTINGS'] = '1'
            if label != 'installed': env['PULSE_TEST_SETTINGS_RESTART'] = '1'
        try:
            run([pulse, '--test-instance', fixture], env=env)
        except RuntimeError as error:
            evidence = capture/'report.txt'
            if evidence.exists(): raise RuntimeError(evidence.read_text(encoding='utf-8')) from error
            raise
        report = (capture/'report.txt').read_text()
        if 'Result: PASS' not in report or '[FAIL]' in report: raise RuntimeError(report)
        print('[PASS] '+label+' real 7-Zip dependency, panel and enable lifecycle')
    first = author('v1')
    run(cli+['enable', 'sample.native-archive', '--store', store, '--allow-native']); launch('installed')
    second = author('v2', True)
    assert second != first
    launch('upgraded')
    run(cli+['rollback', 'sample.native-archive', '--store', store, '--allow-native']); launch('rolled-back')
    registry = json.loads((store/'registry.json').read_text())
    assert registry['packages'][0]['activeRevision'] == first
    for name in ('License.txt', '7z.dll'):
        target = store/'packages'/first/name
        original = target.read_bytes(); target.write_bytes(original+b'tampered')
        before = (store/'registry.json').read_bytes()
        try:
            denied = run(cli+['enable', 'sample.native-archive', '--store', store, '--allow-native'], expected=2)
            assert denied['error']['code'] == 'native_hash_mismatch'
            assert before == (store/'registry.json').read_bytes()
        finally: target.write_bytes(original)
        print('[PASS] tampered '+name+' rejected without registry change')
    print('Result: PASS')
if __name__ == '__main__': main()
