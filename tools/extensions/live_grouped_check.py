"""Opt-in real-host grouped panel authoring and static preview checks."""
import argparse
import copy
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
    output.mkdir(exist_ok=False)
    profile = output / "isolated-data"
    profile.mkdir()
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(profile)
    checks = []

    def check(ok, name):
        checks.append({"name": name, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + name, flush=True)
        if not ok:
            raise AssertionError(name)

    def call(*arguments, expected=0):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"), "--pulse",
             str(args.pulse.absolute()), "--json", *map(str, arguments)],
            env=env, capture_output=True, timeout=70,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        result = json.loads(completed.stdout.decode("utf-8"))
        check(completed.returncode == expected and result["ok"] == (expected == 0),
              str(arguments[0]) + " exits " + str(expected))
        return result

    try:
        api = call("inspect-sdk")["api"]
        check("groupedPanels" in api["developerFeatures"] and api["limits"]["panelGroups"] == 4 and
              api["panelTitleTones"] == ["default", "accent"], "real host discovers bounded semantic UI")
        created = call("init", output / "package", "--id", "live.grouped", "--name", "Grouped tools",
                       "--template", "grouped-panel")
        path = Path(created["manifest"])
        draft = json.loads(path.read_text(encoding="utf-8"))
        panel = draft["panel"]
        grouped = [command for group in panel["groups"] for command in group["commands"]]
        check(len(grouped) == len(set(grouped)) and set(grouped) == set(panel["commands"]),
              "template groups exactly partition panel commands")
        check(panel["primaryCommand"] in panel["commands"] and panel["titleTone"] == "accent",
              "template uses host-owned primary and title semantics")
        tested = call("test", path)["test"]
        check(not tested["functionalExecutionTested"] and tested["commandsExecuted"] == 0,
              "authoring check makes no execution claim")
        preview = call("preview", path, "--output", output / "preview")["preview"]
        check(preview["mockData"] and preview["commandsExecuted"] == 0 and len(preview["images"]) == 6,
              "native static previews cover both themes and three scales")
        for image in preview["images"]:
            data = Path(image).read_bytes()
            check(data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) > 1000, Path(image).name + " contains PNG content")
        archive = output / "grouped.pulse-ext"
        call("pack", path, "--output", archive)
        call("verify", archive)
        mutations = [
            ("duplicate", lambda p: p["groups"][0]["commands"].append(p["groups"][0]["commands"][0])),
            ("missing", lambda p: p["groups"][0]["commands"].pop()),
            ("unknown-primary", lambda p: p.update(primaryCommand="missing")),
            ("raw-color", lambda p: p.update(titleTone="#ff0000")),
        ]
        for name, mutate in mutations:
            invalid = copy.deepcopy(draft)
            mutate(invalid["panel"])
            candidate = output / (name + ".json")
            candidate.write_text(json.dumps(invalid), encoding="utf-8")
            call("validate", candidate, expected=2)
        check(not list(profile.iterdir()), "authoring does not alter user preferences")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; UI interaction requires the separate application probe.")


if __name__ == "__main__":
    main()
