// ai_rich_text_test.cpp — Pulse 助手's rich answers (ui/ai_rich_text.h):
// Markdown blocks, local path chips, and what a selection copies.
#include "../ui/ai_rich_text.h"

#include <cstdio>
#include <string>

using namespace pulse::ui::airich;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("  failed: %s\n", what);
    }
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) {
        if (c == L'\n') out += "\\n";
        else if (c == L'\t') out += "\\t";
        else if (c < 0x80) out.push_back(static_cast<char>(c));
        else out += "?";
    }
    return out;
}

void Same(const std::wstring& got, const std::wstring& want, const char* what) {
    if (got == want) return;
    ++failures;
    std::printf("  failed: %s\n    got  \"%s\"\n    want \"%s\"\n", what, Narrow(got).c_str(), Narrow(want).c_str());
}

const Run* RunWith(const Block& b, uint32_t flag) {
    for (const Run& r : b.runs)
        if (r.flags & flag) return &r;
    return nullptr;
}

void Blocks() {
    const Doc d = Parse(L"## 结果\n\n第一段 **加粗** 和 `code`。\n\n- 一\n- 二\n  1. 甲\n  2. 乙\n\n> 引用\n\n---\n\n"
                        L"```powershell\nGet-ChildItem\nWrite-Host 1\n```\n");
    Check(d.blocks.size() == 9, "nine leaf blocks");
    if (d.blocks.size() < 9) return;
    Check(d.blocks[0].kind == Kind::Heading && d.blocks[0].level == 2, "heading level 2");
    Same(d.blocks[1].text, L"第一段 加粗 和 code。", "paragraph text without markers");
    Check(RunWith(d.blocks[1], kBold) != nullptr, "bold run");
    Check(RunWith(d.blocks[1], kCode) != nullptr, "code run");
    Check(d.blocks[2].marker == L"u" && d.blocks[2].indent == 1, "bullet item");
    Check(d.blocks[4].marker == L"o1" && d.blocks[4].indent == 2, "nested ordered item");
    Check(d.blocks[6].quote == 1, "block quote");
    Check(d.blocks[7].kind == Kind::Rule, "rule");
    Check(d.blocks[8].kind == Kind::Code && d.blocks[8].lang == L"powershell", "code block with language");
    Same(d.blocks[8].text, L"Get-ChildItem\nWrite-Host 1", "code text without the closing new line");
}

void Rules() {
    const Doc d = Parse(L"上面\n\n---\n\n下面\n");
    Same(CopyAll(d, false), L"上面\n\n下面", "a rule leaves one gap in plain text");
    Same(CopyAll(d, true), L"上面\n\n---\n\n下面", "a rule stays in Markdown");
}

void Streaming() {
    const Doc d = Parse(L"看这里：\n\n```python\nprint(1)\nprint(");
    Check(d.blocks.size() == 2 && d.blocks[1].kind == Kind::Code, "an open fence is a code block");
    if (d.blocks.size() == 2) Same(d.blocks[1].text, L"print(1)\nprint(", "open fence keeps its text");
}

void Tables() {
    const Doc d = Parse(L"| 盘 | 可用 |\n|---|---:|\n| C: | 38 GB |\n| D: | 120 GB |\n");
    Check(d.tables.size() == 1, "one table");
    if (d.tables.size() != 1) return;
    Check(d.tables[0].columns == 2 && d.tables[0].rows == 3, "2 columns, 3 rows");
    Check(d.tables[0].count == 6, "six cells");
    Check(d.blocks[0].header && !d.blocks[2].header, "header row");
    Check(d.blocks[3].col == 1 && d.blocks[3].row == 1 && d.blocks[3].align == L'r', "cell position and alignment");
    Same(TableTsv(d, 0), L"盘\t可用\nC:\t38 GB\nD:\t120 GB", "table as TSV");
    Same(CopyAll(d, true), L"| 盘 | 可用 |\n| --- | --- |\n| C: | 38 GB |\n| D: | 120 GB |", "table as Markdown");
    Same(CopyAll(d, false), L"盘\t可用\nC:\t38 GB\nD:\t120 GB", "table as plain text");
}

void Paths() {
    Doc d = Parse(L"已放到 C:\\Users\\Ann\\Downloads\\报告.pdf，另见 `D:\\Work Files\\a b` 和 `\\\\nas\\share\\x`。");
    Check(d.blocks.size() == 1, "one paragraph");
    if (d.blocks.empty()) return;
    int paths = 0;
    for (const Run& r : d.blocks[0].runs)
        if (r.flags & kPath) ++paths;
    Check(paths == 3, "three path runs");
    const Run* first = RunWith(d.blocks[0], kPath);
    if (first) Same(first->target, L"C:\\Users\\Ann\\Downloads\\报告.pdf", "path stops at ，");
    bool spaced = false;
    for (const Run& r : d.blocks[0].runs)
        if ((r.flags & kPath) && r.target == L"D:\\Work Files\\a b") spaced = !(r.flags & kCode);
    Check(spaced, "a code span that is a path becomes a path (spaces kept)");
    Doc u = Parse(L"see http://x.com/C:\\no and abc:\\no");
    Check(RunWith(u.blocks[0], kPath) == nullptr, "no path inside words or links");
    Check(LooksLikePath(L"C:\\") && !LooksLikePath(L"C:") && !LooksLikePath(L"\\\\nas"), "LooksLikePath");
    Doc end = Parse(L"打开 C:\\Temp.");
    const Run* t = RunWith(end.blocks[0], kPath);
    if (t) Same(t->target, L"C:\\Temp", "a sentence's full stop is not part of the path");
    else Check(false, "path before a full stop");
}

void Copying() {
    const Doc d = Parse(L"## 标题\n\n正文 **重点** 见 [文档](https://example.com/a,b)。\n\n- 第一\n- 第二\n");
    Same(CopyAll(d, false), L"标题\n\n正文 重点 见 文档。\n\n- 第一\n- 第二", "plain copy of the whole answer");
    Same(CopyAll(d, true), L"## 标题\n\n正文 **重点** 见 [文档](https://example.com/a,b)。\n\n- 第一\n- 第二",
         "Markdown copy of the whole answer");
    // Inside one paragraph: just the characters, no marker.
    Same(CopyRange(d, {1, 3}, {1, 5}, false), L"重点", "partial plain copy");
    Same(CopyRange(d, {1, 3}, {1, 5}, true), L"**重点**", "partial Markdown copy keeps the emphasis");
    Same(CopyRange(d, {2, 1}, {2, 2}, false), L"一", "inside a list item: no marker");
    Same(CopyRange(d, {1, 8}, {3, 2}, false), L"文档。\n\n- 第一\n- 第二", "across blocks: markers kept");
    Same(CopyRange(d, {1, 5}, {1, 3}, false), L"", "a reversed range is empty");
    const Doc c = Parse(L"```js\nlet a = 1;\n```");
    Same(CopyAll(c, true), L"```js\nlet a = 1;\n```", "code block as Markdown");
    Same(CopyAll(c, false), L"let a = 1;", "code block as plain text");
    Same(ToPlain(L"**粗** 和 *斜*"), L"粗 和 斜", "ToPlain");
}

void Languages() {
    Same(ExtensionForLanguage(L"PowerShell"), L".ps1", "powershell");
    Same(ExtensionForLanguage(L"c++"), L".cpp", "c++");
    Same(ExtensionForLanguage(L"python {linenos}"), L".py", "attributes after the name");
    Same(ExtensionForLanguage(L""), L"", "no language");
}

} // namespace

int main() {
    Blocks();
    Rules();
    Streaming();
    Tables();
    Paths();
    Copying();
    Languages();
    if (failures) {
        std::printf("[FAIL] ai_rich_text_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("[PASS] ai_rich_text_test\n");
    return 0;
}
