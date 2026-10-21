"""Verify shared UI sources, renderer selection and the resulting PE architecture."""
import argparse
import json
import pathlib
import re
import struct


SHARED_SOURCES = (
    "src/app/app_main.cpp",
    "src/app/app_commands.cpp",
    "src/app/settings_controller.cpp",
    "src/app/view_menu.cpp",
    "src/app/touch_input.cpp",
    "src/ui/ui_pane_list.cpp",
    "src/ui/ui_fluent_svg.cpp",
    "src/ui/view_layout.cpp",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--lumatext", required=True, choices=("ON", "OFF"))
    parser.add_argument("--machine", required=True, choices=("x64", "arm64"))
    args = parser.parse_args()
    commands = json.loads((args.build_dir / "compile_commands.json").read_text(encoding="utf-8-sig"))
    for source in SHARED_SOURCES:
        matching = []
        for entry in commands:
            filename = entry["file"].replace("\\", "/").lower()
            command = entry.get("command") or " ".join(entry.get("arguments", []))
            if filename.endswith("/" + source) and re.search(r"CMakeFiles[\\/]pulse\.dir[\\/]", command, re.I):
                matching.append(command)
        if len(matching) != 1:
            raise SystemExit(f"[FAIL] Expected one main-program compile entry for {source}, got {len(matching)}")
        has_lumatext = re.search(r"(?:/D|-D)PULSE_HAS_LUMATEXT=1\b", matching[0]) is not None
        if has_lumatext != (args.lumatext == "ON"):
            raise SystemExit(f"[FAIL] Unexpected renderer configuration for {source}")
        print(f"[PASS] Shared main-program source: {source}")

    image = (args.build_dir / "pulse.exe").read_bytes()
    if image[:2] != b"MZ":
        raise SystemExit("[FAIL] pulse.exe is not a PE image")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("[FAIL] Missing PE signature")
    machine = struct.unpack_from("<H", image, pe + 4)[0]
    expected = {"x64": 0x8664, "arm64": 0xAA64}[args.machine]
    if machine != expected:
        raise SystemExit(f"[FAIL] Unexpected PE machine: {machine:#x}, expected {expected:#x}")
    print(f"shared_touch_build: OK (machine={args.machine}, lumatext={args.lumatext})")


if __name__ == "__main__":
    main()
