"""Read and export diagnostics produced by an already completed isolated host run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-operation', action='append', default=[])
    args = parser.parse_args()
    pulse, profile, output = args.pulse.resolve(), args.profile.resolve(), args.output.resolve()
    output.mkdir(exist_ok=False)
    root = profile / 'extension-audit'
    checks, calls = [], []
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('PULSE_')}
    env['LOCALAPPDATA'] = str(output / 'local-app-data')

    def check(ok, name):
        checks.append({'name': name, 'passed': bool(ok)})
        if not ok:
            raise AssertionError(name)

    def call(*arguments, expected=0):
        result = subprocess.run([str(pulse), *map(str, arguments)], env=env,
                                capture_output=True, timeout=30)
        parsed = json.loads(result.stdout.decode('utf-8'))
        calls.append({'arguments': list(map(str, arguments)), 'exitCode': result.returncode, 'result': parsed})
        check(result.returncode == expected and parsed['ok'] == (expected == 0),
              str(arguments[0]) + ' returns expected structured result')
        return parsed

    def digests(directory):
        return {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                for file in directory.iterdir() if file.is_file()}

    completed = False
    try:
        discovery = subprocess.run([str(pulse), '--extension-api'], env=env, capture_output=True, timeout=30)
        api = json.loads(discovery.stdout.decode('utf-8'))
        check(discovery.returncode == 0 and 'persistentHostDiagnostics' in api.get('developerFeatures', [])
              and all(any(command in note for note in api.get('developmentNotes', []))
                      for command in ('--extension-audit-read ROOT', '--extension-audit-export ROOT DEST')),
              'shared host discovery advertises both local diagnostics commands before execution')
        (output / 'host-api.json').write_bytes(discovery.stdout)
        check(root.is_dir(), 'actual isolated host created its own audit directory')
        before = digests(root)
        result = call('--extension-audit-read', root)
        records = result['records']
        check(not result['partial'] and result['grantsAuthority'] is False and bool(records),
              'actual host persisted records without granting package authority')
        fields = {'packageId', 'operation', 'outcome', 'utcMs', 'sessionId'}
        check(all(set(record) == fields and all(isinstance(value, str) for value in record.values())
                  and record['utcMs'].isdigit() and bool(record['sessionId']) for record in records),
              'records contain only the five documented string fields')
        operations = {record['operation'] for record in records}
        check(set(args.require_operation) <= operations, 'required real host operations were persisted')
        serialized = json.dumps(records, ensure_ascii=False)
        check(str(profile) not in serialized and 'unchanged.txt' not in serialized
              and 'fixture' not in serialized and 'resource-source' not in serialized,
              'host fixture paths and selection filenames are absent from persisted records')
        status = result['lastWriterStatus']
        check(int(status['persisted']) >= len(records) and int(status['accepted']) == int(status['persisted'])
              and int(status['failures']) == 0
              and int(status['dropped']) == 0 and status['lastError'] == '',
              'normal shutdown persisted all admitted events without silent writer errors')
        check(sum(file.stat().st_size for file in root.iterdir()) <= result['retention']['maximumDiskBytes']
              and len(list(root.glob('audit-*.jsonl'))) <= result['retention']['segments'],
              'actual store respects its advertised disk retention budget')
        destination = output / 'export.json'
        exported = call('--extension-audit-export', root, destination)
        check(exported['exported'] and json.loads(destination.read_text(encoding='utf-8')) == result,
              'new export contains exactly the validated snapshot')
        export_hash = hashlib.sha256(destination.read_bytes()).hexdigest()
        call('--extension-audit-export', root, destination, expected=2)
        check(hashlib.sha256(destination.read_bytes()).hexdigest() == export_hash,
              'existing export is not overwritten')
        refused = call('--extension-audit-export', root, root / 'forbidden.json', expected=2)
        check(refused['error'] == 'destination_inside_audit_root' and digests(root) == before,
              'export cannot contaminate or change the source audit directory')
        invalid = call('--extension-audit-read', root, '--packageId', 'other', expected=2)
        check(invalid['error'] == 'invalid_args', 'caller identity and extra launch arguments are rejected')
        missing = output / 'missing-audit-root'
        call('--extension-audit-read', missing, expected=2)
        check(not missing.exists(), 'read does not create a missing store')
        corrupt = output / 'corrupt-audit-root'
        shutil.copytree(root, corrupt)
        segment = next(corrupt.glob('audit-*.jsonl'))
        secret = 'MUST_NOT_DISCLOSE_PRIVATE_PAYLOAD_7b03'
        segment.write_text(json.dumps({'payload': secret}) + '\n', encoding='utf-8')
        damaged = call('--extension-audit-read', corrupt, expected=2)
        check(secret not in json.dumps(damaged) and damaged['grantsAuthority'] is False,
              'malformed stored payload is rejected without echoing its contents')
        completed = True
    finally:
        report = {'completed': completed, 'checks': checks, 'calls': calls,
                  'testedHostSha256': hashlib.sha256(pulse.read_bytes()).hexdigest()}
        (output / 'checks.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'completed': completed, 'checks': len(checks), 'records': len(records)}))


if __name__ == '__main__':
    main()
