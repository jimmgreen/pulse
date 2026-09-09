"""Render update states in isolated hidden windows for visual verification."""
from pathlib import Path
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / (sys.argv[1] if len(sys.argv) > 1 else 'build-ci')
output = root / 'bench_data' / 'update-visuals'
output.mkdir(parents=True, exist_ok=True)
profile = output / 'profile'
profile.mkdir(exist_ok=True)
environment = dict(os.environ, PULSE_TEST_HIDDEN_SHOT='1', PULSE_TEST_DATA_DIR=str(profile))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
cases = [
    ('available-narrow-en', 'available', 'en-US', '--light', '640x1100', '1'),
    ('available-stacked-en', 'available', 'en-US', '--light', '480x1100', '1'),
    ('downloading-zh', 'downloading', 'zh-CN', '--dark', '900x1050', '1'),
    ('cancelled-en', 'cancelled', 'en-US', '--light', '900x1050', '1'),
    ('failed-zh', 'failed', 'zh-CN', '--light', '820x1050', '1'),
    ('installing-en', 'installing', 'en-US', '--dark', '900x1050', '1'),
    ('available-dpi-zh', 'available', 'zh-CN', '--light', '1200x1600', '1.5'),
]
results = []
for name, state, language, theme, size, scale in cases:
    image = output / (name + '.png')
    command = [str(build / 'pulse.exe'), '--test-instance', '--shot', str(image),
               '--shot-update-state', state, '--shot-language', language, theme,
               '--size', size, '--shot-scale', scale, 'pulse:settings:about']
    process = subprocess.run(command, cwd=root, env=environment, startupinfo=startup,
                             capture_output=True, timeout=30)
    passed = process.returncode == 0 and image.exists() and image.stat().st_size > 1000
    results.append({'name': name, 'passed': passed, 'exit_code': process.returncode})
    print(name, 'PASS render' if passed else 'FAIL render')
(output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
raise SystemExit(0 if all(row['passed'] for row in results) else 1)
