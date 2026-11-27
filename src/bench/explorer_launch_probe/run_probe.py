"""Own Node/receiver processes only; no installed app edits or user-app injection."""
import argparse, ctypes, hashlib, json, os, pathlib, queue, shutil, subprocess, threading, time, uuid

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    build, output = pathlib.Path(args.build).resolve(), pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = pathlib.Path(__file__).resolve().parent
    node = shutil.which('node')
    assert node
    folder = output / 'fixtures' / '中文 folder,=100%'
    folder.mkdir(parents=True, exist_ok=True)
    selected = folder / '选中 item.txt'
    selected.write_text('probe-owned file\n', encoding='utf-8')
    original, modified = output/'ORIGINAL_FILE.cjs', output/'MODIFIED_FILE.cjs'
    shutil.copyfile(source/'fixture.cjs', original)
    modified.write_text('require('+json.dumps(str(build/'explorer_probe.node'))+');\n'+original.read_text(), encoding='utf-8')
    endpoint = 'Local\\Pulse.ExplorerProbe.' + str(uuid.uuid4())
    env = dict(os.environ, PULSE_EXPLORER_PROBE_BRIDGE=str(build/'explorer_bridge.exe'),
               PULSE_EXPLORER_PROBE_ENDPOINT=endpoint,
               PULSE_EXPLORER_PROBE_FALLBACK_FIXTURE=os.environ['COMSPEC'])
    records = []
    def execute(label, command, environment=env):
        result = subprocess.run(command, env=environment, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=40)
        record = dict(label=label, command=command, exit_status=result.returncode, stdout=result.stdout, stderr=result.stderr)
        records.append(record)
        (output/(label+'.log')).write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
        return result
    def cases_file(name, cases):
        path = output/(name+'.json');path.write_text(json.dumps(cases, ensure_ascii=False), encoding='utf-8');return str(path)
    control = cases_file('control', [dict(name='unrelated-child', command=node,
        args=['-e', "process.stdout.write(process.env.PROBE_CHILD_ENV+'|'+process.cwd());process.stderr.write('stderr-preserved');process.exitCode=7"], cwd=str(folder))])
    baseline = execute('BASELINE', [node, str(original), control])
    control_hooked = execute('control-hooked', [node, str(modified), control])
    def decoded(result): return [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    b, c = decoded(baseline), decoded(control_hooked)
    assert baseline.returncode == control_hooked.returncode == 0 and len(b) == len(c) == 1
    assert all(x[0]['code']==7 and x[0]['callbacks']==1 and not x[0]['killed'] and
               x[0]['stdout']=='preserved|'+str(folder) and x[0]['stderr']=='stderr-preserved' for x in [b,c])
    unit = execute('parser', [str(build/'explorer_route_test.exe')]); assert unit.returncode == 0
    def receiver(mode):
        proc = subprocess.Popen([str(build/'explorer_receiver.exe'), endpoint, mode], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        lines, ready = [], threading.Event()
        def read():
            for line in proc.stdout:
                lines.append(line.rstrip('\n'))
                if line.rstrip()=='READY':ready.set()
        thread=threading.Thread(target=read, daemon=True);thread.start()
        if not ready.wait(8):
            proc.terminate();proc.wait();raise RuntimeError('receiver did not publish endpoint')
        return proc, lines, thread
    # Snapshot polling is supplementary, not proof of all process starts. The
    # addon also verifies the returned OS child image and the bridge logs ACK.
    observed=set(); stop=threading.Event()
    user32=ctypes.WinDLL('user32', use_last_error=True)
    callback=ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_ssize_t)
    def windows():
        found=set()
        @callback
        def enum(hwnd, _):
            name=ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(ctypes.c_void_p(hwnd),name,128)
            if name.value=='CabinetWClass':found.add(hwnd)
            return True
        user32.EnumWindows(enum,0);return found
    before=windows()
    def monitor():
        while not stop.wait(.01):observed.update(windows()-before)
    watcher=threading.Thread(target=monitor,daemon=True);watcher.start()
    proc, lines, thread=receiver('accept')
    success=[]
    try:
        explorer=str(pathlib.Path(os.environ['WINDIR'])/'explorer.exe')
        # Same Unicode normalization as the installed DeepSeek explorerTarget.
        url=folder.as_uri()
        import re, urllib.parse
        deepseek_url=re.sub(r'(?:%[89A-F][0-9A-F])+',lambda m:urllib.parse.unquote(m.group()),url, flags=re.I)
        input_path=cases_file('accepted', [dict(name='DeepSeek-execFile-'+str(i),command='explorer.exe',args=[deepseek_url],cwd=str(folder)) for i in range(5)] +
            [dict(name='explicit-system-image',command=explorer,args=[url],cwd=str(folder)),
             dict(name='select-item',command='explorer.exe',args=['/select,'+selected.as_uri()],cwd=str(folder))])
        result=execute('MODIFIED',[node,str(modified),input_path],dict(env,PULSE_EXPLORER_PROBE_EXPECT_CWD=str(folder)));success=decoded(result)
        time.sleep(.15)
        assert result.returncode==0 and len(success)==7
        assert result.stderr.count('bridge_image_verified=1')==7, result.stderr
        assert all(x['callbacks']==1 and x['code']==0 and not x['killed'] and x['stdout']=='CONTEXT cwd-env-preserved\r\nACK accepted\r\n' and not x['stderr'] for x in success),success
        assert lines.count('REQUEST folder '+str(folder))==6 and lines.count('REQUEST select '+str(selected))==1,lines
    finally:
        proc.terminate();proc.wait();thread.join(2)
        (output/'accepted-receiver.log').write_text('\n'.join(lines),encoding='utf-8')
        stop.set();watcher.join(2)
    assert not observed, 'new Explorer windows observed: '+str(observed)
    proc, lines, thread=receiver('reject')
    try:
        rejection=cases_file('rejection',[dict(name='explicit-rejection',command='explorer.exe',args=[deepseek_url])])
        result=execute('rejection',[node,str(modified),rejection]);r=decoded(result)
        assert len(r)==1 and r[0]['callbacks']==1 and r[0]['code']==37 and 'ACK rejected fallback=test-stand-in' in r[0]['stdout']
        assert len(lines)==1,lines
    finally:
        proc.terminate();proc.wait();thread.join(2)
        (output/'rejected-receiver.log').write_text('\n'.join(lines),encoding='utf-8')
    result=execute('absent-endpoint',[node,str(modified),rejection]);r=decoded(result)
    assert len(r)==1 and r[0]['code']==37 and not r[0]['killed']
    # A fresh process with the addon line removed proves no persistent hook.
    restored=output/'restored-copy.cjs';shutil.copyfile(modified,restored)
    rollback=output/'ROLLBACK.sh'
    rollback.write_text('#!/bin/sh\nset -eu\n[ "$#" -eq 1 ] || { echo "usage: ROLLBACK.sh TARGET_COPY" >&2; exit 2; }\nbase=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\ncp -- "$base/ORIGINAL_FILE.cjs" "$1"\n',encoding='utf-8',newline='\n')
    bash=pathlib.Path('C:/Program Files/Git/bin/bash.exe')
    restored_result=execute('rollback-copy',[str(bash),str(rollback),str(restored)]);assert restored_result.returncode==0
    result=execute('ROLLBACK',[node,str(restored),control]);r=decoded(result)
    assert hashlib.sha256(original.read_bytes()).digest()==hashlib.sha256(restored.read_bytes()).digest()
    assert len(r)==1 and r[0]['code']==7 and r[0]['stdout']==b[0]['stdout'] and r[0]['stderr']==b[0]['stderr'] and '[HOOK]' not in result.stderr
    summary=dict(parser='24 passed',accepted_requests=len(success),new_explorer_windows_observed=len(observed),
        accepted_elapsed_ms=[x['elapsed_ms'] for x in success],fallback='explicit rejection / absent endpoint: real cmd.exe stand-in exit 37',
        baseline='unrelated Node child contract, not unhooked Explorer UI latency',
        receiver='production SingleInstanceCoordinator and OpenRequestPipe, private namespace; not Pulse GUI',
        restored_sha256=hashlib.sha256(restored.read_bytes()).hexdigest(),modified_sha256=hashlib.sha256(modified.read_bytes()).hexdigest(),
        limitations=['no real DeepSeek injection or menu click','this private suite is not Pulse GUI presentation; separate run_live.py covers GUI logs',
        'native Explorer fallback is not executed in this suite','polling alone cannot exclude sub-10ms windows',
        'timeout/cancellation of a long-lived fallback descendant is not implemented or verified'])
    (output/'results.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    (output/'commands.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(summary,ensure_ascii=False,indent=2))

if __name__=='__main__': main()
