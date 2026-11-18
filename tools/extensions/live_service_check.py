"""Opt-in isolated regression checks for shared host capability routing."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build-extensions/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(exist_ok=False, parents=True)
    results = []
    try:
        for scenario in ("workspace", "grouped"):
            profile = output / scenario
            profile.mkdir()
            for folder in ("files", "pane-b", "background"):
                (profile / folder).mkdir()
                (profile / folder / "example.txt").write_text("isolated routing fixture\n", encoding="utf-8")
            if scenario == "workspace":
                manifest = json.loads((ROOT / "examples/extensions/workspace-layout/manifest.json").read_text(encoding="utf-8"))
                manifest["panel"]["placement"] = "sidebar"
                manifest["panel"]["fields"] = [{"label": "Folder", "binding": "context.directory"}]
            else:
                manifest = {"apiVersion": 1, "id": "test.service.grouped", "name": "Routing fixture",
                    "commands": [{"id": mode, "title": mode, "capability": "view.setMode", "args": {"mode": mode}}
                                 for mode in ("details", "list", "tiles")],
                    "panel": {"title": "Grouped views", "commands": ["details", "list", "tiles"],
                              "groups": [{"title": "Browse", "commands": ["tiles", "details"]},
                                         {"title": "Simple", "commands": ["list"]}],
                              "primaryCommand": "details", "titleTone": "accent"}}
            path = profile / "manifest.json"
            path.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
            env = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
            env.update(LOCALAPPDATA=str(profile / "local-app-data"), PULSE_TEST_DATA_DIR=str(profile),
                       PULSE_TEST_EXTENSION_CAPTURE=str(profile / "capture"))
            with (profile / "process.log").open("wb") as log:
                process = subprocess.Popen([str(args.pulse.absolute()), "--test-instance", "--extension-manifest",
                                           str(path), str(profile / "files")], env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    code = process.wait(timeout=120)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=20)
                    raise
            report = (profile / "capture/report.txt").read_text(encoding="utf-8")
            passed, failed = report.count("[PASS]"), report.count("[FAIL]")
            results.append({"scenario": scenario, "exitCode": code, "passed": passed, "failed": failed})
            print(scenario + ": " + str(passed) + " passed, " + str(failed) + " failed; exit " + str(code), flush=True)
            if code or failed or not passed:
                raise AssertionError(scenario + " native routing probe failed")
    finally:
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
