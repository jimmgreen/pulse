#include "../common/delete_diagnostics.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>

struct Event { std::string name; std::map<std::string, uint64_t> fields; };
static std::vector<Event> events;
namespace pulse::diagnostics::runtime {
bool Enabled() noexcept { return true; }
void Event(const char* name, std::initializer_list<Field> fields, Level) noexcept {
    ::Event event{name, {}};
    for (const auto& field : fields) event.fields[field.name] = field.value;
    events.push_back(std::move(event));
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::diagnostics::deletion;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check(Key(L"\\\\?\\C:\\Folder\\FILE.txt") == Key(L"c:/folder/file.txt"), "extended path and case correlation");
    check(Parent(Key(L"C:\\file.txt")) == Key(L"C:\\"), "drive root correlation");
    check(Key(L"\\\\?\\UNC\\server\\share\\folder") == Key(L"\\\\server\\share\\folder\\"), "UNC correlation");
    History bounded;
    for (size_t i = 0; i < kHistoryLimit + 1; ++i) bounded.Add(i, 0, L"C:\\test\\file", 100);
    auto records = bounded.ForDirectory(L"c:\\TEST", 100);
    check(records.size() == kHistoryLimit && records.front().task == 1, "bounded retained history");
    check(bounded.ForDirectory(L"C:\\other", 100).empty(), "unrelated directory excluded");
    check(bounded.ForDirectory(L"C:\\test", kLifetimeMs + 101).empty(), "history expires");
    History concurrent;
    std::thread writer([&] { for (unsigned i = 0; i < 100; ++i) concurrent.Add(i, 0, L"C:\\test\\file", 1); });
    for (unsigned i = 0; i < 100; ++i) concurrent.ForDirectory(L"C:\\test", 1);
    writer.join();
    check(concurrent.ForDirectory(L"C:\\test", 1).size() == 100, "worker and UI history synchronization");
    if (argc != 2) return 2;
    const auto root = std::filesystem::path(argv[1]) / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto source = (root / L"sample.txt").wstring();
    const auto create = [&] {
        const HANDLE h = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h); return true;
    };
    check(create(), "create isolated file");
    Item(1, 0, source, E_ACCESSDENIED, false, false, false);
    check(events.back().fields.at("source_state") == 1 && history.ForDirectory(root.wstring(), GetTickCount64()).empty(),
          "failed deletion records present without confirmed tracking");
    check(SourceState(INVALID_FILE_ATTRIBUTES, ERROR_ACCESS_DENIED) == 2 &&
          SourceState(INVALID_FILE_ATTRIBUTES, ERROR_BAD_NETPATH) == 2 &&
          SourceState(INVALID_FILE_ATTRIBUTES, ERROR_INVALID_NAME) == 2,
          "access denied, network failure and invalid name are unknown rather than missing");
    check(DeleteFileW(source.c_str()) != FALSE, "delete isolated file");
    Item(2, 0, source, S_OK, true, false, true);
    check(events.back().fields.at("source_state") == 0 && events.back().fields.at("probe_error") == ERROR_FILE_NOT_FOUND,
          "successful deletion records missing and exact Win32 result");
    struct Entry { std::wstring name; };
    Snapshot(root.wstring(), std::vector<Entry>{}, 10, 0);
    check(events.back().fields.at("present") == 0 && events.back().fields.at("task") == 2, "worker snapshot links deleted item");
    Snapshot(root.wstring(), std::vector<Entry>{{L"sample.txt"}}, 9, 1, 123);
    check(events.back().fields.at("present") == 1 && events.back().fields.at("generation") == 9 &&
          events.back().fields.at("window") == 123, "stale applied snapshot identifies generation and window");
    check(create(), "recreate same file name");
    Snapshot(root.wstring(), std::vector<Entry>{{L"sample.txt"}}, 11, 0);
    check(events.back().fields.at("present") == 1, "subsequent presence remains visible to diagnostics");
    const auto count = events.size();
    Item(3, kItemLimit, source, S_OK, true, false, true);
    check(events.size() == count, "per-task item logging cap");
    std::filesystem::remove_all(root);
    std::cout << "RESULT failures=" << failures << '\n';
    return failures ? 1 : 0;
}
