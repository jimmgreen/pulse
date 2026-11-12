import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('adapter', Path(__file__).with_name('prepare_lumatext_x86.py'))
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)

class PreparationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / 'upstream'
        self.source.mkdir()
        for part in adapter.PARTS:
            if part in ('CMakeLists.txt', 'LICENSE'):
                (self.source / part).write_text(adapter.GUARD if part == 'CMakeLists.txt' else 'license', encoding='utf-8')
            else:
                (self.source / part).mkdir()
                (self.source / part / 'fixture').write_text(part, encoding='utf-8')
        self.before = {str(p.relative_to(self.source)): p.read_bytes() for p in self.source.rglob('*') if p.is_file()}
    def tearDown(self):
        self.temp.cleanup()
    def test_only_guard_changes_and_source_preserved(self):
        output = self.root / 'copy'
        adapter.prepare(self.source, output)
        manifest = json.loads((output / 'x86-source.json').read_text())
        self.assertEqual([x['path'] for x in manifest['files'] if x['source_sha256'] != x['copy_sha256']], ['CMakeLists.txt'])
        for relative, original in self.before.items():
            self.assertEqual((self.source / relative).read_bytes(), original)
        self.assertIn(adapter.PORT, (output / 'CMakeLists.txt').read_text())
        self.assertIn('-if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)', (output / 'x86-source.patch').read_text())
        adapter.rollback(output)
        self.assertEqual((output / 'CMakeLists.txt').read_bytes(), self.before['CMakeLists.txt'])
    def test_changed_guard_is_not_silently_ported(self):
        (self.source / 'CMakeLists.txt').write_text('changed upstream')
        with self.assertRaisesRegex(ValueError, 'guard changed'):
            adapter.prepare(self.source, self.root / 'copy')
    def test_existing_copy_is_preserved(self):
        output = self.root / 'copy'
        output.mkdir()
        marker = output / 'marker'
        marker.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'NEW output'):
            adapter.prepare(self.source, output)
        self.assertEqual(marker.read_bytes(), b'keep')
    def test_never_write_into_external_source(self):
        with self.assertRaisesRegex(ValueError, 'NEW output'):
            adapter.prepare(self.source, self.source / 'copy')
    def test_crlf_is_preserved(self):
        original = adapter.GUARD.replace('\n', '\r\n').encode()
        (self.source / 'CMakeLists.txt').write_bytes(original)
        output = self.root / 'copy'
        adapter.prepare(self.source, output)
        self.assertEqual((output / 'CMakeLists.txt').read_bytes(), adapter.PORT.replace('\n', '\r\n').encode())
        adapter.rollback(output)
        self.assertEqual((output / 'CMakeLists.txt').read_bytes(), original)

if __name__ == '__main__':
    unittest.main()
