"""Integration checks against real Windows IFileOpenDialog/IFileSaveDialog.

Build pulse, then from an x64 developer prompt in the repository:
  cl /nologo /std:c++20 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE src/bench/dialog_jump_fixture.cpp /Fobuild/dialog-jump-fixture.obj /Febuild/dialog-jump-fixture.exe ole32.lib shell32.lib user32.lib uuid.lib
  python scripts/test_dialog_jump.py build/pulse.exe

Uses a separate Pulse profile and cancels/terminates only its own fixtures;
OnFileOk rejects acceptance, so no user file can be saved or opened.
Foreground events and WM_HOTKEY exercise production handlers. On a desktop
without input focus, this does not test physical Alt+Tab / Ctrl+G input.
"""
import ctypes as C
import ctypes.wintypes as W
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent / 'build'
u = C.WinDLL('user32', use_last_error=True)
k = C.WinDLL('kernel32')
u.SetProcessDpiAwarenessContext(C.c_void_p(-4))
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
u.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t)]
u.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, C.c_int]
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
def windows(pid):
    out=[]
    @CALLBACK
    def visit(h,l):
        p=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(p))
        if p.value==pid:
            name=C.create_unicode_buffer(256);u.GetClassNameW(h,name,256);out.append((h,name.value))
        return True
    u.EnumWindows(visit,0);return out
u.SetForegroundWindow.argtypes=[W.HWND];u.IsWindowVisible.argtypes=[W.HWND]
u.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
u.GetDlgCtrlID.argtypes=[W.HWND]
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetParent.argtypes=[W.HWND];u.GetParent.restype=W.HWND
u.ShowWindow.argtypes=[W.HWND,C.c_int]
u.GetForegroundWindow.restype=W.HWND
u.GetWindowRect.argtypes=[W.HWND,C.POINTER(W.RECT)]
u.GetDpiForWindow.argtypes=[W.HWND];u.GetDpiForWindow.restype=W.UINT
out=Path(tempfile.mkdtemp(prefix='dialog-jump-test-', dir=root))
print('Fixture directory:',out,flush=True)
profile=out/'profile';profile.mkdir(exist_ok=True)
initial=out/'Initial';dest=out/('Destination \u7a7a\u683c '+('x'*60));frequent=out/'Frequent'
for d in [initial,dest,frequent]:d.mkdir(exist_ok=True)
(profile/'app.json').write_text(json.dumps(dict(global_search_enabled=True,global_search_modifiers=7,global_search_key=135,global_search_double_ctrl=False,dialog_jump_bar=True,dialog_jump_auto=False,jump_history=True,global_search_apps=False)),encoding='utf-8')
(profile/'jump_history.tsv').write_text(str(frequent)+'\t5.0000\t'+str(int(time.time()))+'\n',encoding='utf-8')
def wait_for(fn,seconds=6):
    end=time.time()+seconds
    while time.time()<end:
        v=fn()
        if v:return v
        time.sleep(.05)
    return None
def find(p,name):return next((h for h,n in windows(p.pid) if n==name),None)
def rect(h):
    r=W.RECT();u.GetWindowRect(h,C.byref(r));return (r.left,r.top,r.right,r.bottom)
def textof(h):
    b=C.create_unicode_buffer(1024);r=C.c_size_t();u.SendMessageTimeoutW(h,13,1024,C.addressof(b),2,300,C.byref(r));return b.value
def children(h):
    result=[]
    @CALLBACK
    def visit(child,l):
        b=C.create_unicode_buffer(100);u.GetClassNameW(child,b,100);result.append((child,b.value,u.GetDlgCtrlID(child),textof(child)));return True
    u.EnumChildWindows(W.HWND(h),visit,0);return result
def foreground(h):
    tid=u.GetWindowThreadProcessId(h,None);me=k.GetCurrentThreadId()
    attached=u.AttachThreadInput(me,tid,True)
    u.SetForegroundWindow(h)
    if attached:u.AttachThreadInput(me,tid,False)
    # The sandbox desktop has no input foreground; exercise the WinEvent callback
    # with a notification for this fixture window only.
    if not u.GetForegroundWindow():u.NotifyWinEvent(3,W.HWND(h),0,0)
    time.sleep(.3)
def send(h,msg,w=0,l=0):
    result=C.c_size_t();u.SendMessageTimeoutW(h,msg,w,l,2,1500,C.byref(result))
def launch(args,env=None):
    si=subprocess.STARTUPINFO();si.dwFlags=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0
    return subprocess.Popen(list(map(str,args)),env=env,startupinfo=si)
env=os.environ.copy();env.update(PULSE_TEST_DATA_DIR=str(profile),PULSE_TEST_PERSIST='1',PULSE_TEST_GLOBAL_HOTKEY='1',LOCALAPPDATA=str(profile),APPDATA=str(profile))
exe=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'pulse.exe'
p=launch([exe,'--test-instance',dest],env);child=None
checks=0
def check(condition, label):
    global checks
    if not condition:raise AssertionError(label)
    checks+=1;print('[PASS]',label,flush=True)
def folder():
    try:return status.read_text(encoding='utf-16le')
    except (OSError,UnicodeError):return ''
def click(h,x,y):send(h,0x201,1,(int(y)<<16)|int(x));send(h,0x202,0,(int(y)<<16)|int(x))
try:
    main=wait_for(lambda:find(p,'PulseMainWindow'));assert main
    time.sleep(1)
    for mode in ['save','open']:
        status=out/(mode+'-folder.txt')
        child=launch([root/'dialog-jump-fixture.exe',mode,initial,status])
        dlg=wait_for(lambda:next((h for h,n in windows(child.pid) if n=='#32770' and u.IsWindowVisible(h)),None));assert dlg
        u.ShowWindow(dlg,5);time.sleep(.5);foreground(dlg)
        bar=wait_for(lambda:find(p,'Pulse.DialogJump'));assert bar
        check(wait_for(lambda:u.IsWindowVisible(bar)),mode+' bar shown')
        listing=find(p,'Pulse.DialogJumpList')
        check(not u.IsWindowVisible(listing),mode+' no unsolicited frequent popup')
        name=next(h for h,n,i,t in children(dlg) if n=='Edit' and i!=41477)
        original=textof(name)
        scale=u.GetDpiForWindow(bar)/96
        click(bar,270*scale,20*scale)
        check(wait_for(lambda:folder()==str(dest)),mode+' chip changes actual IFileDialog folder')
        check(textof(name)==original,mode+' typed filename preserved')
        check(u.IsWindowVisible(dlg),mode+' navigation does not accept/close dialog')
        edit=next(h for h,n,i,t in children(bar) if n=='Edit')
        foreground(main)
        check(not u.IsWindowVisible(bar),mode+' bar hidden on switch away')
        foreground(dlg)
        check(wait_for(lambda:u.IsWindowVisible(bar)),mode+' bar restored on return')
        value=C.create_unicode_buffer('Destination');send(edit,12,0,C.addressof(value))
        value=C.create_unicode_buffer('');send(edit,12,0,C.addressof(value));time.sleep(.2)
        check(not u.IsWindowVisible(listing),mode+' programmatic clear does not open frequent popup')
        click(bar,90*scale,20*scale)
        check(wait_for(lambda:u.IsWindowVisible(listing)),mode+' explicit search focus opens frequent places')
        check(rect(listing)[1] >= rect(bar)[3],mode+' frequent popup opens outside dialog')
        # A history row is a group header (25 DIP), then a 38 DIP item.
        click(listing,100*scale,50*scale);time.sleep(.4)
        check(not u.IsWindowVisible(listing),mode+' selecting frequent place closes popup')
        check(wait_for(lambda:folder()==str(frequent)),mode+' frequent row changes actual folder')
        # Move the native address field to a different directory, then exercise
        # the registered Ctrl+G handler without synthesizing global keyboard input.
        crumb=next(h for h,n,i,t in children(dlg) if n=='ToolbarWindow32' and i==1001)
        r=rect(crumb);click(crumb,r[2]-r[0]-2,(r[3]-r[1])/2)
        address=wait_for(lambda:next((h for h,n,i,t in children(dlg) if n=='Edit' and i==41477 and u.IsWindowVisible(h)),None));assert address
        value=C.create_unicode_buffer(str(initial));send(address,12,0,C.addressof(value))
        send(address,0x100,13,1);send(address,0x101,13,0xc0000001)
        check(wait_for(lambda:folder()==str(initial)),mode+' reset native folder')
        foreground(dlg);send(bar,0x312,0x5054,0)
        check(wait_for(lambda:folder()==str(dest)),mode+' Ctrl+G handler changes actual folder')
        check(textof(name)==original,mode+' Ctrl+G preserves filename')
        child.terminate();child.wait();child=None;time.sleep(.4)
    print('PASSED',checks,flush=True)
finally:
    for proc in [child,p]:
        if proc and proc.poll() is None:proc.terminate();proc.wait()
