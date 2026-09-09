"""Run release regression tests with isolated logs and a self-test profile."""
from pathlib import Path
import json
import os
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
out = root / 'bench_data' / 'win81-tests'
out.mkdir(exist_ok=True)
profile = out / 'profile'
profile.mkdir(exist_ok=True)
names = ['pulse_index_migration_test', 'pulse_preview_test', 'pulse_ops_test', 'pulse_localization_test', 'pulse_crash_test',
         'pulse_index_test', 'pulse_index_engine_test', 'pulse_content_search_test',
         'pulse_duplicate_scan_test', 'pulse_saved_search_test', 'pulse_search_query_test',
         'pulse_update_test', 'pulse_shell_icons_test',
         'pulse_material_test', 'pulse_app_controllers_test', 'pulse_index_host_stress', 'pulse']
env = dict(os.environ, PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_HIDDEN_SHOT='1')
results = []
prior_path = out / 'results.json'
prior = json.loads(prior_path.read_text(encoding='utf-8')) if prior_path.exists() else []
selected = set(sys.argv[1:])
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
for name in names:
    done = next((r for r in prior if r['test'] == name), None)
    if (selected and name not in selected) or (not selected and done and done['exit'] == 0):
        if done: results.append(done)
        continue
    start = time.monotonic()
    args = [str(root / 'build-win81' / (name + '.exe'))]
    if name == 'pulse': args += ['--selftest']
    with (out / (name + '.log')).open('wb') as log:
        try:
            p = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, cwd=root,
                               env=env, timeout=180, startupinfo=startup)
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    row = dict(test=name, exit=code, seconds=round(time.monotonic()-start, 2))
    results.append(row)
    print(json.dumps(row), flush=True)
    (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
(out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
raise SystemExit(any(row['exit'] != 0 for row in results))
