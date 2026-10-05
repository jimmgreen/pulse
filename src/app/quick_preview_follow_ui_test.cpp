// quick_preview_follow_ui_test.cpp — Behaviour coverage for the quick preview
// following the focused listing row (#91). Drives a real AppState with a real
// snapshot so SyncQuickPreviewSelection is exercised through the same path the
// render loop uses.
#include "app_internal.h"
#include "app_commands.h"
#include "app_runtime.h"
#include "places.h"
#include "../fs/fs_enum.h"
#include <fstream>
#include <filesystem>

#ifdef PULSE_WITH_SELFTEST
using namespace pulse;
void Render(pulse::AppState&);

namespace pulse::app {
// Writes `count` tiny files into a scratch folder and returns it.
static std::wstring MakeFollowFixture(int count) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::filesystem::path dir = std::filesystem::path(temp) / L"pulse_follow_fixture";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    for (int i = 0; i < count; ++i) {
        wchar_t name[64]{};
        swprintf_s(name, L"follow-%02d.txt", i);
        std::ofstream out(dir / name, std::ios::binary);
        out << "pulse quick preview follow fixture " << i << "\n";
    }
    return dir.wstring();
}

// Two previewable files plus a directory, listed as the focused tab.
static void SeedListing(app::Tab& tab, const std::wstring& dir) {
    tab.current_path = dir;
    tab.loading = false;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 2; ++i) {
        fs::DirEntry e;
        wchar_t name[64]{};
        swprintf_s(name, L"follow-%02d.txt", i);
        e.name = name;
        e.size = 64;
        e.mtime.dwLowDateTime = 1000 + i;
        e.attrs = FILE_ATTRIBUTE_ARCHIVE;
        entries->push_back(e);
    }
    fs::DirEntry folder;
    folder.name = L"subfolder";
    folder.is_dir = true;
    folder.attrs = FILE_ATTRIBUTE_DIRECTORY;
    entries->push_back(folder);
    tab.snapshot = entries;
    tab.selected_index = 0;
}
} // namespace pulse::app

int RunQuickPreviewFollowTest(AppState& s, const wchar_t* output) {
    using namespace pulse::app;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };

    s.appPrefs.persist = false;
    s.quickPreviewAnchorView = -1;
    const std::wstring dir = MakeFollowFixture(2);
    auto* tab = ActiveTab(s);
    if (!tab) {
        log << "[FAIL] no active tab\n";
        return 1;
    }
    SeedListing(*tab, dir);
    const std::wstring first = EntryFullPath(*tab, 0);
    const std::wstring second = EntryFullPath(*tab, 1);

    ToggleQuickPreview(s);
    check(s.quickPreview.visible(), "quick preview opens for the focused row");
    check(s.quickPreview.item().path == first, "quick preview shows the initially selected file");

    // The row moves; the next painted frame must retarget the preview. This
    // goes through Render() so the production wiring (app_main.cpp) is what is
    // under test, not just the helper.
    tab->SelectOnly(1);
    Render(s);
    check(s.quickPreview.item().path == second, "selecting another row retargets the preview");
    check(s.quickPreview.visible(), "following the selection keeps the window open");

    // Same row again: no reload churn (holding an arrow key must not restart
    // playback every frame).
    const auto shown = s.quickPreview.item();
    SyncQuickPreviewSelection(s);
    check(s.quickPreview.item().path == shown.path &&
          s.quickPreview.item().modified == shown.modified &&
          s.quickPreview.item().size == shown.size,
        "re-running on the unchanged row keeps the shown entry untouched");

    // A folder in a normal location is previewable too: it lists its contents,
    // so the preview retargets to the folder.
    tab->SelectOnly(2);
    SyncQuickPreviewSelection(s);
    const std::wstring folder = EntryFullPath(*tab, 2);
    check(s.quickPreview.visible() && s.quickPreview.item().path == folder,
        "a folder row in a normal location retargets the preview to that folder");

    // The recycle bin never previews folder rows, so the window must hold the
    // last good entry instead of blanking or closing.
    tab->current_path = MakeRecyclePath();
    tab->SelectOnly(2);
    SyncQuickPreviewSelection(s);
    check(s.quickPreview.visible() && s.quickPreview.item().path == folder,
        "an unpreviewable recycle-bin row keeps the previous preview open");

    s.quickPreview.Close();
    check(!s.quickPreview.visible(), "preview closes cleanly at the end of the case");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
#endif // PULSE_WITH_SELFTEST
