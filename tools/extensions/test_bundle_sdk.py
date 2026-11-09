import hashlib
import io
import json
import re
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import bundle_sdk as sdk


class BundleSdkTests(unittest.TestCase):
    def test_native_directory_permissions_are_additive_and_match_host(self):
        schema = json.loads((sdk.ROOT / 'schemas/extensions/manifest.schema.json').read_text(encoding='utf-8'))
        resources = schema['properties']['resources']
        old_permissions = ['context.read', 'files.readCurrentDirectory', 'operations.own', 'settings.own', 'audit.own', 'files.renameCurrentDirectory']
        added_permissions = ['files.readDirectory', 'files.watchDirectory', 'files.open', 'context.events', 'files.transferCurrentSelection', 'files.deleteCurrentSelection', 'files.transferToDirectory', 'files.transferFromDirectory', 'files.transferDirectoryTree']
        self.assertEqual(resources['items']['enum'], old_permissions + added_permissions)
        self.assertEqual(resources['maxItems'], 15)
        self.assertTrue(resources['uniqueItems'])
        header = (sdk.ROOT / 'src/app/extensions/manifest.h').read_text(encoding='utf-8')
        source = (sdk.ROOT / 'src/app/extensions/manifest.cpp').read_text(encoding='utf-8')
        maximum = int(re.search(r'kMaxResources\s*=\s*(\d+)', header).group(1))
        allowed = re.search(r'constexpr std::string_view allowed\[\]\s*=\s*\{([^}]+)\}', source).group(1)
        self.assertEqual(maximum, resources['maxItems'])
        self.assertEqual(re.findall(r'"([^"]+)"', allowed), resources['items']['enum'])
        metadata = json.loads((sdk.ROOT / 'schemas/extensions/api_metadata.json').read_text(encoding='utf-8'))
        operations = metadata['resourceApi']['operations']
        self.assertEqual({operation['resourceDeclaration'] for operation in operations}, set(old_permissions))
        requests = json.loads((sdk.ROOT / 'schemas/extensions/resource-request.schema.json').read_text(encoding='utf-8'))
        self.assertEqual([operation['op'] for operation in operations], [branch['properties']['op']['const'] for branch in requests['oneOf']])
        self.assertIn('sdk/native/README.md', sdk.FIXED_FILES)
        self.assertIn('sdk/native/README.md', sdk.FIXED_FILES)
        self.assertIn('sdk/native/README.md', sdk.FIXED_FILES)

    def test_reproducible_bytes_and_inventory(self):
        one, release = sdk.archive_bytes({'b.txt': b'b', 'a.txt': b'a'}, {'formatVersion': 1})
        two, _ = sdk.archive_bytes({'a.txt': b'a', 'b.txt': b'b'}, {'formatVersion': 1})
        self.assertEqual(one, two)
        self.assertEqual(release['files'][0], {'path': 'a.txt', 'bytes': 1, 'sha256': hashlib.sha256(b'a').hexdigest()})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sdk.zip'
            path.write_bytes(one)
            verified, files = sdk.verify_archive(path)
            self.assertEqual(verified, release)
            self.assertEqual(files, {'a.txt': b'a', 'b.txt': b'b'})

    def test_windows_aliases_and_traversal_rejected(self):
        for name in ('../escape', '/absolute', 'a\\b', 'C:x', 'a//b', 'a/../b', 'NUL.txt',
                     'dir/COM1', 'dir/COM¹', 'name.', 'name ', 'a:b', 'a\x00b', 'file?'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                sdk.archive_bytes({name: b'x'}, {'formatVersion': 1})
        with self.assertRaises(ValueError):
            sdk.archive_bytes({'README': b'a', 'readme': b'b'}, {})
        with self.assertRaises(ValueError):
            sdk.archive_bytes({'SDK-RELEASE.JSON': b'x'}, {})

    def test_offline_docs_preserve_public_links_and_mark_repository_references(self):
        files = {'docs/guide.md': b'[API](api.json) [Code](../src/model.h)', 'docs/api.json': b'{}'}
        sdk.portable_documents(files)
        self.assertIn(b'[API](api.json)', files['docs/guide.md'])
        self.assertIn(b'source repository: `src/model.h`', files['docs/guide.md'])
        with self.assertRaises(ValueError):
            sdk.portable_documents({'README.md': b'[Missing](missing.md)'})

    def test_repository_sdk_documentation_links_are_complete(self):
        files = {name: (sdk.ROOT / name).read_bytes() for name in sdk.FIXED_FILES}
        files['README.md'] = sdk.root_readme(files['docs/extensions-sdk-quickstart.md'])
        for name in sdk.template_catalog.names():
            relative = 'examples/native/' + name + '/manifest.json'
            files[relative] = (sdk.ROOT / relative).read_bytes()
        sdk.portable_documents(files)
        self.assertTrue(files['sdk/native/README.md'])
        self.assertIn(b'](sdk/native/README.md)', files['README.md'])

    def test_non_native_preview_is_rejected(self):
        with patch.object(sdk, 'regular_bytes', return_value=b'not a PE image'), \
                patch.object(sdk.template_catalog, 'names', return_value=()):
            with self.assertRaises(ValueError):
                sdk.collect(Path('fixture'), Path('preview.exe'))

    def test_declarative_bundle_has_no_script_engine_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "Pulse.ExtensionPreview.exe"
            image = bytearray(128)
            image[:2] = b"MZ"
            image[60:64] = (64).to_bytes(4, "little")
            image[64:70] = b"PE\x00\x00\x64\x86"
            preview.write_bytes(image)
            (root / "lumatext.dll").write_bytes(image)
            notices = root / "licenses/LumaText"
            notices.mkdir(parents=True)
            (notices / "LICENSE").write_text("fixture", encoding="utf-8")
            (root / "docs").mkdir()
            (root / "docs/extensions-sdk-quickstart.md").write_text("SDK", encoding="utf-8")
            with patch.object(sdk, "FIXED_FILES", ("docs/extensions-sdk-quickstart.md",)), \
                    patch.object(sdk.template_catalog, "names", return_value=()), \
                    patch.object(sdk, "root_readme", return_value=b"SDK"), patch.object(sdk, "portable_documents"):
                files = sdk.collect(root, preview)
            self.assertIn("bin/Pulse.ExtensionPreview.exe", files)
            self.assertNotIn("bin/Pulse.ExtensionHost.exe", files)
            self.assertFalse(any("quickjs" in name.lower() for name in files))

    def test_archive_budgets(self):
        with patch.object(sdk, 'MAX_FILES', 1), self.assertRaises(ValueError):
            sdk.archive_bytes({'a': b'', 'b': b''}, {})
        with patch.object(sdk, 'MAX_TOTAL', 2), self.assertRaises(ValueError):
            sdk.archive_bytes({'a': b'abc'}, {})
        with patch.object(sdk, 'MAX_FILE', 2), self.assertRaises(ValueError):
            sdk.archive_bytes({'a': b'abc'}, {})

    def test_bounded_read_and_reparse_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input'
            path.write_bytes(b'1234')
            with self.assertRaises(ValueError):
                sdk.regular_bytes(path, 3)
            self.assertEqual(sdk.regular_bytes(path, 4), b'1234')
            class Reparse:
                st_mode = 0o100644
                st_file_attributes = 0x400
            with patch.object(Path, 'lstat', return_value=Reparse()), self.assertRaises(ValueError):
                sdk.regular_bytes(path)

    def test_existing_output_refused_before_host_execution(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(sdk.pulse_ext, 'host_call') as host:
            with self.assertRaises(ValueError):
                sdk.build(Path('missing.exe'), Path('missing-preview.exe'), Path(directory))
            host.assert_not_called()

    def test_tampering_and_unlisted_members_rejected(self):
        raw, _ = sdk.archive_bytes({'a.txt': b'original'}, {'formatVersion': 1})
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            release = archive.read('sdk-release.json')
        for payload in ({'a.txt': b'changed'}, {'a.txt': b'original', 'extra': b'x'}):
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'bad.zip'
                with zipfile.ZipFile(path, 'w') as archive:
                    archive.writestr('sdk-release.json', release)
                    for name, value in payload.items():
                        archive.writestr(name, value)
                with self.assertRaises(ValueError):
                    sdk.verify_archive(path)

    def test_oversized_directory_rejected_before_zip_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'many.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                for i in range(sdk.MAX_FILES + 2):
                    archive.writestr(str(i), b'')
            with patch.object(sdk.zipfile, 'ZipFile', side_effect=AssertionError('must reject before allocating entries')):
                with self.assertRaises(ValueError):
                    sdk.verify_archive(path)


if __name__ == '__main__':
    unittest.main()
