#include "../app/search_history.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <cstdio>

// Production resolves this through session.cpp; tests never access user settings.
namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int wmain() {
    using pulse::app::SearchHistory;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    SearchHistory history;
    check(!history.Record(L" \t ", L"pulse:search:x"), "empty query ignored");
    check(!history.Record(L"x", L"C:\\x") &&
          !history.Record(L"x", L"pulse:search: ") &&
          !history.Record(std::wstring(4097, L'x'), L"pulse:search:x") &&
          !history.Record(L"x", L"pulse:search:" + std::wstring(32768, L'x')),
          "invalid and oversized input rejected");
    check(history.Record(L"  合同  ", L"pulse:search:合同 path:\"C:\\文档\"") &&
          history.entries.front().query == L"合同", "trim and Unicode");
    history.Record(L"Budget", L"pulse:search:budget path:C:\\Docs");
    history.Record(L"BUDGET", L"pulse:search:BUDGET path:c:\\docs");
    check(history.entries.size() == 2 && history.entries.front().query == L"BUDGET",
          "case insensitive duplicate updates spelling");
    history.Record(L"Budget", L"pulse:search:budget path:D:\\Docs");
    check(history.entries.size() == 3, "distinct scope retained");
    SearchHistory parsed;
    check(parsed.FromJson(history.ToJson()) && parsed.ToJson() == history.ToJson(),
          "JSON round trip");
    const auto before = parsed.ToJson();
    check(!parsed.FromJson(L"{\"version\":1,\"entries\":[{\"query\":\"x\",\"path\":\"C:\\\\x\"}]}") &&
          !parsed.FromJson(history.ToJson() + L"junk") &&
          !parsed.FromJson(L"{\"version\":1,\"entries\":[{\"query\":\"unterminated") &&
          parsed.ToJson() == before, "corruption rejected without losing entries");
    for (int i = 0; i < 60; ++i)
        history.Record(std::to_wstring(i), L"pulse:search:" + std::to_wstring(i));
    check(history.entries.size() == 50 && history.entries.front().query == L"59" &&
          history.entries.back().query == L"10", "capacity and recency");
    check(history.Remove(L"pulse:search:59") && !history.Remove(L"pulse:search:59") &&
          history.entries.size() == 49, "remove");
    wchar_t temp[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    const bool allocated = GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"psh", 0, file);
    check(allocated, "isolated temporary fixture");
    if (allocated) {
        {
            pulse::app::SearchHistoryWriter writer(file);
            SearchHistory changing;
            for (int i = 0; i < 200; ++i) {
                const auto query = std::to_wstring(i);
                const auto path = L"pulse:search:" + query;
                changing.Record(query, path);
                writer.Submit(changing);
                changing.Remove(path);
                writer.Submit(changing);
            }
            changing.Record(L"last", L"pulse:search:last");
            writer.Submit(changing);
            writer.Flush();
            check(parsed.LoadFromFile(file) && parsed.entries.size() == 1 &&
                  parsed.entries.front().query == L"last", "writer flush latest snapshot");
            changing.Clear();
            writer.Submit(changing);
            writer.Stop();
            check(parsed.LoadFromFile(file) && parsed.entries.empty(), "writer stop persists final clear");
        }
        {
            pulse::app::SearchHistoryWriter writer(file);
            writer.Submit(history);
        }
        check(parsed.LoadFromFile(file) && parsed.ToJson() == history.ToJson(),
              "writer destructor drains pending save");
        check(history.SaveToFile(file) && parsed.LoadFromFile(file) &&
              parsed.ToJson() == history.ToJson(), "file round trip");
        check(history.Clear() && !history.Clear() && history.SaveToFile(file) &&
              parsed.LoadFromFile(file) && parsed.entries.empty(), "clear persisted");
        history.persist = false;
        check(!history.SaveToFile(file) && !history.LoadFromFile(file), "persistence disabled");
        pulse::WriteUtf8FileAtomic(file, L"{broken");
        check(!parsed.LoadFromFile(file), "corrupt file rejected");
        HANDLE large = CreateFileW(file, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (large != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER end{};
            end.QuadPart = 8 * 1024 * 1024 + 1;
            const bool sized = SetFilePointerEx(large, end, nullptr, FILE_BEGIN) && SetEndOfFile(large);
            CloseHandle(large);
            check(sized && !parsed.LoadFromFile(file), "oversized file rejected");
        } else check(false, "oversized fixture opened");
        DeleteFileW(file);
        DeleteFileW((std::wstring(file) + L".tmp").c_str());
    }
    return failures ? 1 : 0;
}
