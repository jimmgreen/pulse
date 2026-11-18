"""Check actual Windows binary string resources without executing the binary.

Accepts EXE/DLL paths and --installer paths (checks every packaged picker DLL).
The default checks the picker places menu regression IDs in all three locales.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import lzma
from pathlib import Path
import struct
import sys
import tempfile


LANGUAGES = {"zh-CN": 0x0804, "en-US": 0x0409, "zh-TW": 0x0404}
DEFAULT_NAMES = {"PickerHidePlace", "PickerRestorePlaces", "PickerRestoreAllPlaces",
                 "PickerPlacesUpdateFailed", "Open", "CopyPath", "Unstar"}


def check_binary(path: Path, entries: list[tuple[str, int]], label: str = "") -> bool:
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    api.LoadLibraryExW.restype = wintypes.HMODULE
    api.FindResourceExW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD]
    api.FindResourceExW.restype = wintypes.HRSRC
    api.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    api.LoadResource.restype = wintypes.HGLOBAL
    api.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    api.SizeofResource.restype = wintypes.DWORD
    api.LockResource.argtypes = [wintypes.HGLOBAL]
    api.LockResource.restype = ctypes.c_void_p
    api.FreeLibrary.argtypes = [wintypes.HMODULE]
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p,
                                      ctypes.c_void_p, wintypes.WORD, ctypes.c_ssize_t)
    api.EnumResourceLanguagesW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p,
                                          callback_type, ctypes.c_ssize_t]
    module = api.LoadLibraryExW(str(path.resolve()), None, 0x02 | 0x20)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    okay = True
    try:
        for name, string_id in entries:
            block, index = string_id // 16 + 1, string_id % 16
            languages = set()

            @callback_type
            def collect(_module, _type, _name, language, _param):
                languages.add(language)
                return True

            api.EnumResourceLanguagesW(module, 6, block, collect, 0)
            for locale, language in LANGUAGES.items():
                text = ""
                resource = api.FindResourceExW(module, 6, block, language) if language in languages else None
                if resource:
                    loaded = api.LoadResource(module, resource)
                    address = api.LockResource(loaded)
                    size = api.SizeofResource(module, resource)
                    if address:
                        data = ctypes.string_at(address, size)
                        offset = 0
                        for slot in range(index + 1):
                            if offset + 2 > len(data):
                                break
                            length = struct.unpack_from("<H", data, offset)[0]
                            offset += 2
                            if offset + 2 * length > len(data):
                                break
                            if slot == index:
                                text = data[offset:offset + 2 * length].decode("utf-16-le")
                            offset += 2 * length
                passed = bool(text.strip())
                okay = okay and passed
                print(f"[{'PASS' if passed else 'FAIL'}] {label or path} {locale} {name} ({string_id}): {text!r}")
    finally:
        api.FreeLibrary(module)
    return okay


def check_installer(path: Path, entries: list[tuple[str, int]]) -> bool:
    binary = path.read_bytes()
    trailer = struct.Struct("<8sQQ32s")
    position = binary.rfind(b"PULSEPK1")
    if position < 0 or position + trailer.size > len(binary):
        raise ValueError("missing installer payload trailer")
    _, start, size, digest = trailer.unpack_from(binary, position)
    payload = binary[start:start + size]
    if start + size > position or hashlib.sha256(payload).digest() != digest:
        raise ValueError("invalid installer payload hash or bounds")
    raw = lzma.decompress(payload)
    if raw[:4] != b"PPKM":
        raise ValueError("invalid installer payload header")
    manifest_size = struct.unpack_from("<I", raw, 4)[0]
    manifest = json.loads(raw[8:8 + manifest_size])
    offset = 8 + manifest_size
    found, okay = 0, True
    with tempfile.TemporaryDirectory(prefix="pulse-resource-check-") as directory:
        for entry in manifest["files"]:
            length = entry["size"]
            if length < 0 or offset + length > len(raw):
                raise ValueError("invalid packaged file bounds")
            if entry["path"].replace("\\", "/").rsplit("/", 1)[-1].lower() == "pulsepickeruiproof.dll":
                content = raw[offset:offset + length]
                if hashlib.sha256(content).hexdigest() != entry["sha256"]:
                    raise ValueError("invalid packaged picker hash")
                # Never use an untrusted manifest path as a filesystem path.
                extracted = Path(directory) / f"picker-{found}.dll"
                extracted.write_bytes(content)
                found += 1
                okay = check_binary(extracted, entries, f"{path}!{entry['path']}") and okay
            offset += length
    if not found:
        raise ValueError("installer contains no PulsePickerUiProof.dll")
    return okay


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binaries", nargs="*", type=Path)
    parser.add_argument("--installer", action="append", default=[], type=Path)
    parser.add_argument("--all", action="store_true", help="check every catalog ID")
    args = parser.parse_args()
    if not args.binaries and not args.installer:
        parser.error("provide a binary or --installer")
    entries = []
    for line in (Path(__file__).parent / "catalog.tsv").read_text(encoding="utf-8-sig").splitlines():
        columns = line.split("\t")
        if len(columns) >= 6 and columns[2].isdigit() and (args.all or columns[0] in DEFAULT_NAMES):
            entries.append((columns[0], int(columns[2])))
    if not args.all and {name for name, _ in entries} != DEFAULT_NAMES:
        raise ValueError("required regression strings missing from catalog")
    okay = True
    for binary in args.binaries:
        okay = check_binary(binary, entries) and okay
    for installer in args.installer:
        okay = check_installer(installer, entries) and okay
    return 0 if okay else 1


if __name__ == "__main__":
    raise SystemExit(main())
