"""Opt-in isolated native confirmation and controlled transfer acceptance test."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build-extensions/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(exist_ok=False, parents=True)
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(output / "local-app-data")
    pulse = args.pulse.absolute()
    checks = []

    def check(ok, label):
        checks.append({"name": label, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + label, flush=True)
        if not ok:
            raise AssertionError(label)

    def cli(*arguments, expected=0):
        result = subprocess.run([sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"),
            "--pulse", str(pulse), "--json", *map(str, arguments)], env=env,
            capture_output=True, timeout=90, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        parsed = json.loads(result.stdout.decode("utf-8"))
        check(result.returncode == expected and parsed["ok"] == (expected == 0),
              str(arguments[0]) + " returns expected contract result")
        return parsed

    try:
        api = cli("inspect-sdk")["api"]
        check("controlledFileTransfers" in api["developerFeatures"] and api["limits"]["transferSources"] == 64 and "context.transferStatus" in api["bindings"],
              "real host advertises bounded controlled transfers and owned status display")
        created = cli("init", output / "package", "--id", "live.transfer", "--name", "Transfer test",
                      "--template", "transfer-panel")
        manifest = Path(created["manifest"])
        cli("test", manifest)
        original = json.loads(manifest.read_text(encoding="utf-8"))
        candidate = output / "invalid.json"
        for extra in ({}, {"userConfirmed": True}, {"path": str(output)}):
            changed = json.loads(json.dumps(original))
            changed["commands"][0]["args"] = extra
            candidate.write_text(json.dumps(changed), encoding="utf-8")
            cli("validate", candidate, expected=2)
        for folder in ("source", "target", "alternate"):
            (output / folder).mkdir()
        for name in ("copy", "move", "cancel", "disabled", "conflict", "changed"):
            (output / "source" / (name + ".txt")).write_bytes(("isolated source: " + name + "\n").encode())
        for index in range(100):
            (output / "source" / ("boundary-%03d.txt" % index)).write_bytes(b"selection boundary fixture\n")
        (output / "target" / "conflict.txt").write_bytes(b"existing destination must survive\n")
        env.update(PULSE_TEST_DATA_DIR=str(output), PULSE_TEST_EXTENSION_CAPTURE=str(output / "capture"),
                   PULSE_TEST_EXTENSION_TRANSFER="1")
        with (output / "process.log").open("wb") as log:
            process = subprocess.Popen([str(pulse), "--test-instance", "--extension-manifest", str(manifest),
                                        str(output / "source")], env=env, stdout=log, stderr=subprocess.STDOUT)
            (output / "process.json").write_text(json.dumps({"pid": process.pid}) + "\n", encoding="utf-8")
            print("Native probe PID " + str(process.pid), flush=True)
            try:
                code = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                process.terminate()  # Only this isolated process created above.
                process.wait(timeout=20)
                raise
        check(code == 0, "real application completes transfer UI probe")
        report = (output / "capture/report.txt").read_text(encoding="utf-8")
        check("[FAIL]" not in report and "failures=0" in report, "all real-window assertions pass")
        check((output / "target/copy.txt").read_bytes() == (output / "source/copy.txt").read_bytes(),
              "copied bytes independently match original")
        check((output / "target/move.txt").exists() and not (output / "source/move.txt").exists(),
              "move leaves only the committed destination")
        check((output / "target/conflict.txt").read_bytes() == b"existing destination must survive\n",
              "existing conflicting file remains unchanged")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
