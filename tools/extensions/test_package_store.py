import ctypes
import os
import threading
import hashlib
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

import package_store as store


def manifest(identifier="sample", name="Sample", capability="view.refresh"):
    return json.dumps({"apiVersion": 1, "id": identifier, "name": name,
                       "commands": [{"id": "command", "title": "Command", "capability": capability}]}).encode("utf-8")


class StoreTests(unittest.TestCase):
    def test_removed_script_contract_rejected_before_store_creation(self):
        for change in ({"runtime": {"kind": "javascript", "version": 1, "source": "function run() {}", "entrypoints": ["run"]}},
                       {"commands": [{"id": "run", "title": "Run", "capability": "runtime.invoke", "args": {"entrypoint": "run"}}]}):
            value = json.loads(manifest())
            value.update(change)
            with self.subTest(field=next(iter(change))), self.assertRaises(store.StoreError) as error:
                store.install(self.root, json.dumps(value).encode("utf-8"))
            self.assertEqual(error.exception.code, "invalid_manifest")
            self.assertFalse(self.root.exists())

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "store"

    def tearDown(self):
        self.temp.cleanup()

    def test_install_upgrade_rollback_toggle_uninstall(self):
        first = store.install(self.root, manifest())
        self.assertFalse(first["package"]["enabled"])
        self.assertFalse(store.install(self.root, manifest())["changed"])
        store.update(self.root, "sample", "enable")
        second = store.install(self.root, manifest(name="Updated"))
        self.assertTrue(second["package"]["enabled"])
        self.assertEqual(second["package"]["previousRevision"], first["package"]["activeRevision"])
        self.assertEqual(store.update(self.root, "sample", "rollback")["package"]["name"], "Sample")
        self.assertFalse(store.update(self.root, "sample", "disable")["package"]["enabled"])
        self.assertFalse(store.update(self.root, "sample", "disable")["changed"])
        self.assertTrue(store.update(self.root, "sample", "uninstall")["immutableVersionsRetained"])
        self.assertEqual(store.listing(self.root)["packages"], [])
        self.assertEqual(len(list((self.root / "versions").glob("*.json"))), 2)

    @unittest.skipUnless(os.name == "nt", "Windows replacement sharing semantics")
    def test_registry_commit_waits_for_short_lived_target_reader(self):
        self.root.mkdir()
        target = self.root / "registry.json"
        target.write_bytes(b"original")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(target), 0x80000000, 5, None, 3, 0x200000, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        release = threading.Timer(.12, lambda: kernel.CloseHandle(handle))
        release.start()
        registry = {"formatVersion": 1, "packages": []}
        try:
            store.save(self.root, registry)
        finally:
            release.join()
        self.assertEqual(json.loads(target.read_bytes()), registry)
        self.assertEqual(list(self.root.glob(".registry-*")), [])

    def test_registry_commit_retry_budget_preserves_original(self):
        self.root.mkdir()
        target = self.root / "registry.json"
        target.write_bytes(b"original")
        registry = {"formatVersion": 1, "packages": []}
        for code in (5, 32, 33):
            blocked = OSError("injected Windows replacement failure")
            blocked.winerror = code
            with self.subTest(winerror=code), patch.object(store.os, "replace", side_effect=blocked) as replace, \
                    patch.object(store.time, "sleep") as sleep:
                with self.assertRaises(OSError) as error:
                    store.save(self.root, registry)
                self.assertIs(error.exception, blocked)
                self.assertEqual(replace.call_count, 50)
                self.assertEqual(sleep.call_count, 49)
                self.assertTrue(all(call.args == (.01,) for call in sleep.call_args_list))
            self.assertEqual(target.read_bytes(), b"original")
            self.assertEqual(list(self.root.glob(".registry-*")), [])
        unrelated = OSError("injected invalid parameter")
        unrelated.winerror = 87
        with patch.object(store.os, "replace", side_effect=unrelated) as replace, patch.object(store.time, "sleep") as sleep:
            with self.assertRaises(OSError) as error:
                store.save(self.root, registry)
            self.assertIs(error.exception, unrelated)
            self.assertEqual(replace.call_count, 1)
            sleep.assert_not_called()
        self.assertEqual(target.read_bytes(), b"original")
        self.assertEqual(list(self.root.glob(".registry-*")), [])

    def test_resource_permission_upgrade_requires_acceptance(self):
        store.install(self.root, manifest())
        changed = json.loads(manifest())
        changed["resources"] = ["settings.own"]
        data = json.dumps(changed).encode("utf-8")
        with self.assertRaises(store.StoreError) as error:
            store.install(self.root, data)
        self.assertEqual(error.exception.code, "capability_confirmation_required")
        self.assertTrue(store.install(self.root, data, True)["changed"])

    def test_new_capability_requires_explicit_acceptance(self):
        original = store.install(self.root, manifest())
        with self.assertRaises(store.StoreError) as error:
            store.install(self.root, manifest(capability="navigation.up"))
        self.assertEqual(error.exception.code, "capability_confirmation_required")
        self.assertEqual(store.listing(self.root)["packages"][0], original["package"])
        self.assertTrue(store.install(self.root, manifest(capability="navigation.up"), True)["changed"])

    def test_failed_registry_commit_preserves_active(self):
        original = store.install(self.root, manifest())
        with patch.object(store.os, "replace", side_effect=PermissionError("busy")):
            with self.assertRaises(PermissionError):
                store.install(self.root, manifest(name="Updated"))
        self.assertEqual(store.listing(self.root)["packages"][0], original["package"])
        with store.locked(self.root):
            pass

    def test_lock_refuses_concurrent_mutation(self):
        store.install(self.root, manifest())
        with store.locked(self.root):
            with self.assertRaises(store.StoreError) as error:
                store.update(self.root, "sample", "enable")
            self.assertEqual(error.exception.code, "store_busy")
        with store.locked(self.root):
            pass

    def test_process_exit_releases_lock_without_manual_cleanup(self):
        store.install(self.root, manifest())
        script = "import os,sys;from pathlib import Path;import package_store;\nwith package_store.locked(Path(sys.argv[1])): os._exit(0)"
        result = subprocess.run([sys.executable, "-c", script, str(self.root)],
                                cwd=Path(store.__file__).parent, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / ".registry.lock").exists())
        self.assertTrue(store.update(self.root, "sample", "enable")["package"]["enabled"])

    def test_target_validation_failure_preserves_enable_and_rollback(self):
        first = store.install(self.root, manifest())
        def reject(data):
            raise ValueError("host semantic rejection")
        with self.assertRaises(ValueError):
            store.update(self.root, "sample", "enable", reject)
        self.assertFalse(store.listing(self.root)["packages"][0]["enabled"])
        second = store.install(self.root, manifest(name="Updated"))
        with self.assertRaises(ValueError):
            store.update(self.root, "sample", "rollback", reject)
        self.assertEqual(store.listing(self.root)["packages"][0]["activeRevision"], second["package"]["activeRevision"])

    def test_save_rejects_size_before_replacing_registry(self):
        store.install(self.root, manifest())
        original = (self.root / "registry.json").read_bytes()
        with self.assertRaises(store.StoreError):
            store.save(self.root, {"large": "x" * 65536})
        self.assertEqual((self.root / "registry.json").read_bytes(), original)

    def test_bad_hash_can_disable_uninstall_or_rollback(self):
        store.install(self.root, manifest())
        upgraded = store.install(self.root, manifest(name="Updated"))
        path = self.root / "versions" / (upgraded["package"]["activeRevision"] + ".json")
        path.write_bytes(b"corrupt")
        with self.assertRaises(store.StoreError):
            store.update(self.root, "sample", "enable")
        store.update(self.root, "sample", "disable")
        self.assertEqual(store.update(self.root, "sample", "rollback")["package"]["name"], "Sample")
        store.update(self.root, "sample", "uninstall")
        self.assertTrue(path.exists())

    def test_duplicate_unknown_version_path_and_name_rejected(self):
        installed = store.install(self.root, manifest())
        entry = installed["package"]
        bad = [{"formatVersion": 2, "packages": []},
               {"formatVersion": True, "packages": []},
               {"formatVersion": 1.5, "packages": []},
               {"formatVersion": 1, "packages": [entry, entry]},
               {"formatVersion": 1, "packages": [{**entry, "activeRevision": "../outside"}]},
               {"formatVersion": 1, "packages": [{**entry, "previousRevision": None}]},
               {"formatVersion": 1, "packages": [{**entry, "id": "../outside"}]},
               {"formatVersion": 1, "packages": [{**entry, "name": "bad\nname"}]},
               {"formatVersion": 1, "packages": [], "extra": True}]
        for registry in bad:
            (self.root / "registry.json").write_text(json.dumps(registry), encoding="utf-8")
            with self.subTest(registry=registry), self.assertRaises(store.StoreError):
                store.listing(self.root)

    def test_numeric_version_one_matches_native_parser(self):
        self.root.mkdir()
        for number in ("1", "1.0", "1e0"):
            (self.root / "registry.json").write_text('{"formatVersion":' + number + ',"packages":[]}', encoding="utf-8")
            self.assertEqual(store.listing(self.root)["packages"], [])

    def test_limit_32_and_no_id_paths(self):
        for i in range(32):
            store.install(self.root, manifest(identifier="id." + str(i)))
        with self.assertRaises(store.StoreError) as error:
            store.install(self.root, manifest(identifier="overflow"))
        self.assertEqual(error.exception.code, "store_full")
        self.assertTrue(all(len(path.stem) == 64 for path in (self.root / "versions").iterdir()))

    def test_existing_corrupt_revision_not_overwritten(self):
        data = manifest()
        self.root.mkdir()
        (self.root / "versions").mkdir()
        target = self.root / "versions" / (hashlib.sha256(data).hexdigest() + ".json")
        target.write_bytes(b"existing wrong content")
        with self.assertRaises(store.StoreError):
            store.install(self.root, data)
        self.assertEqual(target.read_bytes(), b"existing wrong content")
        self.assertFalse((self.root / "registry.json").exists())

    def test_disabled_missing_revision_is_manageable(self):
        installed = store.install(self.root, manifest())
        (self.root / "versions" / (installed["package"]["activeRevision"] + ".json")).unlink()
        self.assertEqual(len(store.listing(self.root)["packages"]), 1)
        store.update(self.root, "sample", "uninstall")

    def test_registry_size_limit(self):
        self.root.mkdir()
        (self.root / "registry.json").write_bytes(b" " * 65537)
        with self.assertRaises(store.StoreError):
            store.listing(self.root)


if __name__ == "__main__":
    unittest.main()
