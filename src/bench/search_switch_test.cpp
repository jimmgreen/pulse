// Search master switch: the machine index config keeps the switch, and the
// toolbar drops the search field (only the folder filter remains) when it is off.
#include "../index/index_config.h"
#include "../ui/toolbar_layout.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const char* what, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)

bool WriteText(const std::wstring& path, const char* text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(strlen(text));
    const bool ok = WriteFile(file, text, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}

bool LoadSwitch(const std::wstring& path, const char* json, bool& enabled) {
    if (json && !WriteText(path, json)) return false;
    pulse::index::IndexConfig config;
    if (!pulse::index::LoadIndexConfigFrom(path, L"C:\\PulseIndexTest", config)) return false;
    enabled = config.search_enabled;
    return true;
}

void ConfigTests() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"pulse_search_switch_test_" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\config.json";
    bool enabled = false;
    // No config file yet: search stays on.
    CHECK(LoadSwitch(path, nullptr, enabled) && enabled);
    // Configs written before the switch existed: on.
    CHECK(LoadSwitch(path, "{\n  \"version\":1,\n  \"generation\":3\n}\n", enabled) && enabled);
    CHECK(LoadSwitch(path, "{\n  \"version\":1,\n  \"generation\":3,\n  \"search_enabled\":false\n}\n",
                     enabled) && !enabled);
    CHECK(LoadSwitch(path, "{\n  \"version\":1,\n  \"generation\":4,\n  \"search_enabled\":true\n}\n",
                     enabled) && enabled);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(dir.c_str());
}

void ToolbarTests() {
    const float widths[] = {1600.0f, 1000.0f, 620.0f, 500.0f, 420.0f};
    const float scales[] = {1.0f, 1.25f, 2.0f};
    for (float scale : scales) {
        for (float dip : widths) {
            const float w = dip * scale, margin = 8.0f * scale, create = 84.0f * scale;
            const auto shown = pulse::ui::MakeToolbarLayout(w, scale, 0.0f, margin, create, 0.0f, 0.0f, 0.0f);
            const auto hidden = pulse::ui::MakeToolbarLayout(w, scale, 0.0f, margin, create, 0.0f, 0.0f, -1.0f);
            CHECK(shown.search.right - shown.search.left >= 31.0f * scale);
            // Off: no field and no narrow-window search button either.
            CHECK(hidden.search.right - hidden.search.left <= 0.5f);
            // The address bar takes the room the field used.
            CHECK(hidden.address_text.right > shown.address_text.right);
            CHECK(hidden.address_text.right <= hidden.navigation[3].left);
            CHECK(hidden.navigation[3].right <= w - margin + 0.5f);
            // Everything else in the toolbar is unchanged.
            CHECK(hidden.create.left == shown.create.left && hidden.create.right == shown.create.right);
        }
    }
    // A results page asks for a wider field; hidden still wins.
    const auto editing = pulse::ui::MakeToolbarLayout(1400.0f, 1.0f, 0.0f, 8.0f, 84.0f, 0.0f, 0.0f, 520.0f);
    CHECK(editing.search.right - editing.search.left >= 500.0f);
    for (float scale : scales) {
        for (float width : {320.0f, 459.0f, 460.0f, 599.0f, 600.0f, 759.0f, 760.0f, 1100.0f}) {
            const auto layout = pulse::ui::MakeResponsiveNavigation(width * scale, 0, 0, 8 * scale, scale, false, 0);
            CHECK(layout.merged == (width < 460));
            CHECK(layout.refresh_inside == (width < 760));
            CHECK((layout.navigation[1].right > layout.navigation[1].left) == (width >= 600));
            CHECK(layout.address_text.right <= layout.navigation[3].left);
            CHECK(layout.navigation[3].right <= width * scale - 8 * scale);
            if (width >= 460 && width < 600) CHECK(layout.search.right - layout.search.left == 112 * scale);
            if (width >= 600 && width < 760) {
                CHECK(layout.search.right - layout.search.left >= 180 * scale);
                CHECK(layout.search.right - layout.search.left <= 260 * scale);
            }
            const auto query = pulse::ui::MakeResponsiveNavigation(width * scale, 0, 0, 8 * scale, scale, false, 520);
            CHECK(query.search.left >= query.navigation[2].right);
            CHECK(query.search.right > query.search.left);
            if (width < 460) {
                CHECK(query.address.right == query.address.left);
                CHECK(query.navigation[3].right == query.navigation[3].left);
                CHECK(query.search.right - query.search.left >= 200 * scale);
            }
        }
    }
}

} // namespace

int wmain() {
    ConfigTests();
    ToolbarTests();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
