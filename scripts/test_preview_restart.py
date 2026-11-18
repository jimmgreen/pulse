"""Verify replacement preview hosts connect while the previous host is still alive."""
import ctypes
from ctypes import wintypes
import pathlib
import os
import struct
import subprocess
import sys
import time

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                              wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.GetNamedPipeServerProcessId.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG)]
kernel.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
exe = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/Pulse.Preview.exe").resolve()
hosts, pipes = [], []
token = str(os.getpid())

def start(process_pipe):
    host = subprocess.Popen([str(exe), token] + (["--process-pipe"] if process_pipe else []),
                            creationflags=subprocess.CREATE_NO_WINDOW)
    hosts.append(host)
    return host

def connect(host, process_pipe):
    name = rf"\\.\pipe\PulsePreview-{token}" + (f"-{host.pid}" if process_pipe else "")
    deadline = time.monotonic() + 3
    while True:
        pipe = kernel.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)
        if pipe != ctypes.c_void_p(-1).value:
            pipes.append(pipe)
            pid = wintypes.ULONG()
            if not kernel.GetNamedPipeServerProcessId(pipe, ctypes.byref(pid)) or pid.value != host.pid:
                raise RuntimeError("connected to wrong host")
            return pipe
        if host.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"connect failed: error={ctypes.get_last_error()}, exit={host.poll()}")
        time.sleep(0.025)

def request(pipe, request_id):
    # Properties of an offline placeholder deliberately avoid any provider or
    # filesystem access and return a successful empty property list.
    path = "offline-placeholder.txt".encode("utf-16-le")
    packet = struct.pack("<IIQIIIIII", 0x57565250, request_id, 73, 1, 32,
                         0x1000, len(path) // 2, 0, 0) + path
    count = wintypes.DWORD()
    if not kernel.WriteFile(pipe, packet, len(packet), ctypes.byref(count), None) or count.value != len(packet):
        raise RuntimeError("request write failed")
    deadline = time.monotonic() + 3
    # MSVC x64 PreviewResponse: 84 bytes of fields, 4 alignment bytes,
    # then 32-byte Integrity. Poll first so failures cannot hang the test.
    available = wintypes.DWORD()
    while True:
        if not kernel.PeekNamedPipe(pipe, None, 0, None, ctypes.byref(available), None):
            raise RuntimeError("response pipe closed")
        if available.value >= 120:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError("response timeout")
        time.sleep(0.025)
    buffer = ctypes.create_string_buffer(120)
    if not kernel.ReadFile(pipe, buffer, 120, ctypes.byref(count), None) or count.value != 120:
        raise RuntimeError("response read failed")
    magic, ident, generation, status, kind = struct.unpack_from("<IIQII", buffer.raw)
    mapping, text, error, properties = struct.unpack_from("<IIII", buffer.raw, 36)
    if (magic, ident, generation, status, kind, mapping, text, error, properties) != (0x57565250, request_id, 73, 0, 0, 0, 0, 0, 0):
        raise RuntimeError("invalid or stale response")

try:
    old = start(False)
    connect(old, False)
    conflicting = start(False)
    if conflicting.wait(timeout=5) != 3:
        raise RuntimeError("legacy negative control did not fail with exit 3")
    print("[PASS] legacy replacement fails with exit 3 while original pipe remains open")
    # Keep the old connection alive and idle, modelling a timed-out decoder
    # whose process has not yet released its pipe during cancellation.
    for attempt in range(3):
        host = start(True)
        request(connect(host, True), attempt + 1)
        if any(previous.poll() is not None for previous in hosts if previous is not conflicting):
            raise RuntimeError("an original host exited unexpectedly")
    print("[PASS] replacement hosts return valid responses from distinct PIDs while old pipes remain open")
finally:
    for pipe in pipes:
        kernel.CloseHandle(pipe)
    for host in hosts:
        if host.poll() is None:
            host.kill()
        host.wait(timeout=5)
