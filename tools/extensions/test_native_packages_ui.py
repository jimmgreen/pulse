"""Install real native packages and exercise normal entry points with in-app captures.

All mutations stay under a new output directory. No developer DLL launch flags.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--examples-build', type=Path, required=True)
    parser.add_argument('--missing-entry', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--archives', action='store_true')
    parser.add_argument('--case', choices=('all', 'multiple'), default='all',
                        help='Run only installed package entry points, or the full lifecycle suite')
    args = parser.parse_args()
    for key in ('pulse', 'examples_build', 'missing_entry', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    if args.output.exists():
        parser.error('output must be a new isolated directory')
    args.output.mkdir(parents=True)
    commands, checks, runs = [], [], []
    result = {'hostSha256': digest(args.pulse), 'commands': commands, 'checks': checks, 'runs': runs}
    cli = [sys.executable, str(ROOT / 'tools/extensions/pulse_ext.py'), '--pulse', str(args.pulse), '--json']

    def check(value, label):
        checks.append({'passed': bool(value), 'label': label})
        if not value:
            raise AssertionError(label)

    def command(arguments, expected=0, env=None):
        completed = subprocess.run([str(v) for v in arguments], env=env, capture_output=True, timeout=120)
        item = {'command': [str(v) for v in arguments], 'exitCode': completed.returncode,
                'stdout': completed.stdout.decode('utf-8', errors='replace'),
                'stderr': completed.stderr.decode('utf-8', errors='replace')}
        commands.append(item)
        check(completed.returncode == expected, 'expected exit ' + str(expected) + ' for ' + str(arguments[4:]) +
              ' (got ' + str(completed.returncode) + ')\n' + item['stdout'] + item['stderr'])
        return json.loads(item['stdout']) if item['stdout'].startswith('{') else item

    def author(template, dll, label, package_id=None):
        folder = args.output / 'authors' / label
        folder.mkdir(parents=True)
        copied = folder / dll.name
        shutil.copy2(dll, copied)
        base = json.loads((ROOT / 'examples/native' / template / 'manifest.json').read_text(encoding='utf-8'))
        if package_id:
            base['id'] = package_id
        base['name'] = 'Installed native example'
        (folder / 'template.json').write_text(json.dumps(base), encoding='utf-8')
        manifest = folder / 'manifest.json'
        command(cli + ['native-manifest', folder / 'template.json', '--library', copied, '--output', manifest])
        package = manifest
        if args.archives:
            package = folder / (label + '.pulse-ext')
            command(cli + ['pack', manifest, '--output', package])
            command(cli + ['verify', package])
        return package, manifest

    def install(package, store, **kwargs):
        return command(cli + ['install', package, '--store', store, '--allow-native', '--accept-capabilities'], **kwargs)

    def enable(package_id, store, **kwargs):
        return command(cli + ['enable', package_id, '--store', store, '--allow-native'], **kwargs)

    def launch(profile, label, variant=None):
        fixture = profile / ('fixture-' + label)
        fixture.mkdir(parents=True)
        contents = {'image alpha.jpg': b'native plugin image fixture\x00\xff',
                    'Notes \u03b2.txt': 'native plugin Unicode fixture: \u03b2\n'.encode()} if variant else {
                    'one.txt': b'first text', 'TWO.TXT': b'second text file', 'three.bin': b'\x00\x10\x20\xff'}
        for name, data in contents.items():
            (fixture / name).write_bytes(data)
        capture = profile / ('capture-' + label)
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
        env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(capture),
                   PULSE_TEST_NATIVE_PACKAGES='1', PULSE_TEST_NATIVE_CASE=variant or 'packages',
                   LOCALAPPDATA=str(profile / 'local-app-data'))
        invocation = [str(args.pulse), '--test-instance', str(fixture)]
        command(invocation, env=env)
        report = (capture / 'report.txt').read_text(encoding='utf-8')
        startup_path = capture / 'package-startup.txt'
        startup = startup_path.read_text(encoding='utf-8') if startup_path.exists() else ''
        check('Result: PASS' in report and '[FAIL]' not in report + startup, label + ' actual host checks')
        if variant:
            check(startup.count('[PASS]') == 2, label + ' normal lazy startup')
            transform = str.maketrans('abcdefghijklmnopqrstuvwxyz', 'ABCDEFGHIJKLMNOPQRSTUVWXYZ') if variant == 'upper' else str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')
            expected = {name.translate(transform): hashlib.sha256(data).hexdigest() for name, data in contents.items()}
            numbers = set()
            actual = list(fixture.iterdir())
            check(len(actual) == 2, label + ' output count')
            for path in actual:
                match = re.fullmatch(r'demo-([12])-(.+)', path.name)
                check(match is not None and expected.get(match.group(2)) == digest(path), label + ' names and bytes ' + path.name)
                numbers.add(match.group(1))
            check(numbers == {'1', '2'}, label + ' unique rename sequence')
            required = {'prepared-panel.png', 'native-refusal.png', 'native-confirm.png', 'completed-panel.png', 'renamed-host.png'}
        else:
            check({p.name: p.read_bytes() for p in fixture.iterdir()} == contents, 'multi-package directory remains unchanged')
            required = {'installed-palette.png', 'inspector-installed.png', 'reactive-installed.png', 'inspector-reenabled.png'}
        images = list(capture.glob('*.png'))
        check({p.name for p in images} == required and all(p.stat().st_size > 4096 and p.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for p in images), label + ' application captures')
        runs.append({'case': label, 'passCount': (report + startup).count('[PASS]'), 'capture': str(capture), 'screenshots': len(images),
                     'files': {p.name: digest(p) for p in fixture.iterdir()}})

    try:
        inspector, inspector_manifest = author('inspector', args.examples_build / 'build-inspector/pulse_inspector.dll', 'inspector')
        reactive, _ = author('reactive-panel', args.examples_build / 'build-reactive-panel/pulse_reactive.dll', 'reactive')
        bad, _ = author('reactive-panel', args.missing_entry, 'bad-entry', 'sample.native-bad-entry')
        multi = args.output / 'multiple'
        store = multi / 'extensions'
        denied = command(cli + ['install', inspector, '--store', store], expected=2)
        check(denied['error']['code'] == 'native_confirmation_required' and not (store / 'registry.json').exists(), 'install requires explicit native consent without registry mutation')
        for package, package_id in ((inspector, 'sample.native-inspector'), (reactive, 'sample.native-reactive'), (bad, 'sample.native-bad-entry')):
            install(package, store)
            before = digest(store / 'registry.json')
            denied = command(cli + ['enable', package_id, '--store', store], expected=2)
            check(denied['error']['code'] == 'native_confirmation_required' and digest(store / 'registry.json') == before, 'enable requires explicit native consent')
            enable(package_id, store)
        launch(multi, 'multiple')
        if args.case == 'multiple':
            print((multi / 'capture-multiple' / 'report.txt').read_text(encoding='utf-8'), end='')
            return 0

        # Corrupt only our isolated copy. Disabled packages remain manageable.
        command(cli + ['disable', 'sample.native-inspector', '--store', store])
        declaration = json.loads(inspector_manifest.read_text())['native']
        image_path = store / 'binaries' / (declaration['sha256'] + '.dll')
        original = image_path.read_bytes()
        image_path.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        before = digest(store / 'registry.json')
        failed = enable('sample.native-inspector', store, expected=2)
        check(failed['error']['code'] == 'native_hash_mismatch' and digest(store / 'registry.json') == before, 'tampered image rejected without enabling')
        command(cli + ['list', '--store', store])
        image_path.write_bytes(original)
        enable('sample.native-inspector', store)

        lower, _ = author('rename-panel', args.examples_build / 'build-rename-panel/pulse_rename_lower.dll', 'lower')
        upper, _ = author('rename-panel', args.examples_build / 'build-rename-panel/pulse_rename_upper.dll', 'upper')
        lifecycle = args.output / 'lifecycle'
        store = lifecycle / 'extensions'
        first = install(lower, store)['installation']['package']['activeRevision']
        enable('sample.native-rename', store)
        launch(lifecycle, 'installed-lower', 'lower')
        second = install(upper, store)['installation']['package']['activeRevision']
        check(first != second, 'upgrade publishes a distinct immutable revision')
        launch(lifecycle, 'upgraded-upper', 'upper')
        before = digest(store / 'registry.json')
        denied = command(cli + ['rollback', 'sample.native-rename', '--store', store], expected=2)
        check(denied['error']['code'] == 'native_confirmation_required' and digest(store / 'registry.json') == before, 'rollback requires explicit native consent')
        command(cli + ['rollback', 'sample.native-rename', '--store', store, '--allow-native'])
        launch(lifecycle, 'rolled-back-lower', 'lower')
        check(runs[-1]['files'] == runs[-3]['files'] and runs[-2]['files'] != runs[-1]['files'], 'rollback restores real algorithm and byte-preserving output')
        command(cli + ['uninstall', 'sample.native-rename', '--store', store])
        check(not json.loads((store / 'registry.json').read_text())['packages'], 'uninstall removes discovery entry')
        result['passed'] = True
    except Exception as error:
        result['passed'] = False
        result['error'] = str(error)
    finally:
        if args.case == 'all':
            (args.output / 'report.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.case == 'multiple':
        print('[FAIL] ' + result['error'])
        return 1
    print(json.dumps({key: value for key, value in result.items() if key != 'commands'}, ensure_ascii=True))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
