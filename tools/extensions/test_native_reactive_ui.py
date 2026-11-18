import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--pulse',type=Path,required=True)
    parser.add_argument('--plugin',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,default=Path(__file__).resolve().parents[2]/'examples/native/reactive-panel/manifest.json')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    for key in ('pulse','plugin','manifest','output'):setattr(args,key,getattr(args,key).resolve())
    if args.output.exists():parser.error('output must be a new isolated directory')
    args.output.mkdir(parents=True)
    profile=args.output/'profile'; fixture=profile/'fixture';fixture.mkdir(parents=True)
    for name,content in {'one.txt':b'first text','TWO.TXT':b'second text file','three.bin':b'\x00\x10\x20\xff'}.items():(fixture/name).write_bytes(content)
    before={p.name:sha(p) for p in fixture.iterdir()}
    env={k:v for k,v in os.environ.items() if not k.upper().startswith('PULSE_')}
    env.update(PULSE_TEST_DATA_DIR=str(profile),PULSE_TEST_EXTENSION_CAPTURE=str(args.output/'capture'),PULSE_TEST_NATIVE_CASE='reactive',LOCALAPPDATA=str(args.output/'local-app-data'))
    command=[str(args.pulse),'--test-instance','--extension-manifest',str(args.manifest),'--extension-native',str(args.plugin),str(fixture)]
    start=time.monotonic();timedout=False
    with (args.output/'stdout.log').open('wb') as stdout,(args.output/'stderr.log').open('wb') as stderr:
     process=subprocess.Popen(command,env=env,stdout=stdout,stderr=stderr)
     try:code=process.wait(timeout=120)
     except subprocess.TimeoutExpired:timedout=True;process.terminate();code=process.wait(timeout=15)
    report_path=args.output/'capture/report.txt'
    report=report_path.read_text(encoding='utf-8') if report_path.exists() else ''
    after={p.name:sha(p) for p in fixture.iterdir() if p.is_file()}
    images=list((args.output/'capture').glob('*.png'))
    validimages={p.name for p in images}=={'reactive-light.png','reactive-dark.png'} and all(p.stat().st_size>4096 and p.read_bytes()[:8]==b'\x89PNG\r\n\x1a\n' for p in images)
    result=dict(exitCode=code,exitHex=f'0x{code&0xffffffff:08X}',timedOut=timedout,hostSha256=sha(args.pulse),pluginSha256=sha(args.plugin),elapsedSeconds=round(time.monotonic()-start,3),passCount=report.count('[PASS]'),failCount=report.count('[FAIL]'),filesUnchanged=before==after,screenshotsValid=validimages)
    result['passed']=code==0 and not timedout and 'Result: PASS' in report and '[FAIL]' not in report and before==after and validimages
    (args.output/'report.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result),flush=True)
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
