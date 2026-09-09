"""Render isolated, hidden application windows to PNG; no desktop automation."""
from pathlib import Path
import json
import os
import subprocess
import shutil
import ctypes

root = Path(__file__).resolve().parents[1]
out = root / 'bench_data' / 'win81-visuals'
out.mkdir(exist_ok=True)
profile = out / 'profile'
profile.mkdir(exist_ok=True)
(profile / 'places.json').write_text(json.dumps({
    'quick_access_paths': [str(root / 'src'), str(root / 'tools')]
}), encoding='utf-8')
base_env = dict(os.environ, PULSE_TEST_HIDDEN_SHOT='1', PULSE_TEST_DATA_DIR=str(profile))
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
listing = out / 'hidden-fixture'
listing.mkdir(exist_ok=True)
for leaf in ['Visible.txt', '.ordinary', 'desktop.ini']:
    item = listing / leaf
    if not item.exists(): item.write_text('Pulse isolated fixture', encoding='utf-8')
    if leaf == 'desktop.ini':
        if not ctypes.windll.kernel32.SetFileAttributesW(str(item), 2):
            raise ctypes.WinError()
hidden_folder = listing / 'Hidden folder'
hidden_folder.mkdir(exist_ok=True)
if not ctypes.windll.kernel32.SetFileAttributesW(str(hidden_folder), 2):
    raise ctypes.WinError()
cases = [
    ('hidden-off', False, ['--shot', str(out/'hidden-off.png'), '--light', '--size','1100x780', str(listing)]),
    ('hidden-on', False, ['--shot', str(out/'hidden-on.png'), '--light', '--size','1100x780', str(listing)]),
    ('hidden-narrow-dark', True, ['--shot', str(out/'hidden-narrow-dark.png'), '--dark', '--size','700x600', str(listing)]),
    ('hidden-settings-dark-en', True, ['--shot', str(out/'hidden-settings-dark-en.png'), '--dark', '--shot-scale','1', '--shot-language','en-US', '--size','820x1700', 'pulse:settings:appearance']),
    ('hidden-settings', False, ['--shot', str(out/'hidden-settings.png'), '--light', '--shot-scale','1', '--size','1000x1600', 'pulse:settings:appearance']),
    ('fluent-toast', False, ['--shot', str(out/'fluent-toast.png'), '--light', '--size','1100x780', str(root/'src')]),
    ('fluent-toast-legacy-dark', True, ['--shot', str(out/'fluent-toast-legacy-dark.png'), '--dark', '--shot-scale','1.5', '--size','1200x900', str(root/'src')]),
    ('migration-index', False, ['--shot', str(out/'migration-index.png'), '--light', '--size','820x760', 'pulse:settings:index']),
    ('modern-light', False, ['--shot', str(out/'modern-light.png'), '--light', '--size','1100x780', str(root/'src')]),
    ('legacy-dark', True, ['--shot', str(out/'legacy-dark.png'), '--dark', '--size','1100x780', str(root/'src')]),
    ('legacy-settings', True, ['--shot', str(out/'legacy-settings.png'), '--light', '--size','820x760', 'pulse:settings:appearance']),
    ('legacy-dpi', True, ['--shot', str(out/'legacy-dpi.png'), '--light','--shot-scale','1.5','--size','1440x1050',str(root/'src')]),
    ('pin-menu', False, ['--menushot',str(out/'pin-menu.png'),'--light',str(root)]),
    ('unpin-menu-legacy', True, ['--menushot',str(out/'unpin-menu-legacy.png'),'--dark',str(root/'src')]),
]
results = []
for name, legacy, args in cases:
    (profile / 'app.json').write_text(json.dumps({'show_hidden_files': name == 'hidden-on'}), encoding='utf-8')
    env = dict(base_env, PULSE_COMPAT_81='1' if legacy else '0', PULSE_TEST_QUICK_MENU='1')
    env['PULSE_TEST_INDEX_MIGRATING'] = '1' if name == 'migration-index' else '0'
    env['PULSE_TEST_TOAST'] = '1' if name.startswith('fluent-toast') else '0'
    with (out/(name+'.log')).open('wb') as log:
        result = subprocess.run([str(root/'build-win81/pulse.exe'), *args], cwd=root, env=env,
            startupinfo=startup, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    row = dict(case=name, exit=result.returncode, image_exists=(out/(name+'.png')).exists())
    results.append(row)
    print(json.dumps(row), flush=True)
fixture = out/'optional-renderer'
fixture.mkdir(exist_ok=True)
for name in ['pulse.exe','pulse_shell.exe','Pulse.Preview.exe','Pulse.Index.exe']:
    shutil.copy2(root/'build-win81'/name, fixture/name)
for name in ['missing-lumatext', 'invalid-lumatext']:
    dll = fixture/'lumatext.dll'
    if name == 'missing-lumatext':
        dll.unlink(missing_ok=True)
    else:
        dll.write_bytes(b'Invalid optional renderer test fixture')
    image_path = out/(name+'.png')
    env = dict(base_env, PULSE_COMPAT_81='0', PULSE_TEST_QUICK_MENU='1')
    with (out/(name+'.log')).open('wb') as log:
        result = subprocess.run([str(fixture/'pulse.exe'), '--menushot', str(image_path), str(root)],
            cwd=root, env=env, startupinfo=startup, timeout=60, stdout=log, stderr=subprocess.STDOUT)
    row = dict(case=name, exit=result.returncode, image_exists=image_path.exists())
    results.append(row)
    print(json.dumps(row), flush=True)
(out/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
raise SystemExit(any(r['exit'] != 0 or not r['image_exists'] for r in results))
