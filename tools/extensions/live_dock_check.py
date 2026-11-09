"""Opt-in real-host authoring checks for sidebar packages (not UI execution)."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulse", type=Path, default=ROOT / "build/pulse.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.mkdir(exist_ok=False)
    profile = output / "isolated-data"
    profile.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(profile)
    checks = []

    def check(ok, label):
        checks.append({"name": label, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + label, flush=True)
        if not ok:
            raise AssertionError(label)

    def call(*arguments, expected=0):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"), "--pulse",
             str(args.pulse.absolute()), "--json", *map(str, arguments)],
            env=env, capture_output=True, timeout=70,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        value = json.loads(result.stdout.decode("utf-8"))
        check(result.returncode == expected and value["ok"] == (expected == 0),
              str(arguments[0]) + " exits " + str(expected))
        return value

    try:
        api = call("inspect-sdk")["api"]
        check(api.get("panelPlacements") == ["window", "sidebar"] and
              "sidebarPanels" in api.get("developerFeatures", []),
              "real host discovers sidebar placement")
        created = call("init", output / "dock", "--id", "live.dock", "--name",
                       "Dock authoring", "--template", "dock-panel")
        manifest = Path(created["manifest"])
        draft = json.loads(manifest.read_text(encoding="utf-8"))
        check(draft["panel"]["placement"] == "sidebar", "template requests sidebar")
        tested = call("test", manifest)["test"]
        check(tested["contractCompatible"] and "panelPlacement" in tested["checks"] and
              not tested["functionalExecutionTested"] and
              tested["commandsExecuted"] == 0, "authoring validation does not claim UI execution")
        archive = output / "dock.pulse-ext"
        call("pack", manifest, "--output", archive)
        call("verify", archive)
        store = output / "store"
        installed = call("install", archive, "--store", store)["installation"]
        check(not installed["package"]["enabled"], "sidebar package starts disabled")
        call("enable", "live.dock", "--store", store)
        for value in (None, 1, "left"):
            draft["panel"]["placement"] = value
            invalid = output / "invalid.json"
            invalid.write_text(json.dumps(draft), encoding="utf-8")
            call("validate", invalid, expected=2)
        draft["panel"].pop("placement")
        legacy = output / "default-window.json"
        legacy.write_text(json.dumps(draft), encoding="utf-8")
        call("test", legacy)
        draft["panel"]["placement"] = "window"
        floating = output / "explicit-window.json"
        floating.write_text(json.dumps(draft), encoding="utf-8")
        call("test", floating)
        check(not list(profile.iterdir()), "authoring leaves isolated preferences untouched")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
