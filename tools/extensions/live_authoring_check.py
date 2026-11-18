"""Opt-in real-host checks for the public toolbar/layout authoring contract."""
import argparse
import json
import os
from pathlib import Path
import struct
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
    (profile / "keep.txt").write_text("unchanged", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(profile)
    checks = []

    def check(ok, label):
        checks.append({"name": label, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + label, flush=True)
        if not ok:
            raise AssertionError(label)

    def call(*arguments, expected=0):
        result = subprocess.run([sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"), "--pulse",
                                 str(args.pulse.absolute()), "--json", *map(str, arguments)],
                                env=env, capture_output=True, timeout=70,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        value = json.loads(result.stdout.decode("utf-8"))
        check(result.returncode == expected and value["ok"] == (expected == 0), str(arguments[0]) + " exits " + str(expected))
        return value

    try:
        api = call("inspect-sdk")["api"]
        check(api["panelLayouts"] == ["auto", "stack", "grid"] and api["panelDensities"] == ["comfortable", "compact"] and
              api["limits"]["toolbarCommands"] == 8 and "toolbarContributions" in api["developerFeatures"],
              "real host discovers UI presets and toolbar limit")
        created = call("init", output / "workspace", "--id", "live.workspace", "--name", "Workspace UI", "--template", "workspace-panel")
        manifest = Path(created["manifest"])
        draft = json.loads(manifest.read_text(encoding="utf-8"))
        check(draft["panel"]["layout"] == "stack" and draft["panel"]["density"] == "compact" and
              draft["toolbar"]["commands"] == ["details", "extra-large"], "new template retains declared toolbar order and compact stack")
        tested = call("test", manifest)["test"]
        check("panelPresets" in tested["checks"] and "toolbarReferences" in tested["checks"] and not tested["functionalExecutionTested"],
              "package contract test checks discovered UI metadata without claiming execution")
        for layout, density in (("stack", "compact"), ("grid", "compact"), ("auto", "comfortable")):
            variant = output / (layout + "-" + density + ".json")
            draft["panel"].update(layout=layout, density=density)
            variant.write_text(json.dumps(draft, ensure_ascii=True), encoding="utf-8")
            rendered = call("preview", variant, "--output", output / (layout + "-" + density))["preview"]
            check(rendered["commandsExecuted"] == 0 and rendered["mockData"], layout + " preview is explicitly static")
            dimensions = []
            for path in rendered["images"]:
                png = Path(path).read_bytes()
                check(png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 1000, layout + " " + Path(path).name + " renders native content")
                dimensions.append(struct.unpack(">II", png[16:24]))
            check(len(dimensions) == 6 and dimensions[:3] == dimensions[3:] and dimensions[0][0] < dimensions[1][0] < dimensions[2][0],
                  layout + " preview covers three scales and both themes")
        archive = output / "workspace.pulse-ext"
        call("pack", manifest, "--output", archive)
        call("verify", archive)
        store = output / "store"
        installed = call("install", archive, "--store", store)["installation"]
        check(not installed["package"]["enabled"], "UI package installation remains disabled by default")
        call("enable", "live.workspace", "--store", store)
        draft["panel"]["layout"] = "unsupported"
        invalid = output / "invalid-layout.json"
        invalid.write_text(json.dumps(draft), encoding="utf-8")
        rejected = call("preview", invalid, "--output", output / "invalid-preview", expected=2)
        check(rejected["error"]["code"] == "invalid_manifest" and not (output / "invalid-preview").exists(),
              "invalid UI preset is rejected by real parser before preview output")
        check([p.name for p in profile.iterdir()] == ["keep.txt"], "authoring tools do not modify user preferences")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
