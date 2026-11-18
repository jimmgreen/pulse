"""Coordinate temporary signing keys and real two-window host trust-revocation probes."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import package_signing as signing
import package_store as store
from live_dev_check import child_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root, profile = output / 'store', output / 'profile'
    root.mkdir()
    profile.mkdir()
    fixture = profile / 'fixture'
    fixture.mkdir()
    (fixture / 'unchanged.txt').write_text('Isolated trust fixture', encoding='utf-8')
    cng = signing.Cng()
    try:
        cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), 2048, 0))
        cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
        private, public = cng.export(True), cng.export()
    finally:
        cng.close()
    pin = hashlib.sha256(public).hexdigest()
    public_file = output / 'temporary-public.blob'
    public_file.write_bytes(public)
    policy = dict(formatVersion=1, requireSigned=False, publishers=[dict(id=pin, publicKey=public.hex())])
    (root / 'signature-policy.json').write_text(json.dumps(policy), encoding='utf-8')
    for signed in (True, False):
        package_id = 'sample.trust-signed' if signed else 'sample.trust-unsigned'
        value = dict(apiVersion=1, id=package_id, name='Trust signed' if signed else 'Trust unsigned',
                     commands=[dict(id='view', title='View', capability='view.setMode',
                                    args=dict(mode='details' if signed else 'tiles'))])
        data = json.dumps(value).encode('utf-8')
        signature = json.dumps(signing.sign(data, private)).encode('utf-8') if signed else None
        store.install(root, data, signature=signature)
        store.update(root, package_id, 'enable')
    private = None
    registry = (root / 'registry.json').read_bytes()
    checks = []

    def check(ok, name):
        checks.append(dict(name=name, passed=bool(ok)))
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)
        if not ok:
            raise AssertionError(name)

    def policy_command(command):
        arguments = [sys.executable, str(Path(signing.__file__).resolve()), command,
                     '--store', str(root), '--fingerprint', pin]
        if command == 'trust':
            arguments += ['--public-key', str(public_file)]
        result = subprocess.run(arguments, capture_output=True, timeout=20,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        (output / (command + '.json')).write_bytes(result.stdout)
        check(result.returncode == 0, 'explicit local ' + command + ' succeeds')

    try:
        discovery = subprocess.run([str(args.pulse.resolve()), '--extension-api'], capture_output=True, timeout=20,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        api = json.loads(discovery.stdout)
        check(discovery.returncode == 0 and 'runtimeTrustRevocation' in api.get('developerFeatures', [])
              and any(note.startswith('runtimeTrustRevocation:') for note in api.get('developmentNotes', [])),
              'shared discovery describes local runtime trust revocation')
        for phase in ('revoke', 'restart'):
            capture = output / phase
            capture.mkdir()
            env = {key: value for key, value in os.environ.items() if not key.upper().startswith('PULSE_')}
            env.update(LOCALAPPDATA=str(output / 'local-app-data'), PULSE_TEST_DATA_DIR=str(profile),
                       PULSE_TEST_EXTENSION_CAPTURE=str(capture), PULSE_TEST_TRUST_PHASE=phase)
            with (capture / 'process.log').open('wb') as log:
                process = subprocess.Popen([str(args.pulse.resolve()), '--test-instance', '--extension-store',
                                            str(root), str(fixture)], env=env, stdout=log, stderr=subprocess.STDOUT)
                seen = set()
                try:
                    deadline = time.monotonic() + 120
                    while process.poll() is None and time.monotonic() < deadline:
                        try:
                            status = json.loads((capture / 'ready.json').read_text(encoding='utf-8'))
                        except (OSError, ValueError):
                            status = {}
                        waiting = status.get('phase')
                        if phase == 'revoke' and waiting not in seen:
                            if waiting == 'await-revoke':
                                policy_command('untrust')
                                seen.add(waiting)
                            elif waiting == 'await-restore':
                                check('await-revoke' in seen, 'host observed revocation before restoration')
                                policy_command('trust')
                                marker = capture / 'policy-action.pending'
                                marker.write_text(json.dumps(dict(phase='restored')), encoding='utf-8')
                                os.replace(marker, capture / 'policy-action.json')
                                seen.add(waiting)
                        time.sleep(.03)
                    exit_code = process.poll()
                    (capture / 'process-result.json').write_text(json.dumps(dict(
                        pid=process.pid, exitCode=exit_code,
                        exitCodeHex=None if exit_code is None else f'0x{exit_code & 0xffffffff:08X}',
                        completed=exit_code is not None), indent=2), encoding='utf-8')
                    check(exit_code is not None, phase + ' real host probe completes before deadline')
                    report = (capture / 'report.txt').read_text(encoding='utf-8')
                    check(process.returncode == 0 and '[FAIL]' not in report and 'Result: PASS' in report,
                          phase + f' native assertions pass (exitCode={exit_code})')
                    images = sorted(capture.glob('*.png'))
                    check(len(images) == (4 if phase == 'revoke' else 1) and all(
                        image.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n' for image in images),
                          phase + ' preserves native screenshot inventory')
                    if phase == 'revoke':
                        check(seen == {'await-revoke', 'await-restore'}, 'both live policy transitions were exercised')
                    check(child_process(process.pid, ('pulse.index.exe', 'pulse.content.exe')) is None,
                          phase + ' leaves no search helpers')
                    audit = subprocess.run([str(args.pulse.resolve()), '--extension-audit-read',
                                            str(profile / 'extension-audit')], env=env, capture_output=True, timeout=20)
                    (capture / 'audit.json').write_bytes(audit.stdout)
                    audit_value = json.loads(audit.stdout)
                    records = audit_value.get('records', [])
                    check(audit.returncode == 0 and audit_value.get('ok') and any(
                        item.get('packageId') == 'sample.trust-signed' and item.get('operation') == 'package.trust'
                        and item.get('outcome') == 'revoked' for item in records), phase + ' persists host trust revocation reason')
                    check(not any(item.get('packageId') == 'sample.trust-unsigned' and
                                  item.get('operation') == 'package.trust' for item in records),
                          phase + ' records no false revocation for unrelated package')
                    check(str(root).encode() not in audit.stdout and public.hex().encode() not in audit.stdout,
                          phase + ' audit excludes store path and publisher key payload')
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=15)
        check((root / 'registry.json').read_bytes() == registry, 'live trust refresh never rewrites enable flags or versions')
        check((fixture / 'unchanged.txt').read_text(encoding='utf-8') == 'Isolated trust fixture', 'fixture unchanged')
    finally:
        (output / 'checks.json').write_text(json.dumps(checks, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
