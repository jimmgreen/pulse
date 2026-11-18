"""Opt-in Windows real CLI dev reload check, using only a new isolated fixture.

Closes only the Pulse window belonging to the child of the CLI process it starts.
The application's separate dev probe supplies screenshots and disabled-state tests.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def child_process(parent, names=("pulse.exe",)):
    class Entry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = Entry()
        entry.dwSize = ctypes.sizeof(entry)
        valid = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while valid:
            if entry.th32ParentProcessID == parent and entry.szExeFile.lower() in names:
                return entry.th32ProcessID
            valid = kernel.Process32NextW(handle, ctypes.byref(entry))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES is the only successful exhaustion.
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
    return None


def close_window(pid):
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    @callback_type
    def visit(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(256)
        user.GetClassNameW(window, name, len(name))
        if owner.value == pid and name.value == "PulseMainWindow":
            user.PostMessageW(window, 0x0010, 0, 0)
        return True
    user.EnumWindows(visit, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows is required")
    output = args.output.absolute()
    output.mkdir(exist_ok=False)
    manifest = output / "manifest.json"
    data = json.loads((ROOT / "examples/extensions/view-panel/manifest.json").read_text(encoding="utf-8"))
    manifest.write_text(json.dumps(data, ensure_ascii=True), encoding="utf-8")
    profile = output / "profile"
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    process = subprocess.Popen([sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"), "--pulse",
                                str(args.pulse.absolute()), "--json", "dev", str(manifest), "--profile", str(profile)],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    status_path = profile / "extension-dev-status.json"
    checks = []
    child = None

    def check(ok, label):
        checks.append({"name": label, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + label, flush=True)
        if not ok:
            raise AssertionError(label)

    def wait_status(predicate):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stdout, stderr = process.communicate()
                raise RuntimeError("CLI exited early: " + repr((stdout, stderr)))
            try:
                status = json.loads(status_path.read_text(encoding="utf-8"))
                if predicate(status):
                    return status
            except (OSError, ValueError):
                pass
            time.sleep(0.1)
        raise TimeoutError("Timed out waiting for isolated development status")

    try:
        initial = wait_status(lambda state: state["status"] == "active")
        child = child_process(process.pid)
        check(child is not None, "CLI starts its own identifiable Pulse child")
        check(initial["enabled"], "initial development manifest is active")
        data["name"] = "Live reloaded package"
        manifest.write_text(json.dumps(data, ensure_ascii=True), encoding="utf-8")
        changed = wait_status(lambda state: state["status"] == "active" and state["appliedRevision"] > initial["appliedRevision"])
        check(changed["revision"] == changed["appliedRevision"], "valid edit commits a newer revision")
        manifest.write_text("{", encoding="utf-8")
        invalid = wait_status(lambda state: state["status"] == "invalid")
        check(invalid["appliedRevision"] == changed["appliedRevision"] and bool(invalid["error"]), "invalid edit keeps previous applied revision and reports error")
        data["name"] = "Live recovered package"
        manifest.write_text(json.dumps(data, ensure_ascii=True), encoding="utf-8")
        recovered = wait_status(lambda state: state["status"] == "active" and state["appliedRevision"] > invalid["revision"])
        check(recovered["enabled"] and not recovered["error"], "valid correction recovers development reload")
        close_window(child)
        stdout, stderr = process.communicate(timeout=30)
        result = json.loads(stdout.decode("utf-8"))
        check(process.returncode == 0 and result["ok"] and result["dev"]["isolated"] and result["dev"]["exitCode"] == 0,
              "closing own development window completes CLI with success")
        final = json.loads(status_path.read_text(encoding="utf-8"))
        check(final["status"] == "stopped", "shutdown stops worker and records final status")
    finally:
        if process.poll() is None:
            child = child or child_process(process.pid)
            if child:
                close_window(child)
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.communicate(timeout=5)
        (output / "checks.json").write_text(json.dumps({"checks": checks, "pulsePid": child,
                                                       "cliPid": process.pid}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
