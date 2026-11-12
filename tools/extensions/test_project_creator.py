"""Unit tests for the unified full-project Native SDK generator (project_creator.py)."""
import json
from pathlib import Path
import shutil
import tempfile
import unittest

import project_creator
import template_catalog

ROOT = Path(__file__).resolve().parents[2]


class ProjectCreatorTests(unittest.TestCase):
    def test_template_enumeration_excludes_basic_and_matches_catalog(self):
        expected = template_catalog.names(ROOT / "examples/native")
        self.assertEqual(len(expected), 9)
        self.assertNotIn("basic", expected)
        self.assertNotIn("dock-panel", expected)
        self.assertEqual(project_creator.templates(ROOT), expected)
        self.assertEqual(project_creator.abi_samples(ROOT), ("basic",))

    def test_create_project_renders_all_eight_packable_templates(self):
        with tempfile.TemporaryDirectory() as directory:
            out_root = Path(directory)
            for idx, template in enumerate(project_creator.templates(ROOT)):
                pkg_id = f"com.acme.plugin-{idx}"
                disp_name = f"Acme Plugin {template} 中文"
                dest = out_root / template
                created = project_creator.create_project(ROOT, dest, pkg_id, disp_name, template)
                self.assertEqual(created["packageId"], pkg_id)
                self.assertEqual(created["displayName"], disp_name)
                self.assertEqual(created["template"], template)
                self.assertTrue((dest / "plugin.cpp").is_file())

                manifest = json.loads((dest / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["id"], pkg_id)
                self.assertEqual(manifest["name"], disp_name)
                self.assertEqual(manifest["contributes"], {"commands": []})
                self.assertNotIn("native", manifest)

                cmake = (dest / "CMakeLists.txt").read_text(encoding="utf-8")
                stem = created["libraryStem"]
                self.assertIn(f"project({project_creator.cmake_project_identifier(pkg_id)} LANGUAGES CXX)", cmake)
                if template == "rename-panel":
                    self.assertEqual(created["targets"], [stem + "_lower", stem + "_upper"])
                    self.assertEqual(created["primaryLibrary"], stem + "_lower.dll")
                    self.assertIn(f"add_library({stem}_${{variant}} SHARED", cmake)
                    self.assertNotIn("pulse_rename_${variant}", cmake)
                else:
                    self.assertEqual(created["targets"], [stem])
                    self.assertEqual(created["primaryLibrary"], stem + ".dll")
                    self.assertIn(f"add_library({stem} SHARED", cmake)

                readme = (dest / "README.md").read_text(encoding="utf-8")
                self.assertIn(pkg_id, readme)
                self.assertIn(disp_name, readme)
                self.assertIn(created["primaryLibrary"], readme)
                self.assertIn("stop-host", readme)

    def test_rejects_basic_and_unknown_templates(self):
        with tempfile.TemporaryDirectory() as directory:
            dest = Path(directory) / "proj"
            with self.assertRaises(project_creator.ProjectCreationError) as basic_err:
                project_creator.create_project(ROOT, dest, "com.acme.test", "Test", "basic")
            self.assertEqual(basic_err.exception.code, "template_not_packable")
            self.assertFalse(dest.exists())

            with self.assertRaises(project_creator.ProjectCreationError) as dock_err:
                project_creator.create_project(ROOT, dest, "com.acme.test", "Test", "dock-panel")
            self.assertEqual(dock_err.exception.code, "unknown_template")
            self.assertFalse(dest.exists())

    def test_validates_package_id_and_blocks_reserved_sample_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for bad_id, expected_code in (
                ("", "invalid_id"),
                ("a" * 65, "invalid_id"),
                ("bad/slash", "invalid_id"),
                ("bad space", "invalid_id"),
                ("中文.id", "invalid_id"),
                ("CON.plugin", "invalid_id"),
                ("lpt1.plugin", "invalid_id"),
                ("sample.native-rename", "reserved_id"),
                ("sample.custom-plugin", "reserved_id"),
                ("pulse.native-sdk", "reserved_id"),
            ):
                with self.subTest(bad_id=bad_id):
                    with self.assertRaises(project_creator.ProjectCreationError) as caught:
                        project_creator.create_project(ROOT, base / "out", bad_id, "Valid Name", "inspector")
                    self.assertEqual(caught.exception.code, expected_code)
                    self.assertFalse((base / "out").exists())

    def test_validates_display_name(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for bad_name in ("", "   ", "Line\nBreak", "Null\x00Char", "汉" * 45):
                with self.subTest(bad_name=bad_name):
                    with self.assertRaises(project_creator.ProjectCreationError) as caught:
                        project_creator.create_project(ROOT, base / "out", "com.acme.valid", bad_name, "inspector")
                    self.assertEqual(caught.exception.code, "invalid_name")

    def test_derive_package_id_produces_distinct_non_sample_identities(self):
        first = project_creator.derive_package_id("rename-alpha")
        second = project_creator.derive_package_id("rename-beta")
        self.assertEqual(first, "local.rename-alpha")
        self.assertEqual(second, "local.rename-beta")
        self.assertNotEqual(first, second)
        self.assertEqual(project_creator.derive_display_name("rename-alpha"), "Rename Alpha")

    def test_rejects_existing_destination_and_pre_validate_failure_leaves_no_files(self):
        with tempfile.TemporaryDirectory() as directory:
            existing = Path(directory) / "existing"
            existing.mkdir()
            (existing / "keep.txt").write_text("keep", encoding="utf-8")
            with self.assertRaises(project_creator.ProjectCreationError) as exists_err:
                project_creator.create_project(ROOT, existing, "com.acme.one", "One", "inspector")
            self.assertEqual(exists_err.exception.code, "destination_exists")
            self.assertEqual((existing / "keep.txt").read_text(encoding="utf-8"), "keep")

            aborted = Path(directory) / "aborted"
            def fail_validate(_manifest_bytes):
                raise RuntimeError("host rejected")
            with self.assertRaisesRegex(RuntimeError, "host rejected"):
                project_creator.create_project(
                    ROOT, aborted, "com.acme.two", "Two", "inspector", pre_create_validate=fail_validate
                )
            self.assertFalse(aborted.exists())

    def test_rejects_symlinks_in_destination_ancestors_and_template(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real_dir = root / "real"
            real_dir.mkdir()
            link_dir = root / "link"
            try:
                link_dir.symlink_to(real_dir, target_is_directory=True)
            except OSError as exc:
                self.skipTest(f"Symlink creation unavailable: {exc}")

            with self.assertRaises(project_creator.ProjectCreationError) as link_err:
                project_creator.create_project(ROOT, link_dir / "child", "com.acme.link", "Link", "inspector")
            self.assertEqual(link_err.exception.code, "symlink_rejected")

            # Also verify a symlink inside a template directory is rejected.
            fake_sdk = root / "fake-sdk"
            (fake_sdk / "examples/native/inspector").mkdir(parents=True)
            shutil.copyfile(ROOT / "examples/native/catalog.json", fake_sdk / "examples/native/catalog.json")
            for member in ("CMakeLists.txt", "manifest.json", "plugin.cpp", "README.md"):
                shutil.copyfile(ROOT / "examples/native/inspector" / member, fake_sdk / "examples/native/inspector" / member)
            outside = root / "secret.txt"
            outside.write_text("secret", encoding="utf-8")
            (fake_sdk / "examples/native/inspector/leak.txt").symlink_to(outside)
            with self.assertRaises(project_creator.ProjectCreationError) as tpl_err:
                project_creator.create_project(fake_sdk, root / "clean-dest", "com.acme.tpl", "Tpl", "inspector")
            self.assertEqual(tpl_err.exception.code, "symlink_rejected")
            self.assertFalse((root / "clean-dest").exists())


if __name__ == "__main__":
    unittest.main()
