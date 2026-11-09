"""Exercise two independently compiled C++ plugins in one unchanged Pulse host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run_case(args, variant):
    out = args.output / variant
    out.mkdir()
    profile = out / "profile"
    fixture = profile / "fixture"
    fixture.mkdir(parents=True)
    contents = {"image alpha.jpg": b"native plugin image fixture\x00\xff", "Notes \u03b2.txt": "native plugin Unicode fixture: \u03b2\n".encode()}
    for name, value in contents.items():
        (fixture / name).write_bytes(value)
    dll = getattr(args, variant)
    capture = out / "capture"
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PULSE_")}
    env.update(PULSE_TEST_DATA_DIR=str(profile), PULSE_TEST_EXTENSION_CAPTURE=str(capture),
               PULSE_TEST_NATIVE_CASE=variant, LOCALAPPDATA=str(out / "local-app-data"))
    command = [str(args.pulse), "--test-instance", "--extension-manifest", str(args.manifest),
               "--extension-native", str(dll), str(fixture)]
    start = time.monotonic()
    with (out / "stdout.log").open("wb") as stdout, (out / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, env=env, stdout=stdout, stderr=stderr)
        timed_out = False
        try:
            code = process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.terminate()
            code = process.wait(timeout=15)
    report_file = capture / "report.txt"
    report = report_file.read_text(encoding="utf-8") if report_file.exists() else ""
    transform = str.maketrans("abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ") if variant == "upper" else str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
    expected = {name.translate(transform): hashlib.sha256(content).hexdigest() for name, content in contents.items()}
    after = {p.name: sha(p) for p in fixture.iterdir() if p.is_file()}
    numbers = set()
    valid = len(after) == 2
    for name, digest in after.items():
        match = re.fullmatch(r"demo-([12])-(.+)", name)
        valid &= bool(match) and expected.get(match.group(2) if match else "") == digest
        if match: numbers.add(match.group(1))
    valid &= numbers == {"1", "2"}
    images = list(capture.glob("*.png"))
    required = {"prepared-panel.png", "native-refusal.png", "native-confirm.png", "completed-panel.png", "renamed-host.png"}
    images_valid = {p.name for p in images} == required and all(p.stat().st_size > 4096 and p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" for p in images)
    result = dict(variant=variant, hostSha256=sha(args.pulse), pluginSha256=sha(dll), exitCode=code,
                  exitHex=f"0x{code & 0xffffffff:08X}", timedOut=timed_out, elapsedSeconds=round(time.monotonic()-start,3),
                  passCount=report.count("[PASS]"), failCount=report.count("[FAIL]"), namesAndHashes=valid,
                  screenshotsValid=images_valid, after=after)
    result["passed"] = code == 0 and not timed_out and "Result: PASS" in report and "[FAIL]" not in report and valid and images_valid
    (out / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulse", type=Path, required=True)
    parser.add_argument("--lower", type=Path, required=True)
    parser.add_argument("--upper", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=ROOT / "examples/native/rename-panel/manifest.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", choices=["both", "lower", "upper"], default="both")
    args = parser.parse_args()
    for field in ("pulse", "lower", "upper", "manifest", "output"):
        setattr(args, field, getattr(args, field).resolve())
    if args.output.exists(): parser.error("output must be a new isolated directory")
    args.output.mkdir(parents=True)
    results = []
    for variant in (["lower", "upper"] if args.variant == "both" else [args.variant]):
        result = run_case(args, variant)
        results.append(result)
        if not result["passed"]: break
    passed = all(r["passed"] for r in results) and (args.variant != "both" or len(results) == 2 and results[0]["hostSha256"] == results[1]["hostSha256"] and results[0]["after"] != results[1]["after"])
    (args.output / "report.json").write_text(json.dumps(dict(passed=passed, results=results), ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    raise SystemExit(0 if passed else 1)
