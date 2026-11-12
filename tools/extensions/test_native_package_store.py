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

    @staticmethod
    def _generate_cng_keypair():
        import ctypes
        import package_signing as signing
        cng = signing.Cng()
        try:
            cng.check(cng.dll.BCryptGenerateKeyPair(cng.algorithm, ctypes.byref(cng.key), 2048, 0))
            cng.check(cng.dll.BCryptFinalizeKeyPair(cng.key, 0))
            return cng.export(True), cng.export()
        finally:
            cng.close()

    @unittest.skipUnless(os.name == "nt", "Windows CNG required")
    def test_publisher_continuity_rotation_and_forced_publisher_change(self):
        import package_signing as signing
        priv_a, pub_a = self._generate_cng_keypair()
        priv_b, pub_b = self._generate_cng_keypair()
        pin_a = hashlib.sha256(pub_a).hexdigest()
        pin_b = hashlib.sha256(pub_b).hexdigest()

        self.root.mkdir(parents=True, exist_ok=True)
        policy = {
            "formatVersion": 1,
            "requireSigned": False,
            "publishers": [
                {"id": pin_a, "publicKey": pub_a.hex()},
                {"id": pin_b, "publicKey": pub_b.hex()},
            ],
        }
        (self.root / "signature-policy.json").write_text(json.dumps(policy), encoding="utf-8")

        m1 = self.manifest("Signed v1")
        sig_a1 = json.dumps(signing.sign(m1, priv_a)).encode("utf-8")
        installed_v1 = store.install(self.root, m1, source_dir=self.directory, allow_native=True, signature=sig_a1)
        store.update(self.root, "sample.native", "enable", allow_native=True)
        self.assertTrue(store.listing(self.root)["packages"][0]["enabled"])

        # 1. Signed -> Unsigned upgrade must be rejected even when requireSigned is False
        self.library.write_bytes(pe(b"v2-unsigned"))
        m2_unsigned = self.manifest("Unsigned v2")
        with self.assertRaises(store.StoreError) as err_unsigned:
            store.install(self.root, m2_unsigned, source_dir=self.directory, allow_native=True)
        self.assertEqual(err_unsigned.exception.code, "installed_publisher_mismatch")

        # 2. Publisher A -> Publisher B without rotation or allow_publisher_change must fail
        self.library.write_bytes(pe(b"v2-pub-b"))
        m2_b = self.manifest("Publisher B v2")
        sig_b2 = json.dumps(signing.sign(m2_b, priv_b)).encode("utf-8")
        with self.assertRaises(store.StoreError) as err_mismatch:
            store.install(self.root, m2_b, source_dir=self.directory, allow_native=True, signature=sig_b2)
        self.assertEqual(err_mismatch.exception.code, "installed_publisher_mismatch")
        self.assertEqual(store.listing(self.root)["packages"][0]["activeRevision"], installed_v1["package"]["activeRevision"])
        self.assertTrue(store.listing(self.root)["packages"][0]["enabled"])

        # 3. Invalid rotation proof (wrong package id) must be rejected
        bad_rotation = json.dumps(signing.rotate("other.package", priv_a, pub_a, pub_b)).encode("utf-8")
        with self.assertRaises(store.StoreError) as err_bad_rot:
            store.install(self.root, m2_b, source_dir=self.directory, allow_native=True, signature=sig_b2, rotation=bad_rotation)
        self.assertEqual(err_bad_rot.exception.code, "installed_publisher_mismatch")

        # 4. Valid rotation proof from A -> B succeeds AND preserves enabled=True
        valid_rotation = json.dumps(signing.rotate("sample.native", priv_a, pub_a, pub_b)).encode("utf-8")
        rotated = store.install(self.root, m2_b, source_dir=self.directory, allow_native=True, signature=sig_b2, rotation=valid_rotation)
        self.assertTrue(rotated["package"]["enabled"])
        self.assertTrue(rotated["publisherRotated"])
        self.assertTrue((self.root / "versions" / (rotated["package"]["activeRevision"] + ".rotation.json")).is_file())

        # 5. Publisher B -> Publisher A WITHOUT rotation proof, using allow_publisher_change=True,
        # must succeed AND reset enabled to False (never silently inherit enabled across unrotated publishers)
        self.library.write_bytes(pe(b"v3-pub-a-forced"))
        m3_a = self.manifest("Publisher A forced v3")
        sig_a3 = json.dumps(signing.sign(m3_a, priv_a)).encode("utf-8")
        forced = store.install(
            self.root, m3_a, source_dir=self.directory, allow_native=True,
            signature=sig_a3, allow_publisher_change=True,
        )
        self.assertFalse(forced["package"]["enabled"])
        self.assertTrue(forced["publisherChanged"])

        # Rollback from A -> B without rotation/allow_publisher_change must also enforce publisher continuity
        store.update(self.root, "sample.native", "enable", allow_native=True)
        with self.assertRaises(store.StoreError) as err_rb_pub:
            store.update(self.root, "sample.native", "rollback", allow_native=True)
        self.assertEqual(err_rb_pub.exception.code, "installed_publisher_mismatch")
        rolled = store.update(self.root, "sample.native", "rollback", allow_native=True, allow_publisher_change=True)
        self.assertFalse(rolled["package"]["enabled"])

    def test_rollback_requires_capability_confirmation_when_previous_revision_expands_permissions(self):
        v1 = json.dumps({
            "apiVersion": 1,
            "id": "sample.native",
            "name": "Native v1",
            "resources": ["context.read", "settings.own"],
            "native": {
                "version": 1,
                "architecture": "windows-x64",
                "library": "plugin.dll",
                "sha256": hashlib.sha256(self.library.read_bytes()).hexdigest(),
            },
        }).encode("utf-8")
        store.install(self.root, v1, accept_capabilities=True, source_dir=self.directory, allow_native=True)

        self.library.write_bytes(pe(b"narrower-v2"))
        v2 = json.dumps({
            "apiVersion": 1,
            "id": "sample.native",
            "name": "Native v2",
            "resources": ["context.read"],
            "native": {
                "version": 1,
                "architecture": "windows-x64",
                "library": "plugin.dll",
                "sha256": hashlib.sha256(self.library.read_bytes()).hexdigest(),
            },
        }).encode("utf-8")
        store.install(self.root, v2, accept_capabilities=True, source_dir=self.directory, allow_native=True)

        with self.assertRaises(store.StoreError) as rejected:
            store.update(self.root, "sample.native", "rollback", allow_native=True)
        self.assertEqual(rejected.exception.code, "capability_confirmation_required")

        accepted = store.update(self.root, "sample.native", "rollback", allow_native=True, accept_capabilities=True)
        self.assertEqual(accepted["package"]["name"], "Native v1")

    def test_gc_cleans_unreferenced_versions_binaries_packages_and_skips_locked_files(self):
        v1 = self.install("v1")
        rev1 = v1["package"]["activeRevision"]
        bin1 = json.loads(self.manifest("v1"))["native"]["sha256"]

        self.library.write_bytes(pe(b"v2-payload"))
        v2 = self.install("v2")
        rev2 = v2["package"]["activeRevision"]
        bin2 = json.loads(self.manifest("v2"))["native"]["sha256"]

        self.library.write_bytes(pe(b"v3-payload"))
        v3 = self.install("v3")
        rev3 = v3["package"]["activeRevision"]
        bin3 = json.loads(self.manifest("v3"))["native"]["sha256"]

        policy_file = self.root / "signature-policy.json"
        policy_file.write_text('{"formatVersion":1,"requireSigned":false,"publishers":[]}', encoding="utf-8")
        orphan_pkg_dir = self.root / "packages" / ("f" * 64)
        orphan_pkg_dir.mkdir(parents=True)
        (orphan_pkg_dir / "helper.txt").write_text("orphan", encoding="utf-8")
        locked_orphan_dll = self.root / "binaries" / ("e" * 64 + ".dll")
        locked_orphan_dll.write_bytes(pe(b"locked"))

        if os.name == "nt":
            import ctypes
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CreateFileW.argtypes = [
                ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
            ]
            kernel32.CreateFileW.restype = ctypes.c_void_p
            kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
            # Open with FILE_SHARE_READ (1) only -> blocks deletion on Windows
            handle = kernel32.CreateFileW(str(locked_orphan_dll), 0x80000000, 1, None, 3, 0, None)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            try:
                gc_result = store.gc(self.root)
            finally:
                kernel32.CloseHandle(handle)
            self.assertEqual(len(gc_result["skipped"]), 1)
            self.assertTrue(locked_orphan_dll.is_file())
        else:
            gc_result = store.gc(self.root)

        # v1 and orphan_pkg_dir must be removed; v2 (previousRevision) and v3 (activeRevision) preserved!
        self.assertFalse((self.root / "versions" / f"{rev1}.json").exists())
        self.assertFalse((self.root / "binaries" / f"{bin1}.dll").exists())
        self.assertFalse(orphan_pkg_dir.exists())
        self.assertTrue((self.root / "versions" / f"{rev2}.json").is_file())
        self.assertTrue((self.root / "versions" / f"{rev3}.json").is_file())
        self.assertTrue((self.root / "binaries" / f"{bin2}.dll").is_file())
        self.assertTrue((self.root / "binaries" / f"{bin3}.dll").is_file())
        self.assertTrue((self.root / "registry.json").is_file())
        self.assertTrue(policy_file.is_file())
        self.assertGreaterEqual(gc_result["reclaimedBytes"], 512)

        # Second GC pass after closing locked file handle removes the remaining orphan DLL
        gc_second = store.gc(self.root)
        self.assertEqual(gc_second["skipped"], [])
        self.assertFalse(locked_orphan_dll.exists())


if __name__ == "__main__":
    unittest.main()
