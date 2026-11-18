import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from unittest.mock import patch
import native_archive as archive
import package_store as store
from test_native_package_store import pe

class MultiFileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "store"
        self.files = {"plugin.dll": pe(), "7z.dll": pe(b"engine"), "License.txt": b"license"}
        self.manifest = {"apiVersion": 1, "id": "sample.archive", "name": "Archive", "native": {
            "version": 1, "architecture": "windows-x64", "library": "plugin.dll",
            "sha256": hashlib.sha256(self.files["plugin.dll"]).hexdigest(), "files": [
                {"path": name, "sha256": hashlib.sha256(data).hexdigest()}
                for name, data in self.files.items() if name != "plugin.dll"]}}
    def encoded(self):
        return json.dumps(self.manifest).encode()
    def install(self):
        return store.install(self.root, self.encoded(), allow_native=True, file_data=self.files)["package"]
    def test_atomic_upgrade_rollback_and_tamper(self):
        first = self.install()
        store.update(self.root, "sample.archive", "enable", allow_native=True)
        self.files["License.txt"] = b"updated license"
        self.manifest["native"]["files"][1]["sha256"] = hashlib.sha256(self.files["License.txt"]).hexdigest()
        second = self.install()
        self.assertTrue(second["enabled"])
        self.assertNotEqual(first["activeRevision"], second["activeRevision"])
        current = store.update(self.root, "sample.archive", "rollback", allow_native=True)["package"]
        self.assertEqual(first["activeRevision"], current["activeRevision"])
        for name in ("License.txt", "7z.dll"):
            target = self.root / "packages" / first["activeRevision"] / name
            saved = target.read_bytes()
            target.write_bytes(b"tampered")
            before = (self.root / "registry.json").read_bytes()
            with self.assertRaises(store.StoreError):
                store.update(self.root, "sample.archive", "enable", allow_native=True)
            self.assertEqual(before, (self.root / "registry.json").read_bytes())
            target.write_bytes(saved)
    def test_archive_exact_members_hash_and_budget(self):
        data = self.encoded()
        snapshot = archive.PackageSnapshot(archive.metadata_for(data, self.manifest), data, self.files["plugin.dll"], self.files)
        stream = io.BytesIO(); archive.write(stream, snapshot)
        self.assertEqual(archive.parse(stream.getvalue()).files, self.files)
        for changed in ({**self.files, "extra.txt": b"extra"}, {k:v for k,v in self.files.items() if k != "7z.dll"},
                        {**self.files, "License.txt": b"corrupt"}):
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, "w") as output:
                output.writestr("manifest.json", data)
                output.writestr("package.json", json.dumps(snapshot.metadata))
                for name, content in changed.items(): output.writestr(name, content)
            with self.assertRaises((ValueError, store.StoreError)): archive.parse(stream.getvalue())
        with patch.object(store, "MAX_PACKAGE_BYTES", 600), self.assertRaises(store.StoreError):
            store.validate_native_files(self.manifest["native"], self.files)
    def test_invalid_names_and_duplicate_identity(self):
        for name in ("../x", "a/b", "a\\b", "CON.txt", "a.", "manifest.json", "PACKAGE.JSON", "PLUGIN.DLL", "7Z.DLL"):
            manifest = copy.deepcopy(self.manifest)
            manifest["native"]["files"][1]["path"] = name
            with self.subTest(name=name), self.assertRaises(store.StoreError): store.native_declaration(manifest)
    def test_failed_publish_preserves_registry(self):
        first = self.install()
        before = (self.root / "registry.json").read_bytes()
        self.manifest["name"] = "Changed"
        self.files["License.txt"] = b"corrupt"
        with self.assertRaises(store.StoreError): self.install()
        self.assertEqual(before, (self.root / "registry.json").read_bytes())
        self.assertEqual([p.name for p in (self.root / "packages").iterdir()], [first["activeRevision"]])

if __name__ == "__main__": unittest.main()
