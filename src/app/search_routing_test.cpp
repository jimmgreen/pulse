#ifdef PULSE_WITH_SELFTEST
// search_routing_test.cpp - Multi-window M5a: results of the shared search and
// change services reach the window whose tab waits for them. The host hands
// those results to the most recent window, which need not be the one that
// searched; before M5a an older window's search lost its results that way.
#include "app_internal.h"
#include <filesystem>
#include <fstream>
#include <memory>

namespace pulse::app {
namespace {
using PendingSearch = decltype(AppCore::pendingIndexSearches)::mapped_type;

void AddPendingSearch(AppCore& core, uint32_t id) {
    PendingSearch pending;
    pending.query.limit = 50;
    pending.network_expected = false;
    pending.recycle_ready = true;
    pending.network_ready = true;  // local results alone complete the request
    core.pendingIndexSearches[id] = std::move(pending);
}
}  // namespace

bool RunSearchRoutingTest() {
    const auto output = std::filesystem::absolute(L"bench_data/search-routing");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    bool ok = log.good();
    const auto check = [&](bool value, const char* label) {
        log << (value ? "[PASS] " : "[FAIL] ") << label << std::endl;
        ok &= value;
        return value;
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseSearchRoutingTest";
    RegisterClassW(&wc);
    const auto make_window = [&] {
        return CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 200, 200,
                               nullptr, nullptr, wc.hInstance, nullptr);
    };
    auto core = std::make_unique<AppCore>();
    auto older = std::make_unique<AppState>(*core);
    auto recent = std::make_unique<AppState>(*core);
    older->hwnd = make_window();
    recent->hwnd = make_window();
    if (check(older->hwnd && recent->hwnd, "two hidden windows share one core")) {
        older->window_tabs.NewTab(L"C:\\");
        recent->window_tabs.NewTab(L"C:\\");
        core->windows.Add(older->hwnd, older.get());
        core->windows.Add(recent->hwnd, recent.get());
        core->windows.Touch(recent->hwnd);
        Tab* search = older->window_tabs.Active()->panes.front()->ActiveTab();
        search->current_path = L"pulse:search:report";

        // The M4 path: the host's window takes the result for itself.
        AddPendingSearch(*core, 7);
        search->pending_generation = 7;
        AcceptIndexProviderResult(*recent, 7, index::SearchResult{}, false);
        check(!search->search_entries && !core->pendingIndexSearches.contains(7),
              "delivered only to the most recent window, the older window's search result is consumed and lost");

        AddPendingSearch(*core, 8);
        search->pending_generation = 8;
        AppState& owner = SearchResultWindow(*recent, 8);
        check(&owner == older.get(), "an index result finds the older window whose tab waits for it");
        AcceptIndexProviderResult(owner, 8, index::SearchResult{}, false);
        check(search->search_entries != nullptr && !core->pendingIndexSearches.contains(8),
              "routed to that window, the result fills its search tab");

        search->pending_generation = 0;
        search->filename_live_generation = 9;
        check(&SearchResultWindow(*recent, 9) == older.get(), "a live filename refresh finds its tab too");
        search->filename_live_generation = 0;
        older->paletteSearchId = 31;
        check(&SearchResultWindow(*recent, 31) == older.get(), "a palette search finds the window that opened the palette");
        older->paletteSearchId = 0;
        check(&SearchResultWindow(*recent, 999) == recent.get() && &SearchResultWindow(*recent, 0) == recent.get(),
              "a result no window waits for stays with the receiving window");

        search->search_live_generation = 77;
        check(&ContentSearchWindow(*recent, 77) == older.get(), "a content search update finds the window running it");
        search->search_live_generation = 0;
        search->current_path = L"C:\\Windows";
        search->pending_generation = 78;
        check(&ContentSearchWindow(*recent, 78) == recent.get(),
              "a folder load with the same number is not taken for a content search");
        search->pending_generation = 0;

        core->changes.details[41] = ChangeDetailPending{search, search->current_path, search->view_generation, false};
        check(&ChangeResultWindow(*recent, 41) == older.get(), "change details find the window showing the change view");
        check(&ChangeResultWindow(*recent, 42) == recent.get(), "an unknown change request stays with the receiving window");
        core->changes.details.clear();

        core->windows.Remove(recent->hwnd);
        core->windows.Remove(older->hwnd);
    }
    for (HWND* hwnd : {&older->hwnd, &recent->hwnd}) {
        if (*hwnd) DestroyWindow(*hwnd);
        *hwnd = nullptr;
    }
    recent.reset();
    older.reset();
    core.reset();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
}  // namespace pulse::app
#endif
