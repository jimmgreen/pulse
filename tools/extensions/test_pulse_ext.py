import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import warnings
import zipfile
from unittest.mock import patch

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
                         ("rollback", "x"), ("uninstall", "x"), ("run",)]:
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
            self.assertIn(f"python {quote(script)} --pulse {quote(host)} validate {quote(manifest)}", readme)
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


if __name__ == "__main__":
    unittest.main()
