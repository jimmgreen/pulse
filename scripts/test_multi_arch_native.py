"""Run the unchanged native control entrypoint in explicit isolated-test helpers.

The fixture helper is not the production binary. Its identity handshake is
mandatory before any transaction, and both test helpers require a GUID-only
private-hive token. Production packages are never modified by this runner.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'pack_dialog_experiment'))
import multi_arch
import pack

def package(args):
    output = args.output.absolute()
    sources = output.with_name(output.name + '-sources')
    if output.exists() or sources.exists():
        raise ValueError('NEW fixture output and source staging required')
    builds = {}
    for arch in multi_arch.ARCHITECTURES:
        build = getattr(args, 'build_' + arch).resolve(strict=True)
        helper = getattr(args, 'helper_' + arch).resolve(strict=True)
        manifest, selected = pack.plan(build, 'native-isolated-fixture', arch, args.repository.resolve(), automatic=True)
        stage = sources / arch
        stage.mkdir(parents=True)
        for relative, source in selected.items():
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open('rb') as src, target.open('xb') as dst:
                shutil.copyfileobj(src, dst)
            if pack.digest(target) != next(item['sha256'] for item in manifest['files'] if item['path'] == relative):
                raise ValueError('source changed while staging')
        shutil.copyfile(helper, stage / multi_arch.CONTROL_NAME)
        builds[arch] = stage
    result = multi_arch.package(builds, output, 'native-isolated-fixture', args.repository.resolve())
    print(json.dumps({'result': 'verified-isolated-test-package', 'directory': str(output), 'index': result}))

def run(args):
    directory = args.directory.resolve(strict=True)
    multi_arch.verify(directory)
    output = args.output.absolute()
    if output.exists():
        raise ValueError('NEW result output required')
    output.mkdir()
    original_run = subprocess.run
    old_hive = os.environ.get('PULSE_AUTO_TEST_HIVE')
    old_fail = os.environ.get('PULSE_AUTO_TEST_FAIL_ENABLE32')
    token = str(uuid.uuid4())
    os.environ['PULSE_AUTO_TEST_HIVE'] = token
    os.environ.pop('PULSE_AUTO_TEST_FAIL_ENABLE32', None)
    calls, reports = [], []
    def recorded(*arguments, **keywords):
        result = original_run(*arguments, **keywords)
        calls.append({'command': [str(x) for x in arguments[0]], 'stdin': 'DEVNULL',
                      'stdout': result.stdout.decode('utf-8', 'replace') if isinstance(result.stdout, bytes) else result.stdout,
                      'stderr': result.stderr.decode('utf-8', 'replace') if isinstance(result.stderr, bytes) else result.stderr,
                      'exit': result.returncode})
        return result
    identified = []
    try:
        for arch in multi_arch.ARCHITECTURES:
            helper = directory / arch / multi_arch.CONTROL_NAME
            result = recorded([str(helper), '--test-identity'], stdin=subprocess.DEVNULL, capture_output=True)
            expected = {'fixture': 'pulse-native-control-isolated-test', 'architecture': arch}
            if result.returncode or json.loads(result.stdout) != expected:
                raise ValueError('isolated helper identity required; no production helper transactions allowed')
            identified.append(helper)
        multi_arch.subprocess.run = recorded
        with multi_arch.control_lock():
            def operate(command, selected=multi_arch.ARCHITECTURES):
                report = multi_arch.operate(directory, command, selected)
                reports.append(report)
                return report
            before = operate('status')
            assert before['result'] == 'ok' and all(not x['enabled'] for x in before['architectures'].values())
            assert operate('enable')['result'] == 'ok'
            enabled = operate('status')
            assert enabled['result'] == 'ok' and all(x['enabled'] for x in enabled['architectures'].values())
            assert operate('disable')['result'] == 'ok'
            os.environ['PULSE_AUTO_TEST_FAIL_ENABLE32'] = '1'
            failed = operate('enable')
            assert failed['result'] == 'rolled-back', failed
            assert [(x['architecture'], x['command']) for x in failed['rollback']] == [('x86', 'disable'), ('x64', 'disable')]
            os.environ.pop('PULSE_AUTO_TEST_FAIL_ENABLE32')
            after = operate('status')
            assert after['result'] == 'ok' and all(not x['enabled'] for x in after['architectures'].values())
            assert operate('enable', ('x86',))['result'] == 'ok'
            partial = operate('status')
            assert not partial['architectures']['x64']['enabled'] and partial['architectures']['x86']['enabled']
            assert operate('disable', ('x86',))['result'] == 'ok'
        summary = {'result': 'PASS', 'private_hive_token': token, 'full_enable_disable': True,
                   'failure_after_x86_mutation': 'rolled-back', 'rollback_order': ['x86', 'x64'],
                   'x86_only': True, 'production_registration_modified': False}
    finally:
        multi_arch.subprocess.run = original_run
        cleanup = []
        for helper in identified:
            result = recorded([str(helper), '--test-cleanup'], stdin=subprocess.DEVNULL, capture_output=True)
            cleanup.append(result.returncode)
        (output / 'native-calls.json').write_text(json.dumps(calls, indent=2) + '\n', encoding='utf-8')
        (output / 'transactions.json').write_text(json.dumps(reports, indent=2) + '\n', encoding='utf-8')
        (output / 'cleanup.json').write_text(json.dumps(cleanup) + '\n', encoding='utf-8')
        if old_hive is None: os.environ.pop('PULSE_AUTO_TEST_HIVE', None)
        else: os.environ['PULSE_AUTO_TEST_HIVE'] = old_hive
        if old_fail is None: os.environ.pop('PULSE_AUTO_TEST_FAIL_ENABLE32', None)
        else: os.environ['PULSE_AUTO_TEST_FAIL_ENABLE32'] = old_fail
    assert cleanup == [0, 0], cleanup
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    create = sub.add_parser('package')
    for arch in multi_arch.ARCHITECTURES:
        create.add_argument('--build-' + arch, type=Path, required=True)
        create.add_argument('--helper-' + arch, type=Path, required=True)
    create.add_argument('--output', type=Path, required=True)
    create.add_argument('--repository', type=Path, default=Path(__file__).resolve().parents[1])
    execute = sub.add_parser('run')
    execute.add_argument('--directory', type=Path, required=True)
    execute.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args) if args.command == 'package' else run(args)

if __name__ == '__main__':
    main()
