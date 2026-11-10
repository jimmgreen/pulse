"""Directed store/CLI tests use synthetic PE headers, not executable plugin fixtures."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import package_store as store
import pulse_ext as cli


def pe(marker=b"first"):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, 0x8664)
    struct.pack_into("<H", data, 148, 240)
    struct.pack_into("<H", data, 150, 0x2002)
    struct.pack_into("<H", data, 152, 0x20b)
    data[400:400 + len(marker)] = marker
    return bytes(data)


class NativeStoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.root = self.directory / "store"
        self.library = self.directory / "plugin.dll"
        self.library.write_bytes(pe())

    def tearDown(self):
        self.temp.cleanup()

    def manifest(self, name="Native"):
        return json.dumps({"apiVersion": 1, "id": "sample.native", "name": name,
            "native": {"version": 1, "architecture": "windows-x64", "library": "plugin.dll",
                       "sha256": hashlib.sha256(self.library.read_bytes()).hexdigest()}}).encode()

    def install(self, name="Native", **kwargs):
        return store.install(self.root, self.manifest(name), source_dir=self.directory, allow_native=True, **kwargs)

    def test_consent_install_enable_upgrade_and_rollback(self):
        with self.assertRaises(store.StoreError) as caught:
            store.install(self.root, self.manifest(), source_dir=self.directory)
        self.assertEqual(caught.exception.code, "native_confirmation_required")
        self.assertFalse(self.root.exists())
        first = self.install()
        self.assertFalse(first["package"]["enabled"])
        with self.assertRaises(store.StoreError):
            store.update(self.root, "sample.native", "enable")
        store.update(self.root, "sample.native", "enable", allow_native=True)
        self.library.write_bytes(pe(b"second"))
        with self.assertRaises(store.StoreError):
            store.install(self.root, self.manifest("Updated"), source_dir=self.directory)
        second = self.install("Updated")
        self.assertTrue(second["package"]["enabled"])
        with self.assertRaises(store.StoreError):
            store.update(self.root, "sample.native", "rollback")
        result = store.update(self.root, "sample.native", "rollback", allow_native=True)
        self.assertEqual(result["package"]["activeRevision"], first["package"]["activeRevision"])
        self.assertEqual(len(list((self.root / "binaries").glob("*.dll"))), 2)

    def test_hash_pe_architecture_and_size_rejection(self):
        manifest = self.manifest()
        self.library.write_bytes(pe(b"changed"))
        with self.assertRaises(store.StoreError) as caught:
            store.install(self.root, manifest, source_dir=self.directory, allow_native=True)
        self.assertEqual(caught.exception.code, "native_hash_mismatch")
        for value in (b"not PE", pe()[:100], pe()[:132] + b"\x4c\x01" + pe()[134:]):
            with self.subTest(value=value[:8]), self.assertRaises(store.StoreError):
                store.validate_native_binary(value)
        with patch.object(store, "MAX_NATIVE_BYTES", 256), self.assertRaises(store.StoreError):
            store.validate_native_binary(pe())
        self.assertFalse((self.root / "registry.json").exists())

    def test_exact_closed_native_contract(self):
        baseline = json.loads(self.manifest())
        for field, value in (("version", True), ("version", 2), ("architecture", "windows-arm64"),
                             ("library", "../plugin.dll"), ("library", "sub\\plugin.dll"),
                             ("library", "plugin.dll:ads"), ("library", "plugin.DLL"),
                             ("sha256", "A" * 64), ("extra", True)):
            manifest = json.loads(json.dumps(baseline))
            manifest["native"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(store.StoreError):
                store.native_declaration(manifest)
        baseline["runtime"] = {}
        with self.assertRaises(store.StoreError):
            store.native_declaration(baseline)

    def test_existing_corrupt_binary_is_preserved(self):
        sha = json.loads(self.manifest())["native"]["sha256"]
        path = self.root / "binaries" / (sha + ".dll")
        path.parent.mkdir(parents=True)
        path.write_bytes(b"corrupt")
        with self.assertRaises(store.StoreError):
            self.install()
        self.assertEqual(path.read_bytes(), b"corrupt")
        self.assertFalse((self.root / "registry.json").exists())

    def test_idempotent_install_revalidates_binary(self):
        self.install()
        self.assertFalse(self.install()["changed"])
        store.native_binary(self.root, json.loads(self.manifest())).write_bytes(b"tampered")
        with self.assertRaises(store.StoreError):
            self.install()

    def test_bounded_source_read_and_missing_source_preserve_registry(self):
        with patch.object(store, "MAX_NATIVE_BYTES", 256), self.assertRaises(store.StoreError):
            self.install()
        self.assertFalse((self.root / "registry.json").exists())
        with self.assertRaises(store.StoreError) as caught:
            store.install(self.root, self.manifest(), allow_native=True)
        self.assertEqual(caught.exception.code, "native_source_required")

    def test_registry_commit_failure_does_not_activate_new_binary(self):
        self.install()
        before = (self.root / "registry.json").read_bytes()
        self.library.write_bytes(pe(b"second"))
        with patch.object(store.os, "replace", side_effect=PermissionError("registry busy")):
            with self.assertRaises(PermissionError):
                self.install("Updated")
        self.assertEqual((self.root / "registry.json").read_bytes(), before)
        self.assertEqual(len(list((self.root / "binaries").glob("*.dll"))), 2)
        self.assertFalse(list(self.root.rglob(".binary-*")))

    def test_missing_previous_binary_prevents_rollback(self):
        first_manifest = json.loads(self.manifest())
        self.install()
        self.library.write_bytes(pe(b"second"))
        self.install("Updated")
        before = (self.root / "registry.json").read_bytes()
        store.native_binary(self.root, first_manifest).unlink()
        with self.assertRaises(FileNotFoundError):
            store.update(self.root, "sample.native", "rollback", allow_native=True)
        self.assertEqual((self.root / "registry.json").read_bytes(), before)

    def test_tampered_previous_binary_prevents_rollback(self):
        first_manifest = json.loads(self.manifest())
        self.install()
        self.library.write_bytes(pe(b"second"))
        self.install("Updated")
        before = (self.root / "registry.json").read_bytes()
        store.native_binary(self.root, first_manifest).write_bytes(pe(b"tampered"))
        with self.assertRaises(store.StoreError) as caught:
            store.update(self.root, "sample.native", "rollback", allow_native=True)
        self.assertEqual(caught.exception.code, "native_hash_mismatch")
        self.assertEqual((self.root / "registry.json").read_bytes(), before)

    def test_disabled_corruption_does_not_block_other_packages(self):
        self.install()
        store.native_binary(self.root, json.loads(self.manifest())).write_bytes(b"tampered")
        with self.assertRaises(store.StoreError):
            store.update(self.root, "sample.native", "enable", allow_native=True)
        other = b'{"apiVersion":1,"id":"other","name":"Other"}'
        store.install(self.root, other)
        store.update(self.root, "other", "enable")
        self.assertEqual(len(store.listing(self.root)["packages"]), 2)
        store.update(self.root, "sample.native", "disable")
        store.update(self.root, "sample.native", "uninstall")

    def test_concurrent_mutation_rejected_and_lock_reusable(self):
        self.install()
        with store.locked(self.root):
            with self.assertRaises(store.StoreError) as caught:
                self.install("Updated")
            self.assertEqual(caught.exception.code, "store_busy")
        self.assertTrue(self.install("Updated")["changed"])

    def test_reparse_source_rejected(self):
        real = store.ordinary
        def reject(path):
            if path == self.library:
                store.fail("Store paths must not be reparse points")
            real(path)
        with patch.object(store, "ordinary", side_effect=reject), self.assertRaises(store.StoreError):
            self.install()
        self.assertFalse((self.root / "registry.json").exists())

    def test_binary_publication_precedes_registry_commit(self):
        real = store.save
        def verify(root, registry):
            stored = store.native_binary(root, json.loads(self.manifest()))
            self.assertEqual(stored.read_bytes(), self.library.read_bytes())
            real(root, registry)
        with patch.object(store, "save", side_effect=verify):
            self.install()

    def test_cli_author_install_and_default_store_are_explicit(self):
        template = self.directory / "template.json"
        template.write_text('{"apiVersion":1,"id":"sample.native","name":"Native"}')
        output = self.directory / "manifest.json"
        def run(*args):
            capture = io.StringIO()
            with contextlib.redirect_stdout(capture):
                result = cli.main(["--json", *args])
            return result, json.loads(capture.getvalue())
        with patch.object(cli, "validate_bytes"), patch.object(cli, "validated"), patch.dict(os.environ, {"LOCALAPPDATA": str(self.directory)}):
            code, authored = run("native-manifest", str(template), "--library", str(self.library), "--output", str(output))
            self.assertEqual(code, 0, authored)
            self.assertFalse(authored["nativeManifest"]["executed"])
            self.assertEqual(authored["nativeManifest"]["manifestSha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            code, rejected = run("install", str(output))
            self.assertEqual(rejected["error"]["code"], "native_confirmation_required")
            code, installed = run("install", str(output), "--allow-native")
            self.assertEqual(code, 0, installed)
            self.assertTrue((self.directory / "Pulse/extensions/registry.json").is_file())
            code, enabled = run("enable", "sample.native", "--allow-native")
            self.assertEqual(code, 0, enabled)
            self.assertTrue(enabled["update"]["package"]["enabled"])

    def test_native_archive_pack_contains_verified_declared_library(self):
        output = self.directory / "native.zip"
        with patch.object(cli, "checked_snapshot", return_value=(self.manifest(), json.loads(self.manifest()))):
            result = cli.pack_package(Path("pulse.exe"), self.directory / "manifest.json", output)
        self.assertEqual(result["package"]["formatVersion"], 2)
        self.assertEqual(cli.read_package_snapshot(output).binary_bytes, self.library.read_bytes())

    def test_run_revalidates_enabled_binary_without_new_execution_switch(self):
        self.install()
        store.update(self.root, "sample.native", "enable", allow_native=True)
        with patch.object(cli, "host_call", return_value={"developerFeatures": ["nativePackages"]}), \
                patch.object(cli, "validate_bytes"), patch.object(cli.subprocess, "Popen") as popen:
            popen.return_value.pid = 123
            result = cli.run_store(Path("pulse.exe"), self.root, self.directory / "profile")
            self.assertFalse(result["loadVerified"])
            self.assertIn("--extension-store", popen.call_args.args[0])
            self.assertNotIn("--extension-native-store", popen.call_args.args[0])
            store.native_binary(self.root, json.loads(self.manifest())).write_bytes(b"tampered")
            popen.reset_mock()
            with self.assertRaises(store.StoreError):
                cli.run_store(Path("pulse.exe"), self.root, self.directory / "next-profile")
            popen.assert_not_called()

    def test_schema_and_catalog_expose_contract_without_grant(self):
        import generate_sdk
        schema = generate_sdk.read_json(generate_sdk.SCHEMA)
        definition = schema["$defs"]["nativePackage"]
        self.assertFalse(definition["additionalProperties"])
        self.assertEqual(set(definition["required"]), {"version", "architecture", "library", "sha256"})
        self.assertNotIn("runtime", schema["properties"])
        self.assertFalse(schema["additionalProperties"])
        api = generate_sdk.describe(schema)
        self.assertEqual(api["nativeSdk"]["catalogPath"], "sdk/native/api-catalog.json")
        types = generate_sdk.generate(schema)[generate_sdk.TYPES]
        self.assertIn("native?: NativePackage", types)


if __name__ == "__main__":
    unittest.main()
