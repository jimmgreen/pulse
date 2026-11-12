"""Unit tests for publish_extension_release.py and verify_sdk_bundle.py lifecycle gates."""
import base64
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import bundle_native_sdk
import native_archive
import package_signing
import package_store
import publish_extension_release as rel
import test_native_sdk
import verify_sdk_bundle


def make_synthetic_pe64_dll(marker=b"pulse-ext-test"):
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<H", data, 132, 0x8664)
    struct.pack_into("<H", data, 148, 240)
    struct.pack_into("<H", data, 150, 0x2002)
    struct.pack_into("<H", data, 152, 0x20B)
    data[400:400 + len(marker)] = marker
    return bytes(data)


def run_cli(*argv):
    stdout_buf = io.StringIO()
    stderr_buf = io.StringIO()
    with contextlib.redirect_stdout(stdout_buf), contextlib.redirect_stderr(stderr_buf):
        code = rel.main(list(argv))
    return code, stdout_buf.getvalue(), stderr_buf.getvalue()


class PublishExtensionReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Generate two 2048-bit CNG RSA keypairs once for fast cryptographic test execution.
        cls.official_key = rel.generate_keypair(2048)
        cls.untrusted_key = rel.generate_keypair(2048)

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="pulse-rel-test-")
        self.root = Path(self._tmp.name).resolve()
        # Guard against any accidental fallback to %LOCALAPPDATA%.
        self._default_root_patch = patch.object(
            package_store,
            "default_root",
            side_effect=AssertionError("Tests must never access user LOCALAPPDATA store"),
        )
        self._default_root_patch.start()

    def tearDown(self):
        self._default_root_patch.stop()
        self._tmp.cleanup()

    def _create_extension_source(self, folder_name, ext_id="pulse.sample-archive", title="Sample Archive", version=None, dll_marker=b"v1"):
        src_dir = self.root / folder_name
        src_dir.mkdir(parents=True, exist_ok=True)
        dll_path = src_dir / "pulse_sample_archive.dll"
        dll_bytes = make_synthetic_pe64_dll(dll_marker)
        dll_path.write_bytes(dll_bytes)
        manifest = {
            "apiVersion": 1,
            "id": ext_id,
            "name": title,
            "resources": ["context.read", "settings.own"],
            "contributes": {"commands": []},
        }
        if version is not None:
            manifest["version"] = version
            manifest_path = src_dir / "pulse.extension.json"
        else:
            manifest_path = src_dir / "manifest.json"
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return src_dir, dll_path

    def _publisher_spec(self, key_info=None, official=True):
        key_info = key_info or self.official_key
        return {
            "id": key_info["publisherId"],
            "name": "Pulse Team",
            "publicKey": key_info["publicKey"],
            "official": official,
            "url": "https://github.com/jimmgreen/pulse",
        }

    def _extension_meta(self, ext_id="pulse.sample-archive", publisher_id=None):
        return {
            "id": ext_id,
            "publisher": publisher_id or self.official_key["publisherId"],
            "name": {"zh": "示例归档扩展", "en": "Sample Archive Extension"},
            "summary": {"zh": "官方签名扩展示例", "en": "Official signed extension sample"},
            "description": {"zh": "用于验证签名发布流水线。", "en": "Verifies the signed extension release pipeline."},
            "category": "fileops",
            "icon": {"glyph": "archive", "color": "#2B6CB0"},
            "license": "MIT",
            "homepage": "https://github.com/jimmgreen/pulse-extensions",
            "source": "https://github.com/jimmgreen/pulse-extensions",
            "tags": ["archive", "official"],
        }

    def test_official_builtin_key_and_pin_match_cpp_client(self):
        catalog_h = (rel.ROOT / "src/app/extensions/store/catalog.h").read_text(encoding="utf-8")
        catalog_cpp = (rel.ROOT / "src/app/extensions/store/catalog.cpp").read_text(encoding="utf-8")
        pin_match = re.search(r'kOfficialCatalogPublisher\s*=\s*"([0-9a-f]{64})"', catalog_h)
        self.assertIsNotNone(pin_match)
        self.assertEqual(rel.OFFICIAL_PUBLISHER_ID, pin_match.group(1))

        key_block = re.search(r'kOfficialKey\s*=\s*((?:\s*"[0-9a-f]+")+)\s*;', catalog_cpp)
        self.assertIsNotNone(key_block)
        cpp_hex = "".join(re.findall(r'"([0-9a-f]+)"', key_block.group(1)))
        self.assertEqual(rel.OFFICIAL_PUBLIC_KEY_HEX, cpp_hex)

        blob, actual_hex, actual_pin = rel.load_public_key_blob(rel.OFFICIAL_PUBLIC_KEY_HEX)
        self.assertEqual(actual_hex, rel.OFFICIAL_PUBLIC_KEY_HEX)
        self.assertEqual(actual_pin, rel.OFFICIAL_PUBLISHER_ID)
        self.assertEqual(len(blob), 24 + 3 + 384)  # 3072-bit RSAPUBLICBLOB

    def test_end_to_end_keygen_pack_build_and_verify_catalog_cli(self):
        key_json = self.root / "publisher-key.json"
        pub_blob_path = self.root / "publisher.pub.blob"
        priv_blob_path = self.root / "publisher.priv.blob"
        code, out, err = run_cli(
            "keygen",
            "--bits",
            "2048",
            "--output",
            str(key_json),
            "--public-key-out",
            str(pub_blob_path),
            "--private-key-out",
            str(priv_blob_path),
        )
        self.assertEqual(code, 0, err)
        kp = json.loads(out)
        self.assertEqual(len(kp["publisherId"]), 64)
        self.assertEqual(hashlib.sha256(base64.b64decode(kp["publicKey"])).hexdigest(), kp["publisherId"])
        self.assertEqual(pub_blob_path.read_bytes()[:4], b"RSA1")
        self.assertEqual(priv_blob_path.read_bytes()[:4], b"RSA2")

        src_100, dll_100 = self._create_extension_source(
            "src-1.0.0", title="Sample Archive 1.0.0", version="1.0.0", dll_marker=b"dll-1.0.0"
        )
        src_110, dll_110 = self._create_extension_source(
            "src-1.1.0", title="Sample Archive 1.1.0", version="1.1.0", dll_marker=b"dll-1.1.0"
        )

        artifacts_dir = self.root / "artifacts"
        out_100 = artifacts_dir / "v1.0.0"
        out_110 = artifacts_dir / "v1.1.0"

        for ver, src, dll, dest in (
            ("1.0.0", src_100, dll_100, out_100),
            ("1.1.0", src_110, dll_110, out_110),
        ):
            code, out, err = run_cli(
                "pack-release",
                "--source",
                str(src),
                "--version",
                ver,
                "--library",
                str(dll),
                "--private-key",
                str(key_json),
                "--output-dir",
                str(dest),
                "--min-host-version",
                "1.0.60",
                "--sdk-preview",
                "3",
                "--published",
                "2026-11-11",
                "--release-notes",
                json.dumps({"zh": f"版本 {ver} 更新", "en": f"Release {ver} notes"}),
            )
            self.assertEqual(code, 0, err)
            packed = json.loads(out)
            entry = packed["entry"]
            self.assertIs(entry["nativeCode"], True)
            self.assertEqual(entry["architecture"], "windows-x64")
            self.assertIs(type(entry["size"]), int)
            self.assertIs(type(entry["sdkPreview"]), int)

            sha_file = Path(packed["sha256Path"])
            archive_file = Path(packed["archivePath"])
            expected_sha_line = f"{hashlib.sha256(archive_file.read_bytes()).hexdigest()}  {archive_file.name}\n"
            self.assertEqual(sha_file.read_text(encoding="utf-8"), expected_sha_line)

        cfg_path = self.root / "catalog-config.json"
        cfg_path.write_text(
            json.dumps(
                {
                    "publishers": [self._publisher_spec(kp, official=True)],
                    "extensions": [self._extension_meta("pulse.sample-archive", kp["publisherId"])],
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )

        catalog_path = self.root / "catalog" / "catalog.json"
        sig_path = self.root / "catalog" / "catalog.sig.json"
        # Pass 1.0.0 first and 1.1.0 second to verify automatic descending SemVer sort.
        code, out, err = run_cli(
            "build-catalog",
            "--config",
            str(cfg_path),
            "--entry",
            str(out_100 / "release-entry.json"),
            "--entry",
            str(out_110 / "release-entry.json"),
            "--private-key",
            str(priv_blob_path),
            "--generated",
            "2026-11-11T12:00:00Z",
            "--expires-days",
            "30",
            "--output",
            str(catalog_path),
            "--signature-output",
            str(sig_path),
        )
        self.assertEqual(code, 0, err)

        catalog_raw = catalog_path.read_bytes()
        self.assertTrue(rel._strict_integer_tokens(catalog_raw))
        catalog_doc = json.loads(catalog_raw.decode("utf-8"))
        self.assertEqual(catalog_doc["formatVersion"], 1)
        self.assertEqual(catalog_doc["sequence"], 1)
        self.assertEqual(catalog_doc["generated"], "2026-11-11T12:00:00Z")
        self.assertEqual(catalog_doc["expires"], "2026-12-11T12:00:00Z")
        versions = [item["version"] for item in catalog_doc["extensions"][0]["versions"]]
        self.assertEqual(versions, ["1.1.0", "1.0.0"])

        code, out, err = run_cli(
            "verify-catalog",
            "--catalog",
            str(catalog_path),
            "--signature",
            str(sig_path),
            "--trusted-key",
            kp["publicKey"],
            "--trusted-pin",
            kp["publisherId"],
            "--min-sequence",
            "1",
            "--now",
            "2026-11-12T00:00:00Z",
            "--artifacts-dir",
            str(artifacts_dir),
        )
        self.assertEqual(code, 0, err)
        verified = json.loads(out)
        self.assertTrue(verified["ok"])
        self.assertEqual(verified["verifiedArtifacts"], 2)

    def test_build_catalog_directly_from_local_pulse_ext_and_rejects_bad_package_sig(self):
        src_dir, dll_path = self._create_extension_source("direct-src", dll_marker=b"direct")
        out_dir = self.root / "direct-out"
        packed = rel.pack_release(
            src_dir,
            "2.0.0",
            self.official_key["privateKey"],
            out_dir,
            library_path=dll_path,
            published="2026-11-11",
        )
        # Remove release-entry.json to prove build-catalog extracts metadata directly from .pulse-ext + .sig.json.
        Path(packed["releaseEntryPath"]).unlink()

        catalog_path = self.root / "direct-catalog.json"
        sig_path = self.root / "direct-catalog.sig.json"
        rel.build_catalog(
            [self._publisher_spec()],
            [self._extension_meta()],
            catalog_path,
            release_entries=[packed["archivePath"]],
            generated="2026-11-11T10:00:00Z",
            expires="2026-12-11T10:00:00Z",
            private_key_source=self.official_key["privateKey"],
            signature_output_path=sig_path,
        )
        verified = rel.verify_catalog(
            catalog_path,
            sig_path,
            trusted_key=self.official_key["publicKey"],
            trusted_pin=self.official_key["publisherId"],
            now="2026-11-15T00:00:00Z",
            artifacts_dir=out_dir,
        )
        self.assertEqual(verified["verifiedArtifacts"], 1)

        # Tamper with detached package signature; build_catalog must reject it!
        sig_doc = json.loads(Path(packed["signaturePath"]).read_text(encoding="utf-8"))
        sig_doc["signature"] = ("00" if not sig_doc["signature"].startswith("00") else "ff") + sig_doc["signature"][2:]
        Path(packed["signaturePath"]).write_text(json.dumps(sig_doc), encoding="utf-8")
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.build_catalog(
                [self._publisher_spec()],
                [self._extension_meta()],
                self.root / "bad-pkg-catalog.json",
                release_entries=[packed["archivePath"]],
            )
        self.assertEqual(ctx.exception.code, "package_signature_invalid")

    def test_previous_catalog_monotonic_sequence_and_rollback_rejection(self):
        src_dir, dll_path = self._create_extension_source("seq-src")
        packed = rel.pack_release(
            src_dir,
            "1.0.0",
            self.official_key["privateKey"],
            self.root / "seq-art",
            library_path=dll_path,
        )
        cat_1 = self.root / "cat-1.json"
        res_1 = rel.build_catalog(
            [self._publisher_spec()],
            [self._extension_meta()],
            cat_1,
            release_entries=[packed["releaseEntryPath"]],
            sequence=10,
            generated="2026-11-11T00:00:00Z",
        )
        self.assertEqual(res_1["sequence"], 10)

        # Auto-increment without explicit --sequence -> 11
        cat_2 = self.root / "cat-2.json"
        res_2 = rel.build_catalog(
            [self._publisher_spec()],
            [self._extension_meta()],
            cat_2,
            release_entries=[packed["releaseEntryPath"]],
            previous_catalog=cat_1,
            generated="2026-11-11T01:00:00Z",
        )
        self.assertEqual(res_2["sequence"], 11)

        # Reject equal sequence (11 <= 11) and lower sequence (9 <= 11)
        for bad_seq in (11, 9):
            with self.subTest(bad_seq=bad_seq):
                with self.assertRaises(rel.ReleasePipelineError) as ctx:
                    rel.build_catalog(
                        [self._publisher_spec()],
                        [self._extension_meta()],
                        self.root / f"cat-bad-{bad_seq}.json",
                        release_entries=[packed["releaseEntryPath"]],
                        previous_catalog=cat_2,
                        sequence=bad_seq,
                        generated="2026-11-11T02:00:00Z",
                    )
                self.assertEqual(ctx.exception.code, "sequence_rollback")

    def test_verify_catalog_security_rejections(self):
        src_dir, dll_path = self._create_extension_source("sec-src")
        packed = rel.pack_release(
            src_dir,
            "1.0.0",
            self.official_key["privateKey"],
            self.root / "sec-art",
            library_path=dll_path,
        )
        cat_path = self.root / "sec-catalog.json"
        sig_path = self.root / "sec-catalog.sig.json"
        rel.build_catalog(
            [self._publisher_spec()],
            [self._extension_meta()],
            cat_path,
            release_entries=[packed["releaseEntryPath"]],
            sequence=5,
            generated="2026-11-11T10:00:00Z",
            expires="2026-11-20T10:00:00Z",
            private_key_source=self.official_key["privateKey"],
            signature_output_path=sig_path,
        )

        # 1. Anti-replay: reject sequence < min_sequence
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.verify_catalog(
                cat_path,
                sig_path,
                trusted_key=self.official_key["publicKey"],
                trusted_pin=self.official_key["publisherId"],
                min_sequence=6,
                now="2026-11-12T00:00:00Z",
            )
        self.assertEqual(ctx.exception.code, "catalog_replay_rejected")

        # 2. Expiry: reject expires <= now (both boundary equality and past expiry)
        for expired_now in ("2026-11-20T10:00:00Z", "2026-11-21T00:00:00Z"):
            with self.subTest(now=expired_now):
                with self.assertRaises(rel.ReleasePipelineError) as ctx:
                    rel.verify_catalog(
                        cat_path,
                        sig_path,
                        trusted_key=self.official_key["publicKey"],
                        trusted_pin=self.official_key["publisherId"],
                        min_sequence=5,
                        now=expired_now,
                    )
                self.assertEqual(ctx.exception.code, "catalog_expired")

        # 3. Single-byte tamper in catalog.json
        tampered_cat = self.root / "tampered-catalog.json"
        raw = bytearray(cat_path.read_bytes())
        raw.append(ord("\n"))
        tampered_cat.write_bytes(bytes(raw))
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.verify_catalog(
                tampered_cat,
                sig_path,
                trusted_key=self.official_key["publicKey"],
                trusted_pin=self.official_key["publisherId"],
                now="2026-11-12T00:00:00Z",
            )
        self.assertEqual(ctx.exception.code, "catalog_signature_invalid")

        # 4. Domain-confusion forgery: signing catalog.json with Manifest signature prefix must be rejected!
        full_priv, _, _, _ = rel.load_private_key_blobs(self.official_key["privateKey"])
        manifest_domain_sig = package_signing.sign(cat_path.read_bytes(), full_priv)
        forged_domain_sig_path = self.root / "forged-domain.sig.json"
        forged_domain_sig_path.write_text(json.dumps(manifest_domain_sig, indent=2) + "\n", encoding="utf-8")
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.verify_catalog(
                cat_path,
                forged_domain_sig_path,
                trusted_key=self.official_key["publicKey"],
                trusted_pin=self.official_key["publisherId"],
                now="2026-11-12T00:00:00Z",
            )
        self.assertEqual(ctx.exception.code, "catalog_signature_invalid")

        # 5. Untrusted public key / rogue signer rejection (both unspoofed and spoofed publisher pin)
        rogue_sig_doc = rel.sign_catalog_bytes(cat_path.read_bytes(), self.untrusted_key["privateKey"])
        rogue_sig_path = self.root / "rogue.sig.json"
        rogue_sig_path.write_text(json.dumps(rogue_sig_doc, indent=2) + "\n", encoding="utf-8")
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.verify_catalog(
                cat_path,
                rogue_sig_path,
                trusted_key=self.official_key["publicKey"],
                trusted_pin=self.official_key["publisherId"],
                now="2026-11-12T00:00:00Z",
            )
        self.assertEqual(ctx.exception.code, "unknown_catalog_signer")

        spoofed_sig_doc = dict(rogue_sig_doc, publisher=self.official_key["publisherId"])
        spoofed_sig_path = self.root / "spoofed.sig.json"
        spoofed_sig_path.write_text(json.dumps(spoofed_sig_doc, indent=2) + "\n", encoding="utf-8")
        with self.assertRaises(rel.ReleasePipelineError) as ctx:
            rel.verify_catalog(
                cat_path,
                spoofed_sig_path,
                trusted_key=self.official_key["publicKey"],
                trusted_pin=self.official_key["publisherId"],
                now="2026-11-12T00:00:00Z",
            )
        self.assertEqual(ctx.exception.code, "catalog_signature_invalid")

    def test_install_packed_release_into_isolated_package_store(self):
        src_dir, dll_path = self._create_extension_source("store-src", ext_id="org.pulse.signed-demo", title="Signed Demo")
        packed = rel.pack_release(
            src_dir,
            "1.0.0",
            self.official_key["privateKey"],
            self.root / "store-art",
            library_path=dll_path,
        )

        # Install with trusted publisher -> succeeds, enabled, signature & native binary verified.
        trusted_store = self.root / "isolated-trusted-store"
        installed = rel.install_release_archive(
            packed["archivePath"],
            trusted_store,
            signature_path=packed["signaturePath"],
            trusted_publishers=[self.official_key["publicKey"]],
        )
        self.assertTrue(installed["package"]["enabled"])
        self.assertEqual(installed["package"]["id"], "org.pulse.signed-demo")
        rev_sha = installed["package"]["activeRevision"]
        self.assertEqual(rev_sha, packed["entry"]["manifestSha256"])

        stored_bytes, stored_manifest = package_store.revision(trusted_store, rev_sha)
        sig_status = package_store.check_signature(trusted_store, stored_bytes, rev_sha)
        self.assertEqual(
            sig_status,
            {"signed": True, "trusted": True, "publisher": self.official_key["publisherId"]},
        )
        self.assertTrue(package_store.native_binary(trusted_store, stored_manifest, rev_sha).is_file())

        # Install into a store pinning only untrusted_key -> rejected by package_store!
        other_store = self.root / "isolated-other-store"
        with self.assertRaises(package_store.StoreError) as ctx:
            rel.install_release_archive(
                packed["archivePath"],
                other_store,
                signature_path=packed["signaturePath"],
                trusted_publishers=[self.untrusted_key["publicKey"]],
            )
        self.assertEqual(ctx.exception.code, "signature_invalid")
        self.assertEqual(package_store.listing(other_store)["packages"], [])

    def test_sdk_bundle_compiled_lifecycle_helper_in_isolated_temp_dir(self):
        sdk_zip = self.root / "Pulse-NativeSDK-test-windows-x64.zip"
        bundle_native_sdk.bundle(sdk_zip)
        extracted_sdk = test_native_sdk.extract(sdk_zip, self.root / "extracted-sdk")

        examples_catalog = json.loads((extracted_sdk / "examples/native/catalog.json").read_text(encoding="utf-8"))
        projects_root = self.root / "generated-projects"
        projects_root.mkdir()
        generated_projects = []

        import project_creator
        for idx, tpl in enumerate(examples_catalog["templates"]):
            slug = tpl.replace("-", "_")
            pkg_id = f"org.pulse.sdkverify.{slug}"
            dest = projects_root / f"proj-{tpl}"
            created = project_creator.create_project(
                extracted_sdk,
                dest,
                pkg_id,
                f"SDK Verify {tpl}",
                tpl,
            )
            build_dir = dest / "build"
            build_dir.mkdir()
            for t_idx, target_name in enumerate(created["targets"]):
                (build_dir / f"{target_name}.dll").write_bytes(
                    make_synthetic_pe64_dll(f"{tpl}-{idx}-{t_idx}".encode("ascii"))
                )
            generated_projects.append({
                "template": tpl,
                "packageId": pkg_id,
                "path": str(dest),
                "targets": created["targets"],
                "primaryLibrary": created["primaryLibrary"],
            })

        summary = verify_sdk_bundle._verify_compiled_lifecycle(extracted_sdk, generated_projects, self.root)
        self.assertEqual(summary["packedArchiveCount"], 8)
        self.assertEqual(summary["lifecyclePackageId"], "org.pulse.sdkverify.inspector")
        self.assertNotEqual(summary["initialRevision"], summary["upgradedRevision"])
        self.assertGreater(summary["reclaimedBytes"], 0)


if __name__ == "__main__":
    unittest.main()
