// ai_search_spec_test.cpp - Regression tests for Batch 2: full-text content
// search in search_files / run_task.find (SearchSpecFromArgs, CompileSearchQuery,
// SearchChips, ClearSearchField) + real local document matching via ReadSearchableDocument.
#include "../app/ai_search_spec.h"
#include "../app/search_query.h"
#include "../common/json_value.h"
#include "../index/document_reader.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace pulse;
using namespace pulse::app;
using namespace pulse::app::ai;

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}

json::Value Obj(std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value v = json::Value::Object();
    for (const auto& [k, val] : kv) v.set(k, val);
    return v;
}

bool WriteText(const std::wstring& path, const std::string& utf8) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
    return ok && written == utf8.size();
}

} // namespace

int main() {
    // 1. Content-only search spec (all words default)
    {
        AdvancedSearchSpec spec;
        ToolOutcome err;
        const bool ok = SearchSpecFromArgs(
            Obj({{"content", "\xE6\x8A\xA5\xE4\xBB\xB7\xE5\x8D\x95 \xE5\x90\x88\xE5\x90\x8C"},
                 {"type", "document"},
                 {"modified_after", "2026-10-01"}}),
            spec, err);
        Check(ok && spec.content == L"\u62A5\u4EF7\u5355 \u5408\u540C" &&
                  spec.content_mode == index::ContentMatchMode::AllWords,
              "SearchSpecFromArgs parses content + type + date");
        const std::wstring compiled = CompileSearchQuery(spec);
        Check(compiled.find(L"content:\u62A5\u4EF7\u5355") != std::wstring::npos &&
                  compiled.find(L"content:\u5408\u540C") != std::wstring::npos &&
                  compiled.find(L"type:document") != std::wstring::npos,
              "CompileSearchQuery emits content: clauses");

        const auto chips = SearchChips(spec);
        bool has_content_chip = false;
        for (const auto& ch : chips)
            if (ch.field == SearchField::Content) has_content_chip = true;
        Check(has_content_chip, "SearchChips includes Content chip");

        ClearSearchField(spec, SearchField::Content);
        Check(spec.content.empty() && SearchHasCondition(spec), "ClearSearchField clears content while keeping type/date");
    }

    // 2. Phrase mode + content_exclude
    {
        AdvancedSearchSpec spec;
        ToolOutcome err;
        const bool ok = SearchSpecFromArgs(
            Obj({{"content", "quarterly budget"},
                 {"content_mode", "phrase"},
                 {"content_exclude", "draft"}}),
            spec, err);
        Check(ok && spec.content_mode == index::ContentMatchMode::Phrase && spec.content_exclude == L"draft",
              "SearchSpecFromArgs parses phrase mode and content_exclude");
        const std::wstring q = CompileSearchQuery(spec);
        Check(q.find(L"contentmode:phrase") != std::wstring::npos && q.find(L"!content:draft") != std::wstring::npos,
              "CompileSearchQuery emits contentmode:phrase and !content:");
    }

    // 3. Any mode
    {
        AdvancedSearchSpec spec;
        ToolOutcome err;
        const bool ok = SearchSpecFromArgs(Obj({{"content", "alpha beta"}, {"content_mode", "any"}}), spec, err);
        Check(ok && spec.content_mode == index::ContentMatchMode::AnyWord &&
                  CompileSearchQuery(spec).find(L"contentmode:any") != std::wstring::npos,
              "SearchSpecFromArgs parses any mode");
    }

    // 4. Validation rejections
    {
        AdvancedSearchSpec spec;
        ToolOutcome err;
        Check(!SearchSpecFromArgs(Obj({{"content", "abc"}, {"type", "folder"}}), spec, err) &&
                  err.error.code == "invalid_args",
              "content + type:folder rejected");
        Check(!SearchSpecFromArgs(Obj({{"content", "abc"}, {"content_mode", "regex"}}), spec, err) &&
                  err.error.code == "invalid_args",
              "invalid content_mode rejected");
        Check(!SearchSpecFromArgs(Obj({{"content", "a\"b"}}), spec, err) && err.error.code == "invalid_args",
              "quotes in content rejected");
    }

    // 5. Real-machine isolated file content extraction & match verification
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path root =
            std::filesystem::path(exe).parent_path().parent_path() / L"bench_data" /
            (L"aisearchspec-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        const std::wstring match_file = (root / L"q4_quote.md").wstring();
        const std::wstring miss_file = (root / L"notes.txt").wstring();
        Check(WriteText(match_file, "# Q4 Summary\nFinal \xE6\x8A\xA5\xE4\xBB\xB7\xE5\x8D\x95 approved for client."),
              "isolated fixture match file created");
        Check(WriteText(miss_file, "Unrelated meeting minutes."), "isolated fixture non-match file created");

        std::wstring text1, text2;
        uint64_t bytes1 = 0, bytes2 = 0;
        DWORD err1 = 0, err2 = 0;
        Check(index::ReadSearchableDocument(match_file, 1024 * 1024, text1, bytes1, &err1) &&
                  text1.find(L"\u62A5\u4EF7\u5355") != std::wstring::npos,
              "ReadSearchableDocument reads UTF-8 Chinese text from real fixture");
        Check(index::ReadSearchableDocument(miss_file, 1024 * 1024, text2, bytes2, &err2) &&
                  text2.find(L"\u62A5\u4EF7\u5355") == std::wstring::npos,
              "ReadSearchableDocument does not falsely match non-matching fixture");

        std::filesystem::remove_all(root, ec);
    }

    std::printf("Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
