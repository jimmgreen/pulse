import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile
from unittest.mock import patch

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import pulse_ext as cli


def reply(value, code=0):
    return subprocess.CompletedProcess([], code, json.dumps(value).encode("utf-8"), b"")


def valid_reply(command, **kwargs):
    return reply({"valid": True, "error": "", "code": "", "path": command[-2]})


class CliTests(unittest.TestCase):
    def test_native_doctor_does_not_require_preview_host(self):
        with patch.object(cli.subprocess, "run", side_effect=self.development_host):
            code, result = self.run_cli("doctor")
        self.assertEqual(code, 0)
        self.assertTrue(result["doctor"]["healthy"])
        self.assertNotIn("previewHost", result["doctor"]["checks"])

    def test_removed_preview_is_rejected_before_launch(self):
        with patch.object(cli.subprocess, "run") as run:
            self.assertEqual(self.run_cli("preview", "sample.json", "--output", "preview.png")[0], 2)
            run.assert_not_called()

    def test_legacy_allow_scripts_is_rejected_before_launch(self):
        with patch.object(cli.subprocess, "run") as run, patch.object(cli.subprocess, "Popen") as popen:
            for args in (("dev", "sample.json", "--profile", "new"), ("run", "store", "--profile", "new")):
                with self.subTest(command=args[0]):
                    code, result = self.run_cli(*args, "--allow-scripts")
                    self.assertEqual(code, 2)
                    self.assertEqual(result["error"]["code"], "usage")
                    self.assertIn("--allow-scripts", result["error"]["message"])
            run.assert_not_called()
            popen.assert_not_called()











    def test_template_discovery_preserves_catalog(self):
        api = self.structured_api()
        api["developerFeatures"].append("declarativeTemplates")
        api["templates"] = list(cli.TEMPLATES)
        with patch.object(cli.subprocess, "run", return_value=reply(api)):
            self.assertEqual(cli.host_call(Path("pulse"), "--extension-api")["templates"], list(cli.TEMPLATES))

    def test_template_discovery_rejects_inconsistent_catalog(self):
        for names in (None, [], ["x", "x"], ["../outside"], ["x" * 65], [1], ["x%d" % n for n in range(33)]):
            api = self.structured_api()
            api["developerFeatures"].append("declarativeTemplates")
            if names is not None:
                api["templates"] = names
            with self.subTest(names=names), patch.object(cli.subprocess, "run", return_value=reply(api)), self.assertRaises(cli.Failure):
                cli.host_call(Path("pulse"), "--extension-api")

    def structured_api(self):
        api = json.loads(self.development_host(["pulse", "--extension-api"]).stdout)
        api["developerFeatures"] = ["structuredCapabilityDetails"]
        api["capabilityDetails"] = [
            {"id": identifier, "summary": "Refresh the focused pane", "effect": "view",
             "target": "focusedPane", "modifiesFiles": False, "persistsPreferences": False,
             "supportsCancellation": False, "argsRequired": False, "argsSchema": None}
            for identifier in api["capabilities"]]
        api["bindingDetails"] = [{"id": identifier, "summary": "Display current context", "displayOnly": True}
                                 for identifier in api["bindings"]]
        return api

    def test_structured_discovery_preserves_details(self):
        api = self.structured_api()
        with patch.object(cli.subprocess, "run", return_value=reply(api)):
            self.assertEqual(cli.host_call(Path("pulse"), "--extension-api"), api)

    def test_structured_feature_requires_complete_matching_details(self):
        for change in (lambda api: api.pop("capabilityDetails"),
                       lambda api: api["bindingDetails"].clear(),
                       lambda api: api["capabilityDetails"].append(api["capabilityDetails"][0]),
                       lambda api: api["capabilityDetails"][0].update(id="unknown")):
            api = self.structured_api()
            change(api)
            with self.subTest(api=api), patch.object(cli.subprocess, "run", return_value=reply(api)):
                with self.assertRaises(cli.Failure) as caught:
                    cli.host_call(Path("pulse"), "--extension-api")
                self.assertEqual(caught.exception.error["code"], "host_protocol_error")

    def test_structured_discovery_rejects_ambiguous_types(self):
        for field, value in (("modifiesFiles", 0), ("persistsPreferences", "false"),
                             ("argsRequired", 1), ("argsSchema", []), ("summary", " "),
                             ("argsRequired", True)):
            api = self.structured_api()
            api["capabilityDetails"][0][field] = value
            with self.subTest(field=field, value=value), patch.object(cli.subprocess, "run", return_value=reply(api)):
                with self.assertRaises(cli.Failure):
                    cli.host_call(Path("pulse"), "--extension-api")

    def test_structured_discovery_allows_required_object_arguments(self):
        api = self.structured_api()
        schema = {"type": "object", "properties": {"mode": {"enum": ["details"]}}, "required": ["mode"]}
        api["capabilityDetails"][0].update(argsRequired=True, argsSchema=copy.deepcopy(schema))
        with patch.object(cli.subprocess, "run", return_value=reply(api)):
            result = cli.host_call(Path("pulse"), "--extension-api")
        self.assertEqual(result["capabilityDetails"][0]["argsSchema"], schema)

    def test_run_rejects_uninitialized_store_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile"
            with patch.object(cli.subprocess, "Popen") as launch:
                code, result = self.run_cli("run", "--store", directory, "--profile", str(profile))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "invalid_store")
            self.assertFalse(profile.exists())
            launch.assert_not_called()


    @staticmethod
    def development_host(command, **kwargs):
        if command[1] == "--extension-validate-json":
            return valid_reply(command)
        return reply({"apiVersion": 1, "capabilities": ["view.refresh"], "bindings": ["context.directory"],
                      "developmentNotes": ["preview"], "manifestSchema": {"type": "object"},
                      "limits": {key: 1 for key in ("manifestBytes", "commands", "idBytes", "labelBytes", "panelCommands", "panelDescriptionBytes", "panelFields")}})

    @staticmethod
    def write_manifest(directory):
        path = Path(directory) / "manifest.json"
        path.write_text(json.dumps({"apiVersion": 1, "id": "x", "name": "X",
                                  "commands": [{"id": "r", "title": "Refresh", "capability": "view.refresh"}],
                                  "panel": {"title": "Tools", "commands": ["r"],
                                            "fields": [{"label": "Directory", "binding": "context.directory"}]}}), encoding="utf-8")
        return path

    def test_package_reports_contract_only(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            with patch.object(cli.subprocess, "run", side_effect=self.development_host) as run:
                code, result = self.run_cli("test", str(path))
            self.assertEqual(code, 0, result)
            self.assertFalse(result["test"]["functionalExecutionTested"])
            self.assertEqual(result["test"]["commandsExecuted"], 0)
            self.assertEqual(run.call_count, 3)

    def test_install_manage_and_run_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            archive = Path(directory) / "package.zip"
            store = Path(directory) / "store"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                self.assertEqual(self.run_cli("pack", str(path), "--output", str(archive))[0], 0)
                code, result = self.run_cli("install", str(archive), "--store", str(store))
            self.assertEqual(code, 0, result)
            self.assertFalse(result["installation"]["package"]["enabled"])
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                self.assertEqual(self.run_cli("enable", "x", "--store", str(store))[0], 0)
            code, result = self.run_cli("list", "--store", str(store))
            self.assertEqual(code, 0)
            self.assertTrue(result["registry"]["packages"][0]["enabled"])
            def host(command, **kwargs):
                response = self.development_host(command)
                if command[1] == "--extension-api":
                    data = json.loads(response.stdout)
                    data["developerFeatures"] = ["nativePackages"]
                    return reply(data)
                return response
            profile = Path(directory) / "profile"
            with patch.object(cli.subprocess, "run", side_effect=host), patch.object(cli.subprocess, "Popen") as popen:
                popen.return_value.pid = 123
                code, result = self.run_cli("run", "--store", str(store), "--profile", str(profile))
            self.assertEqual(code, 0, result)
            self.assertFalse(result["run"]["loadVerified"])
            self.assertIn("--extension-store", popen.call_args.args[0])
            self.assertIn("--test-instance", popen.call_args.args[0])
            self.assertEqual(popen.call_args.kwargs["env"]["PULSE_TEST_DATA_DIR"], str(profile))
            self.assertEqual(result["run"]["processLog"], str(profile / "extension-store-process.log"))
            self.assertTrue(popen.call_args.kwargs["stdout"].closed)
            self.assertEqual(self.run_cli("disable", "x", "--store", str(store))[0], 0)
            self.assertEqual(self.run_cli("uninstall", "x", "--store", str(store))[0], 0)

    def test_store_commands_require_localappdata_or_explicit_store(self):
        with patch.dict(cli.os.environ, {}, clear=True):
            for args in [("install", "x.zip"), ("list",), ("enable", "x"), ("disable", "x"),
                         ("rollback", "x"), ("uninstall", "x"), ("gc",), ("run",)]:
                code, result = self.run_cli(*args)
                self.assertEqual(code, 2)
                self.assertEqual(result["error"]["code"], "store_not_found")

    def test_run_launch_failure_keeps_log(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Path(directory) / "store"
            store.mkdir()
            (store / "registry.json").write_text(json.dumps({"formatVersion": 1, "packages": []}), encoding="utf-8")
            profile = Path(directory) / "profile"
            def host(command, **kwargs):
                data = json.loads(self.development_host(command).stdout)
                data["developerFeatures"] = ["nativePackages"]
                return reply(data)
            with patch.object(cli.subprocess, "run", side_effect=host), patch.object(cli.subprocess, "Popen", side_effect=OSError("launch failure")):
                code, result = self.run_cli("run", "--store", str(store), "--profile", str(profile))
            self.assertEqual(code, 3)
            self.assertEqual(result["error"]["code"], "store_launch_failed")
            self.assertTrue(Path(result["error"]["processLog"]).exists())


    def test_pack_exact_members_hash_and_preserve_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            output = Path(directory) / "package.zip"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                code, result = self.run_cli("pack", str(path), "--output", str(output))
                self.assertEqual(code, 0, result)
                self.assertTrue(result["archive"]["verified"])
                self.assertFalse(result["archive"]["hostInstallSupported"])
                self.assertEqual(cli.verify_package(output)["id"], "x")
                original = output.read_bytes()
                code, _ = self.run_cli("pack", str(path), "--output", str(output))
                self.assertEqual(code, 2)
                self.assertEqual(output.read_bytes(), original)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(sorted(archive.namelist()), ["manifest.json", "package.json"])
                self.assertEqual(archive.read("manifest.json"), path.read_bytes())

    def test_pack_race_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            output = Path(directory) / "package.zip"
            def race(source, destination):
                output.write_bytes(b"other writer")
                raise FileExistsError()
            with patch.object(cli.subprocess, "run", side_effect=valid_reply), patch.object(cli.os, "link", side_effect=race):
                code, result = self.run_cli("pack", str(path), "--output", str(output))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "destination_exists")
            self.assertEqual(output.read_bytes(), b"other writer")
            self.assertEqual({item.name for item in Path(directory).iterdir()}, {"manifest.json", "package.zip"})

    def test_pack_verify_rejects_script_and_tampering(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            output = Path(directory) / "package.zip"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                self.assertEqual(self.run_cli("pack", str(path), "--output", str(output))[0], 0)
            with zipfile.ZipFile(output, "a") as archive:
                archive.writestr("run.cmd", "echo bad")
            with self.assertRaises(ValueError):
                cli.verify_package(output)
            with zipfile.ZipFile(output, "w") as archive:
                archive.writestr("manifest.json", path.read_bytes())
                archive.writestr("package.json", '{}')
            with self.assertRaises(ValueError):
                cli.verify_package(output)

    def test_verify_cli_revalidates_without_extracting(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            output = Path(directory) / "package.zip"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                self.assertEqual(self.run_cli("pack", str(path), "--output", str(output))[0], 0)
            with patch.object(cli.subprocess, "run", side_effect=valid_reply) as run:
                code, result = self.run_cli("verify", str(output))
            self.assertEqual(code, 0, result)
            self.assertTrue(result["verification"]["hostValidated"])
            self.assertEqual(result["verification"]["signature"], {"signed": False, "trusted": False})
            review = result["verification"]["authorizationReview"]
            self.assertEqual(review["commandCapabilities"], sorted({item["capability"] for item in json.loads(path.read_bytes())["commands"]}))
            self.assertEqual(review["resourcePermissions"], [])
            self.assertFalse(review["grantsAuthority"])

            self.assertEqual(run.call_count, 1)
            self.assertEqual({item.name for item in Path(directory).iterdir()}, {"manifest.json", "package.zip"})

    def test_verify_rejects_oversize_and_duplicate_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "package.zip"
            for member in ("manifest.json", "package.json"):
                with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr("manifest.json", b" " * 65537 if member == "manifest.json" else b"{}")
                    archive.writestr("package.json", b" " * 4097 if member == "package.json" else b"{}")
                with patch.object(cli.subprocess, "run") as run:
                    code, result = self.run_cli("verify", str(output))
                self.assertEqual(code, 2)
                self.assertEqual(result["error"]["code"], "invalid_archive")
                run.assert_not_called()
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(output, "w") as archive:
                    archive.writestr("manifest.json", "{}")
                    archive.writestr("manifest.json", "{}")
                    archive.writestr("package.json", "{}")
            with patch.object(cli.subprocess, "run") as run:
                self.assertEqual(self.run_cli("verify", str(output))[0], 2)
                run.assert_not_called()

    def test_snapshot_growth_and_strict_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_manifest(directory)
            def growing_host(command, **kwargs):
                path.write_bytes(b" " * 65537)
                return valid_reply(command)
            with patch.object(cli.subprocess, "run", side_effect=growing_host):
                code, result = self.run_cli("test", str(path))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "too_large")
        for data in (b'{"x":NaN}', b'\xef\xbb\xbf{}', b'{"x":1,"x":2}'):
            with self.assertRaises(ValueError):
                cli.decode_json(data)

    def test_verify_bounds_archive_before_zip_directory_parse(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "oversized.zip"
            output.write_bytes(b"x" * (256 * 1024 + 1))
            with patch.object(cli.zipfile, "ZipFile") as parser, patch.object(cli.subprocess, "run") as host:
                code, result = self.run_cli("verify", str(output))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "invalid_archive")
            parser.assert_not_called()
            host.assert_not_called()

    def test_verify_hash_and_semantics_are_separate_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "invalid.zip"
            data = b'{"apiVersion":1,"id":"x","name":"X","commands":[{"id":"x","title":"X","capability":"not.public"}]}'
            metadata = {"format": "pulse-extension-local", "formatVersion": 1, "apiVersion": 1,
                        "id": "x", "manifestSha256": "bad"}
            def write_archive():
                with zipfile.ZipFile(output, "w") as archive:
                    archive.writestr("manifest.json", data)
                    archive.writestr("package.json", json.dumps(metadata))
            write_archive()
            with patch.object(cli.subprocess, "run") as host:
                code, result = self.run_cli("verify", str(output))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "invalid_archive")
            host.assert_not_called()
            metadata["manifestSha256"] = cli.hashlib.sha256(data).hexdigest()
            write_archive()
            def reject(command, **kwargs):
                self.assertEqual(Path(command[-2]).read_bytes(), data)
                return reply({"valid": False, "error": "Unsupported capability", "code": "invalid_manifest", "path": command[-2]}, 2)
            with patch.object(cli.subprocess, "run", side_effect=reject) as host:
                code, result = self.run_cli("verify", str(output))
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "invalid_manifest")
            host.assert_called_once()


    def test_doctor_error_redacts_host_message(self):
        with patch.object(cli.subprocess, "run", side_effect=PermissionError("C:/Users/Private/secret.exe")):
            code, result = self.run_cli("doctor")
        self.assertEqual(code, 3)
        self.assertNotIn("Private", json.dumps(result))
        self.assertEqual(result["error"]["checks"]["host"]["code"], "host_launch_failed")

    @classmethod
    def dev_host(cls, command, **kwargs):
        response = cls.development_host(command)
        if command[1] == "--extension-api":
            data = json.loads(response.stdout)
            data["developerFeatures"] = ["isolatedDevReload"]
            return reply(data)
        return response

    def test_dev_isolated_wait_and_no_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "isolated"
            with patch.object(cli.subprocess, "run", side_effect=self.dev_host), \
                    patch.object(cli.subprocess, "Popen") as popen, \
                    patch.dict(cli.os.environ, {"PULSE_TEST_EXTENSION_CAPTURE": "bad", "PULSE_OTHER": "bad"}):
                popen.return_value.wait.return_value = 0
                code, result = self.run_cli("dev", "sample.json", "--profile", str(profile))
                self.assertEqual(code, 0, result)
                argv = popen.call_args.args[0]
                self.assertEqual(argv[1:3], ["--test-instance", "--extension-dev"])
                self.assertNotIn("--extension-check", argv)
                self.assertNotIn("--extension-script-dev", argv)
                environment = popen.call_args.kwargs["env"]
                self.assertEqual(environment["PULSE_TEST_DATA_DIR"], str(profile))
                self.assertNotIn("PULSE_TEST_EXTENSION_CAPTURE", environment)
                self.assertNotIn("PULSE_OTHER", environment)
                popen.return_value.wait.assert_called_once_with()
            self.assertFalse(result["dev"]["reloadVerified"])
            self.assertEqual(result["dev"]["diagnostic"], str(profile / "extension-dev-status.json"))

    def test_dev_rejects_unsupported_host_before_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "new"
            with patch.object(cli.subprocess, "run", side_effect=self.development_host), patch.object(cli.subprocess, "Popen") as popen:
                code, result = self.run_cli("dev", "sample.json", "--profile", str(profile))
            self.assertEqual(code, 3)
            self.assertEqual(result["error"]["code"], "dev_unsupported")
            self.assertFalse(profile.exists())
            popen.assert_not_called()

    def test_dev_rejects_existing_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(cli.subprocess, "run", side_effect=self.dev_host), patch.object(cli.subprocess, "Popen") as popen:
                code, result = self.run_cli("dev", "sample.json", "--profile", directory)
            self.assertEqual(code, 2)
            self.assertEqual(result["error"]["code"], "destination_exists")
            popen.assert_not_called()

    def test_dev_failure_retains_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "new"
            with patch.object(cli.subprocess, "run", side_effect=self.dev_host), patch.object(cli.subprocess, "Popen") as popen:
                popen.return_value.wait.return_value = 2
                code, result = self.run_cli("dev", "sample.json", "--profile", str(profile))
            self.assertEqual(code, 3)
            self.assertEqual(result["error"]["code"], "dev_process_failed")
            self.assertTrue((profile / "extension-dev-process.log").exists())

    def test_dev_interrupt_stops_only_owned_process(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(cli.subprocess, "run", side_effect=self.dev_host), patch.object(cli.subprocess, "Popen") as popen:
                popen.return_value.wait.side_effect = [KeyboardInterrupt(), subprocess.TimeoutExpired("dev", 5), 1]
                code, result = self.run_cli("dev", "sample.json", "--profile", str(Path(directory) / "new"))
                popen.return_value.terminate.assert_called_once_with()
                popen.return_value.kill.assert_called_once_with()
            self.assertEqual(code, 0)
            self.assertTrue(result["dev"]["stoppedByUser"])

    def run_cli(self, *args):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = cli.main(["--json", *args])
        return code, json.loads(output.getvalue())

    def test_usage_machine_error(self):
        code, value = self.run_cli("init", "somewhere")
        self.assertEqual(code, 2)
        self.assertEqual(value["error"]["code"], "usage")









    def test_help_is_single_json(self):
        code, value = self.run_cli("--help")
        self.assertEqual(code, 0)
        self.assertTrue(value["ok"])

    def test_validate_no_shell_and_absolute_path(self):
        with patch.object(cli.subprocess, "run", side_effect=valid_reply) as run:
            code, value = self.run_cli("--pulse", "host with spaces.exe", "validate", "manifest.json")
        self.assertEqual(code, 0)
        self.assertTrue(value["validation"]["valid"])
        command = run.call_args.args[0]
        self.assertEqual(command[0], "host with spaces.exe")
        self.assertTrue(Path(command[-2]).is_absolute())
        self.assertEqual(command[-1], "--extension-check")
        self.assertFalse(run.call_args.kwargs["shell"])
        self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_host_rejection_preserved(self):
        def reject(command, **kwargs):
            return reply({"valid": False, "error": "unknown capability", "code": "invalid_manifest", "path": command[-2]}, 2)
        with patch.object(cli.subprocess, "run", side_effect=reject):
            code, value = self.run_cli("validate", "bad.json")
        self.assertEqual(code, 2)
        self.assertEqual(value["error"]["host"]["path"], str(Path("bad.json").absolute()))
        self.assertEqual(value["error"]["code"], "invalid_manifest")

    def test_host_failures(self):
        for error, expected in [(FileNotFoundError(), "host_not_found"),
                                (PermissionError(), "host_launch_failed"),
                                (subprocess.TimeoutExpired("pulse", 30), "host_timeout")]:
            with self.subTest(expected=expected), patch.object(cli.subprocess, "run", side_effect=error):
                code, value = self.run_cli("inspect-sdk")
                self.assertEqual(code, 3)
                self.assertEqual(value["error"]["code"], expected)

    def test_bad_output_not_success(self):
        for output in [b"", b"not json", b"\xff", b"{}{}", b"[]", b'{"valid":true,"valid":false}', b'{"x":NaN}']:
            with self.subTest(output=output), patch.object(cli.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, output, b"")):
                code, value = self.run_cli("validate", "x.json")
                self.assertEqual(code, 3)
                self.assertEqual(value["error"]["code"], "host_protocol_error")

    def test_validation_shape_and_exit_mismatch(self):
        absolute = str(Path("x.json").absolute())
        cases = [({"valid": True, "error": "", "code": "", "path": absolute}, 2),
                 ({"valid": False, "error": "bad", "code": "invalid_manifest", "path": absolute}, 0),
                 ({"valid": 1, "error": "", "code": "", "path": absolute}, 0),
                 ({"valid": True, "error": "", "code": "", "path": "wrong"}, 0)]
        for value, status in cases:
            with self.subTest(value=value), patch.object(cli.subprocess, "run", return_value=reply(value, status)):
                code, _ = self.run_cli("validate", "x.json")
                self.assertEqual(code, 3)

    def test_inspect_checks_contract_shape(self):
        api = {"apiVersion": 1, "capabilities": ["view.refresh"], "bindings": ["context.directory"],
               "developmentNotes": ["preview"], "manifestSchema": {"type": "object"},
               "limits": {key: 1 for key in ("manifestBytes", "commands", "idBytes", "labelBytes", "panelCommands", "panelDescriptionBytes", "panelFields")}}
        with patch.object(cli.subprocess, "run", return_value=reply(api)):
            code, value = self.run_cli("inspect-sdk")
            self.assertEqual(code, 0)
            self.assertEqual(value["api"], api)
        api["apiVersion"] = True
        with patch.object(cli.subprocess, "run", return_value=reply(api)):
            self.assertEqual(self.run_cli("inspect-sdk")[0], 3)

    def test_init_templates_validate_before_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            for template in cli.TEMPLATES:
                destination = Path(directory) / template
                seen = []
                def validate(command, **kwargs):
                    self.assertFalse(destination.exists())
                    path = Path(command[-2])
                    seen.append(path)
                    text = json.loads(path.read_text(encoding="utf-8"))
                    expected = json.loads((cli.ROOT / "examples/native" / template / "manifest.json").read_text(encoding="utf-8"))
                    expected.update(id="test.package", name="测试名称")
                    self.assertEqual(text, expected)
                    return valid_reply(command)
                with patch.object(cli.subprocess, "run", side_effect=validate):
                    code, value = self.run_cli("init", str(destination), "--id", "test.package", "--name", "测试名称", "--template", template)
                self.assertEqual(code, 0, value)
                self.assertEqual({path.name for path in destination.iterdir()}, {"manifest.json", "README.md"})
                self.assertFalse(seen[0].exists())









    def test_init_rejects_existing_empty_directory(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(cli.subprocess, "run") as run:
            code, value = self.run_cli("init", directory, "--id", "x", "--name", "X")
            self.assertEqual(code, 2)
            self.assertEqual(value["error"]["code"], "destination_exists")
            run.assert_not_called()

    def test_init_readme_quotes_validate_and_load_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "package's folder"
            host = Path(directory) / "host's folder" / "pulse.exe"
            script = Path(directory) / "tool's folder" / "pulse_ext.py"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply), patch.object(cli, "__file__", str(script)):
                code, value = self.run_cli("--pulse", str(host), "init", str(destination), "--id", "x", "--name", "X")
            self.assertEqual(code, 0, value)
            quote = lambda path: "'" + str(path).replace("'", "''") + "'"
            readme = (destination / "README.md").read_text(encoding="utf-8")
            manifest = destination / "manifest.json"
            self.assertIn(f"python {quote(script.resolve())} --pulse {quote(host)} validate {quote(manifest)}", readme)
            self.assertIn(f"& {quote(host)} --extension-manifest {quote(manifest)}", readme)
            self.assertIn("native-manifest", readme)

    def test_init_host_failure_creates_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new"
            with patch.object(cli.subprocess, "run", side_effect=FileNotFoundError()):
                self.assertEqual(self.run_cli("init", str(destination), "--id", "x", "--name", "X")[0], 3)
            self.assertFalse(destination.exists())

    def test_init_invalid_parameters_use_host_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new"
            seen = []
            def reject(command, **kwargs):
                path = Path(command[-2])
                seen.append(path)
                self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["id"], "bad/id")
                return reply({"valid": False, "error": "Invalid manifest id", "code": "invalid_manifest", "path": str(path)}, 2)
            with patch.object(cli.subprocess, "run", side_effect=reject):
                code, value = self.run_cli("init", str(destination), "--id", "bad/id", "--name", "X")
            self.assertEqual(code, 2)
            self.assertEqual(value["error"]["code"], "invalid_manifest")
            self.assertFalse(destination.exists())
            self.assertFalse(seen[0].exists())

    def test_init_write_failure_cleans_only_own_outputs(self):
        original_open = Path.open
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new"
            def failing_open(path, *args, **kwargs):
                if path == destination / "README.md" and args and args[0] == "x":
                    raise PermissionError("simulated write failure")
                return original_open(path, *args, **kwargs)
            with patch.object(cli.subprocess, "run", side_effect=valid_reply), patch.object(Path, "open", failing_open):
                code, value = self.run_cli("init", str(destination), "--id", "x", "--name", "X")
            self.assertEqual(code, 2)
            self.assertEqual(value["error"]["code"], "write_failed")
            self.assertFalse(destination.exists())

    def test_init_mkdir_race_preserves_other_files(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new"
            def racing_host(command, **kwargs):
                destination.mkdir()
                (destination / "owned.txt").write_text("other")
                return valid_reply(command)
            with patch.object(cli.subprocess, "run", side_effect=racing_host):
                self.assertEqual(self.run_cli("init", str(destination), "--id", "x", "--name", "X")[0], 2)
            self.assertEqual((destination / "owned.txt").read_text(), "other")

    def test_init_file_race_preserves_competing_file(self):
        original_open = Path.open
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new"
            def racing_open(path, *args, **kwargs):
                if path == destination / "README.md" and args and args[0] == "x":
                    with original_open(path, "w") as stream:
                        stream.write("other writer")
                return original_open(path, *args, **kwargs)
            with patch.object(cli.subprocess, "run", side_effect=valid_reply), patch.object(Path, "open", racing_open):
                self.assertEqual(self.run_cli("init", str(destination), "--id", "x", "--name", "X")[0], 2)
            self.assertEqual((destination / "README.md").read_text(), "other writer")
            self.assertFalse((destination / "manifest.json").exists())

    def test_new_creates_full_packable_project_and_rejects_basic_or_reserved_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            dest = Path(directory) / "acme-renamer"
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                code, result = self.run_cli(
                    "--pulse", "pulse.exe", "new", str(dest),
                    "--id", "com.acme.renamer", "--name", "Acme Renamer", "--template", "rename-panel",
                )
            self.assertEqual(code, 0, result)
            self.assertEqual(result["project"]["packageId"], "com.acme.renamer")
            self.assertEqual(result["project"]["libraryStem"], "com_acme_renamer")
            for member in ("CMakeLists.txt", "manifest.json", "plugin.cpp", "README.md"):
                self.assertTrue((dest / member).is_file())
            manifest = json.loads((dest / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["id"], "com.acme.renamer")
            self.assertEqual(manifest["contributes"], {"commands": []})

            code_basic, err_basic = self.run_cli(
                "new", str(Path(directory) / "basic-attempt"),
                "--id", "com.acme.basic", "--name", "Basic", "--template", "basic",
            )
            self.assertEqual(code_basic, 2)
            self.assertEqual(err_basic["error"]["code"], "template_not_packable")

            code_res, err_res = self.run_cli(
                "new", str(Path(directory) / "reserved-attempt"),
                "--id", "sample.native-rename", "--name", "Reserved", "--template", "rename-panel",
            )
            self.assertEqual(code_res, 2)
            self.assertEqual(err_res["error"]["code"], "reserved_id")

    def test_doctor_reports_sdk_preview_toolchain_and_manifest_without_leaking_paths(self):
        def host_with_preview(command, **kwargs):
            if command[1] == "--extension-validate-json":
                return valid_reply(command)
            api = json.loads(self.development_host(command).stdout)
            api["nativeSdk"] = {"catalog": {"sdkPreview": 3, "abiVersion": 1}}
            api["manifestSchema"] = {
                "type": "object",
                "properties": {"resources": {"items": {"enum": ["context.read", "settings.own"]}}},
            }
            return reply(api)

        fake_tc = {
            "ready": True,
            "targetArchitecture": "windows-x64",
            "cmake": {"available": True, "compatible": True, "version": "3.30.0"},
            "ninja": {"available": True, "compatible": True, "version": "1.12.1"},
            "msvc": {"available": True, "compatible": True, "architecture": "x64"},
            "windowsSdk": {"available": True, "compatible": True},
            "missingReasons": [],
        }
        with tempfile.TemporaryDirectory() as secret_dir:
            manifest_path = Path(secret_dir) / "manifest.json"
            manifest_path.write_text(json.dumps({
                "apiVersion": 1,
                "id": "com.acme.test",
                "name": "Test",
                "resources": ["context.read"],
                "contributes": {"commands": [{"id": "c1", "title": "Run", "kinds": ["file"], "minSelection": 1, "maxSelection": 1}]},
            }), encoding="utf-8")
            with patch.object(cli.subprocess, "run", side_effect=host_with_preview), \
                    patch.object(cli, "inspect_toolchain", return_value=fake_tc):
                code, result = self.run_cli("doctor", "--toolchain", "--manifest", str(manifest_path))
            self.assertEqual(code, 0, result)
            doc = result["doctor"]
            self.assertTrue(doc["healthy"])
            self.assertFalse(doc["pathsIncluded"])
            self.assertFalse(doc["environmentIncluded"])
            self.assertEqual(doc["checks"]["host"]["sdkPreview"], 3)
            self.assertEqual(doc["checks"]["host"]["architecture"], "windows-x64")
            self.assertTrue(doc["checks"]["toolchain"]["ready"])
            self.assertTrue(doc["checks"]["manifest"]["valid"])
            self.assertEqual(doc["checks"]["manifest"]["contributedCommandCount"], 1)
            self.assertNotIn(Path(secret_dir).name, json.dumps(result))

            arm_manifest = Path(secret_dir) / "arm.json"
            arm_manifest.write_text(json.dumps({
                "apiVersion": 1,
                "id": "com.acme.arm",
                "name": "Arm",
                "native": {"version": 1, "architecture": "windows-arm64", "library": "x.dll", "sha256": "0" * 64},
            }), encoding="utf-8")
            with patch.object(cli.subprocess, "run", side_effect=host_with_preview):
                code_arm, res_arm = self.run_cli("doctor", "--manifest", str(arm_manifest))
            self.assertEqual(code_arm, 0)
            self.assertFalse(res_arm["doctor"]["healthy"])
            self.assertEqual(res_arm["doctor"]["checks"]["manifest"]["code"], "architecture_mismatch")

    @unittest.skipUnless(cli.os.name == "nt", "Windows cooperative window close required")
    def test_stop_host_cooperatively_closes_isolated_window_and_refuses_unrelated_pid(self):
        import sys
        child_code = (
            "import ctypes, sys\n"
            "from ctypes import wintypes\n"
            "u = ctypes.WinDLL('user32', use_last_error=True)\n"
            "u.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,"
            " ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]\n"
            "u.CreateWindowExW.restype = wintypes.HWND\n"
            "hwnd = u.CreateWindowExW(0, 'STATIC', 'PulseIsolatedHostTest', 0, 0, 0, 0, 0, None, None, None, None)\n"
            "assert hwnd\n"
            "print('READY', flush=True)\n"
            "msg = wintypes.MSG()\n"
            "while u.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:\n"
            "    if msg.message == 0x0010:\n"
            "        break\n"
            "    u.DispatchMessageW(ctypes.byref(msg))\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "profile"
            profile.mkdir()
            child = subprocess.Popen(
                [sys.executable, "-c", child_code, "--test-instance"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            try:
                ready = child.stdout.readline().strip()
                self.assertEqual(ready, "READY")
                record = {
                    "formatVersion": 1,
                    "processId": child.pid,
                    "hostPath": sys.executable,
                    "store": str(Path(directory) / "store"),
                    "profile": str(profile),
                    "processStartFileTime": cli._process_start_filetime(child.pid),
                }
                (profile / "extension-host-instance.json").write_text(json.dumps(record), encoding="utf-8")

                code_mismatch, res_mismatch = self.run_cli(
                    "--pulse", sys.executable, "stop-host", "--pid", str(child.pid + 99999), "--profile", str(profile),
                )
                self.assertEqual(code_mismatch, 2)
                self.assertEqual(res_mismatch["error"]["code"], "pid_mismatch")

                code, result = self.run_cli(
                    "--pulse", sys.executable, "stop-host", "--pid", str(child.pid), "--profile", str(profile), "--timeout", "5",
                )
                self.assertEqual(code, 0, result)
                self.assertTrue(result["stop"]["stopped"])
                self.assertFalse(result["stop"]["forceKilled"])
                self.assertEqual(child.wait(timeout=3), 0)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=3)
                if child.stdout:
                    child.stdout.close()
                if child.stderr:
                    child.stderr.close()

            unrelated = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                with patch.object(cli, "_query_process_command_line", return_value=f'"{sys.executable}" normal_session.py'):
                    code_unrel, res_unrel = self.run_cli("--pulse", sys.executable, "stop-host", "--pid", str(unrelated.pid))
                self.assertEqual(code_unrel, 2)
                self.assertEqual(res_unrel["error"]["code"], "not_test_host")
                self.assertIsNone(unrelated.poll())
            finally:
                unrelated.kill()
                unrelated.wait(timeout=3)

    def test_cli_gc_and_rollback_accept_capabilities(self):
        with tempfile.TemporaryDirectory() as directory:
            store_dir = Path(directory) / "store"
            m1 = Path(directory) / "m1.json"
            m2 = Path(directory) / "m2.json"
            m1.write_text(json.dumps({"apiVersion": 1, "id": "pkg", "name": "Pkg v1", "resources": ["context.read", "settings.own"]}), encoding="utf-8")
            m2.write_text(json.dumps({"apiVersion": 1, "id": "pkg", "name": "Pkg v2", "resources": ["context.read"]}), encoding="utf-8")
            with patch.object(cli.subprocess, "run", side_effect=valid_reply):
                self.assertEqual(self.run_cli("install", str(m1), "--store", str(store_dir), "--accept-capabilities")[0], 0)
                self.assertEqual(self.run_cli("install", str(m2), "--store", str(store_dir), "--accept-capabilities")[0], 0)
                code_rb_fail, res_rb_fail = self.run_cli("rollback", "pkg", "--store", str(store_dir))
                self.assertEqual(code_rb_fail, 2)
                self.assertEqual(res_rb_fail["error"]["code"], "capability_confirmation_required")
                code_rb_ok, _ = self.run_cli("rollback", "pkg", "--store", str(store_dir), "--accept-capabilities")
                self.assertEqual(code_rb_ok, 0)

            orphan = store_dir / "versions" / ("0" * 64 + ".json")
            orphan.write_text("{}", encoding="utf-8")
            code_gc, res_gc = self.run_cli("gc", "--store", str(store_dir))
            self.assertEqual(code_gc, 0, res_gc)
            self.assertFalse(orphan.exists())
            self.assertGreaterEqual(len(res_gc["gc"]["removed"]), 1)

    @staticmethod
    def _synthetic_plugin_pe64_dll(include_exports=True):
        import struct
        buf = bytearray(0x400)
        buf[0:2] = b"MZ"
        struct.pack_into("<I", buf, 0x3C, 0x80)
        buf[0x80:0x84] = b"PE\x00\x00"
        struct.pack_into("<HHIIIHH", buf, 0x84, 0x8664, 1, 0, 0, 0, 0xF0, 0x2022)
        struct.pack_into("<H", buf, 0x98, 0x20B)
        struct.pack_into("<I", buf, 0x98 + 108, 16)
        if include_exports:
            struct.pack_into("<II", buf, 0x98 + 112, 0x1000, 0x100)
        buf[0x188:0x190] = b".rdata\x00\x00"
        struct.pack_into("<IIII", buf, 0x188 + 8, 0x200, 0x1000, 0x200, 0x200)
        if include_exports:
            # IMAGE_EXPORT_DIRECTORY at RVA 0x1000 (file 0x200)
            struct.pack_into("<IIIIIIIIII", buf, 0x200, 0, 0, 0, 0x1050, 1, 2, 2, 0x1028, 0x1030, 0x1038)
            struct.pack_into("<II", buf, 0x228, 0x1100, 0x1110)
            struct.pack_into("<II", buf, 0x230, 0x1060, 0x1080)
            struct.pack_into("<HH", buf, 0x238, 0, 1)
            buf[0x250:0x259] = b"test.dll\x00"
            buf[0x260:0x273] = b"PulsePluginEntryV1\x00"
            buf[0x280:0x296] = b"PulsePluginSdkPreview\x00"
        return bytes(buf)

    def test_offline_manifest_validation_enforces_exact_utf8_budgets_and_schema_rules(self):
        import offline_manifest
        valid_doc = {
            "apiVersion": 1,
            "id": "com.acme.offline-check",
            "name": "Offline Check",
            "resources": list(offline_manifest.KNOWN_RESOURCES),
            "contributes": {
                "commands": [{
                    "id": "run-check",
                    "title": {"zh": "检查", "en": "Run Check"},
                    "kinds": ["files"],
                    "minSelection": 1,
                    "maxSelection": 4,
                    "extensions": [".txt"],
                    "shortcut": {"key": "K", "ctrl": True, "shift": True},
                }],
            },
        }
        res_ok = offline_manifest.validate_manifest_bytes(json.dumps(valid_doc, ensure_ascii=False).encode("utf-8"))
        self.assertTrue(res_ok["valid"], res_ok)

        # UTF-8 BOM, oversized manifest (>65536), duplicate keys, id > 64, displayText > 128 UTF-8 bytes
        self.assertFalse(offline_manifest.validate_manifest_bytes(b"\xef\xbb\xbf{}")["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(b" " * 65537)["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(b'{"apiVersion":1,"id":"a.b","id":"a.c","name":"x"}')["valid"])

        exact_128_utf8 = "测" * 42 + "ab"  # 42 * 3 + 2 == 128 bytes
        over_129_utf8 = "测" * 43  # 43 * 3 == 129 bytes
        self.assertTrue(offline_manifest.validate_manifest_bytes(
            json.dumps({"apiVersion": 1, "id": "com.acme.ok", "name": exact_128_utf8}, ensure_ascii=False).encode("utf-8")
        )["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(
            json.dumps({"apiVersion": 1, "id": "com.acme.bad", "name": over_129_utf8}, ensure_ascii=False).encode("utf-8")
        )["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(
            json.dumps({"apiVersion": 1, "id": "a" * 65, "name": "Too Long ID"}).encode("utf-8")
        )["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(
            json.dumps({"apiVersion": 1, "id": "com.acme.legacy", "name": "Legacy", "runtime": "js"}).encode("utf-8")
        )["valid"])
        self.assertFalse(offline_manifest.validate_manifest_bytes(
            json.dumps({"apiVersion": 1, "id": "com.acme.res", "name": "Bad Res", "resources": ["unknown.cap"]}).encode("utf-8")
        )["valid"])

    def test_offline_native_manifest_pack_verify_install_and_doctor_without_host(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            proj = root / "offline-proj"
            with patch.object(cli, "host_call", side_effect=AssertionError("offline mode must never invoke host_call")):
                code_new, res_new = self.run_cli(
                    "--offline", "new", str(proj), "--id", "com.acme.offline", "--name", "Offline Plugin", "--template", "inspector",
                )
                self.assertEqual(code_new, 0, res_new)
                self.assertTrue(res_new["offlineValidated"])

                dll_path = root / "com_acme_offline.dll"
                dll_path.write_bytes(self._synthetic_plugin_pe64_dll(include_exports=True))
                staged_manifest = root / "staged-manifest.json"
                code_nm, res_nm = self.run_cli(
                    "--offline", "native-manifest", str(proj / "manifest.json"), "--library", str(dll_path), "--output", str(staged_manifest),
                )
                self.assertEqual(code_nm, 0, res_nm)
                self.assertTrue(res_nm["nativeManifest"]["offlineValidated"])

                pkg_path = root / "com.acme.offline.pulse-ext"
                code_pack, _ = self.run_cli("--offline", "pack", str(staged_manifest), "--output", str(pkg_path))
                self.assertEqual(code_pack, 0)

                code_ver, res_ver = self.run_cli("--offline", "verify", str(pkg_path))
                self.assertEqual(code_ver, 0, res_ver)
                self.assertTrue(res_ver["verification"]["offlineValidated"])
                self.assertTrue(res_ver["verification"]["nativeBinary"]["hasEntryV1"])
                self.assertTrue(res_ver["verification"]["nativeBinary"]["hasSdkPreview"])

                store_dir = root / "isolated-store"
                code_inst, _ = self.run_cli(
                    "--offline", "install", str(pkg_path), "--store", str(store_dir), "--allow-native", "--accept-capabilities",
                )
                self.assertEqual(code_inst, 0)
                code_en, _ = self.run_cli("--offline", "enable", "com.acme.offline", "--store", str(store_dir), "--allow-native")
                self.assertEqual(code_en, 0)

                code_doc, res_doc = self.run_cli("--offline", "doctor", "--manifest", str(staged_manifest))
                self.assertEqual(code_doc, 0, res_doc)
                self.assertTrue(res_doc["doctor"]["healthy"])
                self.assertTrue(res_doc["doctor"]["checks"]["host"]["offlineMode"])

    def test_build_and_test_subcommands_support_auto_msvc_and_headless_harness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            proj = root / "plugin-proj"
            proj.mkdir()
            (proj / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.25)\nproject(p LANGUAGES CXX)\n", encoding="utf-8")
            (proj / "manifest.json").write_text(json.dumps({
                "apiVersion": 1,
                "id": "com.acme.smoke",
                "name": "Smoke Plugin",
                "resources": ["context.read"],
                "contributes": {"commands": [{"id": "ping", "title": "Ping", "kinds": ["files"], "minSelection": 0, "maxSelection": 4}]},
            }), encoding="utf-8")

            fake_resolved = {
                "env": {"PATH": "C:\\FakeVS\\bin"},
                "auto_discovered": True,
                "vcvars_used": True,
                "cmake_executable": "C:\\FakeVS\\bin\\cmake.exe",
                "ninja_executable": "C:\\FakeVS\\bin\\ninja.exe",
                "cl_executable": "C:\\FakeVS\\bin\\cl.exe",
            }
            dll_bytes = self._synthetic_plugin_pe64_dll(include_exports=True)

            def fake_build_run(cmd, **kwargs):
                if "--build" in cmd:
                    bdir = Path(cmd[cmd.index("--build") + 1])
                    bdir.mkdir(parents=True, exist_ok=True)
                    (bdir / "com_acme_smoke.dll").write_bytes(dll_bytes)
                return subprocess.CompletedProcess(cmd, 0, b"ok", b"")

            with patch.object(cli.msvc_env, "resolve_build_environment", return_value=fake_resolved), \
                    patch.object(cli, "_ORIGINAL_SUBPROCESS_RUN", side_effect=fake_build_run):
                code_b, res_b = self.run_cli("--offline", "build", str(proj), "--config", "Release")
            self.assertEqual(code_b, 0, res_b)
            self.assertTrue(res_b["build"]["autoDiscoveredToolchain"])
            self.assertEqual(len(res_b["build"]["libraries"]), 1)
            self.assertTrue(res_b["build"]["binaryInspection"]["com_acme_smoke.dll"]["hasEntryV1"])

            fake_harness = root / "pulse_plugin_harness.exe"
            fake_harness.write_bytes(b"MZ")
            harness_payload = {
                "ok": True,
                "unloadClean": True,
                "registeredCommands": [{"id": "ping"}],
                "commandInvocations": [{"id": "ping", "invoked": True, "status": 0}],
                "openPanelsAtDeactivate": 0,
                "errors": [],
            }
            with patch.object(
                cli,
                "_ORIGINAL_SUBPROCESS_RUN",
                return_value=subprocess.CompletedProcess([], 0, json.dumps(harness_payload).encode("utf-8"), b""),
            ):
                code_t, res_t = self.run_cli(
                    "--offline", "test", str(proj), "--harness", str(fake_harness), "--smoke-commands", "--strict-resources",
                )
            self.assertEqual(code_t, 0, res_t)
            self.assertTrue(res_t["test"]["harnessExecuted"])
            self.assertTrue(res_t["test"]["unloadClean"])
            self.assertEqual(res_t["test"]["commandsExecuted"], 1)

            # Missing ABI exports must be rejected before harness launch
            bad_dll = root / "bad.dll"
            bad_dll.write_bytes(self._synthetic_plugin_pe64_dll(include_exports=False))
            code_bad, res_bad = self.run_cli("--offline", "test", str(proj), "--library", str(bad_dll))
            self.assertEqual(code_bad, 2)
            self.assertEqual(res_bad["error"]["code"], "abi_exports_missing")


if __name__ == "__main__":
    unittest.main()
