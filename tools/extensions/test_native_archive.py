"""Native v2 archive integrity/consent tests; synthetic PE data never executes."""
import copy
import hashlib
import io
import json
from pathlib import Path
import stat
import struct
import tempfile
import unittest
import warnings
from unittest.mock import patch
import zipfile

import native_archive as archive
import package_store as store
import pulse_ext as cli
from test_native_package_store import pe


class NativeArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.binary = pe()
        self.manifest = {"apiVersion": 1, "id": "archive.native", "name": "Archive native",
            "native": {"version": 1, "architecture": "windows-x64", "library": "plugin.dll",
                       "sha256": hashlib.sha256(self.binary).hexdigest()}}
        self.data = json.dumps(self.manifest).encode()
        self.metadata = archive.metadata_for(self.data, self.manifest)
        self.path = self.root / "plugin.pulse-ext"

    def tearDown(self):
        self.temp.cleanup()

    def snapshot(self):
        return archive.PackageSnapshot(self.metadata, self.data, self.binary)

    def write(self, extra=(), manifest=None, metadata=None, binary=None):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.path, "w", compression=zipfile.ZIP_DEFLATED) as output:
                output.writestr("manifest.json", self.data if manifest is None else manifest)
                output.writestr("package.json", json.dumps(self.metadata if metadata is None else metadata))
                output.writestr("plugin.dll", self.binary if binary is None else binary)
                for name, data in extra:
                    output.writestr(name, data)

    def test_pack_verify_install_upgrade_rollback_roundtrip(self):
        manifest = self.root / "manifest.json"
        library = self.root / "plugin.dll"
        manifest.write_bytes(self.data)
        library.write_bytes(self.binary)
        with patch.object(cli, "validated"), patch.object(cli, "validate_bytes"):
            packed = cli.pack_package(Path("pulse.exe"), manifest, self.path)
            self.assertEqual(packed["package"]["formatVersion"], 2)
            verified = cli.verify_archive(Path("pulse.exe"), self.path)
            self.assertTrue(verified["integrityVerified"])
            self.assertTrue(verified["authorizationReview"]["nativeExecutionConsentRequired"])
            self.assertFalse(verified["authorizationReview"]["grantsAuthority"])
            root = self.root / "store"
            with self.assertRaises(store.StoreError) as caught:
                cli.install_archive(Path("pulse.exe"), self.path, root, False)
            self.assertEqual(caught.exception.code, "native_confirmation_required")
            first = cli.install_archive(Path("pulse.exe"), self.path, root, False, allow_native=True)
            self.assertFalse(first["package"]["enabled"])
            store.update(root, "archive.native", "enable", allow_native=True)
            self.binary = pe(b"second")
            self.manifest["native"]["sha256"] = hashlib.sha256(self.binary).hexdigest()
            self.data = json.dumps(self.manifest).encode()
            self.metadata = archive.metadata_for(self.data, self.manifest)
            self.write()
            before = (root / "registry.json").read_bytes()
            with self.assertRaises(store.StoreError):
                cli.install_archive(Path("pulse.exe"), self.path, root, False)
            self.assertEqual((root / "registry.json").read_bytes(), before)
            second = cli.install_archive(Path("pulse.exe"), self.path, root, False, allow_native=True)
            self.assertTrue(second["package"]["enabled"])
            result = store.update(root, "archive.native", "rollback", allow_native=True)
            self.assertEqual(result["package"]["activeRevision"], first["package"]["activeRevision"])

    def test_archive_replacement_after_validation_cannot_change_installed_snapshot(self):
        self.write()
        reads = []
        original = store.read_bytes
        def observe(path, limit):
            if path == self.path:
                reads.append(path)
            return original(path, limit)
        def replace_archive(pulse, data):
            self.assertEqual(data, self.data)
            self.path.write_bytes(b"replacement after snapshot validation")
        with patch.object(store, "read_bytes", side_effect=observe), patch.object(cli, "validate_bytes", side_effect=replace_archive):
            cli.install_archive(Path("pulse.exe"), self.path, self.root / "store", False, allow_native=True)
        self.assertEqual(len(reads), 1)
        installed = store.native_binary(self.root / "store", self.manifest)
        self.assertEqual(installed.read_bytes(), self.binary)

    def test_pack_uses_captured_source_dll_when_source_changes(self):
        manifest = self.root / "manifest.json"
        library = self.root / "plugin.dll"
        manifest.write_bytes(self.data)
        library.write_bytes(self.binary)
        original = archive.write
        def changed_source(stream, snapshot):
            library.write_bytes(pe(b"replaced"))
            original(stream, snapshot)
        with patch.object(cli, "validated"), patch.object(cli, "validate_bytes"), patch.object(archive, "write", side_effect=changed_source):
            cli.pack_package(Path("pulse.exe"), manifest, self.path)
        self.assertEqual(archive.read(self.path).binary_bytes, self.binary)

    def test_hashes_and_exact_metadata_bind_both_payloads(self):
        for field in ("manifestSha256", "native_hash", "native_library", "formatVersion", "extra"):
            metadata = copy.deepcopy(self.metadata)
            if field == "native_hash":
                metadata["native"]["sha256"] = "0" * 64
            elif field == "native_library":
                metadata["native"]["library"] = "other.dll"
            else:
                metadata[field] = 1 if field == "formatVersion" else "bad"
            self.write(metadata=metadata)
            with self.subTest(field=field), self.assertRaises(ValueError):
                archive.read(self.path)
        self.write(binary=pe(b"tampered"))
        with self.assertRaises(store.StoreError) as caught:
            archive.read(self.path)
        self.assertEqual(caught.exception.code, "native_hash_mismatch")
        self.write(manifest=self.data + b" ")
        with self.assertRaises(ValueError):
            archive.read(self.path)

    def test_missing_extra_duplicate_and_traversal_members_rejected(self):
        for name in ("extra.dll", "../outside.dll", "dir/plugin.dll", "dir\\plugin.dll", "C:plugin.dll", "plugin.dll"):
            self.write(extra=[(name, self.binary)])
            with self.subTest(name=name), self.assertRaises(ValueError):
                archive.read(self.path)
        with zipfile.ZipFile(self.path, "w") as output:
            output.writestr("manifest.json", self.data)
            output.writestr("package.json", json.dumps(self.metadata))
        with self.assertRaises(ValueError):
            archive.read(self.path)

    def test_symlink_directory_and_encrypted_members_rejected(self):
        for kind in (stat.S_IFLNK, stat.S_IFDIR, stat.S_IFIFO):
            with zipfile.ZipFile(self.path, "w") as output:
                output.writestr("manifest.json", self.data)
                output.writestr("package.json", json.dumps(self.metadata))
                info = zipfile.ZipInfo("plugin.dll")
                info.create_system = 3
                info.external_attr = (kind | 0o644) << 16
                output.writestr(info, self.binary)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                archive.read(self.path)
        self.write()
        data = bytearray(self.path.read_bytes())
        offset = data.index(b"PK\x01\x02")
        struct.pack_into("<H", data, offset + 8, 1)
        with self.assertRaises(ValueError):
            archive.parse(bytes(data))

    def test_directory_and_decompression_are_bounded_before_host_validation(self):
        self.write()
        with patch.object(archive, "MAX_ARCHIVE_BYTES", 16), patch.object(archive.zipfile, "ZipFile") as parser:
            with self.assertRaises(store.StoreError):
                archive.read(self.path)
            parser.assert_not_called()
        with patch.object(store, "MAX_NATIVE_BYTES", 256), self.assertRaises(ValueError):
            archive.read(self.path)
        self.write(manifest=b" " * 65537)
        with self.assertRaises(ValueError):
            archive.read(self.path)
        self.write()
        data = bytearray(self.path.read_bytes())
        end = data.rfind(b"PK\x05\x06")
        struct.pack_into("<HH", data, end + 8, 10000, 10000)
        with patch.object(archive.zipfile, "ZipFile") as parser, self.assertRaises(ValueError):
            archive.parse(bytes(data))
        parser.assert_not_called()

    def test_reserved_dos_library_names_rejected_in_schema_and_store(self):
        import generate_sdk
        schema = generate_sdk.read_json(generate_sdk.SCHEMA)
        self.assertIn("not", schema["$defs"]["nativePackage"]["properties"]["library"])
        names = ["CON", "PRN", "AUX", "NUL"] + [prefix + str(i) for prefix in ("COM", "LPT") for i in range(1, 10)]
        for base in names:
            for name in (base.lower() + ".dll", base + ".more.dll"):
                manifest = copy.deepcopy(self.manifest)
                manifest["native"]["library"] = name
                with self.subTest(name=name), self.assertRaises(store.StoreError):
                    store.native_declaration(manifest)

    def test_archive_validation_never_loads_or_extracts_dll(self):
        self.write()
        with patch.object(cli, "validate_bytes"), patch.object(cli.subprocess, "Popen") as popen, \
                patch.object(zipfile.ZipFile, "extractall") as extract:
            result = cli.verify_archive(Path("pulse.exe"), self.path)
        popen.assert_not_called()
        extract.assert_not_called()
        self.assertFalse(result["installed"])
        self.assertEqual(list(self.root.iterdir()), [self.path])


if __name__ == "__main__":
    unittest.main()
