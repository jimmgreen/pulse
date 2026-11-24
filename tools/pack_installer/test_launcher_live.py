"""Real launcher/process/path tests, isolated files only; build targets first."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'pack_dialog_experiment'))
import pack
from pack_installer import collect

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--app-runtime', action='store_true', help='also verify the staged independent application runtime')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    build = args.build.resolve(strict=True)
    output = args.output.absolute()
    output.mkdir()
    private_build = output / 'build'; private_build.mkdir()
    # Use real reviewed dependency files; only the application's entry becomes
    # the purpose-built argument/stdio/identity probe.
    manifest, sources = pack.plan(build, 'launcher-test-a', 'x64', repo, True, collect(repo, build))
    for item in manifest['files']:
        target = private_build / item['path']; target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sources[item['path']], target)
    shutil.copyfile(build / 'pulse_app_launcher_probe.exe', private_build / 'pulse.exe')
    shutil.copyfile(build / 'pulse_launcher.exe', private_build / 'pulse_launcher.exe')
    installed = output / 'installed'
    pointer_a = pack.package_installed(private_build, installed, 'launcher-test-a', 'x64', repo, collect(repo, private_build))
    launcher = installed / 'pulse.exe'
    record = output / 'args.txt'
    arguments = ['space argument', '中文参数', 'quote"inside', 'trailing\\']
    start = time.monotonic()
    completed = subprocess.run([launcher, '--launcher-wait', '--record', record, *arguments,
        '--exit-code', '37', '--echo'], input=b'owned stdin\n', capture_output=True, cwd=output, timeout=30)
    assert completed.returncode == 37 and completed.stdout == b'owned stdin\n' and completed.stderr == b'probe stderr\n', completed
    text = record.read_text(encoding='utf-8')
    assert all('[' + value + ']' in text for value in arguments), text
    assert '--launcher-wait' not in text
    assert f'root={installed}' in text and f'stable={launcher}' in text and f'cwd={output}' in text, text
    print(f'[PASS] real launcher args/stdin/stdout/stderr/exit/cwd/path mapping ({time.monotonic()-start:.3f}s)', flush=True)
    shell_record = output / 'shell-entry.txt'
    shell_env = dict(os.environ, PULSE_LAUNCHER_PROBE_RECORD=str(shell_record))
    shell_target = str(output / 'folder space 中文')
    completed = subprocess.run([launcher, '--launcher-wait', '--shell-open', shell_target],
        env=shell_env, cwd=output, timeout=30)
    assert completed.returncode == 37, 'created host failure must not become a second native open'
    assert f'arguments=[--shell-open][{shell_target}]' in shell_record.read_text(encoding='utf-8')
    print('[PASS] explicit shell origin reaches host; host failure is preserved without fallback', flush=True)
    old_record = output / 'old.txt'
    start = time.monotonic()
    process = subprocess.Popen([launcher, '--record', old_record, '--delay', '3000'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=output)
    process.wait(timeout=2)
    elapsed = time.monotonic()-start
    assert process.returncode == 0
    until = time.monotonic()+10
    expected = f"runtime={installed / pointer_a['directory']}"
    observed = ''
    while time.monotonic() < until:
        try:
            observed = old_record.read_text(encoding='utf-8')
        except (FileNotFoundError, PermissionError):
            pass
        if expected in observed and '\ncwd=' in observed:
            break
        time.sleep(.025)
    assert expected in observed and '\ncwd=' in observed, observed
    print(f'[PASS] GUI launcher exits promptly while runtime remains alive ({elapsed:.3f}s)', flush=True)
    current = installed / 'dialog-current.json'
    saved_pointer = current.read_bytes()
    current.unlink()
    missing_record = output / 'missing-pointer.txt'
    subprocess.run([installed / pointer_a['directory'] / 'pulse.exe', '--record', missing_record], check=True, timeout=30)
    assert f'root={installed}' in missing_record.read_text(encoding='utf-8')
    current.write_bytes(b'not a valid pointer')
    damaged_record = output / 'damaged-pointer.txt'
    subprocess.run([installed / pointer_a['directory'] / 'pulse.exe', '--record', damaged_record], check=True, timeout=30)
    assert f'root={installed}' in damaged_record.read_text(encoding='utf-8')
    if args.app_runtime:
        rejected = subprocess.run([build / 'pulse_dialog_bundle_test.exe', '--resolve-startup', installed],
                                  capture_output=True, timeout=10)
    else:
        rejected = subprocess.run([launcher, '--launcher-wait'], timeout=10)
    assert rejected.returncode != 0, 'launcher must reject damaged pointer'
    current.write_bytes(saved_pointer)
    print('[PASS] verified version keeps stable identity with missing/damaged pointer; launcher rejects damaged pointer', flush=True)
    pointer_b = pack.package_addressed(private_build, installed / 'DialogBridge', 'launcher-test-b', 'x64', repo, collect(repo, private_build))
    temporary = installed / 'pointer-new'
    temporary.write_text(json.dumps(pointer_b), encoding='utf-8')
    os.replace(temporary, installed / 'dialog-current.json')
    new_record = output / 'new.txt'
    completed = subprocess.run([launcher, '--launcher-wait', '--record', new_record], timeout=30)
    assert completed.returncode == 0
    assert f"runtime={installed / pointer_b['directory']}" in new_record.read_text(encoding='utf-8')
    assert pointer_a != pointer_b
    old_version_record = output / 'old-version-new-pointer.txt'
    subprocess.run([installed / pointer_a['directory'] / 'pulse.exe', '--record', old_version_record], check=True, timeout=30)
    assert f'root={installed}' in old_version_record.read_text(encoding='utf-8')
    pack.verify(installed / pointer_a['directory'])
    print('[PASS] pointer switches new launch to B while A remains intact', flush=True)
    forged = output / 'forged' / 'DialogBridge' / ('0'*64)
    forged.mkdir(parents=True)
    shutil.copyfile(build / 'pulse_app_launcher_probe.exe', forged / 'pulse.exe')
    shutil.copyfile(launcher, forged.parent.parent / 'pulse.exe')
    forged_record = output / 'forged.txt'
    subprocess.run([forged / 'pulse.exe', '--record', forged_record], check=True, timeout=10)
    assert f'root={forged}' in forged_record.read_text(encoding='utf-8')
    print('[PASS] unverified hash-shaped path cannot redirect stable identity', flush=True)
    if args.app_runtime:
        exercise_app_runtime(build, private_build, installed, output, pointer_b, repo)
    return 0


def exercise_app_runtime(build, private_build, installed, output, legacy, repo):
    launcher = installed / 'pulse.exe'
    pointer = pack.package_app_runtime(private_build, installed, 'launcher-test-b', 'x64', repo,
                                       collect(repo, private_build))
    runtime = installed / pointer['directory']
    app_current = installed / 'app-current.json'
    dialog_current = installed / 'dialog-current.json'
    saved = app_current.read_bytes()
    assert not any(pack.dialog_binary(path.name) for path in runtime.iterdir())

    def run_host(case):
        record = output / f'app-{case}.txt'
        result = subprocess.run([launcher, '--launcher-wait', '--record', record], timeout=30)
        assert result.returncode == 0, (case, result)
        lines = record.read_text(encoding='utf-8')
        assert f'runtime={runtime}\n' in lines and f'root={installed}\n' in lines and f'stable={launcher}\n' in lines, lines
        print(f'[PASS] {case}: new process uses independently verified AppRuntime with stable identity', flush=True)

    def reject(case, error):
        # Read-only verifier covers the failure path without opening Explorer or writing user diagnostics.
        result = subprocess.run([build / 'pulse_dialog_bundle_test.exe', '--resolve-startup', installed],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 1 and f'error={error}' in result.stdout, (case, result)
        print(f'[PASS] {case}: app startup rejected, error={error}, no legacy downgrade', flush=True)

    run_host('both-pointers-valid')
    app_current.unlink()
    reject('app-pointer-missing-dialog-valid', 2)
    layout = installed / '.pulse-app-layout'
    layout.unlink()
    result = subprocess.run([build / 'pulse_dialog_bundle_test.exe', '--resolve-startup', installed], timeout=30)
    assert result.returncode == 0
    layout.write_bytes(b'1\n')
    print('[PASS] uncommitted AppRuntime directory does not block a legacy rollback', flush=True)
    app_current.write_bytes(b'not json')
    reject('app-pointer-damaged-dialog-valid', 13)
    redirected = dict(legacy, directory='AppRuntime/' + legacy['manifest_sha256'])
    # Present a valid legacy manifest under the app family; the kind check must reject it.
    wrong_kind = (runtime.parent / legacy['manifest_sha256']).resolve()
    assert wrong_kind.parent == runtime.parent.resolve()
    shutil.copytree(installed / legacy['directory'], wrong_kind)
    app_current.write_text(json.dumps(redirected), encoding='utf-8')
    reject('legacy-kind-in-app-family', 1306)
    app_current.write_bytes(saved)
    extra = runtime / 'log'
    extra.mkdir()
    reject('app-bundle-has-unlisted-directory', 13)
    extra.rmdir()
    host = runtime / 'pulse.exe'
    host_bytes = host.read_bytes()
    host.write_bytes(host_bytes[:-1] + bytes([host_bytes[-1] ^ 1]))
    reject('app-host-hash-mismatch', 23)
    host.write_bytes(host_bytes)

    dialog_current.write_bytes(b'damaged')
    run_host('dialog-pointer-damaged')
    dialog_current.write_text(json.dumps(legacy), encoding='utf-8')
    polluted = installed / legacy['directory'] / 'log'
    polluted.mkdir()
    run_host('dialog-bundle-has-unlisted-directory')
    polluted.rmdir()
    dialog_current.unlink()
    dialog_dir = (installed / 'DialogBridge').resolve(strict=True)
    assert dialog_dir.parent == installed.resolve()
    # The asynchronous legacy launch may still be releasing its verified files.
    pack.publish_directory(dialog_dir, installed / 'retained-dialog-fixture')
    run_host('dialog-pointer-and-bundles-absent')
    shell_record = output / 'app-shell-open.txt'
    target = str(output / 'folder space 中文')
    env = dict(os.environ, PULSE_LAUNCHER_PROBE_RECORD=str(shell_record))
    result = subprocess.run([launcher, '--launcher-wait', '--shell-open', target], env=env, timeout=30)
    assert result.returncode == 37
    contents = shell_record.read_text(encoding='utf-8')
    assert contents.count(f'arguments=[--shell-open][{target}]') == 1 and f'runtime={runtime}\n' in contents, contents
    print('[PASS] shell-open reaches AppRuntime with no DialogBridge', flush=True)


if __name__ == '__main__':
    sys.exit(main())
