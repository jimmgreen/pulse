import json
import hashlib
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
import zipfile
import shutil
from unittest.mock import patch
import generate_native_catalog
import bundle_native_sdk
import test_native_sdk


class NativeBundleTests(unittest.TestCase):
    def test_source_roundtrip_and_no_private_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.zip"
            manifest = bundle_native_sdk.bundle(archive)
            extracted = test_native_sdk.extract(archive, root / "sdk")
            self.assertEqual(test_native_sdk.validate(extracted), extracted.resolve())
            self.assertFalse(manifest["hostIncluded"])
            self.assertTrue(all(not name.startswith("src/") for name in manifest["files"]))
            for name in ("sdk/native/include/pulse/preview.h",
                         "sdk/native/include/pulse/settings_pages.h",
                         "sdk/native/samples/archive/archive_settings.h",
                         "sdk/native/samples/archive/archive_settings.cpp",
                         "sdk/native/samples/archive/archive_reader.h",
                         "sdk/native/samples/archive/archive_reader.cpp",
                         "sdk/native/samples/archive/archive_reader_test.cpp",
                         "sdk/native/include/pulse/ui_data.h", "sdk/native/README.md",
                         "sdk/native/include/pulse/directories.h", "sdk/native/include/pulse/context.h",
                         "sdk/native/README.md", "sdk/native/README.md",
                         "examples/native/directory-browser/CMakeLists.txt",
                         "examples/native/directory-browser/plugin.cpp", "examples/native/directory-browser/provider_test.cpp",
                         "examples/native/directory-browser/manifest.json", "examples/native/directory-browser/README.md",
                         "examples/native/directory-transfer/CMakeLists.txt", "examples/native/directory-transfer/plugin.cpp",
                         "examples/native/directory-transfer/manifest.json", "examples/native/directory-transfer/README.md",
                         "sdk/native/include/pulse/ui_mount.h", "sdk/native/README.md",
                         "sdk/native/include/pulse/tray.h",
                         "sdk/native/include/pulse/search.h",
                         "sdk/native/include/pulse/shell.h",
                         "sdk/native/include/pulse/associations.h",
                         "sdk/native/include/pulse/vfs.h",
                         "sdk/native/include/pulse/columns.h",
                         "sdk/native/include/pulse/statusbar.h",
                         "sdk/native/include/pulse/fileevents.h",
                         "sdk/native/include/pulse/worker.hpp",
                         "tools/extensions/native_author_mcp.py", "examples/native/directory-workbench/plugin.cpp",
                         "examples/native/directory-workbench/manifest.json", "examples/native/directory-workbench/README.md"):
                self.assertIn(name, manifest["files"])
                self.assertEqual((extracted / name).read_bytes(), bundle_native_sdk.source_bytes(bundle_native_sdk.ROOT, name))
            result = subprocess.run([sys.executable, str(extracted / "tools/extensions/pulse_ext.py"), "--json", "native-manifest", "--help"],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--library", json.loads(result.stdout)["help"])
            with self.assertRaises(FileExistsError):
                bundle_native_sdk.bundle(archive)

    def test_catalog_and_bundle_header_contracts_ignore_checkout_line_endings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in bundle_native_sdk.FILES:
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(bundle_native_sdk.ROOT / name, destination)
            schemas = root / "schemas/extensions"
            schemas.mkdir(parents=True, exist_ok=True)
            for name in ("manifest.schema.json", "ui-model.schema.json"):
                shutil.copyfile(bundle_native_sdk.ROOT / "schemas/extensions" / name, schemas / name)
            expected = generate_native_catalog.outputs()
            for path in (root / "sdk/native/include/pulse").iterdir():
                data = path.read_bytes().replace(b"\r\n", b"\n")
                path.write_bytes(data.replace(b"\n", b"\r\n"))
            with patch.object(generate_native_catalog, "ROOT", root):
                actual = generate_native_catalog.outputs()
            for path, text in expected.items():
                self.assertEqual(actual[root / path.relative_to(bundle_native_sdk.ROOT)], text)
            manifest = bundle_native_sdk.bundle(root / "sdk.zip", root)
            with zipfile.ZipFile(root / "sdk.zip") as archive:
                catalog = json.loads(archive.read("sdk/native/api-catalog.json"))
                for interface in catalog["interfaces"]:
                    if "headerContract" not in interface:
                        continue
                    name = "sdk/native/" + interface["header"]
                    data = archive.read(name)
                    self.assertNotIn(b"\r\n", data)
                    self.assertEqual(hashlib.sha256(data).hexdigest(), interface["headerContract"]["sha256"])
                    self.assertEqual(data.decode("utf-8"), interface["headerContract"]["text"])
                    self.assertEqual(manifest["files"][name], interface["headerContract"]["sha256"])

    def test_directory_transfer_manifest_uses_only_required_resources(self):
        path = bundle_native_sdk.ROOT / "examples/native/directory-transfer/manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["resources"],
                         ["files.transferFromDirectory", "files.transferToDirectory", "files.readDirectory", "files.transferDirectoryTree", "operations.own"])

    def test_missing_directory_transfer_authoring_file_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.zip"
            bundle_native_sdk.bundle(archive)
            extracted = test_native_sdk.extract(archive, root / "sdk")
            manifest_path = extracted / "native-sdk-manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            del manifest["files"]["examples/native/directory-transfer/plugin.cpp"]
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Missing native directory transfer authoring files"):
                test_native_sdk.validate(extracted)

    @staticmethod
    def fake_pe(path, machine=0x8664, characteristics=0x0022, size=4096):
        data = bytearray(size)
        data[0:2] = b"MZ"
        data[60:64] = (64).to_bytes(4, "little")
        data[64:68] = b"PE\0\0"
        data[68:70] = machine.to_bytes(2, "little")
        data[86:88] = characteristics.to_bytes(2, "little")
        path.write_bytes(bytes(data))
        return path

    def test_prebuilt_harness_bundled_and_validated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            harness = self.fake_pe(root / "pulse_plugin_harness.exe")
            archive = root / "sdk.zip"
            manifest = bundle_native_sdk.bundle(archive, harness=harness)
            name = bundle_native_sdk.HARNESS_NAME
            self.assertEqual(manifest["delivery"], "source-only")
            self.assertFalse(manifest["hostIncluded"])
            self.assertEqual(manifest["prebuilt"][name]["purpose"], "headless-plugin-harness")
            self.assertEqual(manifest["prebuilt"][name]["bytes"], 4096)
            self.assertIn(name, manifest["files"])
            self.assertIn("tools/extensions/native_strings.py", manifest["files"])
            extracted = test_native_sdk.extract(archive, root / "sdk")
            test_native_sdk.validate(extracted)
            self.assertEqual((extracted / name).read_bytes(), harness.read_bytes())
            report = bundle_native_sdk.compatibility_report(archive_path=archive)
            self.assertEqual(report["prebuilt"], [name])
            # A swapped binary no longer matches the declaration.
            self.fake_pe(extracted / name, size=8192)
            with self.assertRaises(ValueError):
                test_native_sdk.validate(extracted)

    def test_prebuilt_harness_must_be_x64_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (machine, characteristics) in enumerate(((0x014C, 0x0022), (0x8664, 0x2022))):
                harness = self.fake_pe(root / f"h{index}.exe", machine, characteristics)
                with self.assertRaises(ValueError):
                    bundle_native_sdk.bundle(root / f"sdk{index}.zip", harness=harness)
            with self.assertRaises(ValueError):
                bundle_native_sdk.bundle(root / "missing.zip", harness=root / "nope.exe")

    def test_undeclared_or_unknown_prebuilt_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            harness = self.fake_pe(root / "pulse_plugin_harness.exe")
            archive = root / "sdk.zip"
            bundle_native_sdk.bundle(archive, harness=harness)
            with zipfile.ZipFile(archive) as source:
                members = {name: source.read(name) for name in source.namelist()}
            manifest = json.loads(members["native-sdk-manifest.json"])
            # Undeclared: the binary is listed as a file but not as prebuilt.
            undeclared = dict(manifest)
            undeclared.pop("prebuilt")
            # Unknown: another binary declared as prebuilt.
            unknown = json.loads(json.dumps(manifest))
            unknown["prebuilt"]["bin/other.exe"] = unknown["prebuilt"][bundle_native_sdk.HARNESS_NAME]
            unknown["files"]["bin/other.exe"] = manifest["files"][bundle_native_sdk.HARNESS_NAME]
            for label, changed, extra in (("undeclared", undeclared, {}),
                                          ("unknown", unknown, {"bin/other.exe": members[bundle_native_sdk.HARNESS_NAME]})):
                bad = root / f"{label}.zip"
                with zipfile.ZipFile(bad, "w") as out:
                    for name, data in {**members, **extra}.items():
                        out.writestr(name, json.dumps(changed) if name == "native-sdk-manifest.json" else data)
                with self.assertRaises(ValueError, msg=label):
                    test_native_sdk.validate(test_native_sdk.extract(bad, root / f"x-{label}"))

    def test_traversal_rejected_before_extract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as out:
                out.writestr("../escape", "bad")
            with self.assertRaises(ValueError):
                test_native_sdk.extract(archive, root / "sdk")
            self.assertFalse((root / "sdk").exists())

    def test_modified_payload_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.zip"
            bundle_native_sdk.bundle(archive)
            extracted = test_native_sdk.extract(archive, root / "sdk")
            (extracted / "sdk/native/include/pulse/abi.h").write_text("tampered")
            with self.assertRaises(ValueError):
                test_native_sdk.validate(extracted)

    def test_native_data_header_catalog_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "sdk.zip"
            bundle_native_sdk.bundle(archive)
            extracted = test_native_sdk.extract(archive, root / "sdk")
            catalog_path = extracted / "sdk/native/api-catalog.json"
            catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
            data = next(item for item in catalog["interfaces"] if item["name"] == "pulse.ui.data")
            data["headerContract"]["sha256"] = "0" * 64
            catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
            manifest_path = extracted / "native-sdk-manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["files"]["sdk/native/api-catalog.json"] = hashlib.sha256(catalog_path.read_bytes()).hexdigest()
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Native data discovery/header mismatch"):
                test_native_sdk.validate(extracted)

    def test_native_discovery_no_authority(self):
        catalog = bundle_native_sdk.catalog()
        self.assertEqual(catalog["sdkPreview"], 3)
        self.assertTrue(catalog["discovery"]["readOnly"])
        self.assertFalse(catalog["discovery"]["grantsExecution"])
        self.assertEqual(catalog["trust"], "trusted-in-process-not-sandboxed")
        self.assertNotIn("mutuallyExclusiveWith", catalog["packageStore"])
        self.assertEqual(catalog["packageStore"]["removedManifestFields"], ["runtime", "commands", "toolbar", "palette", "workflows", "panel"])
        self.assertEqual(catalog["packageStore"]["removedCapabilities"], ["runtime.invoke"])
        resources = next(item for item in catalog["interfaces"] if item["name"] == "pulse.resources")
        self.assertEqual([item["name"] for item in resources["operations"]],
                         ["stat", "settings_get", "settings_set", "audit_list"])

    def test_native_directory_context_discovery_matches_public_contracts(self):
        catalog = bundle_native_sdk.catalog()
        for name, filename, expected_version, operations in (
                ("pulse.directories", "directories.h", 1, ["open", "refresh", "list", "open_entry", "watch", "unwatch", "close", "cancel"]),
                ("pulse.context", "context.h", 1, ["get", "subscribe", "unsubscribe", "get_selection"]),
                ("pulse.commands", "commands.h", 2, ["register_command", "unregister_command", "update_command", "set_command_flags"]),
                ("pulse.icons", "icons.h", 2, ["register_icon", "update_icon", "unregister_icon", "resolve_builtin", "resolve_icon"]),
                ("pulse.dialogs", "dialogs.h", 2, ["show", "set_states", "close", "subscribe_command", "unsubscribe_command", "update_model", "get_state"]),
                ("pulse.diagnostics", "diagnostics.h", 1, ["get_last_error", "clear_last_error"]),
                ("pulse.pickers", "pickers.h", 1, ["open_files", "save_file", "pick_folders", "quick_pick", "cancel"]),
                ("pulse.workbench", "workbench.h", 3, ["show_snack", "show_notice", "reveal_path", "show_text_diff", "set_clipboard_text", "open_search", "reveal_path_from_rect", "query_appearance", "subscribe_appearance", "unsubscribe_appearance", "query_locale", "subscribe_locale", "unsubscribe_locale"]),
                ("pulse.tray", "tray.h", 1, ["snapshot", "collect", "remove_item", "clear", "release_to_directory", "subscribe", "unsubscribe"]),
                ("pulse.search", "search.h", 1, ["query", "cancel"]),
                ("pulse.shell", "shell.h", 1, ["get_tag_catalog", "get_path_tags", "set_path_tag", "query_context_menu", "invoke_context_menu", "close_context_menu", "read_preview", "cancel_preview"]),
                ("pulse.associations", "associations.h", 1, ["query", "apply", "remove", "cancel", "open_chooser", "open_default_apps"]),
                ("pulse.vfs", "vfs.h", 1, ["register_mount", "unregister_mount", "invalidate_path", "open_path"]),
                ("pulse.columns", "columns.h", 1, ["register_column", "register_badge", "set_title", "invalidate", "unregister"]),
                ("pulse.statusbar", "statusbar.h", 1, ["add_item", "update_item", "set_visible", "remove_item"]),
                ("pulse.fileevents", "fileevents.h", 1, ["subscribe", "unsubscribe"]),
                ("pulse.preview", "preview.h", 1, ["register_provider", "unregister_provider"]),
                ("pulse.tasks", "tasks.h", 1, ["create", "cancel", "get", "release"])):
            with self.subTest(interface=name):
                interface = next(item for item in catalog["interfaces"] if item["name"] == name)
                header = bundle_native_sdk.source_bytes(bundle_native_sdk.ROOT, "sdk/native/include/pulse/" + filename)
                self.assertEqual(interface["headerContract"]["sha256"], hashlib.sha256(header).hexdigest())
                self.assertEqual(interface["headerContract"]["text"], header.decode("utf-8"))
                self.assertEqual(interface["operations"], operations)
                self.assertEqual(interface["version"], expected_version)
                self.assertEqual(interface["status"], "developer-preview")
        directories = next(item for item in catalog["interfaces"] if item["name"] == "pulse.directories")
        self.assertEqual(directories["requestOperations"], {"PULSE_DIRECTORY_OPEN": 1, "PULSE_DIRECTORY_REFRESH": 2, "PULSE_DIRECTORY_LIST": 3, "PULSE_DIRECTORY_OPEN_ENTRY": 4})
        self.assertEqual(directories["changeKinds"], {"PULSE_DIRECTORY_WATCH_READY": 1, "PULSE_DIRECTORY_INVALIDATED": 2, "PULSE_DIRECTORY_WATCH_ERROR": 3})
        self.assertEqual(directories["entryFlags"], {"PULSE_DIRECTORY_ENTRY_FOLDER": 1, "PULSE_DIRECTORY_ENTRY_REPARSE": 2})
        self.assertEqual(directories["limits"]["snapshotRows"], 65536)
        self.assertEqual(directories["limits"]["snapshotNamesUtf8Bytes"], 16 * 1024 * 1024)
        self.assertEqual(directories["resourceDeclarations"]["open_entry"], ["files.readDirectory", "files.open"])
        context = next(item for item in catalog["interfaces"] if item["name"] == "pulse.context")
        self.assertEqual(context["changeFlags"], {"PULSE_CONTEXT_INITIAL": 1, "PULSE_CONTEXT_NAVIGATION": 2, "PULSE_CONTEXT_SELECTION": 4, "PULSE_CONTEXT_VIEW": 8})
        self.assertEqual(context["limits"], {"subscriptionsPerAttachment": 16, "coalescingTickMs": 30, "selectionEntries": 4096, "selectionUtf8Bytes": 4194304})
        self.assertEqual(context["resourceDeclarations"]["subscribe"], ["context.read", "context.events"])
        schema = json.loads((bundle_native_sdk.ROOT / "schemas/extensions/manifest.schema.json").read_text(encoding="utf-8"))
        self.assertEqual(catalog["resourceDeclarations"], schema["properties"]["resources"])

    def test_native_preview_worker_contract(self):
        preview = next(item for item in bundle_native_sdk.catalog()["interfaces"] if item["name"] == "pulse.preview")
        self.assertEqual(preview["results"], ["archive", "bitmap", "ui_model"])
        self.assertEqual(preview["limits"]["concurrentCallsPerInstance"], 2)
        self.assertEqual(preview["limits"]["archiveEntries"], 4096)
        self.assertEqual(preview["limits"]["bitmapBytes"], 16 * 1024 * 1024)
        self.assertEqual(preview["resultKinds"], {"PULSE_PREVIEW_ARCHIVE": 1, "PULSE_PREVIEW_BITMAP": 2, "PULSE_PREVIEW_UI_MODEL": 3})
        self.assertEqual(preview["limits"]["modelNodes"], 128)
        self.assertEqual(preview["limits"]["modelDepth"], 8)
        self.assertEqual(preview["limits"]["modelBytes"], 65536)
        self.assertIn("preview native command callbacks (command_count > 0)", preview["unavailable"])
        self.assertIn("context.* bindings", preview["unavailable"])
        self.assertIn("synchronously copies", preview["publish"])

    def test_native_directory_context_header_catalog_mismatch_rejected(self):
        for interface_name in ("pulse.directories", "pulse.context", "pulse.commands", "pulse.preview"):
            with self.subTest(interface=interface_name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                archive = root / "sdk.zip"
                bundle_native_sdk.bundle(archive)
                extracted = test_native_sdk.extract(archive, root / "sdk")
                catalog_path = extracted / "sdk/native/api-catalog.json"
                catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
                interface = next(item for item in catalog["interfaces"] if item["name"] == interface_name)
                interface["headerContract"]["sha256"] = "0" * 64
                catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
                manifest_path = extracted / "native-sdk-manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest["files"]["sdk/native/api-catalog.json"] = hashlib.sha256(catalog_path.read_bytes()).hexdigest()
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Native interface discovery/header mismatch"):
                    test_native_sdk.validate(extracted)

    def test_native_data_discovery_matches_public_contract(self):
        catalog = bundle_native_sdk.catalog()
        data = next(item for item in catalog["interfaces"] if item["name"] == "pulse.ui.data")
        header = bundle_native_sdk.source_bytes(bundle_native_sdk.ROOT, "sdk/native/include/pulse/ui_data.h")
        self.assertEqual(data["headerContract"]["sha256"], hashlib.sha256(header).hexdigest())
        self.assertEqual(data["headerContract"]["text"], header.decode("utf-8"))
        self.assertEqual(data["version"], 1)
        self.assertEqual(data["status"], "developer-preview")
        self.assertEqual(data["operations"], ["register_provider", "unregister_provider", "bind", "unbind", "request", "publish"])
        self.assertEqual(data["requestOperations"], {"PULSE_UI_DATA_REFRESH": 0, "PULSE_UI_DATA_NEXT_PAGE": 1, "PULSE_UI_DATA_CANCEL": 2})
        self.assertEqual(data["pageStatuses"], {"PULSE_UI_DATA_LOADING": 0, "PULSE_UI_DATA_READY": 1, "PULSE_UI_DATA_ERROR": 2})
        self.assertEqual(data["pageModes"], {"PULSE_UI_DATA_SNAPSHOT": 0, "PULSE_UI_DATA_DELTA": 1})
        self.assertEqual(data["targets"], ["virtualList", "tree", "dataTable"])
        self.assertEqual(data["limits"]["inboxPages"], 64)
        self.assertEqual(data["limits"]["pageSizeMax"], 256)
        self.assertEqual(data["limits"]["displayedRows"], 4096)
        model = next(item for item in catalog["interfaces"] if item["name"] == "pulse.ui.model")
        self.assertIn("non-null data_source", model["unavailable"])
        self.assertIn("context.* bindings", model["unavailable"])
        self.assertIn("non-null action.data_operation", model["unavailable"])

    def test_compatibility_metadata_and_standalone_bundle_verifier(self):
        import verify_sdk_bundle
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "Pulse-NativeSDK-test-windows-x64.zip"
            manifest = bundle_native_sdk.bundle(archive)
            compat = manifest["compatibility"]
            expected_version = (bundle_native_sdk.ROOT / "version.txt").read_text(encoding="utf-8").strip()
            self.assertEqual(compat["pulseVersion"], expected_version)
            self.assertEqual(compat["sdkPreview"], 3)
            self.assertEqual(compat["abiVersion"], 1)
            self.assertEqual(compat["architecture"], "windows-x64")
            self.assertEqual(compat["minWindowsBuild"], 10240)
            self.assertEqual(compat["toolchain"]["generator"], "Ninja")
            for required_script in (
                "tools/extensions/project_creator.py",
                "tools/extensions/check_docs_and_contracts.py",
                "tools/extensions/verify_sdk_bundle.py",
                "tools/extensions/README.md",
            ):
                self.assertIn(required_script, manifest["files"])

            compat_report = bundle_native_sdk.compatibility_report(archive)
            self.assertEqual(compat_report["compatibility"], compat)
            self.assertEqual(compat_report["archiveSha256"], hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertEqual(len(compat_report["templates"]), 11)

            verified = verify_sdk_bundle.verify_bundle(archive, compile_projects=False)
            self.assertTrue(verified["ok"], verified)
            self.assertEqual(verified["templatesVerified"], 11)


if __name__ == "__main__":
    unittest.main()