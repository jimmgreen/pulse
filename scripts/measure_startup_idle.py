import argparse, ctypes, csv, json, os, subprocess, time
from ctypes import wintypes
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--initial', required=True)
parser.add_argument('--target', required=True)
parser.add_argument('--text-baseline', action='store_true')
parser.add_argument('--startup-only', action='store_true')
parser.add_argument('--minimize', action='store_true')
parser.add_argument('--hidden-repaint', action='store_true')
parser.add_argument('--hidden-baseline', action='store_true')
parser.add_argument('--hidden-navigate', action='store_true')
parser.add_argument('--hidden-scroll', action='store_true')
parser.add_argument('--hide', action='store_true')
a = parser.parse_args()
root = Path(a.output).resolve()
root.mkdir(parents=True, exist_ok=False)
for name in ('profile', 'temp'):
    (root/name).mkdir()
env = os.environ.copy()
for key in list(env):
    if key.startswith('PULSE_TEST_'):
        del env[key]
env.update(PULSE_TEST_DATA_DIR=str(root/'profile'), TEMP=str(root/'temp'), TMP=str(root/'temp'),
           PULSE_STARTUP_TRACE='1', PULSE_TEST_P1_SCOPE='selection',
           PULSE_TEST_STARTUP_PERF=str(root/'probe.csv'), PULSE_TEST_STARTUP_TARGET=str(Path(a.target).resolve()))
if a.text_baseline: env['PULSE_TEST_TEXT_BASELINE']='1'
if a.startup_only: env['PULSE_TEST_STARTUP_ONLY']='1'
if a.minimize or a.hide: env['PULSE_TEST_IDLE_MINIMIZE']='1'
if a.hide: env['PULSE_TEST_IDLE_HIDE']='1'
if a.hidden_repaint: env['PULSE_TEST_HIDDEN_REPAINT']='1'
if a.hidden_baseline: env['PULSE_TEST_HIDDEN_RENDER_BASELINE']='1'
if a.hidden_navigate: env['PULSE_TEST_HIDDEN_NAVIGATE']='1'
if a.hidden_scroll: env['PULSE_TEST_HIDDEN_SCROLL']='1'
exe = Path(a.exe).resolve()
process = subprocess.Popen([str(exe), '--test-instance', str(Path(a.initial).resolve())],
                           cwd=exe.parent, env=env)
(root/'process.json').write_text(json.dumps({'pid':process.pid,'exe':str(exe)}))
k32=ctypes.WinDLL('kernel32', use_last_error=True)
psapi=ctypes.WinDLL('psapi', use_last_error=True)
k32.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
k32.OpenProcess.restype=wintypes.HANDLE
k32.CloseHandle.argtypes=[wintypes.HANDLE]
psapi.QueryWorkingSet.argtypes=[wintypes.HANDLE,wintypes.LPVOID,wintypes.DWORD]
psapi.QueryWorkingSet.restype=wintypes.BOOL
handle=k32.OpenProcess(0x410,False,process.pid)
buffer=ctypes.create_string_buffer(2*1024*1024)
private=[];seen=set();start=time.monotonic()
try:
    while process.poll() is None and time.monotonic()-start<135:
        path=root/'probe.csv'
        if path.exists():
            try:
                with path.open() as f:
                    rows=list(csv.DictReader(f))
            except PermissionError:
                time.sleep(.05)
                continue
            for row in rows:
                event=row.get('event','')
                if not (event.startswith('idle_') or event.startswith('minimized_') or event.startswith('restored_')) or event in seen: continue
                seen.add(event)
                if psapi.QueryWorkingSet(handle,buffer,len(buffer)):
                    words=ctypes.cast(buffer,ctypes.POINTER(ctypes.c_size_t))
                    count=words[0]
                    assert (count+1)*ctypes.sizeof(ctypes.c_size_t)<=len(buffer)
                    private.append({'event':event,'private_working_set_bytes':sum(not(words[i+1]&256) for i in range(count))*4096})
                else:
                    private.append({'event':event,'error':ctypes.get_last_error()})
        time.sleep(.05)
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=10)
        raise RuntimeError('Owned test process exceeded 135 seconds')
    rows=list(csv.DictReader((root/'probe.csv').open()))
    assert any(r['event']=='complete' for r in rows), rows
    assert process.returncode==0,process.returncode
    (root/'private_working_set.json').write_text(json.dumps(private,indent=2))
    (root/'run.json').write_text(json.dumps({'exe':str(exe),'pid':process.pid,'exit_code':process.returncode,
        'initial':str(Path(a.initial).resolve()),'target':str(Path(a.target).resolve()),
        'search_services':'disabled via selection scope','text_baseline':a.text_baseline,'startup_only':a.startup_only,'minimize':a.minimize,'hidden_repaint':a.hidden_repaint,'hidden_baseline':a.hidden_baseline,'hidden_navigate':a.hidden_navigate,'hidden_scroll':a.hidden_scroll,'hide':a.hide,'private_samples':private},indent=2))
    print(json.dumps({'output':str(root),'exit':process.returncode,'rows':rows,'private':private}))
finally:
    if process.poll() is None:
        process.terminate()  # Only this harness-owned isolated child.
        process.wait(timeout=10)
    if handle: k32.CloseHandle(handle)
