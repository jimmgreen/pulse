"""Administrator read-only handle metadata probe for a marked temporary VHDX.

Arguments: fixture.ready output.json. Never changes source handles, reads file
contents (except the explicit fixture marker), stops services or detaches disks.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

manifest, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
requested_pid = int(sys.argv[3]) if len(sys.argv) > 3 else None
lines = [line.strip() for line in manifest.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
roots, image = lines[:-1], Path(lines[-1]).resolve()
assert roots and all(re.fullmatch(r"[A-Za-z]:\\", root) for root in roots)
assert all(root[:2].upper() != os.environ.get("SystemDrive", "C:").upper() for root in roots)
assert image.parent == Path(tempfile.gettempdir()).resolve() and image.name.startswith("pulse-")
assert image.suffix.lower() == ".vhdx" and image.is_file()
env = os.environ.copy()
env["PULSE_HANDLE_FIXTURE"] = str(manifest)
verify = r"""
$ErrorActionPreference = 'Stop'
$lines = Get-Content -LiteralPath $env:PULSE_HANDLE_FIXTURE | Where-Object { $_.Trim() }
$imagePath = $lines[-1]
$image = Get-DiskImage -ImagePath $imagePath
if (-not $image.Attached -or $image.DevicePath -notmatch '^\\\\\.\\PhysicalDrive(\d+)$') { throw 'VHD mapping unavailable' }
$disk = Get-Disk -Number ([int]$Matches[1])
if ($disk.IsBoot -or $disk.IsSystem) { throw 'System disk forbidden' }
$volumes = $disk | Get-Partition | Get-Volume
foreach ($root in $lines[0..($lines.Count-2)]) {
    if (-not ($volumes | Where-Object { ($_.DriveLetter + ':\') -eq $root })) { throw 'Volume mapping mismatch' }
}
$marker = Get-Content -LiteralPath (Join-Path $lines[0] 'pulse-eject-fixture.txt') -Raw
if ($marker -notmatch [regex]::Escape($imagePath)) { throw 'Fixture marker mismatch' }
@{ disk_number = $disk.Number; attached = $image.Attached } | ConvertTo-Json -Compress
"""
verified = subprocess.run(["powershell.exe", "-NoProfile", "-Command", verify], env=env,
                          capture_output=True, text=True, timeout=40,
                          creationflags=subprocess.CREATE_NO_WINDOW)
if verified.returncode:
    raise RuntimeError("Fixture validation failed: " + verified.stderr.strip())
identity = json.loads(verified.stdout)

k = c.WinDLL("kernel32", use_last_error=True)
nt = c.WinDLL("ntdll")
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.GetCurrentProcess.restype = w.HANDLE
k.CloseHandle.argtypes = [w.HANDLE]
k.QueryDosDeviceW.argtypes = [w.LPCWSTR, w.LPWSTR, w.DWORD]
k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k.CreateFileW.restype = w.HANDLE
k.GetFileType.argtypes = [w.HANDLE]
k.GetFileType.restype = w.DWORD
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
k.DuplicateHandle.argtypes = [w.HANDLE, w.HANDLE, w.HANDLE, c.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD]
nt.NtQuerySystemInformation.argtypes = [w.ULONG, w.LPVOID, w.ULONG, c.POINTER(w.ULONG)]
nt.NtQuerySystemInformation.restype = c.c_long
nt.NtQueryObject.argtypes = [w.HANDLE, w.ULONG, w.LPVOID, w.ULONG, c.POINTER(w.ULONG)]
nt.NtQueryObject.restype = c.c_long


class Entry(c.Structure):
    _fields_ = [("object", c.c_void_p), ("pid", c.c_size_t), ("handle", c.c_size_t),
                ("access", w.ULONG), ("trace", w.USHORT), ("kind", w.USHORT),
                ("attributes", w.ULONG), ("reserved", w.ULONG)]


class UnicodeString(c.Structure):
    _fields_ = [("length", w.USHORT), ("maximum", w.USHORT), ("buffer", c.c_void_p)]


def device(root):
    text = c.create_unicode_buffer(4096)
    if not k.QueryDosDeviceW(root[:2], text, len(text)):
        raise OSError(c.get_last_error())
    return text.value


prefixes = {root: device(root) for root in roots}
result = {"roots": roots, "requested_pid": requested_pid, "disk_number": identity["disk_number"], "admin": bool(c.windll.shell32.IsUserAnAdmin()),
          "matches": [], "process_access_errors": {}, "duplicate_failures": 0, "name_failures": 0,
          "disk_handles_checked": 0, "timed_out": False, "fixture_valid": True}
lock = threading.Lock()


def save():
    with lock:
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")


def timeout():
    result["timed_out"] = True
    save()
    os._exit(2)  # Ends only this diagnostic child; its duplicates close normally.


timer = threading.Timer(45, timeout)
timer.daemon = True
timer.start()
processes = {}
probe = k.CreateFileW(roots[0], 0, 7, None, 3, 0x02000000, None)
assert probe != c.c_void_p(-1).value
try:
    size = 1024 * 1024
    while True:
        buffer, needed = c.create_string_buffer(size), w.ULONG()
        status = nt.NtQuerySystemInformation(64, buffer, size, c.byref(needed)) & 0xFFFFFFFF
        if status == 0:
            break
        if status != 0xC0000004 or size >= 128 * 1024 * 1024:
            raise OSError(f"System handle metadata: {status:x}")
        size = max(size * 2, needed.value)
    count, offset = c.c_size_t.from_buffer(buffer).value, 2 * c.sizeof(c.c_size_t)
    assert offset + count * c.sizeof(Entry) <= size
    entries = (Entry * count).from_buffer(buffer, offset)
    file_kind = next(entry.kind for entry in entries if entry.pid == os.getpid() and entry.handle == probe)
    k.CloseHandle(probe)
    probe = None
    checked_at = 0
    for entry in entries:
        if entry.kind != file_kind or entry.pid == os.getpid() or (requested_pid is not None and entry.pid != requested_pid):
            continue
        if time.monotonic() - checked_at > 1:
            checked_at = time.monotonic()
            if not image.is_file() or any(device(root) != value for root, value in prefixes.items()):
                result["fixture_valid"] = False
                raise RuntimeError("Fixture disappeared or drive mapping changed; stopping")
        if entry.pid not in processes:
            processes[entry.pid] = k.OpenProcess(0x1040, False, entry.pid)
            if not processes[entry.pid]:
                error = str(c.get_last_error())
                result["process_access_errors"][error] = result["process_access_errors"].get(error, 0) + 1
        process = processes[entry.pid]
        if not process:
            continue
        duplicate = w.HANDLE()
        if not k.DuplicateHandle(process, entry.handle, k.GetCurrentProcess(), c.byref(duplicate), 0, False, 2):
            result["duplicate_failures"] += 1
            continue
        try:
            if k.GetFileType(duplicate) != 1:
                continue
            result["disk_handles_checked"] += 1
            name_buffer, used = c.create_string_buffer(65536), w.ULONG()
            if nt.NtQueryObject(duplicate, 1, name_buffer, len(name_buffer), c.byref(used)) != 0:
                result["name_failures"] += 1
                continue
            name = UnicodeString.from_buffer(name_buffer)
            path = c.wstring_at(name.buffer, name.length // 2) if name.buffer else ""
            matched = [root for root, prefix in prefixes.items()
                       if path.casefold() == prefix.casefold() or path.casefold().startswith(prefix.casefold() + "\\")]
            if not matched:
                continue
            executable, capacity = c.create_unicode_buffer(32768), w.DWORD(32768)
            known = k.QueryFullProcessImageNameW(process, 0, executable, c.byref(capacity))
            result["matches"].append({"pid": entry.pid, "process": Path(executable.value).name if known else "unavailable",
                                      "roots": matched, "type": "disk file/directory/volume", "path": path})
        finally:
            k.CloseHandle(duplicate)  # Our duplicate only; never the source handle.
finally:
    timer.cancel()
    if probe:
        k.CloseHandle(probe)
    for process in processes.values():
        if process:
            k.CloseHandle(process)
    save()
