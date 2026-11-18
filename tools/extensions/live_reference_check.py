from live_dev_check import child_process
"""Run real reference pages inside an isolated Pulse host; never substitutes mock data."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--page', choices=('settings-form', 'collection-browser', 'task-monitor', 'property-sheet', 'resource-client', 'all'), default='all')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    pages = ('settings-form', 'collection-browser', 'task-monitor', 'property-sheet') if args.page == 'all' else (args.page,)
    results = {}
    try:
        for page in pages:
            run = output / page
            run.mkdir()
            profile = run / 'profile'
            folder = profile / 'fixture'
            profile.mkdir()
            folder.mkdir()
            for index in range(137):
                (folder / ('entry-%03d.txt' % index)).write_text('reference fixture %d\n' % index, encoding='utf-8')
            if page == 'task-monitor':
                source, target = profile / 'source', profile / 'target'
                source.mkdir()
                target.mkdir()
                # A one-shot isolated checkpoint holds the worker after its first real write.
                for name, size in (('cancel.bin', 8 * 1024 * 1024), ('complete.bin', 8 * 1024 * 1024)):
                    with (source / name).open('wb') as stream:
                        stream.truncate(size)
            resource_hashes = {}
            if page == 'resource-client':
                source, target = profile / 'resource-source', profile / 'resource-target'
                source.mkdir()
                target.mkdir()
                for name in ('cancel.bin', 'complete.bin'):
                    payload = (('resource real transfer ' + name + '\n').encode('utf-8') * 4096)
                    with (source / name).open('wb') as stream:
                        for _ in range(64):
                            stream.write(payload)
                    resource_hashes[name] = hashlib.sha256((source / name).read_bytes()).hexdigest()
            manifest = root / 'examples/extensions' / page / 'manifest.json'
            if page == 'resource-client':
                # Test-only permission: production resource-client remains a read/settings declaration.
                fixture_manifest = json.loads(manifest.read_text(encoding='utf-8'))
                fixture_manifest['commands'].append({'id': 'resource-probe-copy', 'title': 'Copy isolated fixture',
                                                     'capability': 'files.copySelectionToOtherPane'})
                manifest = profile / 'resource-probe-manifest.json'
                manifest.write_text(json.dumps(fixture_manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            env = {key: value for key, value in os.environ.items() if not key.startswith('PULSE_')}
            env.update(LOCALAPPDATA=str(run / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
                       PULSE_TEST_EXTENSION_CAPTURE=str(run / 'capture'))
            validation = subprocess.run([str(args.pulse.resolve()), '--extension-validate-json', str(manifest)],
                                        env=env, capture_output=True, timeout=30)
            (run / 'validation.json').write_bytes(validation.stdout)
            if validation.returncode or not json.loads(validation.stdout).get('valid'):
                raise RuntimeError('Native validation failed for ' + page)
            launch_option, launch_path = '--extension-manifest', manifest
            if page == 'resource-client':
                import package_store
                foreign_manifest = dict(fixture_manifest, id='sample.resource-foreign', name='Foreign resource fixture')
                foreign_path = profile / 'resource-foreign-manifest.json'
                foreign_path.write_text(json.dumps(foreign_manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
                foreign_validation = subprocess.run([str(args.pulse.resolve()), '--extension-validate-json', str(foreign_path)],
                                                    env=env, capture_output=True, timeout=30)
                (run / 'foreign-validation.json').write_bytes(foreign_validation.stdout)
                if foreign_validation.returncode or not json.loads(foreign_validation.stdout).get('valid'):
                    raise RuntimeError('Foreign resource fixture failed native validation')
                store = profile / 'resource-store'
                for path in (manifest, foreign_path):
                    package_store.install(store, path.read_bytes())
                    package_store.update(store, json.loads(path.read_text(encoding='utf-8'))['id'], 'enable')
                launch_option, launch_path = '--extension-store', store
            with (run / 'process.log').open('wb') as log:
                process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', launch_option, str(launch_path), str(folder)],
                                           env=env, stdout=log, stderr=subprocess.STDOUT)
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
            expected = {'settings-form': 9, 'collection-browser': 8, 'task-monitor': 9, 'property-sheet': 7, 'resource-client': 6}[page]
            valid_images = len(images) == expected and all(p.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for p in images)
            unchanged = all((folder / ('entry-%03d.txt' % i)).read_text(encoding='utf-8') == 'reference fixture %d\n' % i for i in range(137))
            results[page] = {'noSearchHelpers': True, 'exitCode': code, 'passes': report.count('[PASS]'), 'failures': report.count('[FAIL]'),
                             'screenshots': len(images), 'validScreenshotInventory': valid_images, 'fixtureUnchanged': unchanged}
            if code or '[FAIL]' in report or 'Result: PASS' not in report or not valid_images or not unchanged:
                raise RuntimeError('Reference acceptance failed for ' + page + '; inspect capture/report.txt')
            if page == 'task-monitor':
                copied = profile / 'target/complete.bin'
                if not copied.is_file() or copied.stat().st_size != 8 * 1024 * 1024:
                    raise RuntimeError('Actual confirmed copy did not complete after monitor closed')
            if page == 'resource-client':
                sources_unchanged = all(hashlib.sha256((profile / 'resource-source' / name).read_bytes()).hexdigest() == digest
                                        for name, digest in resource_hashes.items())
                copied = profile / 'resource-target/complete.bin'
                copied_exactly = copied.is_file() and hashlib.sha256(copied.read_bytes()).hexdigest() == resource_hashes['complete.bin']
                cancelled_cleaned = {item.name for item in (profile / 'resource-target').iterdir()} == {'complete.bin'}
                results[page].update(resourceSourcesUnchanged=sources_unchanged, resourceCopyExact=copied_exactly,
                                     resourceCancelledDestinationRemoved=cancelled_cleaned)
                if not (sources_unchanged and copied_exactly and cancelled_cleaned):
                    raise RuntimeError('Resource real-task fixture content/cleanup verification failed')
            if page == 'resource-client':
                lifecycle = run / 'lifecycle'
                lifecycle.mkdir()
                reload_profile = lifecycle / 'profile'
                reload_profile.mkdir()
                reload_folder = reload_profile / 'fixture'
                reload_folder.mkdir()
                for index in range(3):
                    (reload_folder / ('entry-%d.txt' % index)).write_text('reload fixture %d\n' % index, encoding='utf-8')
                reload_manifest = reload_profile / 'resource-reload-manifest.json'
                reload_manifest.write_text(json.dumps(fixture_manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
                reload_env = dict(env, PULSE_TEST_DATA_DIR=str(reload_profile),
                                  PULSE_TEST_EXTENSION_CAPTURE=str(lifecycle / 'capture'),
                                  LOCALAPPDATA=str(lifecycle / 'local-app-data'))
                with (lifecycle / 'process.log').open('wb') as log:
                    reload_process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', '--extension-dev', str(reload_manifest), str(reload_folder)],
                                                      env=reload_env, stdout=log, stderr=subprocess.STDOUT)
                    try:
                        reload_code = reload_process.wait(timeout=90)
                    except BaseException:
                        if reload_process.poll() is None:
                            reload_process.terminate()
                            reload_process.wait(timeout=15)
                        raise
                reload_report = (lifecycle / 'capture/report.txt').read_text(encoding='utf-8')
                no_helpers = child_process(reload_process.pid, ('pulse.index.exe', 'pulse.content.exe')) is None
                reload_unchanged = all((reload_folder / ('entry-%d.txt' % i)).read_text(encoding='utf-8') == 'reload fixture %d\n' % i for i in range(3))
                results[page]['lifecycle'] = {'exitCode': reload_code, 'passes': reload_report.count('[PASS]'),
                                             'failures': reload_report.count('[FAIL]'), 'noSearchHelpers': no_helpers,
                                             'fixtureUnchanged': reload_unchanged}
                if reload_code or '[FAIL]' in reload_report or 'Result: PASS' not in reload_report or not no_helpers or not reload_unchanged:
                    raise RuntimeError('Resource development reload acceptance failed; inspect lifecycle/capture/report.txt')
            print(page + ': ' + str(results[page]['passes']) + ' real-host checks, ' + str(len(images)) + ' screenshots')
    finally:
        (output / 'checks.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
