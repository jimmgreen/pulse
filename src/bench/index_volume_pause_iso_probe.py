"""Opt-in read-only lease probe for an explicitly marked temporary ISO fixture.

Never mounts, detaches, locks or writes the volume. The system-volume root is
rejected before starting the isolated host or sending a volume request.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import msvcrt
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

manifest = Path(sys.argv[1]).resolve()
fixture = json.loads(manifest.read_text(encoding="utf-8-sig"))
root, marker, image = fixture["root"], fixture["marker"], Path(fixture["image"]).resolve()
assert re.fullmatch(r"pulseiso[0-9a-f]{32}", marker)
assert re.fullmatch(r"[A-Za-z]:\\", root)
assert root[:2].upper() != os.environ.get("SystemDrive", "C:").upper()
assert image == (Path(tempfile.gettempdir()) / (marker + ".iso")).resolve()
assert image.is_file()
env = os.environ.copy()
env["PULSE_LEASE_FIXTURE_MANIFEST"] = str(manifest)
verification = r"""
$ErrorActionPreference = 'Stop'
$fixture = Get-Content -LiteralPath $env:PULSE_LEASE_FIXTURE_MANIFEST -Raw | ConvertFrom-Json
$image = Get-DiskImage -ImagePath $fixture.image
$volumes = $image | Get-Volume
if (-not $image.Attached -or -not ($volumes | Where-Object { ($_.DriveLetter + ':\') -eq $fixture.root })) {
    throw 'Fixture image-to-volume mapping mismatch'
}
$text = Get-Content -LiteralPath (Join-Path $fixture.root ($fixture.marker + '.txt')) -Raw
if ($text.Trim() -ne $fixture.marker) { throw 'Fixture marker mismatch' }
"""
subprocess.run(["powershell.exe", "-NoProfile", "-Command", verification], env=env,
               creationflags=subprocess.CREATE_NO_WINDOW, check=True, timeout=30)
assert (Path(root) / (marker + ".txt")).read_text().strip() == marker
print("[PASS] fixture image mapping and unique marker verified", flush=True)

k = c.WinDLL("kernel32", use_last_error=True)
k.PeekNamedPipe.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, c.POINTER(w.DWORD), w.LPVOID]
k.PeekNamedPipe.restype = w.BOOL
k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k.CreateFileW.restype = w.HANDLE
k.CloseHandle.argtypes = [w.HANDLE]
k.GetFileType.argtypes = [w.HANDLE]
k.GetFileType.restype = w.DWORD
k.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
k.DuplicateHandle.argtypes = [w.HANDLE, w.HANDLE, w.HANDLE, c.POINTER(w.HANDLE), w.DWORD, w.BOOL, w.DWORD]
k.GetCurrentProcess.restype = w.HANDLE
nt = c.WinDLL("ntdll")
nt.NtQuerySystemInformation.argtypes = [w.ULONG, w.LPVOID, w.ULONG, c.POINTER(w.ULONG)]
nt.NtQuerySystemInformation.restype = c.c_long


class HandleEntry(c.Structure):
    _fields_ = [("object", c.c_void_p), ("pid", c.c_size_t), ("handle", c.c_size_t),
                ("access", w.ULONG), ("trace", w.USHORT), ("kind", w.USHORT),
                ("attributes", w.ULONG), ("reserved", w.ULONG)]


def fixture_handles():
    # Enumerate metadata, but duplicate/query only handles in our own child.
    # Other applications' handles are never inspected or closed.
    size = 1024 * 1024
    while True:
        buffer = c.create_string_buffer(size)
        needed = w.ULONG()
        status = nt.NtQuerySystemInformation(64, buffer, size, c.byref(needed)) & 0xFFFFFFFF
        if status == 0:
            break
        if status != 0xC0000004 or size >= 64 * 1024 * 1024:
            raise OSError(f"handle metadata query: {status:x}")
        size = max(size * 2, needed.value)
    count = c.c_size_t.from_buffer(buffer).value
    offset = 2 * c.sizeof(c.c_size_t)
    assert offset + count * c.sizeof(HandleEntry) <= size
    found = 0
    for index in range(count):
        entry = HandleEntry.from_buffer(buffer, offset + index * c.sizeof(HandleEntry))
        if entry.pid != host.pid:
            continue
        duplicate = w.HANDLE()
        if not k.DuplicateHandle(int(host._handle), entry.handle, k.GetCurrentProcess(), c.byref(duplicate), 0, False, 2):
            continue
        try:
            if k.GetFileType(duplicate) != 1:  # FILE_TYPE_DISK: no pipe/provider queries.
                continue
            path = c.create_unicode_buffer(32768)
            length = k.GetFinalPathNameByHandleW(duplicate, path, len(path), 0)
            if length and length < len(path):
                value = path.value.removeprefix("\\\\?\\").rstrip("\\").casefold()
                if value == root.rstrip("\\").casefold():
                    found += 1
        finally:
            k.CloseHandle(duplicate)
    return found


def eventually(expected, seconds=10):
    deadline = time.monotonic() + seconds
    while True:
        count = fixture_handles()
        if (count > 0) == expected:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"fixture handle presence expected {expected}, count {count}")
        time.sleep(.02)


base = Path(tempfile.mkdtemp(prefix="pulse-index-iso-lease-"))
assert base.resolve().parent == Path(tempfile.gettempdir()).resolve()
cache = base / "cache"
cache.mkdir()
token = "isolease" + str(os.getpid())
pipe_name = r"\\.\pipe\PulseIndex.Test." + token
exe = manifest.parent / "Pulse.Index.exe"
env["PULSE_INDEX_TEST_CALLER_FILTER"] = "1"
host = subprocess.Popen([str(exe), "--test-host", token, root, str(cache)], env=env,
                        creationflags=subprocess.CREATE_NO_WINDOW)
connections = []


def connect():
    deadline = time.monotonic() + 15
    while True:
        try:
            stream = open(pipe_name, "r+b", buffering=0)
            connections.append(stream)
            return stream
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.05)


def send(stream, kind, request, payload=b""):
    stream.write(struct.pack("<IIII", 0x58444950, kind, request, len(payload)) + payload)


def receive(stream, kind, request):
    deadline = time.monotonic() + 15

    def read(size):
        while True:
            available = w.DWORD()
            if not k.PeekNamedPipe(msvcrt.get_osfhandle(stream.fileno()), None, 0, None, c.byref(available), None):
                raise OSError(c.get_last_error())
            if available.value >= size:
                return stream.read(size)
            if time.monotonic() >= deadline:
                raise TimeoutError("isolated host response")
            time.sleep(.005)

    while True:
        magic, response, identifier, size = struct.unpack("<IIII", read(16))
        assert magic == 0x58444950 and size <= 48 * 1024 * 1024
        data = read(size) if size else b""
        if response == kind and identifier == request:
            return data


try:
    client = connect()
    eventually(True)
    print("[PASS] isolated host holds the fixture directory handle", flush=True)
    send(client, 1, 1)
    status = receive(client, 101, 1)
    assert struct.unpack("<I", status[-4:])[0] == 0x56504C31
    raw = root.encode("utf-16le")
    send(client, 20, 2, struct.pack("<II", 1, len(raw) // 2) + raw)
    error = struct.unpack("<I", receive(client, 120, 2))[0]
    assert error == 0, f"lease acknowledgment: {error}"
    eventually(False)
    print("[PASS] authenticated acknowledgment follows real fixture handle release", flush=True)
    client.close()
    eventually(True)
    print("[PASS] disconnect restores the fixture directory watch", flush=True)
    shutdown = connect()
    send(shutdown, 0x7FFF0001, 3)
    host.wait(timeout=15)
    shutdown.close()
    assert host.returncode == 0
    print("[PASS] isolated host exits and releases fixture handles", flush=True)
finally:
    for connection in connections:
        if not connection.closed:
            connection.close()
    if host.poll() is None:
        host.kill()  # Only the test-owned child, never a user's service or process.
        host.wait()
    shutil.rmtree(base)
    print("[PASS] isolated host cache cleaned; ISO remains mounted", flush=True)
