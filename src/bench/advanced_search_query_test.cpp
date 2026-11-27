#include "../ui/advanced_search_dialog.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/localization.h"
#include <cstdio>
#include <set>
#include <filesystem>

namespace pulse::ui {
AdvancedSearchDialogResult TestAdvancedSearchQuery(app::AdvancedSearchSpec spec, int field,
                                                  const std::wstring& value);
// The query fixture never invokes a folder picker or accesses folders.
bool ShowFolderPicker(HWND, const FolderPickerSpec&, bool, D2D1_COLOR_F, std::wstring&) {
    std::abort();
}
}
namespace {
struct File { const wchar_t* name; uint64_t size; WORD year; };
const File files[] = {{L"alpha-keep.txt",21,2020}, {L"alpha-draft.txt",31,2020},
    {L"beta-keep.txt",41,2020}, {L"beta-old.txt",51,2019}, {L"中文报告.txt",61,2020}};
std::set<std::wstring> Results(const std::wstring& raw) {
    const auto query = pulse::index::ParseQuery(raw);
    std::set<std::wstring> out;
    for (const auto& file : files) {
        SYSTEMTIME date{}; date.wYear = file.year; date.wMonth = 6; date.wDay = 15;
        FILETIME ft{}; SystemTimeToFileTime(&date, &ft);
        const auto modified = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        for (const auto& group : query.groups) {
            bool match = true;
            for (const auto& term : group) {
                const auto length = static_cast<uint32_t>(wcslen(file.name));
                match = match && !term.folder &&
                    pulse::index::MatchName(file.name, length, term) != term.name_not &&
                    pulse::index::MatchExt(file.name, length, term) != term.ext_not &&
                    pulse::index::MatchSize(file.size, term) != term.size_not &&
                    pulse::index::MatchDate(modified, term) != term.date_not;
            }
            if (match) { out.insert(file.name); break; }
        }
    }
    return out;
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    puts("[STAGE] COM initialization");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    puts("[STAGE] localization initialization");
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    puts("[STAGE] dialog tests");
    int failures = 0;
    const auto check = [&](bool pass, const char* name) {
        printf("[%s] %s\n", pass ? "PASS" : "FAIL", name); failures += !pass;
    };
    const std::wstring raw = L"alpha !draft dm:2020 | beta dm:2020";
    const std::set<std::wstring> expected{L"alpha-keep.txt", L"beta-keep.txt"};
    check(Results(raw) == expected, "native query fixture has exact two-file result set");
    for (int field = 0; field <= 3; ++field) {
        auto spec = pulse::app::ParseSearchQuery(raw, L"C:\\private-memory-fixture");
        const auto value = field == 1 ? L"alpha beta" : field == 2 ? L"txt" : L"old";
        const auto result = pulse::ui::TestAdvancedSearchQuery(spec, field, value);
        check(result.accepted, "real hidden advanced dialog creates editors and accepts through WM_COMMAND");
        if (field == 0) {
            check(result.query == raw, "unedited real form preserves native query byte for byte");
            check(Results(result.query) == expected, "unedited confirmation retains negation/date/OR result set");
        } else if (field == 1) {
            check(Results(result.query) == expected, "setting same displayed name retains branch negation and dates");
        } else if (field == 2) {
            check(Results(result.query) == expected, "extension-only edit preserves negation/date/OR result set");
        } else {
            check(Results(result.query) == std::set<std::wstring>{L"alpha-keep.txt", L"alpha-draft.txt", L"beta-keep.txt"},
                  "explicit exclusion edit replaces negation while preserving date and OR branches");
        }
        std::wprintf(L"[QUERY] field=%d output=%ls\n", field, result.query.c_str());
    }
    for (const auto* query : {L"alpha !draft ext:txt dm:2020 | beta !old ext:txt dm:2020",
                              L"content:hello !content:noise ext:txt alpha !draft"}) {
        const auto result = pulse::ui::TestAdvancedSearchQuery(pulse::app::ParseSearchQuery(query, L"C:\\private-memory-fixture"), 0, L"");
        check(result.accepted && result.query == query, "untouched custom extensions and hidden exclusions survive actual editor initialization");
    }
    {
        const auto result = pulse::ui::TestAdvancedSearchQuery({}, 4, LR"(^(alpha|beta)-keep\.txt$)");
        check(result.accepted && Results(result.query) == expected,
              "selecting Regex in real form matches both alternation branches");
        const auto reopened = pulse::ui::TestAdvancedSearchQuery(pulse::app::ParseSearchQuery(result.query), 0, L"");
        check(reopened.accepted && reopened.query == result.query,
              "reopening real form retains regex mode and pattern");
        const auto invalid = pulse::ui::TestAdvancedSearchQuery({}, 4, L"[");
        check(!invalid.accepted, "invalid regex blocks real form submission");
        const auto recovered = pulse::ui::TestAdvancedSearchQuery({}, 5, L"[");
        check(recovered.accepted && Results(recovered.query).size() == 2,
              "correcting invalid regex re-enables real form submission");
        auto spec = pulse::app::ParseSearchQuery(result.query);
        const auto edited = pulse::ui::TestAdvancedSearchQuery(spec, 2, L"pdf");
        check(edited.accepted && Results(edited.query).empty(),
              "real regex form honors extension edits");
    }
    for (int field = 10; field < 16; ++field) {
        pulse::app::AdvancedSearchSpec spec;
        spec.name_how = pulse::app::NameMatchHow::Regex;
        spec.name = field == 15 ? L"[" : LR"(^aa[([]2022[)\]]xx.*$)";
        pulse::l10n::SetLanguage(field == 13 ? L"en-US" : field == 14 ? L"zh-TW" : L"zh-CN");
        const auto folder = std::filesystem::current_path() / L"bench_data" / L"advanced-search-regex-20261124";
        std::filesystem::create_directories(folder);
        const auto path = folder / (L"regex-ui-" + std::to_wstring(field) + L".png");
        const auto result = pulse::ui::TestAdvancedSearchQuery(spec, field, path.wstring());
        check(result.accepted, "regex selected pill fits without overlap; real D2D frame captured (theme/DPI/language)");
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
