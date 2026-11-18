"""Exercise ephemeral signed packages through real native store startup."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import package_signing as signing
import package_store as store
from live_store_run_check import main_window
from live_dev_check import close_window, child_process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pulse', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'store'
    root.mkdir()
    profile = output / 'profile'
    profile.mkdir()
    checks = []
    def check(ok, label):
        checks.append({'name': label, 'passed': bool(ok)})
        print(('PASS ' if ok else 'FAIL ') + label, flush=True)
        if not ok:
            raise AssertionError(label)
    cng = signing.Cng()
    try:
        cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), 2048, 0))
        cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
        private, public = cng.export(True), cng.export()
    finally:
        cng.close()
    policy = {'formatVersion': 1, 'requireSigned': True, 'publishers': [
        {'id': hashlib.sha256(public).hexdigest(), 'publicKey': public.hex()}]}
    policy_file = root / 'signature-policy.json'
    policy_file.write_text(json.dumps(policy), encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env['PULSE_TEST_DATA_DIR'] = str(profile)
    command = [str(args.pulse.absolute()), '--test-instance', '--extension-store', str(root)]
    def launch(label, accepted=True):
        with (output / (label + '.log')).open('wb') as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline = time.monotonic() + 20
                while process.poll() is None and not main_window(process.pid) and time.monotonic() < deadline:
                    time.sleep(.05)
                if accepted:
                    check(process.poll() is None and main_window(process.pid), label + ': verified native window opens')
                    close_window(process.pid)
                    check(process.wait(timeout=20) == 0, label + ': native session closes normally')
                    check(child_process(process.pid, ('pulse.index.exe', 'pulse.content.exe')) is None,
                          label + ': no orphaned search helper')
                else:
                    check(not main_window(process.pid) and process.wait(timeout=10) == 2,
                          label + ': native startup rejects before UI')
            finally:
                if process.poll() is None:
                    close_window(process.pid)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        process.wait(timeout=5)
    def package(resources=None):
        value = {'apiVersion': 1, 'id': 'signed.probe', 'name': 'Signed probe',
                 'commands': [{'id': 'refresh', 'title': 'Refresh', 'capability': 'view.refresh'}],
                 'panel': {'title': 'Signed probe', 'commands': ['refresh']}}
        if resources:
            value['resources'] = resources
        return json.dumps(value).encode('utf-8')
    def signature(data):
        return json.dumps(signing.sign(data, private)).encode('utf-8')
    try:
        data = package()
        initial = store.install(root, data, signature=signature(data))
        store.update(root, 'signed.probe', 'enable')
        launch('trusted')
        upgraded = package(['settings.own'])
        before = (root / 'registry.json').read_bytes()
        try:
            store.install(root, upgraded, signature=signature(upgraded))
            check(False, 'signed permission increase requires approval')
        except store.StoreError as error:
            check(error.code == 'capability_confirmation_required' and before == (root / 'registry.json').read_bytes(),
                  'signed permission increase rejected atomically')
        store.install(root, upgraded, True, signature(upgraded))
        launch('approved-upgrade')
        store.update(root, 'signed.probe', 'rollback')
        launch('signed-rollback')
        revision = initial['package']['activeRevision']
        sidecar = root / 'versions' / (revision + '.sig.json')
        valid = sidecar.read_bytes()
        sidecar.write_text('{}', encoding='utf-8')
        launch('tampered-signature', False)
        sidecar.write_bytes(valid)
        policy_file.write_text(json.dumps(dict(policy, publishers=[])), encoding='utf-8')
        launch('revoked-publisher', False)
        policy_file.write_text(json.dumps(policy), encoding='utf-8')
        sidecar.unlink()
        launch('missing-required-signature', False)
        sidecar.write_bytes(valid)
        launch('restored-trust')
    finally:
        (output / 'checks.json').write_text(json.dumps(checks, indent=2), encoding='utf-8')
    print(str(len(checks)) + ' signed native startup checks passed')


if __name__ == '__main__':
    main()
