"""Opt-in real-host checks of structured discovery and argument boundaries."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import template_catalog

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
              " ".join(map(str, arguments[:1])) + " exits " + str(expected))
        return result

    try:
        api = call("inspect-sdk")["api"]
        (output / "api.json").write_text(json.dumps(api, ensure_ascii=False, indent=2), encoding="utf-8")
        check("structuredCapabilityDetails" in api["developerFeatures"], "host advertises structured discovery")
        details = {entry["id"]: entry for entry in api["capabilityDetails"]}
        check(set(details) == set(api["capabilities"]), "every capability has semantic details")
        check(details["view.setMode"]["persistsPreferences"] and
              {identifier for identifier, entry in details.items() if entry["modifiesFiles"]} ==
              {"files.copySelectionToOtherPane", "files.moveSelectionToOtherPane"},
              "file mutation is explicitly limited to the two controlled transfers")
        if "declarativeTemplates" in api["developerFeatures"]:
            check(api["templates"] == list(template_catalog.names()), "host and author CLI share the template catalog")
            for index, name in enumerate(api["templates"]):
                created = call("init", output / ("template-" + name), "--id", "live.template." + str(index),
                               "--name", "Template check", "--template", name)
                call("test", created["manifest"])
                actual = json.loads(Path(created["manifest"]).read_text(encoding="utf-8"))
                expected = template_catalog.load(name)
                expected["id"], expected["name"] = "live.template." + str(index), "Template check"
                check(actual == expected, name + " preserves the shared template except explicit author identity")
        path = output / "candidate.json"
        for identifier, detail in details.items():
            command = {"id": "command", "title": "Contract check", "capability": identifier}
            draft = {"apiVersion": 1, "id": "live.contract", "name": "Contract check", "commands": [command]}
            if detail["argsRequired"]:
                path.write_text(json.dumps(draft), encoding="utf-8")
                call("validate", path, expected=2)
                properties = detail["argsSchema"]["properties"]
                check(len(properties) == 1 and detail["argsSchema"]["required"] == list(properties),
                      identifier + " publishes its required argument")
                name, argument = next(iter(properties.items()))
                command["args"] = {name: argument["enum"][0]}
                path.write_text(json.dumps(draft), encoding="utf-8")
                call("test", path)
            else:
                check(detail["argsSchema"] is None, identifier + " forbids arguments")
                path.write_text(json.dumps(draft), encoding="utf-8")
                call("test", path)
                command["args"] = {}
                path.write_text(json.dumps(draft), encoding="utf-8")
                call("validate", path, expected=2)
        check(not list(profile.iterdir()), "discovery and validation leave preferences untouched")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; no commands executed.")


if __name__ == "__main__":
    main()
