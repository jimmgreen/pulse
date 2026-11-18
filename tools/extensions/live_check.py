"""Opt-in real-host checks for extension author tooling; never uses mocks or installs packs.

Run after building pulse and pulse_extension_preview. Outputs are retained under a
new --output directory. Interactive dev reload has a separate in-app probe.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(exist_ok=False)
    profile = output / "isolated-profile"
    profile.mkdir()
    sentinel = profile / "keep.txt"
    sentinel.write_text("unchanged", encoding="utf-8")
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(profile)
    results = []

    def check(ok, name):
        results.append({"name": name, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + name, flush=True)
        if not ok:
            raise AssertionError(name)

    def call(*arguments, expected=0):
        process = subprocess.run([sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"),
                                  "--pulse", str(args.pulse.absolute()), "--json", *map(str, arguments)],
                                 env=env, capture_output=True, timeout=70,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        data = json.loads(process.stdout.decode("utf-8"))
        check(process.returncode == expected and data["ok"] == (expected == 0),
              str(arguments[0]) + " returns expected status " + str(expected))
        return data

    try:
        api = call("inspect-sdk")["api"]
        check(api["apiVersion"] == 1 and "view.setMode" in api["capabilities"], "host publishes required public capability")
        doctor = call("doctor")["doctor"]
        check(doctor["healthy"] and not doctor["pathsIncluded"] and not doctor["environmentIncluded"], "doctor is healthy and redacted")
        check(str(profile) not in json.dumps(doctor) and str(args.pulse.parent) not in json.dumps(doctor), "doctor omits local paths")
        for template in ("navigation", "navigation-panel", "selection-panel", "view-panel"):
            destination = output / (template + " \u589e\u5f3a\u5305")
            created = call("init", destination, "--id", "live." + template, "--name", "Live " + template,
                           "--template", template)
            manifest = Path(created["manifest"])
            tested = call("test", manifest)["test"]
            check(tested["valid"] and tested["contractCompatible"] and tested["commandsExecuted"] == 0 and
                  tested["functionalExecutionTested"] is False, template + " contract test distinguishes execution")
            archive_path = output / (template + ".pulse-ext")
            packed = call("pack", manifest, "--output", archive_path)["archive"]
            check(packed["verified"] and not packed["installed"] and not packed["hostInstallSupported"], template + " packaging reports actual scope")
            verified = call("verify", archive_path)["verification"]
            check(verified["integrityVerified"] and verified["hostValidated"] and not verified["installed"],
                  template + " archive verification uses the real parser")
            with zipfile.ZipFile(archive_path) as archive:
                check(set(archive.namelist()) == {"manifest.json", "package.json"}, template + " archive has only allowed entries")
                payload = archive.read("manifest.json")
                metadata = json.loads(archive.read("package.json"))
                check(payload == manifest.read_bytes() and metadata["manifestSha256"] == hashlib.sha256(payload).hexdigest(), template + " packaged bytes match validated input")
            original = archive_path.read_bytes()
            rejected = call("pack", manifest, "--output", archive_path, expected=2)
            check(rejected["error"]["code"] == "destination_exists" and archive_path.read_bytes() == original,
                  template + " pack preserves existing output")
        source = output / "view-panel \u589e\u5f3a\u5305" / "manifest.json"
        preview = call("preview", source, "--output", output / "screenshots")["preview"]
        check(preview["mockData"] and preview["commandsExecuted"] == 0, "native preview declares mock data")
        sizes = []
        for filename in preview["images"]:
            png = Path(filename).read_bytes()
            check(png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 1000, Path(filename).name + " is a nonempty PNG")
            sizes.append(struct.unpack(">II", png[16:24]))
        check(len(sizes) == 6 and sizes[0][0] < sizes[1][0] < sizes[2][0] and sizes[:3] == sizes[3:], "native preview covers both themes and three scales")
        invalid = output / "invalid.json"
        invalid.write_text("{", encoding="utf-8")
        call("test", invalid, expected=2)
        call("pack", invalid, "--output", output / "invalid.pulse-ext", expected=2)
        check(not (output / "invalid.pulse-ext").exists(), "invalid package is not published")
        tampered = output / "tampered.pulse-ext"
        with zipfile.ZipFile(output / "view-panel.pulse-ext") as original:
            with zipfile.ZipFile(tampered, "w") as archive:
                archive.writestr("manifest.json", original.read("manifest.json") + b" ")
                archive.writestr("package.json", original.read("package.json"))
        rejected = call("verify", tampered, expected=2)
        check(rejected["error"]["code"] == "invalid_archive", "modified manifest digest is rejected")
        malformed = output / "malformed.pulse-ext"
        # Matching metadata is insufficient: the real host must reject invalid semantics.
        bad_data = b'{"apiVersion":1,"id":"bad"}'
        with zipfile.ZipFile(malformed, "w") as archive:
            archive.writestr("manifest.json", bad_data)
            archive.writestr("package.json", json.dumps({"format": "pulse-extension-local", "formatVersion": 1,
                             "apiVersion": 1, "id": "bad", "manifestSha256": hashlib.sha256(bad_data).hexdigest()}))
        rejected = call("verify", malformed, expected=2)
        check(rejected["error"]["code"] == "invalid_manifest", "matching digest cannot bypass host semantics")
        check(list(profile.iterdir()) == [sentinel] and sentinel.read_text(encoding="utf-8") == "unchanged", "noninteractive tooling leaves isolated preference directory unchanged")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": results, "functionalCommandsExecuted": False},
                                                       ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(str(len(results)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
