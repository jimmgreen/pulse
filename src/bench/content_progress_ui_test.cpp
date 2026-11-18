#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/content_results_ui.h"
#include "../app/app_navigation.h"
#include "../app/update_status.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>
#include <cmath>
#include "history_reference_probe.inc"
#include "operation_flight_probe.inc"
#include "pane_layout_probe.inc"
#include "title_layout_probe.inc"
#include "drag_reference_probe.inc"

int RunContentProgressUiTest(int argc, char** argv) {
    using namespace pulse;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    int failures=0;
    auto check=[&](bool ok,const char* label) {
        std::cout<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;
        if(!ok) ++failures;
    };
    auto state=std::make_unique<AppState>(); auto& s=*state;
    s.isolatedTest=true; s.appPrefs.persist=false; s.searchHistory.persist=false;
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=DefWindowProcW;
    wc.lpszClassName=L"PulseContentProgressFixture"; RegisterClassW(&wc);
    s.hwnd=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1100,720,nullptr,nullptr,wc.hInstance,nullptr);
    if(!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    auto status_content_right = [&](const ui::WindowViewModel& vm, const D2D1_RECT_F& rect, float scale) {
        return vm.layout_controls.visible
            ? std::max(rect.left, ui::PaneLayoutStripBounds(vm, rect, scale, s.compositor.DwriteFactory(), s.compositor.SmallFormat()).left - 4 * scale)
            : rect.right;
    };
    auto progress_span = [&](const ui::StatusBarMetrics& metrics, const D2D1_RECT_F& rect, float scale) {
        const float right = std::max(rect.left, metrics.content_right - metrics.pad);
        const float left = std::min(right, metrics.ai_chip.right > metrics.ai_chip.left
            ? metrics.ai_chip.right + 12 * scale : rect.left + metrics.pad);
        return D2D1::RectF(left, metrics.bar.top, right, metrics.bar.bottom);
    };
    s.window_tabs.EnsureDefault(); s.pane=s.window_tabs.Active()->FocusedPane();
    auto* tab=ActiveTab(s);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TITLE_LAYOUT", nullptr, 0)) return RunTitleLayoutProbe(s);
    if (GetEnvironmentVariableW(L"PULSE_TEST_PANE_LAYOUT", nullptr, 0)) return RunPaneLayoutProbe(s);
    if (GetEnvironmentVariableW(L"PULSE_TEST_OPERATION_FLIGHT", nullptr, 0)) return RunOperationFlightProbe(s);
    if (argc > 1 && std::string_view(argv[1]) == "--history-reference-only") return RunHistoryReferenceProbe(s);
    if (argc > 1 && std::string_view(argv[1]) == "--drag-reference-only") {
        return RunDragReferenceProbe(s);
    }
    tab->current_path=app::MakeSearchPath(L"content:季度预算");
    tab->view_mode=ui::ViewMode::Details;
    auto store=std::make_shared<index::ContentResultStore>(nullptr,0);
    index::ContentHit hit{L"C:\\Fixture\\季度预算.txt",L"季度预算.txt",L"季度预算评审会议记录",120,0,1}; hit.file_id=1;
    uint64_t generation=8100;
    auto start=[&] {tab->pending_generation=++generation;tab->search_content_active=true;tab->search_content_stopped=false;tab->content_count_final=false;};
    auto update=[&](uint64_t scanned,uint64_t total,bool done=false,DWORD error=0,bool truncated=false) {
        index::ContentSearchUpdate result; result.progress.generation=generation;
        result.progress.scanned_files=scanned; result.progress.total_files=total;
        result.progress.done=done; result.progress.error=error; result.progress.truncated=truncated;
        result.results=store; ApplyContentSearchUpdate(s,std::move(result));
    };
    if (argc > 1 && std::string_view(argv[1]) == "--selection-only") {
        auto a=hit,b=hit,c=hit;
        a.path=L"C:\\Fixture\\b.txt"; a.name=L"b.txt"; a.file_id=11;
        b.path=L"C:\\Fixture\\c.txt"; b.name=L"c.txt"; b.file_id=12;
        c.path=L"C:\\Fixture\\a.txt"; c.name=L"a.txt"; c.file_id=13;
        start();
        store->StreamUpsert({a,b},index::ContentResultSort::Name,false); update(2,0);
        RefreshContentResults(s);
        index::ContentResultStore::Row row;
        const auto until=GetTickCount64()+3000;
        while(!store->Get(0,row) && GetTickCount64()<until) Sleep(1);
        RefreshContentResults(s); tab->SelectIndices({0,1});
        check(tab->content_selected_paths.size()==2,"selection records both stable paths before streamed insertion");
        store->StreamUpsert({c},index::ContentResultSort::Name,false);
        RefreshContentResults(s);
        const auto restored=GetTickCount64()+3000;
        while(tab->SelectedCount()!=2 && GetTickCount64()<restored) { Sleep(1); RefreshContentResults(s); }
        check(tab->SelectedCount()==2 && tab->IsSelected(1) && tab->IsSelected(2) && !tab->IsSelected(0),
              "streaming insertion restores multiselection to original files");
        check(tab->selected_index==1,"streaming insertion preserves keyboard focus identity");
        bool acted=false, correct=false;
        DeferContentSelection(s,[&](AppState&) {
            acted=true;
            correct=tab->EntryAt(1).full_path==a.path && tab->EntryAt(2).full_path==b.path;
        });
        const auto action_due=GetTickCount64()+3000;
        while(!acted && GetTickCount64()<action_due) { Sleep(1); CompleteContentSelection(s); }
        check(acted && correct,"deferred operation resolves only original selected file identities");
        tab->content_results.reset(); tab->search_content_active=false;
        tab->current_path=app::MakeSearchPath(L"fixture");
        tab->pending_generation=990; tab->filename_live_generation=990;
        tab->pending_search_offset=0; tab->search_entries.reset();
        AppState::PendingIndexSearch pending; pending.query.subscribe=true;
        pending.query.limit=2048; pending.network_ready=false;
        s.pendingIndexSearches.emplace(990,std::move(pending));
        index::SearchResult local; local.total=1;
        index::Hit local_hit; local_hit.path=a.path; local_hit.name=a.name; local.hits.push_back(local_hit);
        AcceptIndexProviderResult(s,990,std::move(local),false);
        check(tab->EntryCount()==1 && !tab->loading,"local filename hits display while network reply is pending");
        index::SearchResult network; network.error=ERROR_CONNECTION_ABORTED;
        AcceptIndexProviderResult(s,990,std::move(network),true);
        check(tab->EntryCount()==1 && !tab->banner_title.empty(),"network failure preserves local hits and displays incomplete status");
        app::SavedSearch saved; saved.name=L"fixture saved"; saved.query=L"fixture"; saved.root=L"C:\\Fixture";
        check(s.savedSearches.Add(saved),"saved filename query fixture is in memory only");
        tab->current_path=L"pulse:saved-search:0";
        tab->search_total=2200; tab->search_next_offset=2048;
        tab->search_loading_more=false; tab->search_content_stopped=false; tab->search_awaiting_content=false;
        tab->loading=false; tab->search_session_id=9911;
        MaybePrefetchSearchPage(s);
        const auto page=s.pendingIndexSearches.find(static_cast<uint32_t>(tab->pending_generation));
        const bool requested=page!=s.pendingIndexSearches.end() && page->second.query.limit>2048 &&
            page->second.query.path_prefix==saved.root && page->second.query.needle==saved.query;
        check(requested,"saved filename search requests next window and preserves root and query");
        if (requested) {
            const auto id=static_cast<uint32_t>(tab->pending_generation);
            index::SearchResult next; next.total=2200;
            for (int i=0;i<2200;++i) { index::Hit item; item.name=L"fixture"+std::to_wstring(i); item.path=saved.root+L"\\"+item.name; next.hits.push_back(std::move(item)); }
            AcceptIndexProviderResult(s,id,std::move(next),false);
            check(tab->EntryCount()==2200 && tab->search_next_offset==2200,"saved filename search displays hits beyond initial 2048");
        }
        const auto previous_request=tab->pending_generation;
        RefreshActiveTab(s,RefreshReason::Explicit);
        const auto refresh=s.pendingIndexSearches.find(static_cast<uint32_t>(tab->pending_generation));
        check(tab->pending_generation!=previous_request && refresh!=s.pendingIndexSearches.end() &&
              refresh->second.query.path_prefix==saved.root,
              "explicit saved-search refresh dispatches a new query for the same root");
        tab->current_path=L"C:\\Fixture";
        tab->pending_generation=0; tab->banner_title.clear();
        app::WorkResult foreign; foreign.path=tab->current_path; foreign.generation=991; foreign.error=true;
        ApplyWorkerResult(s,foreign);
        check(tab->banner_title.empty(),"completed pane ignores another pane's same-path failure");
        tab->pending_generation=992;
        ApplyWorkerResult(s,foreign);
        check(tab->pending_generation==992 && tab->banner_title.empty(),"pending pane ignores another request generation");
        foreign.generation=992; ApplyWorkerResult(s,foreign);
        check(tab->pending_generation==0 && !tab->banner_title.empty(),"own request failure still completes pane");
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd=nullptr;
        CoUninitialize(); return failures ? 1:0;
    }
    const bool pill_150 = argc > 1 && std::string_view(argv[1]) == "--operation-pill-150";
    const bool pill_only = pill_150 || (argc > 1 && std::string_view(argv[1]) == "--operation-pill-only");
    const bool update_only = argc > 1 && std::string_view(argv[1]) == "--update-only";
    auto check_status_layout_bounds = [&] {
        for (float probe_scale : {1.0f, 1.5f}) {
            s.compositor.RecreateTextFormats(probe_scale);
            for (int width : {0, 16, 32, 64, 96, 160, 360}) for (bool ai : {false, true}) for (bool query : {false, true}) {
                ui::WindowViewModel probe;
                probe.status.ai_state = ai ? ui::StatusAiState::Idle : ui::StatusAiState::Hidden;
                probe.status.ai_label = L"AI";
                probe.status.query_active = probe.status.query_cancellable = query;
                probe.status.query_text = L"Search progress";
                probe.status.task_is_update = !query;
                probe.status.task_text = L"Download progress";
                const auto bounds = D2D1::RectF(0, 0, width * probe_scale, 720 * probe_scale);
                const auto metrics = ui::MakeStatusBarMetrics(probe, bounds, probe_scale, 24 * probe_scale,
                    s.compositor.DwriteFactory(), s.compositor.SmallFormat());
                const auto span = progress_span(metrics, bounds, probe_scale);
                const float end = query ? metrics.cancel_search.right : metrics.task.right;
                bool fits = metrics.content_right >= bounds.left && metrics.content_right <= bounds.right &&
                    std::isfinite(metrics.task.left) && std::isfinite(metrics.task.right) &&
                    metrics.task.left >= span.left && metrics.task.right >= metrics.task.left &&
                    end <= span.right && std::abs((metrics.task.left + end - span.left - span.right) * .5f) < .1f;
                if (query) {
                    fits &= metrics.cancel_search.left >= metrics.task.right &&
                        metrics.cancel_search.right >= metrics.cancel_search.left;
                    if (metrics.cancel_search.right > metrics.cancel_search.left) {
                        fits &= ui::StatusBarHitRegion(probe, bounds,
                            (metrics.cancel_search.left + metrics.cancel_search.right) * .5f,
                            (metrics.cancel_search.top + metrics.cancel_search.bottom) * .5f,
                            probe_scale, 24 * probe_scale, &s.compositor) == ui::HitTestResult::StatusBarCancelSearch;
                    }
                }
                if (!fits) std::cout << "    width=" << width << " scale=" << probe_scale << " ai=" << ai << " query=" << query
                    << " span=" << span.left << ":" << span.right << " task=" << metrics.task.left << ":" << metrics.task.right
                    << " end=" << end << std::endl;
                check(fits, "progress fits between AI and layout controls without negative or overlapping targets");
            }
        }
        s.compositor.RecreateTextFormats(s.scale);
    };
    if (argc > 1 && std::string_view(argv[1]) == "--status-layout-only") {
        check_status_layout_bounds();
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd=nullptr;
        CoUninitialize(); return failures ? 1 : 0;
    }
    if (!pill_only && !update_only) check_status_layout_bounds();
    if (!update_only && !pill_only) {
    const auto output=std::filesystem::absolute(L"../bench_data/content-progress-ui");
    std::filesystem::create_directories(output);
    auto capture=[&](const wchar_t* label, bool hovered=false) {
        for(float scale : {1.0f,1.5f}) for(int width : {720,1100}) for(bool dark : {false,true}) {
            s.scale=scale; s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
            const auto rect=D2D1::RectF(0,0,width*scale,720*scale);
            s.compositor.Resize(static_cast<UINT>(rect.right),static_cast<UINT>(rect.bottom));
            auto vm=app::BuildWindowViewModel(*s.pane,s.sidebar,true,false,dark,&s.places);
            const auto metrics=ui::MakeStatusBarMetrics(vm,rect,scale,24*scale,s.compositor.DwriteFactory(),s.compositor.SmallFormat());
            if (hovered) {
                vm.hover_region=ui::HitTestResult::StatusBarCancelSearch;
                vm.tooltip_text=l10n::Get(l10n::StringId::ContentCancelSearch);
                vm.tooltip_x=(metrics.cancel_search.left+metrics.cancel_search.right)/2;
                vm.tooltip_y=metrics.cancel_search.top;
            }
            if(vm.status.query_active) {
                const float content_right = status_content_right(vm, rect, scale);
                const auto span = progress_span(metrics, rect, scale);
                check(std::abs(metrics.content_right - content_right) < 0.1f &&
                    std::abs((metrics.task.left+metrics.cancel_search.right)/2-(span.left+span.right)/2)<0.1f &&
                    metrics.task.left >= span.left && metrics.cancel_search.right <= span.right,
                    "search progress and cancel icon are centered together before the layout strip");
                const float available=metrics.task.right-metrics.task.left-112*scale;
                check(ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.SmallFormat(),vm.status.query_text)<=available,
                    "query label and progress track fit at requested width and DPI");
                check(metrics.task.right <= metrics.cancel_search.left && metrics.task.right > metrics.task.left &&
                    metrics.cancel_search.right <= rect.right && metrics.cancel_search.left >= rect.left,
                    "progress and cancel control do not overlap at requested width and DPI");
                check(ui::StatusBarHitRegion(vm,rect,(metrics.cancel_search.left+metrics.cancel_search.right)/2,
                    (metrics.cancel_search.top+metrics.cancel_search.bottom)/2,scale,24*scale,&s.compositor)==ui::HitTestResult::StatusBarCancelSearch,
                    "cancel control has its own hit target");
                check(ui::StatusBarHitRegion(vm,rect,metrics.task.left+1,metrics.bar.top+1,scale,24*scale,&s.compositor)==ui::HitTestResult::StatusBar,
                    "query progress does not trigger unrelated operation panel");
            }
            const auto theme=ui::MakeTheme(dark,ui::HexColor(0x0078D4));
            s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm,rect,theme);
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()),"actual content renderer completes");
            if(scale==1.0f || std::wstring_view(label)==L"known") {
                const auto file=output/(std::wstring(label)+L"-"+std::to_wstring(width)+
                    (scale>1.0f ? L"-150" : L"")+(dark ? L"-dark.png":L"-light.png"));
                check(s.compositor.SaveSnapshot(file.c_str()),"content query progress screenshot saved");
            }
        }
    };
    start(); update(0,0);
    check(BuildVm(s,false).status.query_active && BuildVm(s,false).status.query_progress<0,"unknown candidate count uses indeterminate progress");
    check(tab->loading && BuildVm(s,false).status.status_text==L"已匹配 0 个文件","initial empty search exposes active progress and zero matches");
    capture(L"initial");
    check(store->Append({hit}),"isolated synthetic result spool created");
    update(1,0);
    const auto deadline=GetTickCount64()+5000;
    index::ContentResultStore::Row row;
    while(!store->Get(0,row) && GetTickCount64()<deadline) Sleep(2);
    capture(L"unknown");
    update(7200,20000);
    auto vm=BuildVm(s,false);
    check(vm.status.query_active && std::abs(vm.status.query_progress-.36f)<.001f && tab->content_total_files==20000,
        "query candidate denominator reaches view model as 36 percent");
    check(store->Get(0,row) && !tab->loading && vm.status.status_text==L"已匹配 1 个文件","progress updates retain usable list and explicit matched-file count");
    capture(L"known");
    capture(L"known-hover",true);
    l10n::SetLanguage(L"en-US");
    check(l10n::Get(l10n::StringId::ContentCancelSearch)==L"Cancel search · Esc","English cancel tooltip includes shortcut");
    capture(L"known-hover-en",true);
    l10n::SetLanguage(L"zh-CN");
    update(20000,20000,true);
    check(!BuildVm(s,false).status.query_active && tab->content_count_final,"completed query hides active progress and finalizes count");
    capture(L"completed");
    start(); update(400,20000);
    s.addressLiveDue=GetTickCount64()+1;
    const auto cancel_vm=BuildVm(s,false);
    const auto cancel_rect=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    const auto cancel_metrics=ui::MakeStatusBarMetrics(cancel_vm,cancel_rect,s.scale,24*s.scale,
        s.compositor.DwriteFactory(),s.compositor.SmallFormat());
    HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(
        static_cast<int>((cancel_metrics.cancel_search.left+cancel_metrics.cancel_search.right)/2),
        static_cast<int>((cancel_metrics.cancel_search.top+cancel_metrics.cancel_search.bottom)/2)));
    check(!BuildVm(s,false).status.query_active && !BuildVm(s,false).status.query_cancellable &&
        tab->search_content_stopped && !tab->content_count_final,"cancellation stops refresh and hides control without finalizing partial count");
    capture(L"cancelled");
    TickAddressSearch(s,GetTickCount64()+2000);
    check(!s.addressLiveDue && !tab->pending_generation && tab->search_content_stopped,
        "cancel button clears pending input and timer cannot restart query");
    update(20000,20000,true);
    check(tab->search_content_stopped && !tab->content_count_final,
        "late completed response cannot revive cancelled query");
    start(); update(400,20000,true,ERROR_READ_FAULT);
    check(!BuildVm(s,false).status.query_active && !tab->content_count_final && tab->banner_title==l10n::Get(l10n::StringId::SearchIncomplete),
        "query error hides progress and retains incomplete warning");
    capture(L"error");
    start(); update(400,20000,true,0,true);
    check(!BuildVm(s,false).status.query_active && !tab->content_count_final && tab->banner_title==l10n::Get(l10n::StringId::ResultLimitTitle),
        "truncated query remains incomplete and hides progress");
    }
    if (!pill_only) {
    // Update integration: use the production BuildVm adapter and renderer, never a network/installer.
    tab->search_content_active = false;
    tab->pending_generation = 0;
    tab->current_path = L"C:\\Fixture";
    s.shot.active = true;
    s.shot.update_available = true;
    s.update_result_ready = true;
    s.update_result.update_available = true;
    s.update_result.version = L"1.0.34"; // Fixture only; application version is unchanged.
    const auto update_output = std::filesystem::absolute(L"../bench_data/update-status-progress-ui");
    std::filesystem::create_directories(update_output);
    for (const auto language : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(language);
        for (const auto phase : {L"connecting", L"downloading", L"downloading-unknown", L"verifying", L"waiting", L"launching", L"installing"}) {
            if (update_only && std::wstring_view(phase) != L"waiting" &&
                std::wstring_view(phase) != L"launching" && std::wstring_view(phase) != L"installing") continue;
            s.shot.update_state = phase;
            for (const bool settings : {false, true}) {
                tab->current_path = settings ? app::MakeSettingsPath(L"about") : L"C:\\Fixture";
                for (float scale : {1.0f, 1.5f}) for (int width : {360, 720, 1100}) for (bool dark : {false, true}) {
                    s.scale = scale;
                    s.darkMode = dark;
                    s.compositor.RecreateTextFormats(scale);
                    s.renderer.SetScale(scale);
                    const auto rect = D2D1::RectF(0, 0, width * scale, 720 * scale);
                    s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
                    auto update_vm = BuildVm(s, false);
                    if (settings) update_vm.settings_scroll = (std::max)(0.0f,
                        s.renderer.SettingsDestinationOffset(update_vm,
                            static_cast<int>(l10n::StringId::SettingsAutoUpdate), rect.right, rect.bottom) - 180.0f * scale);
                    check(update_vm.settings_open == settings, "fixture exercises the intended normal or Settings tab");
                    check(update_vm.status.task_is_update && !update_vm.status.task_text.empty(),
                        "update snapshot reaches global status bar in both tab types and languages");
                    const bool determinate = std::wstring_view(phase) == L"downloading";
                    check(determinate ? update_vm.status.task_progress == 37.0f : update_vm.status.task_progress < 0,
                        "download uses actual ratio; unknown length and installation stages stay indeterminate");
                    const auto metrics = ui::MakeStatusBarMetrics(update_vm, rect, scale, 24 * scale,
                        s.compositor.DwriteFactory(), s.compositor.SmallFormat());
                    const float content_right = status_content_right(update_vm, rect, scale);
                    const auto span = progress_span(metrics, rect, scale);
                    check(std::abs(metrics.content_right - content_right) < 0.1f &&
                          metrics.task.left >= span.left && metrics.task.right <= span.right &&
                          metrics.task.right > metrics.task.left &&
                          std::abs((metrics.task.left + metrics.task.right) / 2 - (span.left + span.right) / 2) < 0.1f,
                        "update label and progress group remain centered in the available status area at narrow widths and DPI");
                    check(ui::StatusBarHitRegion(update_vm, rect, (metrics.task.left + metrics.task.right) / 2,
                        metrics.bar.top + 1, scale, 24 * scale, &s.compositor) == ui::HitTestResult::StatusBar,
                        "update progress is not a file-operation click target");
                    const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                    s.compositor.Dc()->BeginDraw();
                    s.renderer.Render(update_vm, rect, theme);
                    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "actual update status-bar renderer completes");
                    if (std::wstring_view(language) == L"zh-CN" && std::wstring_view(phase) == L"downloading" &&
                        (width == 360 || width == 1100)) {
                        const auto image = update_output / (L"representative-" + std::to_wstring(width) +
                            (settings ? L"-settings" : L"-normal") + (scale > 1.0f ? L"-150" : L"-100") +
                            (dark ? L"-dark.png" : L"-light.png"));
                        check(s.compositor.SaveSnapshot(image.c_str()), "representative update DPI/theme screenshot saved");
                    }
                    if (settings && scale == 1.0f && width == 1100 && dark) {
                        const auto image = update_output / (std::wstring(language) + L"-" + phase + L".png");
                        check(s.compositor.SaveSnapshot(image.c_str()), "update settings screenshot saved");
                    }
                }
            }
        }
    }
    for (const auto terminal : {L"cancelled", L"failed", L"completed", L""}) {
        s.shot.update_state = terminal;
        check(!BuildVm(s, false).status.task_is_update, "terminal/idle update removes status-bar progress");
    }
    ui::StatusBarView status;
    status.task_text = L"File copy";
    status.task_progress = 64;
    const app::UpdateProgress download{app::UpdatePhase::Downloading, 3, 8};
    app::ApplyUpdateStatus(status, download, true);
    check(!status.task_is_update && status.task_progress == 64 && status.task_text == L"File copy",
        "active file operation retains status-bar ownership");
    status.query_active = status.query_cancellable = true;
    status.query_progress = .36f;
    app::ApplyUpdateStatus(status, download, false);
    check(!status.task_is_update && status.query_cancellable && status.query_progress == .36f,
        "query progress and cancellation retain priority over updates");
    status.query_active = status.query_cancellable = false;
    app::ApplyUpdateStatus(status, download, false);
    check(status.task_is_update && status.task_progress == 37, "update replaces completed operation summary");
    s.shot.active = false;
    check(!BuildVm(s, false).status.task_is_update, "fixture cannot leak progress into idle runtime");

    }
    tab->current_path = L"C:\\Fixture";
    // Operation pill: finished with 撤销 /
    // 打开, partial failure, paused; the two actions have their own targets.
    {
        l10n::SetLanguage(L"zh-CN");   // the update loop above ends in en-US
        const auto output = std::filesystem::absolute(L"../bench_data/content-progress-ui");
        std::filesystem::create_directories(output);
        const float scale = pill_150 ? 1.5f : 1.0f;
        s.scale = scale; s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        auto rect = D2D1::RectF(0, 0, 1100 * scale, 720 * scale);
        s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
        for (const bool dark : {true, false}) {
            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x6474C7));
            auto base = app::BuildWindowViewModel(*s.pane, s.sidebar, true, false, dark, &s.places);
            base.status.query_active = base.status.query_cancellable = false;
            base.status.task_is_update = false;
            auto frame = [&](const ui::WindowViewModel& vm, const wchar_t* name) {
                s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, rect, theme);
                const bool drawn = SUCCEEDED(s.compositor.Dc()->EndDraw());
                if (name) {
                    const auto file = output / (std::wstring(L"pill-") + name + (pill_150 ? L"-150" : L"") + (dark ? L"-dark.png" : L"-light.png"));
                    check(drawn && s.compositor.SaveSnapshot(file.c_str()), "operation pill screenshot saved");
                }
            };
            auto running = base;
            running.status.task_text = L"\x6b63\x5728\x590d\x5236 12 \x4e2a\x9879\x76ee";
            running.status.task_active = true;
            running.status.task_progress = 47.0f;
            frame(running, L"running");
            auto paused = running;
            paused.status.task_paused = true;
            paused.status.task_text = l10n::Get(l10n::StringId::OpCopyPaused);
            frame(paused, L"paused");
            int resume_hits = 0;
            for (float x = 0; x < rect.right; ++x) {
                const auto pill_hit = s.renderer.HitTest(paused, rect, x, rect.bottom - 12 * scale);
                if (pill_hit.region == ui::HitTestResult::StatusBarTask && pill_hit.sub_index == 2) ++resume_hits;
            }
            check(resume_hits > 0, "paused pill exposes resume action index 2");
            const auto wide_rect = rect;
            rect.right = 360 * scale;
            s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
            frame(paused, L"paused-narrow");
            int narrow_resume = 0;
            for (float x = 0; x < rect.right; ++x) {
                const auto pill_hit = s.renderer.HitTest(paused, rect, x, rect.bottom - 12 * scale);
                if (pill_hit.region == ui::HitTestResult::StatusBarTask && pill_hit.sub_index == 2) ++narrow_resume;
            }
            check(narrow_resume > 0, "narrow paused pill retains a bounded resume target");
            rect = wide_rect;
            s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
            auto done = base;
            done.status.task_text = L"\x5df2\x590d\x5236 12 \x4e2a\x9879\x76ee";
            done.status.task_can_undo = done.status.task_can_open = true;
            frame(done, nullptr);
            Sleep(650);   // past the finish pop
            frame(done, L"done");
            // The pill follows counts/history; 定位 precedes 撤销 in the current layout.
            const float y = rect.bottom - 12.0f * scale;
            float undo_x = -1, open_x = -1;
            int plain = 0;
            const auto done_bounds = s.renderer.TaskPillRect();
            bool done_bounds_match = true;
            for (float x = 0; x < rect.right; x += 1.0f) {
                const auto area = s.renderer.HitTest(done, rect, x, y);
                done_bounds_match &= (area.region == ui::HitTestResult::StatusBarTask) == ui::ContainsPt(done_bounds, x, y);
                if (area.region != ui::HitTestResult::StatusBarTask) continue;
                if (area.sub_index == 0 && undo_x < 0) undo_x = x;
                else if (area.sub_index == 1 && open_x < 0) open_x = x;
                else if (area.sub_index < 0) ++plain;
            }
            check(done_bounds_match && open_x >= done_bounds.left && undo_x > open_x && undo_x < done_bounds.right && plain > 0,
                  "finished pill: locate precedes undo with separate semantic targets, the rest opens details");
            if (dark) {
                Sleep(12050);
                frame(done, L"done-compact");
                int compact_hits = 0, compact_actions = 0;
                const auto compact_bounds = s.renderer.TaskPillRect();
                bool compact_bounds_match = true;
                for (float x = 0; x < rect.right; ++x) {
                    const auto pill_hit = s.renderer.HitTest(done, rect, x, y);
                    compact_bounds_match &= (pill_hit.region == ui::HitTestResult::StatusBarTask) == ui::ContainsPt(compact_bounds, x, y);
                    if (pill_hit.region == ui::HitTestResult::StatusBarTask) {
                        ++compact_hits;
                        if (pill_hit.sub_index >= 0) ++compact_actions;
                    }
                }
                check(compact_hits > 0 && compact_bounds_match && compact_actions == 0 &&
                      compact_bounds.left >= rect.left && compact_bounds.right <= status_content_right(done, rect, scale) &&
                      compact_bounds.right - compact_bounds.left < done_bounds.right - done_bounds.left,
                      "after 12 seconds a clickable icon remains without stale action targets");
                rect.right = 360 * scale;
                s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
                frame(done, L"done-compact-narrow");
                int narrow_compact = 0;
                const auto narrow_bounds = s.renderer.TaskPillRect();
                bool narrow_bounds_match = true;
                for (float x = 0; x < rect.right; ++x) {
                    const auto narrow_hit = s.renderer.HitTest(done, rect, x, y);
                    narrow_bounds_match &= (narrow_hit.region == ui::HitTestResult::StatusBarTask) == ui::ContainsPt(narrow_bounds, x, y);
                    if (narrow_hit.region == ui::HitTestResult::StatusBarTask) ++narrow_compact;
                }
                check(narrow_compact > 0 && narrow_bounds_match && narrow_bounds.left >= rect.left &&
                      narrow_bounds.right <= status_content_right(done, rect, scale) &&
                      std::abs((narrow_bounds.right - narrow_bounds.left) - (compact_bounds.right - compact_bounds.left)) < 0.1f,
                      "compact narrow pill matches visible icon bounds and retains the same icon-only width");
                rect = wide_rect;
                s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
                frame(running, nullptr);
                frame(done, nullptr);
            }
            auto hover = done;
            hover.hover_region = ui::HitTestResult::StatusBarTask;
            hover.hover_sub_index = 0;
            frame(hover, L"done-hover");
            auto no_undo = base;
            no_undo.status.task_text = done.status.task_text;
            no_undo.status.task_can_open = true;
            frame(running, nullptr);
            frame(no_undo, nullptr);
            int undo_hits = 0;
            for (float x = 0; x < rect.right; x += 1.0f) {
                const auto no_undo_hit = s.renderer.HitTest(no_undo, rect, x, y);
                if (no_undo_hit.region == ui::HitTestResult::StatusBarTask && no_undo_hit.sub_index == 0) ++undo_hits;
            }
            check(undo_hits == 0, "no undo target when the operation cannot be undone");
            auto bad = base;
            bad.status.task_text = L"2 \x9879\x672a\x6210\x529f \x00b7 \x70b9\x51fb\x67e5\x770b";
            bad.status.task_failed = true;
            bad.status.task_failed_items = 2;
            frame(running, nullptr);
            frame(bad, nullptr);
            Sleep(650);
            frame(bad, L"partial");
            int bad_actions = 0, bad_task = 0;
            const auto bad_bounds = s.renderer.TaskPillRect();
            bool bad_bounds_match = true;
            for (float x = 0; x < rect.right; x += 1.0f) {
                const auto area = s.renderer.HitTest(bad, rect, x, y);
                bad_bounds_match &= (area.region == ui::HitTestResult::StatusBarTask) == ui::ContainsPt(bad_bounds, x, y);
                if (area.region == ui::HitTestResult::StatusBarTask) { ++bad_task; if (area.sub_index >= 0) ++bad_actions; }
            }
            check(bad_task > 0 && bad_actions == 0 && bad_bounds_match, "partial-failure pill opens the list across its visible bounds, no actions");
            frame(running, nullptr);   // reset the finish edge for the next theme
        }
    }
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd=nullptr;
    CoUninitialize(); return failures ? 1:0;
}

#ifndef PULSE_PROGRESS_UI_EMBEDDED
int main(int argc, char** argv) { return RunContentProgressUiTest(argc, argv); }
#endif
