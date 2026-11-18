#include "../app/listing_selection_restore.h"
#include <cstdio>
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
int main() {
    using namespace pulse;
    int failures = 0;
    for (const auto* path : {L"C:\\fixture", L"pulse:recycle", L"pulse:search", L"pulse:starred"}) {
        for (int mode = 0; mode < 4; ++mode) {
            app::Tab tab; tab.current_path = path;
            auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
            for (int i = 0; i < 3; ++i) {
                auto& e = (*entries)[i];
                e.name = fs::IsVirtualPath(path) ? L"same.txt" : std::to_wstring(i);
                e.full_path = L"C:\\original\\" + std::to_wstring(i);
                if (tab.current_path == L"pulse:recycle") {
                    e.full_path = L"C:\\original\\same.txt";
                    e.recycle_path = L"C:\\$Recycle.Bin\\" + std::to_wstring(i);
                }
            }
            tab.SetSnapshot(entries); tab.SelectIndices({0, 1});
            app::CapturePendingListingSelection(tab);
            if (mode == 0) tab.SelectOnly(2);
            if (mode == 1) tab.ClearSelection();
            if (mode == 3) {
                tab.pending_selected_names = {L"arrival"};
                tab.pending_selected_name = L"arrival";
                tab.pending_selection_revision = tab.selection_revision;
            }
            auto restore = app::TakeListingSelection(tab, path);
            auto fresh = std::make_shared<std::vector<fs::DirEntry>>(*entries);
            std::swap((*fresh)[0], (*fresh)[2]);
            if (mode == 3) (*fresh)[0].name = L"arrival";
            tab.SetSnapshot(fresh); app::RestoreListingSelection(tab, restore);
            const auto selected = tab.SelectedIndices();
            bool ok = (mode == 0 || mode == 3) ? selected.size() == 1 && selected[0] == 0 :
                mode == 1 ? selected.empty() : selected.size() == 2 && !tab.IsSelected(0);
            std::printf("[%s] %ls mode=%d\n", ok ? "PASS" : "FAIL", path, mode);
            failures += !ok;
        }
    }
    return failures ? 1 : 0;
}
