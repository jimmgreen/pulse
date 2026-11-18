"""Opt-in real-host package-store lifecycle checks in a new disposable directory.

Does not load packages into a user window; native store UI probes cover execution.
"""
import argparse
import hashlib
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
    store = output / "installed packages"
    isolated = output / "isolated-data"
    isolated.mkdir()
    (isolated / "keep.txt").write_text("unchanged", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("PULSE_")}
    env["LOCALAPPDATA"] = str(isolated)
    checks = []

    def check(ok, message):
        checks.append({"name": message, "passed": bool(ok)})
        print("[" + ("PASS" if ok else "FAIL") + "] " + message, flush=True)
        if not ok:
            raise AssertionError(message)

    def call(*arguments, expected=0):
        completed = subprocess.run([sys.executable, str(ROOT / "tools/extensions/pulse_ext.py"),
                                    "--pulse", str(args.pulse.absolute()), "--json", *map(str, arguments)],
                                   env=env, capture_output=True, timeout=60,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        result = json.loads(completed.stdout.decode("utf-8"))
        check(completed.returncode == expected and result["ok"] == (expected == 0),
              str(arguments[0]) + " exits " + str(expected))
        return result

    def registry():
        return json.loads((store / "registry.json").read_text(encoding="utf-8"))

    def draft(identifier, name, mode, extra=False):
        commands = [{"id": "same-command", "title": "Switch view", "capability": "view.setMode", "args": {"mode": mode}}]
        if extra:
            commands.append({"id": "up", "title": "Go up", "capability": "navigation.up"})
        return {"apiVersion": 1, "id": identifier, "name": name, "commands": commands,
                "panel": {"title": name + " tools", "commands": ["same-command"],
                          "fields": [{"label": "View", "binding": "context.viewMode"}]}}

    def package(filename, manifest):
        source = output / (filename + ".json")
        source.write_text(json.dumps(manifest, ensure_ascii=True), encoding="utf-8")
        archive = output / (filename + ".pulse-ext")
        call("pack", source, "--output", archive)
        return archive, hashlib.sha256(source.read_bytes()).hexdigest()

    try:
        first, first_hash = package("alpha-v1", draft("alpha", "Alpha", "list"))
        second, second_hash = package("beta-v1", draft("beta", "Beta", "details"))
        upgraded, upgrade_hash = package("alpha-v2", draft("alpha", "Alpha updated", "tiles", extra=True))
        call("install", first, "--store", store)
        check(registry()["packages"][0]["enabled"] is False, "new install is disabled by default")
        check((store / "versions" / (first_hash + ".json")).is_file(), "validated manifest is content-addressed")
        before = (store / "registry.json").read_bytes()
        repeated = call("install", first, "--store", store)
        check(repeated["installation"]["changed"] is False and (store / "registry.json").read_bytes() == before,
              "same revision install is idempotent")
        call("install", second, "--store", store)
        check({entry["id"] for entry in registry()["packages"]} == {"alpha", "beta"}, "two packages may reuse a local command ID")
        call("enable", "alpha", "--store", store)
        call("enable", "beta", "--store", store)
        check(all(entry["enabled"] for entry in registry()["packages"]), "explicit enable persists both packages")
        before = (store / "registry.json").read_bytes()
        refused = call("install", upgraded, "--store", store, expected=2)
        check(refused["error"]["code"] == "capability_confirmation_required" and (store / "registry.json").read_bytes() == before,
              "capability expansion preserves active revision without explicit acceptance")
        call("install", upgraded, "--store", store, "--accept-capabilities")
        alpha = registry()["packages"][0]
        check(alpha["activeRevision"] == upgrade_hash and alpha["previousRevision"] == first_hash and alpha["enabled"],
              "accepted upgrade keeps enabled state and previous revision")
        call("rollback", "alpha", "--store", store)
        alpha = registry()["packages"][0]
        check(alpha["activeRevision"] == first_hash and alpha["name"] == "Alpha" and alpha["enabled"],
              "rollback restores manifest identity and previous bytes")
        call("disable", "alpha", "--store", store)
        state = registry()["packages"]
        check(not state[0]["enabled"] and state[1]["enabled"], "disabling alpha does not disable beta")
        listed = call("list", "--store", store)
        check(listed["registry"] == registry(), "list returns persisted registry")
        alpha_blob = store / "versions" / (first_hash + ".json")
        original = alpha_blob.read_bytes()
        alpha_blob.write_bytes(b"{}")
        before = (store / "registry.json").read_bytes()
        call("enable", "alpha", "--store", store, expected=2)
        check((store / "registry.json").read_bytes() == before, "bad content hash cannot be enabled")
        alpha_blob.write_bytes(original)
        # The rollback target is intact even when the active revision has become unreadable.
        alpha_blob.write_bytes(b"corrupt")
        call("rollback", "alpha", "--store", store)
        check(registry()["packages"][0]["activeRevision"] == upgrade_hash, "rollback recovers an intact previous revision from a damaged active blob")
        alpha_blob.write_bytes(original)
        registry_path = store / "registry.json"
        good_registry = registry_path.read_bytes()
        invalid = registry()
        invalid["packages"].append(dict(invalid["packages"][0]))
        registry_path.write_text(json.dumps(invalid), encoding="utf-8")
        call("list", "--store", store, expected=2)
        check(json.loads(registry_path.read_text(encoding="utf-8")) == invalid, "invalid registry is rejected without destructive repair")
        registry_path.write_bytes(good_registry)
        sentinel = store / "unrelated.txt"
        sentinel.write_text("preserve", encoding="utf-8")
        call("uninstall", "beta", "--store", store)
        check([entry["id"] for entry in registry()["packages"]] == ["alpha"] and sentinel.read_text(encoding="utf-8") == "preserve",
              "uninstall removes only package registration")
        check((store / "versions" / (second_hash + ".json")).is_file(), "uninstall retains immutable versions without recursive deletion")
        call("uninstall", "alpha", "--store", store)
        check(registry()["packages"] == [], "all packages can be unregistered cleanly")
        check([p.name for p in isolated.iterdir()] == ["keep.txt"], "package tooling does not write user preferences")
    finally:
        (output / "checks.json").write_text(json.dumps({"checks": checks, "store": str(store)}, indent=2) + "\n", encoding="utf-8")
    print(str(len(checks)) + " checks passed; evidence: " + str(output))


if __name__ == "__main__":
    main()
