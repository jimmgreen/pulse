"""Tests for native_strings.py (strings.tsv -> LocalText header)."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_strings  # noqa: E402

SAMPLE = (
    "# demo\n"
    "key\tzh-CN\tzh-TW\ten\n"
    "MenuTitle\t整理文件\t整理檔案\tTidy up\n"
    "\n"
    "Moved\t已移动 {0} 个文件到 {1}\t已移動 {0} 個檔案到 {1}\tMoved {0} files to {1}\n"
    "OnlyChinese\t仅中文\t\t\n"
    "Tricky\t引号\"和\\\\反斜杠\\t制表\\n换行?\t\tQuote\" back\\\\slash?\n"
)


class ParseTests(unittest.TestCase):
    def test_entries(self):
        entries = native_strings.parse(SAMPLE)
        self.assertEqual([e["key"] for e in entries], ["MenuTitle", "Moved", "OnlyChinese", "Tricky"])
        self.assertEqual(entries[0]["tw"], "整理檔案")
        self.assertEqual(entries[1]["args"], 2)
        self.assertEqual(entries[2]["tw"], "")
        self.assertEqual(entries[3]["zh"], '引号"和\\反斜杠\t制表\n换行?')

    def test_header_optional_and_bom(self):
        entries = native_strings.parse("\ufeffA\t甲\t甲\tA\n")
        self.assertEqual(entries[0]["key"], "A")

    def assert_error(self, text, fragment, line=None):
        with self.assertRaises(native_strings.StringsError) as ctx:
            native_strings.parse(text)
        self.assertIn(fragment, str(ctx.exception))
        if line is not None:
            self.assertEqual(ctx.exception.line, line)

    def test_errors(self):
        self.assert_error("key\tzh\tzh-TW\ten\n", "header row", 1)
        self.assert_error("1abc\t文\t\t\n", "invalid key", 1)
        self.assert_error("class\t文\t\t\n", "reserved", 1)
        self.assert_error("A\t文\t\t\nA\t文\t\t\n", "duplicate key 'A' (first on line 1)", 2)
        self.assert_error("A\t\t繁\tEn\n", "needs Simplified Chinese", 1)
        self.assert_error("A\t文\t\t\textra\n", "too many columns", 1)
        self.assert_error("A\t文 {0}\t\tText\n", "en uses different", 1)
        self.assert_error("A\t文 {0}\t繁 {1}\t\n", "zh-TW uses different", 1)
        self.assert_error("A\t文 {1}\t\t\n", "without gaps", 1)
        self.assert_error("A\t文\\q\t\t\n", "unknown escape", 1)
        self.assert_error("A\t文\\\t\t\n", "dangling backslash", 1)
        self.assert_error("A\t文\\", "dangling backslash", 1)


class GenerateTests(unittest.TestCase):
    def test_header_shape(self):
        header, entries = native_strings.generate(SAMPLE)
        self.assertEqual(len(entries), 4)
        self.assertTrue(header.startswith("// Generated from strings.tsv"))
        self.assertIn("#include <pulse/app.hpp>", header)
        self.assertIn("namespace strings {", header)
        self.assertIn("inline ::pulse::sdk::LocalText MenuTitle() {", header)
        self.assertIn("inline ::pulse::sdk::LocalText Moved(std::string_view a0, std::string_view a1) {", header)
        self.assertIn("::pulse::sdk::Format(", header)
        self.assertIn('"Tidy up"', header)
        self.assertIn('"Quote\\" back\\\\slash\\?"', header)
        # Non-ASCII is escaped: the header itself is pure ASCII outside comments.
        code = "\n".join(line for line in header.splitlines() if not line.startswith("//"))
        self.assertTrue(code.isascii())
        self.assertIn("\\346\\225\\264", code)  # 整
        self.assertIn("// 整理文件", header)

    def test_deterministic(self):
        self.assertEqual(native_strings.generate(SAMPLE)[0], native_strings.generate(SAMPLE)[0])
        crlf = "\ufeff" + SAMPLE.replace("\n", "\r\n")
        self.assertEqual(native_strings.generate(crlf)[0], native_strings.generate(SAMPLE)[0])

    def test_namespace(self):
        header, _ = native_strings.generate(SAMPLE, namespace="demo::text")
        self.assertIn("namespace demo::text {", header)
        with self.assertRaises(native_strings.StringsError):
            native_strings.generate(SAMPLE, namespace="bad name")
        with self.assertRaises(native_strings.StringsError):
            native_strings.generate(SAMPLE, namespace="std")

    def test_comment_cannot_splice(self):
        header, _ = native_strings.generate("A\t结尾反斜杠\\\\\t\t\nB\t乙\t\t\n")
        for line in header.splitlines():
            self.assertFalse(line.endswith("\\"), line)
        self.assertIn("inline ::pulse::sdk::LocalText B()", header)


class FixtureTests(unittest.TestCase):
    def test_app_test_fixture_is_current(self):
        # src/bench/native_sdk/app_strings.gen.hpp is compiled by pulse_native_app_test.
        fixture = Path(__file__).resolve().parents[2] / "src/bench/native_sdk/app_strings.tsv"
        if not fixture.is_file():
            self.skipTest("repository fixture not present (extracted SDK)")
        report = native_strings.sync(fixture, fixture.with_name("app_strings.gen.hpp"), "app_strings", check=True)
        self.assertTrue(report["upToDate"], "regenerate app_strings.gen.hpp with pulse_ext strings")


class SyncTests(unittest.TestCase):
    def test_sync_and_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = Path(tmp)
            (project / "strings.tsv").write_text(SAMPLE, encoding="utf-8")
            first = native_strings.sync(project)
            self.assertTrue(first["written"])
            self.assertEqual(first["keys"], 4)
            self.assertEqual(first["withArguments"], 1)
            self.assertTrue((project / "strings.gen.hpp").is_file())
            again = native_strings.sync(project)
            self.assertTrue(again["upToDate"])
            self.assertFalse(again["written"])
            (project / "strings.tsv").write_text(SAMPLE + "Extra\t额外\t\t\n", encoding="utf-8")
            stale = native_strings.sync(project, check=True)
            self.assertFalse(stale["upToDate"])
            self.assertFalse(stale["written"])

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "text.tsv"
            source.write_text(SAMPLE, encoding="utf-8")
            out = Path(tmp) / "include" / "text.hpp"
            script = Path(native_strings.__file__)
            done = subprocess.run([sys.executable, str(script), str(source), "--output", str(out), "--check"],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 1)
            done = subprocess.run([sys.executable, str(script), str(source), "--output", str(out)],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertTrue(json.loads(done.stdout)["written"])
            source.write_bytes(b"A\t\xff\t\t\n")
            done = subprocess.run([sys.executable, str(script), str(source)], capture_output=True, text=True)
            self.assertEqual(done.returncode, 2)
            self.assertIn("not UTF-8", json.loads(done.stdout)["error"])


if __name__ == "__main__":
    unittest.main()
