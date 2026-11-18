import json
from pathlib import Path
import tempfile
import unittest
import template_catalog as catalog


class TemplateCatalogTests(unittest.TestCase):
    def fixture(self, root, names):
        (root / "catalog.json").write_text(json.dumps({"formatVersion": 1, "templates": names}), encoding="utf-8")

    def test_repository_catalog_matches_all_inputs(self):
        values = catalog.all_templates()
        self.assertEqual(tuple(values), catalog.names())
        self.assertEqual(len(values), len(catalog.names()))
        self.assertIn("inspector", catalog.names())
        self.assertIn("directory-transfer", values)
        self.assertTrue(all(isinstance(value, dict) for value in values.values()))

    def test_duplicate_and_path_identifiers_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for names in (["x", "x"], ["../outside"], ["C:\\outside"], [""], ["中文"], ["x" * 65], []):
                self.fixture(root, names)
                with self.subTest(names=names), self.assertRaises(ValueError):
                    catalog.names(root)

    def test_unknown_template_never_opens_a_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, ["valid"])
            with self.assertRaisesRegex(ValueError, "Unknown template"):
                catalog.load("../outside", root)

    def test_catalog_version_and_unknown_fields_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for value in ({"formatVersion": True, "templates": ["x"]}, {"formatVersion": 1, "templates": ["x"], "execute": True}):
                (root / "catalog.json").write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(ValueError):
                    catalog.names(root)

    def test_duplicate_json_keys_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            path.write_text('{"formatVersion":1,"formatVersion":1,"templates":["x"]}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                catalog.names(path.parent)

    def test_manifest_bytes_and_object_type_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root, ["x"])
            (root / "x").mkdir()
            path = root / "x/manifest.json"
            for raw in (b" " * 65537, b"[]", b'{"x":NaN}'):
                path.write_bytes(raw)
                with self.assertRaises(ValueError):
                    catalog.load("x", root)


if __name__ == "__main__":
    unittest.main()
