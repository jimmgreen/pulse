"""Explicitly opted-in active-desktop acceptance; opens only its own test folder."""
import argparse, ctypes, hashlib, json, os, pathlib, re, shutil, subprocess, threading, time, urllib.parse

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--allow-live-window',action='store_true',required=True)
    parser.add_argument('--build',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--pulse-pid',type=int,required=True)
    parser.add_argument('--production-route',action='store_true')
    parser.add_argument('--watch-exe')
    parser.add_argument('--direct-only',action='store_true')
    parser.add_argument('--installed-hook',action='store_true')
    parser.add_argument('--select',action='store_true',help='Exercise split /select, with a probe-owned file')
    parser.add_argument('--electron-engine',help='Use the actual application executable in ELECTRON_RUN_AS_NODE mode')
    args=parser.parse_args()
    build=pathlib.Path(args.build).resolve();out=pathlib.Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
    node=str(pathlib.Path(args.electron_engine).resolve()) if args.electron_engine else shutil.which('node');assert node
    addon=build/('pulse_explorer_process_node.node' if args.production_route else 'explorer_probe.node')
    bridge=build/('pulse_explorer_bridge.exe' if args.production_route else 'explorer_bridge.exe')
    watcher=pathlib.Path(args.watch_exe).resolve() if args.watch_exe else build/'explorer_watch.exe'
    expected_stdout='' if args.production_route else 'ACK accepted\r\n'
    folder=out/'Pulse Explorer 接管验收';folder.mkdir(exist_ok=True)
    (folder/'01-这是探针创建的测试文件.txt').write_text('Probe-owned acceptance fixture.\n',encoding='utf-8')
    (folder/'02-原生与直达使用同一目录.txt').write_text('Same folder for both routes.\n',encoding='utf-8')
    original=out/'caller-original.cjs';modified=out/'caller-direct.cjs'
    shutil.copyfile(pathlib.Path(__file__).with_name('fixture.cjs'),original)
    modified.write_text('require('+json.dumps(str(addon))+');\n'+original.read_text(),encoding='utf-8')
    uri=re.sub(r'(?:%[89A-F][0-9A-F])+',lambda m:urllib.parse.unquote(m.group()),folder.as_uri(),flags=re.I)
    operands=['/select,',str(folder/'01-这是探针创建的测试文件.txt')] if args.select else [uri]
    expected_entry=2 if args.select else 0
    case=out/'input.json';case.write_text(json.dumps([dict(name='same-folder',command='explorer.exe',args=operands)],ensure_ascii=False),encoding='utf-8')
    env=dict(os.environ,PULSE_EXPLORER_PROBE_BRIDGE=str(build/'explorer_bridge.exe'),
        PULSE_EXPLORER_PROBE_ENDPOINT='Local\\Pulse.ExplorerProbe.LiveApproved',PULSE_EXPLORER_PROBE_LIVE='1')
    env.pop('PULSE_EXPLORER_PROBE_FALLBACK_FIXTURE',None)
    if args.electron_engine:env['ELECTRON_RUN_AS_NODE']='1'
    if args.production_route:env['PULSE_PRODUCTION_PROBE_BRIDGE']=str(bridge)
    if args.installed_hook:env['PULSE_PRODUCTION_PROBE_AUTOLOAD']='1'
    else:env.pop('PULSE_PRODUCTION_PROBE_AUTOLOAD',None)
    provenance={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in
        [pathlib.Path(node),addon,bridge,watcher,original,modified,case,pathlib.Path(__file__).resolve()]}
    watch=subprocess.Popen([str(watcher)],stdout=subprocess.PIPE,text=True,encoding='utf-8')
    events=[];ready=threading.Event()
    def read():
        for line in watch.stdout:
            if line.strip()=='READY':ready.set()
            elif line.startswith('{'):events.append(json.loads(line))
    thread=threading.Thread(target=read,daemon=True);thread.start()
    if not ready.wait(5):watch.terminate();watch.wait();raise RuntimeError('watch not ready')
    kernel=ctypes.WinDLL('kernel32');kernel.GetTickCount64.restype=ctypes.c_ulonglong
    root=pathlib.Path(os.environ['LOCALAPPDATA'])/'Pulse'/'Diagnostics'/'Runtime'
    def logs():
        rows={}
        for path in root.glob('app-'+str(args.pulse_pid)+'.jsonl*'):
            for line in path.read_text(encoding='utf-8',errors='replace').splitlines():
                try:
                    x=json.loads(line);rows[x['seq']]=x
                except (ValueError,KeyError):pass
        return rows
    interesting={'explorer_window_shown','explorer_candidate','explorer_handoff','external_open',
        'window_reveal_begin','window_reveal_end','navigation_request','navigation_enumerated',
        'navigation_snapshot_applied','navigation_end'}
    reports=[]
    try:
        runs=[('direct-1',modified),('direct-2',modified)] if args.direct_only else [('native-1',original),('direct-1',modified),('native-2',original),('direct-2',modified)]
        for label,caller in runs:
            prior=max(logs(),default=0);start=kernel.GetTickCount64()
            result=subprocess.run([node,str(caller),str(case)],env=env,capture_output=True,text=True,encoding='utf-8',timeout=12)
            # Allow real handoff/presentation to settle; callback is not first paint.
            time.sleep(2.5)
            end=kernel.GetTickCount64()
            rows=[x for seq,x in sorted(logs().items()) if seq>prior and x['event'] in interesting]
            observed=[x for x in events if start<=x['tick_ms']<=end]
            decoded=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            report=dict(label=label,command=[node,str(caller),str(case)],start_tick=start,end_tick=end,
                exit_status=result.returncode,stdout=result.stdout,stderr=result.stderr,caller=decoded,
                explorer_events=observed,pulse_events=rows,provenance=provenance)
            reports.append(report)
            (out/(label+'.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
            assert result.returncode==0 and len(decoded)==1 and decoded[0]['callbacks']==1 and not decoded[0]['killed'],report
            if label.startswith('direct'):
                assert decoded[0]['code']==0 and decoded[0]['stdout']==expected_stdout and 'bridge_image_verified=1' in result.stderr,report
                assert not observed,report
                if args.installed_hook:assert '[INSTALLED_CORE] com=0x00000000 ready=1' in result.stderr,report
                # TraceOpenRequest: entry=0 ordinary/direct-folder, 1 takeover,
                # 2 reveal. Wire v4 is not separately encoded in this log field.
                assert any(x['event']=='external_open' and x['data'].get('entry')==expected_entry and x['data'].get('handoff_id')==0 for x in rows),report
                assert any(x['event']=='window_reveal_end' and x['data'].get('reveal_ok')==1 for x in rows),report
            else:
                assert decoded[0]['code'] in (0,1),report
                assert any(x['event']==0x8002 for x in observed),report
                assert any(x['event']=='explorer_handoff' and x['data'].get('phase')==13 for x in rows),report
        summary=[]
        native_ids={x['data']['directory_id'] for r in reports if args.direct_only or r['label'].startswith('native')
                    for x in r['pulse_events'] if x['event']=='navigation_snapshot_applied'}
        assert len(native_ids)==1,native_ids
        for r in reports:
            rows=r['pulse_events']
            assert any(x['event']=='navigation_snapshot_applied' and x['data']['directory_id'] in native_ids and x['data']['entries']==2 for x in rows),r
            assert any(x['event']=='window_reveal_end' and x['data'].get('reveal_ok')==1 for x in rows),r
            if r['label'].startswith('direct'):
                assert r['caller'][0]['code']==0 and r['caller'][0]['stdout']==expected_stdout and not r['explorer_events'],r
                assert any(x['event']=='external_open' and x['data'].get('entry')==expected_entry and x['data'].get('handoff_id')==0 for x in rows),r
            reveals=[x['tick_ms']-r['start_tick'] for x in r['pulse_events'] if x['event']=='window_reveal_end' and x['data'].get('reveal_ok')]
            snapshots=[x['tick_ms']-r['start_tick'] for x in r['pulse_events'] if x['event']=='navigation_snapshot_applied']
            summary.append(dict(label=r['label'],caller_elapsed_ms=r['caller'][0]['elapsed_ms'],
                launch_to_reveal_ms=reveals,launch_to_snapshot_ms=snapshots,
                explorer_show_events=sum(x['event']==0x8002 for x in r['explorer_events'])))
        (out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(summary,ensure_ascii=False,indent=2))
    finally:
        watch.terminate();watch.wait();thread.join(2)
        (out/'window-events.json').write_text(json.dumps(events,indent=2),encoding='utf-8')

if __name__=='__main__':main()
