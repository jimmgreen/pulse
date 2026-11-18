"""Opt-in delivery check: run the extracted SDK, its previewer, and an isolated installed-package window."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

from bundle_sdk import regular_bytes, verify_archive

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output, pulse = args.output.absolute(), args.pulse.absolute()
    output.mkdir(exist_ok=False)
    sdk = output / 'SDK 解压'
    sdk.mkdir()
    checks, commands, preview_images = [], [], []
    completed = False
    archive_digest = ""
    native_check_count = 0
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith('PULSE_') and key.upper() not in ('PYTHONPATH', 'PYTHONHOME')}
    env['LOCALAPPDATA'] = str(output / 'local-app-data')
    env['PYTHONDONTWRITEBYTECODE'] = '1'

    def check(ok, label):
        checks.append({'name': label, 'passed': bool(ok)})
        print(('[' + ('PASS' if ok else 'FAIL') + '] ' + label), flush=True)
        if not ok:
            raise AssertionError(label)

    def call(*arguments, expected=0, wrapper=False):
        prefix = ['cmd', '/d', '/c', 'pulse-ext.cmd'] if wrapper else [sys.executable,
            str(sdk / 'tools/extensions/pulse_ext.py'), '--preview-host', str(sdk / 'bin/Pulse.ExtensionPreview.exe')]
        process = subprocess.run(prefix + ['--pulse', str(pulse), '--json', *map(str, arguments)],
            cwd=sdk, env=env, capture_output=True, timeout=90,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            result = json.loads(process.stdout.decode('utf-8'))
        except (ValueError, UnicodeError) as error:
            raise AssertionError(str(arguments[0]) + ': ' + process.stderr.decode('utf-8', errors='replace')[:1000]) from error
        commands.append({'arguments': list(map(str, arguments)), 'wrapper': wrapper,
                         'exitCode': process.returncode, 'result': result})
        check(process.returncode == expected and result['ok'] == (expected == 0),
              ('wrapper ' if wrapper else 'extracted CLI ') + str(arguments[0]) + ' exits ' + str(expected))
        return result

    try:
        archive_digest = hashlib.sha256(regular_bytes(args.bundle, 65 * 1024 * 1024)).hexdigest()
        release, files = verify_archive(args.bundle)
        check(hashlib.sha256(regular_bytes(args.bundle, 65 * 1024 * 1024)).hexdigest() == archive_digest, "SDK archive remains unchanged during verification")
        check(hashlib.sha256(regular_bytes(pulse)).hexdigest() == release['testedHostSha256'],
              'acceptance uses the exact host validated during SDK publication')
        for name, data in files.items():
            path = sdk / name
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open('xb') as stream:
                stream.write(data)
        (sdk / 'sdk-release.json').write_text(json.dumps(release, ensure_ascii=False, indent=2), encoding='utf-8')
        check(not (sdk / '.git').exists() and not (sdk / 'src').exists(), 'extracted SDK has no checkout or application source dependency')
        api = call('inspect-sdk', wrapper=True)['api']
        check(api == json.loads(files['schemas/extensions/host-api.json']), 'extracted discovery equals packaged host contract')
        comparison = call('compare-sdk', sdk / 'schemas/extensions/host-api.json', '--require-identical', wrapper=True)['comparison']
        check(comparison['identical'] and comparison['commandsExecuted'] == 0 and not comparison['runtimeCompatibilityCertified'],
              'packaged contract comparison preserves identity without claiming execution compatibility')
        changed_baseline = json.loads(files['schemas/extensions/host-api.json'])
        changed_baseline.pop('contractIdentity', None)
        changed_baseline['capabilities'].append('fixture.unavailable')
        changed_path = output / 'changed-baseline.json'
        changed_path.write_text(json.dumps(changed_baseline), encoding='utf-8')
        changed = call('compare-sdk', changed_path, '--require-identical', expected=2)
        check(changed['error']['code'] == 'contract_changed' and changed['error']['comparison']['capabilities']['removed'] == ['fixture.unavailable'],
              'packaged comparison reports removed capability even when apiVersion remains one')
        doctor = call('doctor', wrapper=True)['doctor']
        check(doctor['healthy'] and doctor['checks']['previewHost']['present'] and doctor['checks']['previewHost']['executed'] is False,
              'root launcher resolves its packaged native previewer')
        created = {}
        for template in release['templates']:
            target = output / ('author-' + template)
            result = call('init', target, '--id', 'delivery.' + template, '--name', 'SDK 示例', '--template', template)
            created[template] = Path(result['manifest'])
            tested = call('test', created[template])['test']
            check(tested['functionalExecutionTested'] is False, template + ' validated without claiming execution')
        call('init', output / 'author-transfer-panel', '--id', 'overwrite', '--name', 'Overwrite', expected=2)
        for template in ('transfer-panel', 'workflow-navigation', 'transfer-workflow', 'resource-client', 'selection-sort-panel', 'workspace-control-panel'):
            preview_output = output / ('preview' if template == 'transfer-panel' else 'preview-' + template)
            preview = call('preview', created[template], '--output', preview_output,
                           '--state', 'selected', wrapper=True)['preview']
            check(preview['mockData'] is True and type(preview['commandsExecuted']) is int and
                  preview['commandsExecuted'] == 0,
                  template + ' packaged preview uses mock data and executes no commands')
            expected = {f'panel-{theme}-{scale}.png': (theme, scale)
                        for theme in ('light', 'dark') for scale in (100, 150, 200)}
            check(len(preview['images']) == len(expected) and
                  {Path(image).name for image in preview['images']} == set(expected),
                  template + ' preview covers both themes at 100/150/200 percent')
            for image in preview['images']:
                path = Path(image)
                data = path.read_bytes()
                valid_header = (len(data) >= 33 and data[:8] == b'\x89PNG\r\n\x1a\n' and
                                data[8:16] == b'\x00\x00\x00\rIHDR')
                width, height = struct.unpack('>II', data[16:24]) if valid_header else (0, 0)
                check(valid_header and width > 100 and height > 100,
                      template + ' packaged preview produces ' + path.name)
                theme, scale = expected[path.name]
                preview_images.append({'template': template, 'theme': theme, 'scalePercent': scale,
                                       'path': str(path), 'width': width, 'height': height,
                                       'mockData': True, 'commandsExecuted': 0})
        store = output / 'store'
        manifests = []
        archives = []
        for identifier, mode in (('delivery.alpha', 'details'), ('delivery.beta', 'tiles')):
            manifest = {
                'apiVersion': 1, 'id': identifier, 'name': 'SDK shared name',
                'commands': [{'id': 'switch', 'title': 'Switch view', 'capability': 'view.setMode', 'args': {'mode': mode}}],
                'toolbar': {'commands': ['switch']},
                'panel': {'title': 'Installed SDK panel', 'commands': ['switch'],
                          'fields': [{'label': 'View', 'binding': 'context.viewMode'}]}}
            source = output / (identifier + '.json')
            source.write_text(json.dumps(manifest), encoding='utf-8')
            archive = output / (identifier + '.zip')
            call('pack', source, '--output', archive)
            call('verify', archive)
            call('install', archive, '--store', store)
            registry = call('list', '--store', store)['registry']
            check(not next(item for item in registry['packages'] if item['id'] == identifier)['enabled'], 'fresh installation is disabled: ' + identifier)
            call('enable', identifier, '--store', store)
            manifests.append(manifest)
            archives.append(archive)
        before = (store / 'registry.json').read_bytes()
        call('install', archives[0], '--store', store)
        check((store / 'registry.json').read_bytes() == before, 'identical revision does not rewrite the store')
        upgraded = dict(manifests[0])
        upgraded['commands'] = manifests[0]['commands'] + [{'id': 'up', 'title': 'Up', 'capability': 'navigation.up'}]
        source = output / 'upgrade.json'
        source.write_text(json.dumps(upgraded), encoding='utf-8')
        upgrade = output / 'upgrade.zip'
        call('pack', source, '--output', upgrade)
        refused = call('install', upgrade, '--store', store, expected=2)
        check(refused['error']['code'] == 'capability_confirmation_required' and (store / 'registry.json').read_bytes() == before,
              'upgrade capability expansion is refused without changing active state')
        call('install', upgrade, '--store', store, '--accept-capabilities')
        call('rollback', manifests[0]['id'], '--store', store)
        registry = json.loads((store / 'registry.json').read_text(encoding='utf-8'))
        original = json.loads(before.decode('utf-8'))
        check([entry['activeRevision'] for entry in registry['packages']] == [entry['activeRevision'] for entry in original['packages']],
              'rollback restores both intended active revisions')
        profile = output / 'native-profile'
        folder = profile / 'fixture'
        folder.mkdir(parents=True)
        (folder / 'sample.txt').write_text('SDK isolated fixture', encoding='utf-8')
        capture = profile / 'capture'
        native_env = dict(env, PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_STORE_CAPTURE=str(capture))
        with (output / 'native-process.log').open('wb') as log:
            process = subprocess.Popen([str(pulse), '--test-instance', '--extension-store', str(store), str(folder)],
                                       env=native_env, stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=120)
            except BaseException:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=20)
                raise
        report = (capture / 'report.txt').read_text(encoding='utf-8')
        check(code == 0 and '[FAIL]' not in report and 'Result: PASS' in report,
              'SDK-installed packages execute in real isolated UI and produce internal screenshots')
        native_check_count = report.count("[PASS]")
        check(json.loads((store / 'registry.json').read_text(encoding='utf-8')) == registry,
              'session UI controls do not rewrite persistent store state')
        for manifest in manifests:
            call('disable', manifest['id'], '--store', store)
            call('uninstall', manifest['id'], '--store', store)
        check(call('list', '--store', store)['registry']['packages'] == [], 'SDK uninstall removes both registrations')
        with (output / 'run-check.log').open('wb') as log:
            result = subprocess.run([sys.executable, str(ROOT / 'tools/extensions/live_store_run_check.py'),
                '--sdk-root', str(sdk), '--pulse', str(pulse), '--output', str(output / 'run-check')],
                env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        check(result.returncode == 0, 'extracted CLI run launches and closes its own installed session')
        check(all(regular_bytes(sdk / item['path']) == files[item['path']] for item in release['files']),
              'author workflow leaves every shipped SDK file unchanged')
        check(hashlib.sha256(regular_bytes(args.bundle, 65 * 1024 * 1024)).hexdigest() == archive_digest, 'accepted SDK archive is still the exact supplied artifact')
        completed = True
    finally:
        (output / 'checks.json').write_text(json.dumps({'completed': completed, 'archiveSha256': archive_digest, 'nativeChecksPassed': native_check_count, 'checks': checks, 'commands': commands, 'previewImages': preview_images}, ensure_ascii=False, indent=2), encoding='utf-8')
    print(str(len(checks)) + ' delivery checks passed; evidence: ' + str(output), flush=True)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
