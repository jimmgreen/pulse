#!/usr/bin/env python3
"""Pack Pulse release files into a self-extracting setup.

Layout of the produced executable:

    [bootstrapper exe][xz payload][0-7 zero bytes][trailer]([signature])

The image is padded to a multiple of 8 bytes; Authenticode signing then
appends its certificate table after the trailer.

trailer (56 bytes, little-endian):
    char[8]  magic        "PULSEPK1"
    uint64   payload_off  offset of the xz stream from file start
    uint64   payload_size size of the xz stream
    byte[32] payload_sha  SHA-256 of the xz stream

The xz stream decompresses to:
    char[4]  "PPKM"
    uint32   manifest_len
    byte[]   manifest (UTF-8 JSON: {"version":..,"files":[{"path","size","sha256"}]})
    byte[]   file contents, concatenated in manifest order

Only the Python standard library is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import struct
import sys
import time
import tempfile
from pathlib import Path

MAGIC = b"PULSEPK1"
TRAILER = struct.Struct("<8sQQ32s")

# Similar executables sit next to each other so the solid LZMA2 window can
# share their common static-CRT and library code (measured: ~1.1 MB saved).
DEFAULT_LAYOUT = [
    ("{build}/pulse.exe", "pulse.exe"),
    ("{build}/Pulse.AI.exe", "Pulse.AI.exe"),
    ("{build}/Pulse.Content.exe", "Pulse.Content.exe"),
    ("{build}/Pulse.Document.exe", "Pulse.Document.exe"),
    ("{build}/Pulse.Index.exe", "Pulse.Index.exe"),
    ("{build}/Pulse.Preview.exe", "Pulse.Preview.exe"),
    ("{build}/pulse_elevated.exe", "pulse_elevated.exe"),
    ("{build}/pulse_hotkey_broker.exe", "pulse_hotkey_broker.exe"),
    ("{build}/pulse_shell.exe", "pulse_shell.exe"),
    ("{build}/pulse_start_pin.dll", "pulse_start_pin.dll"),
    ("{build}/pulse_integration.exe", "pulse_integration.exe"),
    ("{build}/PulseDialogAdminAuto.exe", "PulseDialogAdminAuto.exe"),
    ("{build}/lumatext.dll", "lumatext.dll"),
    ("{build}/pdfium.dll", "pdfium.dll"),
    ("{build}/licenses/PDFium/*", "licenses/PDFium/"),
    ("{build}/licenses/LumaText/*", "licenses/LumaText/"),
    ("third_party/ib-pinyin-cpp/LICENSE.txt", "licenses/ib-pinyin/LICENSE.txt"),
    ("third_party/md4c/LICENSE.md", "licenses/md4c/LICENSE.md"),
]


def collect(root: Path, build: Path) -> list[tuple[Path, str]]:
    items: list[tuple[Path, str]] = []
    for pattern, dest in DEFAULT_LAYOUT:
        src = pattern.format(build=build.as_posix())
        src_path = Path(src) if Path(src).is_absolute() else root / src
        if src.endswith("/*"):
            folder = src_path.parent
            if not folder.is_dir():
                raise SystemExit(f"missing directory: {folder}")
            for f in sorted(p for p in folder.rglob("*") if p.is_file()):
                items.append((f, dest + f.relative_to(folder).as_posix()))
        else:
            if not src_path.is_file():
                raise SystemExit(f"missing file: {src_path}")
            items.append((src_path, dest))
    return items


# Channel -> minimum Windows build ([Setup] MinVersion in the iss: 10.0 / 6.3).
CHANNELS = {"windows": 10240, "win81": 9600}


def payload_dictionary_size(raw_size: int) -> int:
    # History beyond the entire input cannot improve compression. Keep the
    # existing 64 MiB ceiling for larger releases, but avoid reserving it in
    # the installer's streaming decoder for smaller payloads.
    return max(4096, min(64 << 20, 1 << (max(raw_size, 1) - 1).bit_length()))


def build_payload(items, version: str, channel: str = "windows") -> tuple[bytes, dict]:
    files, blobs = [], []
    for src, dest in items:
        data = src.read_bytes()
        files.append({"path": dest, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        blobs.append(data)
    manifest = {"format": 1, "version": version, "channel": channel,
                "min_windows_build": CHANNELS[channel], "files": files}
    mbytes = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    raw = b"PPKM" + struct.pack("<I", len(mbytes)) + mbytes + b"".join(blobs)
    filters = [
        {"id": lzma.FILTER_X86},
        {"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME,
         "dict_size": payload_dictionary_size(len(raw)), "nice_len": 273},
    ]
    xz = lzma.compress(raw, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=filters)
    return xz, {"raw": len(raw), "xz": len(xz), "files": len(files)}



def _dialog_tools():
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pack_dialog_experiment"))
    import pack as dialog_pack
    return dialog_pack


def verify_dialog_payload(directory: Path) -> dict:
    """Verify the legacy main pointer and an optional architecture-pure x86 peer."""
    dialog_pack = _dialog_tools()
    directory = Path(directory)
    primary = dialog_pack.verify_installed(directory)
    try:
        (directory / "dialog-x86-current.json").lstat()
    except FileNotFoundError:
        return primary
    main = dialog_pack.verify(directory / primary["directory"])
    if main["architecture"] != "x64":
        raise dialog_pack.PackageError("x86 companion requires an x64 primary bundle")
    pointer_path = dialog_pack.no_links(directory / "dialog-x86-current.json")
    if pointer_path.stat().st_size > 1024:
        raise dialog_pack.PackageError("x86 dialog pointer bound")
    pointer = json.loads(pointer_path.read_text(encoding="utf-8"), object_pairs_hook=dialog_pack.unique_object)
    if not isinstance(pointer, dict) or set(pointer) != {"schema", "directory", "manifest_sha256"} or type(pointer["schema"]) is not int or pointer["schema"] != 1:
        raise dialog_pack.PackageError("x86 dialog pointer schema")
    digest = pointer["manifest_sha256"]
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise dialog_pack.PackageError("x86 dialog pointer hash")
    if pointer["directory"] != "DialogBridge/" + digest:
        raise dialog_pack.PackageError("x86 dialog pointer directory/hash mismatch")
    bundle = dialog_pack.no_links(directory / pointer["directory"])
    if dialog_pack.digest(dialog_pack.no_links(bundle / "manifest.json")) != digest:
        raise dialog_pack.PackageError("x86 dialog pointer manifest hash mismatch")
    manifest = dialog_pack.verify(bundle)
    if manifest['version'] != main['version']:
        raise dialog_pack.PackageError("x86 companion version differs from primary bundle")
    names = {entry["path"].lower() for entry in manifest["files"]}
    thin = manifest['schema'] == 3
    required = dialog_pack.thin_roots() if thin else dialog_pack.roots("x86", automatic=True)
    if manifest["schema"] not in (1, 3) or manifest["architecture"] != "x86" or not all(
            name.lower() in names for name in required):
        raise dialog_pack.PackageError("complete architecture-matched x86 dialog bundle required")
    if thin and manifest['host_bundle']['manifest_sha256'] != primary['manifest_sha256']:
        raise dialog_pack.PackageError('thin companion does not bind current primary host')
    return primary


def package_installed_dual(root: Path, build: Path, output: Path, version: str,
                         items, dialog_build_x86: Path | None = None) -> dict:
    dialog_pack = _dialog_tools()
    root, build = Path(root).resolve(), Path(build).resolve()
    machine = dialog_pack.PE(build / "pulse.exe").machine
    arch = next((name for name, value in dialog_pack.MACHINES.items() if value == machine), None)
    if arch is None:
        raise dialog_pack.PackageError("unsupported Pulse architecture for dialog bundle")
    if dialog_build_x86 is not None:
        if arch != "x64":
            raise dialog_pack.PackageError("--dialog-build-x86 requires an x64 main build")
        dialog_build_x86 = Path(dialog_build_x86).resolve()
        # Fail before creating either destination if the companion is incomplete,
        # mixed-bitness, or silently borrowed from the main build.
        dialog_pack.plan(dialog_build_x86, version, "x86", root, thin=True, host_digest='0' * 64)
    primary = dialog_pack.package_installed(build, output, version, arch, root, items)
    if dialog_build_x86 is not None:
        pointer = dialog_pack.package_addressed(dialog_build_x86, Path(output) / "DialogBridge",
                                               version, "x86", root, thin=True,
                                               host_digest=primary['manifest_sha256'])
        with (Path(output) / "dialog-x86-current.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(pointer, stream, separators=(",", ":"))
            stream.write("\n")
    return verify_dialog_payload(output)

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--build", default="build", help="build directory (relative to root or absolute)")
    ap.add_argument("--dialog-build-x86", help="optional thin x86 dialog bridge build (relative to root or absolute)")
    ap.add_argument("--stub", required=True, help="bootstrapper executable")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--channel", choices=sorted(CHANNELS), default="windows",
                    help="release channel: windows (10/11) or win81")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build)
    stub = Path(args.stub)
    started = time.time()
    items = collect(root, build)
    # Ship one verified application/dialog runtime under its content hash;
    # the root contains only the stable launcher and version pointer.
    actual_build = build if build.is_absolute() else root / build
    dialog_x86 = Path(args.dialog_build_x86) if args.dialog_build_x86 else None
    if dialog_x86 is not None and not dialog_x86.is_absolute():
        dialog_x86 = root / dialog_x86
    with tempfile.TemporaryDirectory(prefix="pulse-dialog-") as staging:
        bundle = Path(staging) / "installed"
        package_installed_dual(root, actual_build, bundle, args.version, items, dialog_x86)
        items = [(f, f.relative_to(bundle).as_posix())
                 for f in sorted(bundle.rglob("*")) if f.is_file()]
        xz, stats = build_payload(items, args.version, args.channel)
    stub_bytes = stub.read_bytes()
    trailer = TRAILER.pack(MAGIC, len(stub_bytes), len(xz), hashlib.sha256(xz).digest())
    # Keep the image 8-byte aligned so Authenticode signing appends its
    # certificate table right after the trailer (setup_payload.cpp reads the
    # trailer in front of that table).
    pad = b"\0" * (-(len(stub_bytes) + len(xz) + TRAILER.size) % 8)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(stub_bytes + xz + pad + trailer)
    total = out.stat().st_size
    print(f"files={stats['files']} raw={stats['raw']} xz={stats['xz']} stub={len(stub_bytes)} "
          f"total={total} ({total / 1048576:.2f} MiB) in {time.time() - started:.1f}s -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
