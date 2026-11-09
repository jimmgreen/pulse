"""Real installed-package launch check; only starts/closes its own isolated Pulse window."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from live_dev_check import close_window

ROOT = Path(__file__).resolve().parents[2]


def main_window(pid):
    user = ctypes.WinDLL("user32", use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    found = []
    @callback
    def visit(window, _):
        process = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(process))
        name = ctypes.create_unicode_buffer(256)
        user.GetClassNameW(window, name, len(name))
        if process.value == pid and name.value == "PulseMainWindow":
            found.append(window)
        return True
    user.EnumWindows(visit, 0)
    return bool(found)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk-root", type=Path, default=ROOT)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(exist_ok=False)
    store = output / "store"
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(output / "local-app-data")
    checks = []
    pid = None
    handle = None
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    def check(ok, name):
        checks.append({"name": name, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + name, flush=True)
        if not ok:
            raise AssertionError(name)

    def call(*arguments):
        result = subprocess.run([sys.executable, str(args.sdk_root.absolute() / "tools/extensions/pulse_ext.py"), "--pulse",
                                 str(args.pulse.absolute()), "--json", *map(str, arguments)],
                                capture_output=True, timeout=40, env=env,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        value = json.loads(result.stdout.decode("utf-8"))
        check(result.returncode == 0 and value["ok"], str(arguments[0]) + " succeeds")
        return value

    try:
        api = call("inspect-sdk")["api"]
        check("installedDeclarativePackages" in api.get("developerFeatures", []), "host advertises installed package support")
        for template in ("navigation-panel", "view-panel"):
            source = args.sdk_root.absolute() / "examples/extensions" / template / "manifest.json"
            manifest = json.loads(source.read_text(encoding="utf-8"))
            archive = output / (template + ".pulse-ext")
            call("pack", source, "--output", archive)
            call("install", archive, "--store", store)
            call("enable", manifest["id"], "--store", store)
        launched = call("run", "--store", store, "--profile", output / "profile")["run"]
        pid = launched["processId"]
        check(launched["enabledPackages"] == 2 and launched["isolated"] and launched["loadVerified"] is False,
              "run reports two isolated packages without inventing completed verification")
        check(Path(launched["processLog"]).is_file(), "run returns retained native output log")
        handle = kernel.OpenProcess(0x00100000 | 0x1000, False, pid)
        check(bool(handle), "own process can be waited without affecting other Pulse instances")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline and not main_window(pid):
            if kernel.WaitForSingleObject(handle, 0) == 0:
                break
            time.sleep(0.1)
        check(main_window(pid), "installed package session opens a real main window")
        close_window(pid)
        check(kernel.WaitForSingleObject(handle, 30000) == 0, "own installed session closes normally")
        code = wintypes.DWORD()
        check(bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and code.value == 0,
              "installed session exits successfully")
        registry = json.loads((store / "registry.json").read_text(encoding="utf-8"))
        check(all(entry["enabled"] for entry in registry["packages"]), "window shutdown leaves persistent enable state intact")
        before = (store / "registry.json").read_bytes()
        registry["packages"][0]["activeRevision"] = "0" * 64
        (store / "registry.json").write_text(json.dumps(registry), encoding="utf-8")
        rejected_env = dict(env, PULSE_TEST_DATA_DIR=str(output / "profile"))
        result = subprocess.run([str(args.pulse.absolute()), "--test-instance", "--extension-store", str(store)],
                                env=rejected_env, capture_output=True, timeout=15,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        check(result.returncode == 2 and b"Pulse extension:" in result.stderr,
              "native startup rejects invalid installed content with diagnostic before UI")
        (store / "registry.json").write_bytes(before)
    finally:
        if handle:
            if kernel.WaitForSingleObject(handle, 0) != 0:
                close_window(pid)
                kernel.WaitForSingleObject(handle, 10000)
            kernel.CloseHandle(handle)
        (output / "checks.json").write_text(json.dumps({"checks": checks, "pulsePid": pid}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
