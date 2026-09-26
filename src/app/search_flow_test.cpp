#include "app_internal.h"
#include "app_input.h"
#include "search_query.h"
#include "../ui/address_search_layout.h"
#include "../common/localization.h"
#include <fstream>
#include <filesystem>

#ifdef PULSE_WITH_SELFTEST
using namespace pulse;
void Render(pulse::AppState&);
int RunSearchManagementTest(AppState& s,const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    s.appPrefs.persist=false;
    auto* tab=ActiveTab(s);
    tab->search_input_path=tab->current_path;tab->search_input_text=L"setup diagnostic";
    ShowAddressSearch(s);s.addressSearchComposing=true;s.addressLiveDue=0;
    const auto path=tab->current_path;
    static AppState* state=nullptr;state=&s;
    static int step=0;step=0;
    static bool stayed=false;stayed=false;
    SetTimer(s.hwnd,0x5350,350,[](HWND hwnd,UINT,UINT_PTR id,DWORD){
        HWND menu=nullptr;
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM param)->BOOL{
            wchar_t title[64]{};GetWindowTextW(window,title,64);
            if(IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0)*reinterpret_cast<HWND*>(param)=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(&menu));
        if(step++==0 && menu) {
            const int y=ui::FluentMenu::kShadowMargin+static_cast<int>(99*state->scale);
            PostMessageW(menu,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(50,y));
            PostMessageW(menu,WM_LBUTTONUP,0,MAKELPARAM(50,y));
        } else {
            stayed=state->addressSearching && state->addressEditing && menu;
            KillTimer(hwnd,id);PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_ESCAPE,0);
        }
    });
    ShowSearchOptions(s);
    check(stayed,"mouse selection opens management while search stays expanded");
    check(s.addressSearching && s.addressEditing,"closing management retains search editor");
    wchar_t query[128]{};GetWindowTextW(s.hwndAddressEdit,query,128);
    check(wcscmp(query,L"setup diagnostic")==0 && tab->current_path==path,"query and current location remain unchanged");
    check(!s.addressIgnoreKillFocus,"focus guard restored after management closes");
    // The inline content-search banner is another route into the same menu.
    // Clicking it must not execute the generic outside-editor dismissal.
    tab->current_path=app::MakeSearchPath(L"content:setup diagnostic");
    tab->banner_title=L"No content folders selected";tab->banner_message=L"Choose folders to search their content.";
    s.addressSearchContent=true;s.addressIgnoreKillFocus=true;
    auto vm=BuildVm(s,false);
    const auto bounds=D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()));
    POINT point{};bool found=false;
    if(!vm.pane_slots.empty()) {
        const auto pane=vm.pane_slots.front().rect;
        point={static_cast<LONG>(pane.right-40*s.scale),static_cast<LONG>(pane.top+s.renderer.PaneHeaderHeight()+16*s.scale)};
        found=s.renderer.HitTest(vm,bounds,static_cast<float>(point.x),static_cast<float>(point.y)).region==ui::HitTestResult::ContentIndexManage;
    }
    check(found,"inline content management target found");
    if(found) {
        SetTimer(s.hwnd,0x5351,350,[](HWND hwnd,UINT,UINT_PTR id,DWORD){
            KillTimer(hwnd,id);PostThreadMessageW(GetCurrentThreadId(),WM_KEYDOWN,VK_ESCAPE,0);
        });
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        check(s.addressSearching && s.addressEditing,"inline manage click does not collapse search");
        check(s.addressIgnoreKillFocus,"management restores an existing focus guard");
    }
    s.addressIgnoreKillFocus=false;
    s.addressSearchComposing=false;s.addressSearchAnimation=1;
    Render(s);check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/L"search-after-management.png").c_str()),"search editor screenshot captured");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}

int RunSearchInteractionTest(AppState& s,const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    s.contentSearch.Stop();
    auto* tab=ActiveTab(s);s.appPrefs.persist=false;
    tab->current_path=app::MakeSearchPath(L"content:budget");tab->pending_generation=900001;
    tab->loading=true;tab->search_retaining_results=true;tab->search_content_active=true;
    tab->search_entries=std::make_shared<std::vector<fs::DirEntry>>();
    tab->search_snippets=std::make_shared<std::vector<std::wstring>>();
    auto old=std::make_shared<std::vector<fs::DirEntry>>();old->push_back({});old->back().name=L"old result.txt";tab->SetSnapshot(old);
    index::ContentSearchUpdate progress;progress.progress.generation=900000;progress.progress.done=true;
    ApplyContentSearchUpdate(s,std::move(progress));check(tab->search_retaining_results,"stale request cannot enable old results");
    progress={};progress.progress.generation=900001;ApplyContentSearchUpdate(s,std::move(progress));
    check(tab->search_retaining_results && tab->snapshot==old,"progress-only update keeps retained results pending");
    auto batch=[&](int count,bool done){
        index::ContentSearchUpdate update;update.progress.generation=900001;update.progress.done=done;
        for(int i=0;i<count;++i)update.hits.push_back({L"C:\\fixture\\budget-"+std::to_wstring(i)+L".txt",L"budget-"+std::to_wstring(i)+L".txt",L"Annual budget planning and expense summary."});
        ApplyContentSearchUpdate(s,std::move(update));
    };
    batch(120,false);
    check(!tab->search_retaining_results && !tab->loading && tab->snapshot->size()==120,"first content batch replaces old results and is immediately interactive");
    auto vm=BuildVm(s,false);auto pane=FocusedPaneRect(s);
    auto row=s.renderer.ItemRectInPane(vm.pane,pane,2);
    const int x=static_cast<int>(row.left+180*s.scale),y=static_cast<int>((row.top+row.bottom)*0.5f);
    HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    HandleLButtonUp(&s,s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    check(tab->selected_index==2 && tab->IsSelected(2),"mouse click selects an arriving content result");
    BYTE saved[256]{},pressed[256]{};GetKeyboardState(saved);memcpy(pressed,saved,sizeof(pressed));pressed[VK_LBUTTON]|=0x80;
    for(bool content:{true,false}) {
        if(!content) { tab->search_snippets.reset(); tab->SetSnapshot(tab->snapshot); }
        tab->current_path=app::MakeSearchPath(content ? L"content:budget" : L"budget");
        tab->banner_title=L"Search scope";tab->banner_message=L"This search has a banner as well as filter controls.";
        tab->scroll_y=0;vm=BuildVm(s,false);pane=FocusedPaneRect(s);
        D2D1_RECT_F track{},thumb{};float maximum=0;
        check(ScrollbarGeometry(s,vm.pane,track,thumb,maximum),"search scrollbar is draggable");
        row=s.renderer.ItemRectInPane(vm.pane,pane,0);
        auto second=s.renderer.ItemRectInPane(vm.pane,pane,1);
        const float cell=second.top-row.top,view=track.bottom-track.top;
        const float expected=std::min(view,std::max(cell,view*view/(cell*vm.pane.EntryCount())));
        check(std::abs(track.top-row.top)<0.1f && std::abs(thumb.bottom-thumb.top-expected)<0.1f,"drag geometry matches rendered list origin and thumb size");
        for(float grip:{0.15f,0.5f,0.85f}) {
            tab->scroll_y=maximum*0.2f;vm=BuildVm(s,false);ScrollbarGeometry(s,vm.pane,track,thumb,maximum);
            const int bx=static_cast<int>(track.right-7*s.scale),by=static_cast<int>(thumb.top+(thumb.bottom-thumb.top)*grip);
            const float before=tab->scroll_y;
            SetKeyboardState(pressed);
            HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(bx,by));
            check(s.scrollbarDragging && std::abs(tab->scroll_y-before)<0.1f,"pressing the visible thumb captures it without jumping");
            const float offset=s.scrollbarGrabOffset;
            const int target=by+static_cast<int>(60*s.scale);
            HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bx,target));
            vm=BuildVm(s,false);ScrollbarGeometry(s,vm.pane,track,thumb,maximum);
            check(std::abs(thumb.top+offset-target)<1.1f,"dragged thumb remains at the original mouse grip point");
            if(content && grip==0.5f) {
                batch(30,false);
                HandleMouseMove(&s,s.hwnd,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(bx,target));
                vm=BuildVm(s,false);ScrollbarGeometry(s,vm.pane,track,thumb,maximum);
                check(std::abs(thumb.top+s.scrollbarGrabOffset-target)<1.1f,"incoming results do not displace the mouse grip");
            }
            HandleLButtonUp(&s,s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(bx,target));SetKeyboardState(saved);
            check(!s.scrollbarDragging,"releasing the mouse ends the drag");
        }
        Render(s);check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/(content ? L"content-drag.png" : L"filename-drag.png")).c_str()),"search interaction screenshot captured");
    }
    tab->current_path=app::MakeSearchPath(L"content:budget");batch(0,true);
    check(!tab->search_retaining_results && !tab->loading && !tab->search_content_active,"completion preserves interactive results");
    tab->pending_generation=900002;tab->search_retaining_results=true;tab->loading=true;
    tab->search_entries=std::make_shared<std::vector<fs::DirEntry>>();tab->search_snippets=std::make_shared<std::vector<std::wstring>>();
    progress={};progress.progress.generation=900002;progress.progress.done=true;ApplyContentSearchUpdate(s,std::move(progress));
    check(!tab->search_retaining_results && !tab->loading && tab->snapshot->empty(),"empty completion clears retained rows and interaction lock");
    log<<"failures="<<failures<<std::endl;return failures ? 1 : 0;
}


int RunSearchExitTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    s.appPrefs.persist = false;
    s.searchHistory.persist = false;
    const auto fixture = std::filesystem::path(output).parent_path() / L"origin";
    std::filesystem::create_directories(fixture);
    const std::wstring folder = fs::NormalizePath(fixture.wstring());
    auto* tab = ActiveTab(s);
    auto prepare = [&](const std::wstring& origin) {
        s.addressLiveDue = s.addressHistoryDue = 0;
        HideAddressEditor(s, false);
        *tab = app::Tab{};
        tab->current_path = origin;
        ShowAddressSearch(s);
        SetWindowTextW(s.hwndAddressEdit, L"budget");
    };
    for (const auto& origin : {folder, std::wstring(L"pulse:home"), std::wstring{}}) {
        prepare(origin);
        SubmitAddressSearch(s, true);
        check(tab->search_origin_valid && tab->search_origin_path == origin, "first search captures exact origin including virtual and This PC");
        SetWindowTextW(s.hwndAddressEdit, L"revised");
        SubmitAddressSearch(s, true);
        SwitchAddressSearchMode(s, true);
        SubmitAddressSearch(s, true);
        s.addressSearchCurrent = true;
        SubmitAddressSearch(s, true);
        check(tab->search_origin_path == origin, "query, type and scope changes preserve search origin");
        const auto stale_generation = tab->pending_generation;
        SetWindowTextW(s.hwndAddressEdit, L"unsubmitted");
        QueueAddressSearch(s);
        const auto requests = s.nextIndexReq;
        const auto generation = tab->view_generation;
        ExitAddressSearch(s);
        check(tab->view_generation == generation + 1, "exit performs one navigation without flushing queued query");
        check(tab->current_path == origin && !s.addressSearching, "exit returns to exact origin");
        check(s.nextIndexReq == requests && !s.addressLiveDue && !s.addressHistoryDue, "exit cancels pending query instead of submitting it");
        index::ContentSearchUpdate stale;
        stale.progress.generation = stale_generation;
        stale.progress.done = true;
        stale.hits.push_back({L"C:\\fixture\\late.txt", L"late.txt", L"late"});
        const auto snapshot = tab->snapshot;
        ApplyContentSearchUpdate(s, std::move(stale));
        check(tab->current_path == origin && tab->snapshot == snapshot, "late content results cannot overwrite restored location");
    }
    prepare(folder);
    QueueAddressSearch(s);
    const auto requests = s.nextIndexReq;
    ExitAddressSearch(s);
    check(tab->current_path == folder && !IsAddressSearchResults(tab) && s.nextIndexReq == requests,
        "immediate exit before debounce leaves original folder without running search");
    prepare(folder);
    SubmitAddressSearch(s, true);
    SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_ESCAPE, 0);
    check(tab->current_path == folder && !s.addressSearching, "Escape in search editor exits to origin");
    prepare(folder);
    SubmitAddressSearch(s, true);
    s.addressSearchComposing = true;
    SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_ESCAPE, 0);
    check(s.addressSearching && IsAddressSearchResults(tab), "IME Escape does not exit search");
    s.addressSearchComposing = false;
    HideAddressEditor(s, false);
    check(IsAddressSearchResults(tab), "editor losing focus does not exit results");
    ExitAddressSearch(s);
    prepare(folder);
    SubmitAddressSearch(s, true);
    tab->search_origin_valid = false;
    tab->back_stack = {};
    tab->back_stack.push(L"pulse:home");
    tab->back_stack.push(app::MakeSearchPath(L"older"));
    ExitAddressSearch(s);
    check(tab->current_path == L"pulse:home", "missing origin falls back across searches to nearest virtual page");
    // A repeated query can occur in multiple history entries with different origins.
    app::Tab history;
    history.current_path = folder;
    const auto same_query = app::MakeSearchPath(L"same");
    history.NavigateTo(same_query);
    history.NavigateTo(L"pulse:home");
    history.NavigateTo(same_query);
    history.GoBack();
    history.GoBack();
    check(history.current_path == same_query && history.search_origin_valid && history.search_origin_path == folder,
        "back restores source belonging to that search history entry");
    history.GoForward();
    history.GoForward();
    check(history.search_origin_valid && history.search_origin_path == L"pulse:home",
        "forward restores a repeated query with its own source");
    app::Tab network;
    network.current_path = L"\\\\server\\share\\folder";
    network.NavigateTo(same_query);
    check(network.search_origin_valid && network.search_origin_path == L"\\\\server\\share\\folder" &&
        history.search_origin_path == L"pulse:home", "network paths and independent tabs keep distinct origins");
    prepare(folder);
    SubmitAddressSearch(s, true);
    tab->search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab->SetSnapshot(tab->search_entries);
    tab->loading = false;
    ExitAddressSearch(s);
    check(tab->current_path == folder, "empty results exit to origin");
    prepare(folder);
    SubmitAddressSearch(s, true);
    const auto filename_generation = static_cast<uint32_t>(tab->pending_generation);
    s.addressSearchAnimation = 1.0f;
    auto vm = BuildVm(s, false);
    const auto layout = ui::LayoutAddressSearch(s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width())), s.scale);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const int x = static_cast<int>((layout.close.left + layout.close.right) * 0.5f);
    const int y = static_cast<int>((layout.close.top + layout.close.bottom) * 0.5f);
    check(s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y)).region == ui::HitTestResult::AddressSearchClose,
        "exit button hit target matches visible search toolbar");
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    check(tab->current_path == folder && !s.addressSearching, "mouse exit button restores search origin");
    const auto snapshot = tab->snapshot;
    DeliverIndexSearchResult(s, filename_generation, {});
    check(tab->snapshot == snapshot && tab->current_path == folder, "late filename results cannot replace restored folder");
    prepare(folder + L"\\removed-folder");
    SubmitAddressSearch(s, true);
    ExitAddressSearch(s);
    check(tab->current_path == folder + L"\\removed-folder", "missing origin directory keeps its address instead of returning to This PC");
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}


int RunSearchColumnAlignmentTest(AppState& s, const wchar_t* output) {
    const auto directory = std::filesystem::path(output).parent_path();
    std::ofstream log{std::filesystem::path(output)};
    std::ofstream regions{directory / L"regions.txt"};
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        log << (ok ? "[PASS] " : "[FAIL] ") << message << std::endl;
        if (!ok) ++failures;
    };
    s.appPrefs.persist = false;
    s.contentSearch.Stop();
    s.addressLiveDue = s.addressHistoryDue = 0;
    HideAddressEditor(s, false);
    auto* tab = ActiveTab(s);
    *tab = app::Tab{};
    tab->view_mode = ui::ViewMode::Details;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 5; ++i) {
        fs::DirEntry entry;
        entry.name = i == 0 ? L"agents.md" : L"build-2027-debug-" + std::to_wstring(i) + L".bat";
        entry.full_path = L"C:\\Users\\W\\Desktop\\SHOWBOX-LUMEN\\" + entry.name;
        entry.size = 10752;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.mtime.dwHighDateTime = 31200000;
        entries->push_back(std::move(entry));
    }
    // Compare the same row geometry with/without snippets, including a row
    // whose content hit has no preview text. Metadata pixels must match.
    for (int density : {36, 40, 48}) {
        s.appPrefs.row_height = density;
        s.renderer.SetRowHeightDip(static_cast<float>(density));
        for (bool content : {false, true}) {
            tab->current_path = app::MakeSearchPath(content ? L"content:budget" : L"agents");
            tab->search_snippets = content ? std::make_shared<std::vector<std::wstring>>(
                std::initializer_list<std::wstring>{L"L18  AutoCAD ObjectARX + LUI", L"L5  rem budget build configuration", L"", L"L8  budget planning", L"L12  budget summary"}) : nullptr;
            tab->SetSnapshot(entries);
            tab->loading = false;
            tab->SelectOnly(0);
            auto vm = BuildVm(s, false);
            const auto first = s.renderer.ItemRectInPane(vm.pane, FocusedPaneRect(s), 0);
            const auto last = s.renderer.ItemRectInPane(vm.pane, FocusedPaneRect(s), 4);
            const auto columns = s.renderer.DetailsColumns(first, vm.pane);
            check(std::abs(first.bottom - first.top - density * s.scale) < 0.2f,
                "filename and content results use the same requested row geometry");
            if (!content) regions << density << ' ' << static_cast<int>(std::ceil(columns.DividerX(0) + 4 * s.scale))
                << ' ' << static_cast<int>(std::ceil(first.top + 2 * s.scale)) << ' '
                << static_cast<int>(std::floor(last.right - 20 * s.scale)) << ' '
                << static_cast<int>(std::floor(last.bottom - 2 * s.scale)) << std::endl;
            Render(s);
            const auto image = directory / ((content ? L"content-" : L"filename-") + std::to_wstring(density) + L".png");
            check(s.compositor.SaveSnapshot(image.c_str()), "search metadata alignment screenshot captured");
        }
    }
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}


int RunContentHistoryTest(AppState& s, const wchar_t* output) {
    const auto directory = std::filesystem::path(output).parent_path();
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    s.appPrefs.persist = false; s.searchHistory.persist = false;
    const auto root = directory / L"indexed";
    std::filesystem::create_directories(root);
    std::ofstream(root / L"report-20.txt") << "20 quarterly planning";
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    s.contentSearch.Configure(config);
    for (int i = 0; i < 200 && s.contentSearch.GetConfig().roots.empty(); ++i) Sleep(25);
    check(!s.contentSearch.GetConfig().roots.empty(), "isolated coverage configuration loaded");
    // Hold result delivery for deterministic pending/partial/final screenshots.
    // The separate IPC regression exercises the actual agent and cancellation.
    s.contentSearch.Stop();
    s.addressLiveDue = s.addressHistoryDue = 0;
    HideAddressEditor(s, false);
    auto* tab = ActiveTab(s); *tab = app::Tab{};
    tab->current_path = L"pulse:home";
    tab->search_input_path = tab->current_path; tab->search_input_text = L"20";
    ShowAddressSearch(s);
    s.addressLiveDue = s.addressHistoryDue = 0;
    app::AdvancedSearchSpec spec; spec.content = L"20";
    const auto raw = app::CompileSearchQuery(spec);
    const auto path = app::MakeSearchPath(raw);
    s.searchHistory.Clear(); RecordSearchHistory(s, path);
    static float menu_scale = 1; menu_scale = s.scale;
    static bool clicked = false; clicked = false;
    SetTimer(s.hwnd, 0x5358, 150, [](HWND hwnd, UINT, UINT_PTR timer, DWORD) {
        HWND menu = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM value)->BOOL {
            wchar_t title[64]{}; GetWindowTextW(window, title, 64);
            if (IsWindowVisible(window) && wcscmp(title, L"PulseMenu") == 0)
                *reinterpret_cast<HWND*>(value) = window;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&menu));
        KillTimer(hwnd, timer);
        if (menu) {
            const int y = ui::FluentMenu::kShadowMargin + static_cast<int>(58 * menu_scale);
            PostMessageW(menu, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(80, y));
            PostMessageW(menu, WM_LBUTTONUP, 0, MAKELPARAM(80, y));
            clicked = true;
        } else PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_ESCAPE, 0);
    });
    const auto previous_request = s.nextIndexReq;
    ShowAddressSearchHistory(s);
    check(clicked && tab->current_path == path && s.addressSearching && s.addressSearchContent,
        "mouse history selection restores query and contents mode");
    const auto generation = tab->pending_generation;
    check(generation != 0 && s.nextIndexReq == previous_request + 1,
        "history click submits exactly one content request");
    wchar_t expected[512]{};
    swprintf_s(expected, l10n::Get(l10n::StringId::SearchLoadingFormat).c_str(), L"20");
    check(tab->loading && tab->search_content_active && tab->virtual_title == expected,
        "history starts in searching state without premature zero results");
    // Exercise the editor's delayed submission after restoring the same query.
    FlushAddressSearch(s);
    check(tab->pending_generation == generation && s.nextIndexReq == previous_request + 1,
        "restored editor does not repeat or cancel the request");
    auto capture = [&](const wchar_t* name) {
        s.addressSearchAnimation = 1; LayoutAddressEditor(s);
        if (s.hwndAddressEdit) RedrawWindow(s.hwndAddressEdit, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        Render(s);
        check(s.compositor.SaveSnapshot((directory / name).c_str()), "search state screenshot captured");
    };
    capture(L"history-pending.png");
    index::ContentSearchUpdate update; update.progress.generation = generation;
    update.progress.scanned_files = 50;
    ApplyContentSearchUpdate(s, std::move(update));
    check(tab->loading && tab->search_content_active && tab->virtual_title.starts_with(expected),
        "progress without hits remains searching");
    capture(L"history-progress.png");
    update = {}; update.progress.generation = generation; update.progress.scanned_files = 51;
    FILETIME modified{}; GetSystemTimeAsFileTime(&modified);
    update.hits.push_back({(root / L"report-20.txt").wstring(), L"report-20.txt", L"20 quarterly planning", 128,
        (static_cast<uint64_t>(modified.dwHighDateTime) << 32) | modified.dwLowDateTime});
    ApplyContentSearchUpdate(s, std::move(update));
    check(!tab->loading && tab->search_content_active && tab->snapshot && tab->snapshot->size() == 1,
        "first result is visible while remaining query continues");
    auto vm = BuildVm(s, false);
    const auto row = s.renderer.ItemRectInPane(vm.pane, FocusedPaneRect(s), 0);
    const auto point = MAKELPARAM(static_cast<int>(row.left + 150 * s.scale), static_cast<int>((row.top + row.bottom) * .5f));
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
    HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
    check(tab->selected_index == 0 && tab->search_content_active, "first streamed row can be selected before completion");
    capture(L"history-first-result.png");
    update = {}; update.progress.generation = generation; update.progress.done = true;
    ApplyContentSearchUpdate(s, std::move(update));
    check(!tab->loading && !tab->search_content_active && tab->search_total == 1,
        "final batch preserves first result and ends searching");
    capture(L"history-completed.png");
    RequestSearchPage(s, *tab, raw, true);
    const auto next_generation = tab->pending_generation;
    update = {}; update.progress.generation = generation; update.progress.done = true;
    ApplyContentSearchUpdate(s, std::move(update));
    check(tab->loading && tab->pending_generation == next_generation, "late previous completion cannot end new history query");
    update = {}; update.progress.generation = next_generation; update.progress.done = true;
    ApplyContentSearchUpdate(s, std::move(update));
    check(!tab->loading && !tab->search_content_active && tab->search_total == 0,
        "empty state appears only after completed no-match query");
    capture(L"history-empty.png");
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}


int RunContentPagingUiTest(AppState& s,const wchar_t* output) {
    const auto directory=std::filesystem::path(output).parent_path();
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    s.appPrefs.persist=false;s.searchHistory.persist=false;s.contentSearch.Stop();
    s.addressLiveDue=s.addressHistoryDue=0;HideAddressEditor(s,false);
    auto* tab=ActiveTab(s);*tab=app::Tab{};
    tab->current_path=app::MakeSearchPath(L"content:20");
    tab->search_origin_valid=true;tab->search_origin_path=L"pulse:home";
    tab->view_mode=ui::ViewMode::Details;tab->pending_generation=900012;tab->search_content_active=true;
    s.appPrefs.row_height=40;s.renderer.SetRowHeightDip(40);
    auto store=std::make_shared<index::ContentResultStore>(s.hwnd,WM_CONTENT_SEARCH);
    constexpr int count=15050;
    auto name=[](int i){wchar_t value[40]{};swprintf_s(value,L"f%05d.txt",i);return std::wstring(value);};
    auto append=[&](int begin,int end){
        bool ok=true;
        std::thread worker([&]{
            std::vector<index::ContentHit> batch;
            for(int i=begin;i<end;++i) {
                FILETIME modified{};GetSystemTimeAsFileTime(&modified);
                batch.push_back({L"C:\\SearchFixture\\"+name(i),name(i),L"20 complete searchable results",static_cast<uint64_t>(i),
                    (static_cast<uint64_t>(modified.dwHighDateTime)<<32)|modified.dwLowDateTime,1});
                if(batch.size()==64 || i==end-1) {ok &= store->Append(batch);batch.clear();}
            }
        });worker.join();check(ok,"fixture hits written off the UI thread");
    };
    auto pump=[&]{
        MSG message{};
        while(PeekMessageW(&message,s.hwnd,WM_CONTENT_SEARCH,WM_CONTENT_SEARCH,PM_REMOVE)) DispatchMessageW(&message);
        while(PeekMessageW(&message,s.hwnd,WM_CONTENT_SELECTION,WM_CONTENT_SELECTION,PM_REMOVE)) DispatchMessageW(&message);
        RefreshContentResults(s);
    };
    auto wait=[&](auto ready){const auto start=GetTickCount64();while(!ready() && GetTickCount64()-start<10000){pump();Sleep(2);}pump();return ready();};
    auto capture=[&](const wchar_t* file){Render(s);check(s.compositor.SaveSnapshot((directory/file).c_str()),"paged content screenshot captured");};
    auto deliver=[&](bool done){index::ContentSearchUpdate update;update.progress.generation=900012;update.progress.done=done;update.results=store;ApplyContentSearchUpdate(s,std::move(update));};
    append(0,64);deliver(false);
    check(wait([&]{store->Prefetch(0);return store->Ready(0);}),"first display page becomes available");
    check(tab->search_content_active && !tab->loading && tab->EntryCount()==64,"first page usable while total is still being counted");
    capture(L"paging-running.png");
    append(64,count);deliver(false);
    check(tab->EntryCount()==count && tab->search_content_active && tab->virtual_title.find(L"15050")!=std::wstring::npos,"running count passes old 10000 limit");
    deliver(true);
    check(!tab->search_content_active && tab->search_total==count,"completed total is the full result count");
    tab->scroll_y=12000*40*s.scale;s.scrollTargetY=tab->scroll_y;
    check(wait([&]{store->Prefetch(12002);return store->Ready(12002);}),"thumb jump loads page beyond old cap directly");
    auto vm=BuildVm(s,false);
    const auto row=s.renderer.ItemRectInPane(vm.pane,FocusedPaneRect(s),12002);
    const auto point=MAKELPARAM(static_cast<int>(row.left+170*s.scale),static_cast<int>((row.top+row.bottom)*.5f));
    HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,point);
    HandleLButtonUp(&s,s.hwnd,WM_LBUTTONUP,0,point);
    check(tab->selected_index==12002 && tab->EntryAt(12002).name==name(12002),"mouse selects correct global file after jumping past 10000");
    capture(L"paging-after-10000.png");
    tab->SelectIndices({7,12001,15049});
    std::vector<std::wstring> selected;
    check(DeferContentSelection(s,[&](AppState& state){selected=SelectedFullPaths(*ActiveTab(state));}),"cross-page action resolves asynchronously");
    check(wait([&]{return !s.contentSelectionAction;}) && selected.size()==3 && selected.back()==L"C:\\SearchFixture\\"+name(15049),"cross-page action includes unloaded last result");
    check(tab->content_action_rows.empty() && !tab->content_action_ready,"temporary action rows released after command");
    DispatchMenuCommand(s,app::CmdSelectAll);
    check(tab->all_selected && tab->SelectedCount()==count,"select all covers full result count");
    selected.clear();DeferContentSelection(s,[&](AppState& state){selected=SelectedFullPaths(*ActiveTab(state));});
    check(wait([&]{return !s.contentSelectionAction;}) && selected.size()==count,"select-all operation resolves every file including unloaded pages");
    check(wait([&]{return ContentSelectionSize(*tab).has_value();}) && ContentSelectionSize(*tab)==static_cast<uint64_t>(count)*(count-1)/2,"selection size sums full result set without loading all display rows");
    size_t focusRows=0;
    DeferContentSelection(s,[&](AppState& state){focusRows=ActiveTab(state)->content_action_rows.size();},true);
    check(wait([&]{return !s.contentSelectionAction;}) && focusRows==1,"focused-only action reads one file even when all rows are selected");
    SelectContentPattern(s,L"f1504*");
    check(wait([&]{return !s.contentSelectionAction && !store->Filtering();}) && tab->SelectedCount()==10 && tab->IsSelected(15049),"wildcard selection reaches unloaded tail using original global indices");
    tab->filter_text=L"f1504";RefreshContentResults(s);
    check(wait([&]{return !store->Filtering();}) && tab->EntryCount()==10,"inline filter includes matches beyond loaded pages");
    check(wait([&]{store->Prefetch(0);return store->Ready(0);}),"filtered page loads");
    capture(L"paging-filtered.png");
    tab->filter_text.clear();RefreshContentResults(s);wait([&]{return !store->Filtering();});
    check(tab->EntryCount()==count && store->CachedRows()<=2048 && !tab->search_entries && tab->snapshot->empty(),"all results remain disk backed with bounded display cache");
    tab->SelectOnly(15049);EnsureRowVisible(s,*tab,15049);
    check(wait([&]{store->Prefetch(15049);return store->Ready(15049);}),"keyboard end can reveal actual last result");
    capture(L"paging-last-result.png");
    tab->SelectOnly(0);tab->search_preserve_selection=L"C:\\SearchFixture\\"+name(14000);
    RefreshContentResults(s);
    check(wait([&]{return !tab->content_selection_restore;}) && tab->selected_index==14000,"refresh restores selection by full path beyond cached pages");
    bool staleAction=false;
    DeferContentSelection(s,[&](AppState&){staleAction=true;});
    tab->SelectOnly(1);tab->SelectOnly(14000);
    check(wait([&]{return !s.contentSelectionAction;}) && !staleAction,"changed selection invalidates pending action even when original indices are reselected");
    tab->search_content_active=true;tab->content_count_final=false;
    CancelActiveContentSearch(s,*tab);
    wchar_t partial[1024]{};swprintf_s(partial,l10n::Get(l10n::StringId::ContentPartialFormat).c_str(),L"20",static_cast<size_t>(count));
    check(!tab->content_count_final && tab->virtual_title==partial,"canceled query does not present partial count as final");
    DeferContentSelection(s,[&](AppState&){staleAction=true;});
    ExitAddressSearch(s);pump();
    check(!s.contentSelectionAction && !staleAction,"exit cancels pending selection action without executing it");
    check(tab->current_path==L"pulse:home" && !tab->content_results,"exit releases result source and restores original location");
    log<<"failures="<<failures<<std::endl;return failures ? 1:0;
}

int RunContentLiveSelectionTest(AppState& s,const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] ":"[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    s.appPrefs.persist=false;s.searchHistory.persist=false;s.contentSearch.Stop();
    s.addressLiveDue=s.addressHistoryDue=0;HideAddressEditor(s,false);
    auto* tab=ActiveTab(s);*tab=app::Tab{};
    tab->current_path=app::MakeSearchPath(L"content:live");
    tab->view_mode=ui::ViewMode::Details;tab->pending_generation=900030;tab->search_content_active=true;
    auto store=std::make_shared<index::ContentResultStore>(s.hwnd,WM_CONTENT_SEARCH);
    auto path=[](int i){wchar_t v[64]{};swprintf_s(v,L"C:\\LiveFixture\\f%05d.txt",i);return std::wstring(v);};
    auto hit=[&](int i,bool removed){index::ContentHit h{path(i),path(i).substr(15),L"live content",static_cast<uint64_t>(i),0,1};h.removed=removed;return h;};
    auto write=[&](std::vector<index::ContentHit> hits,bool live){
        bool ok=false;
        std::thread worker([&]{ok=live ? store->ApplyChanges(hits,index::ContentResultSort::Name,false) : store->Append(hits);});
        worker.join();return ok;
    };
    auto pump=[&]{
        MSG message{};
        while(PeekMessageW(&message,s.hwnd,WM_CONTENT_SEARCH,WM_CONTENT_SEARCH,PM_REMOVE)) DispatchMessageW(&message);
        RefreshContentResults(s);
    };
    auto wait=[&](auto ready){const auto start=GetTickCount64();while(!ready() && GetTickCount64()-start<10000){pump();Sleep(2);}pump();return ready();};
    auto deliver=[&](bool delta){index::ContentSearchUpdate update;update.progress.generation=900030;update.progress.done=true;update.progress.delta=delta;update.results=store;ApplyContentSearchUpdate(s,std::move(update));};
    auto settled=[&]{
        return tab->selected_index>=0 && !tab->content_selection_restore && tab->search_preserve_selection.empty() &&
            (store->Prefetch(static_cast<size_t>(tab->selected_index)),store->Ready(static_cast<size_t>(tab->selected_index)));
    };
    auto selected_path=[&]{return tab->selected_index>=0 ? tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path : std::wstring{};};
    std::vector<index::ContentHit> initial;for(int i=10;i<=200;i+=10) initial.push_back(hit(i,false));
    check(write(initial,false),"initial hits written off the UI thread");
    deliver(false);tab->search_live_generation=900030;
    check(wait([&]{store->Prefetch(0);return store->Ready(0) && tab->EntryAt(5).full_path==path(60);}),"initial rows readable");
    tab->SelectOnly(5);for(int i=0;i<3;++i){pump();Sleep(5);}
    check(write({hit(5,false)},true),"live delta inserts a row above the selection");deliver(true);
    check(wait(settled) && selected_path()==path(60),"selection stays on the same file after a live insert above it");
    check(write({hit(10,true),hit(20,true)},true),"live delta removes rows above the selection");deliver(true);
    check(wait(settled) && selected_path()==path(60),"selection stays on the same file after rows above it are removed");
    tab->ClearSelection();for(int i=0;i<3;++i){pump();Sleep(5);}
    check(write({hit(1,false)},true),"live delta with nothing selected");deliver(true);
    for(int i=0;i<10;++i){pump();Sleep(5);}
    check(tab->selected_index<0 && !tab->IsSelected(0),"live delta does not auto-select the first row");
    log<<"failures="<<failures<<std::endl;return failures ? 1:0;
}

int RunSearchFlowTest(AppState& s, const wchar_t* output) {
    if(GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_LIVE_SELECTION",nullptr,0)) return RunContentLiveSelectionTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_PAGING",nullptr,0)) return RunContentPagingUiTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_HISTORY",nullptr,0)) return RunContentHistoryTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_COLUMN_ALIGNMENT",nullptr,0)) return RunSearchColumnAlignmentTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_EXIT",nullptr,0)) return RunSearchExitTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_INTERACTION",nullptr,0)) return RunSearchInteractionTest(s,output);
    if(GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_MANAGEMENT",nullptr,0)) return RunSearchManagementTest(s,output);
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check(!l10n::Get(l10n::StringId::SearchModeContent).empty() && !l10n::Get(l10n::StringId::ContentIndexNoQuery).empty(), "search labels loaded");
    s.contentSearch.Stop();
    auto* tab = ActiveTab(s);
    tab->current_path = app::MakeSearchPath(L"content:budget");
    tab->pending_generation = 900001;
    tab->search_content_active = true;
    tab->search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab->search_snippets = std::make_shared<std::vector<std::wstring>>();
    tab->search_preserve_selection = L"C:\\fixture\\b.txt";
    index::ContentSearchUpdate first;
    first.progress.generation = 900001;
    first.hits.push_back({L"C:\\fixture\\a.txt", L"a.txt", L"budget"});
    ApplyContentSearchUpdate(s, std::move(first));
    index::ContentSearchUpdate second;
    second.progress.generation = 900001;
    second.progress.done = true;
    second.hits.push_back({L"C:\\fixture\\b.txt", L"b.txt", L"budget"});
    ApplyContentSearchUpdate(s, std::move(second));
    check(tab->snapshot->size() == 2 && !tab->loading, "content batches complete");
    check(tab->selected_index == 1, "selection retained across content batches");
    check(tab->search_next_offset == tab->search_total, "completed content has no filename pages");
    auto generation = s.nextIndexReq;
    RequestSearchPage(s, *tab, L"content:budget", false);
    check(s.nextIndexReq == generation && tab->snapshot->size() == 2, "content scroll cannot dispatch filename search");
    index::ContentSearchUpdate stale;
    stale.progress.generation = 900000;
    stale.progress.done = true;
    stale.hits.push_back({L"C:\\fixture\\stale.txt", L"stale.txt", L"old"});
    ApplyContentSearchUpdate(s, std::move(stale));
    check(tab->snapshot->size() == 2, "stale generation ignored");
    ShowAddressSearch(s);
    s.addressSearchContent = true;
    SetWindowTextW(s.hwndAddressEdit, L"");
    SubmitAddressSearch(s, false);
    check(tab->search_content_empty && tab->snapshot->empty(), "clearing content clears results");
    RequestSearchPage(s, *tab, L"content:budget", true);
    check(tab->snapshot->empty() && !tab->loading, "empty content does not rerun previous query");
    SetWindowTextW(s.hwndAddressEdit, L"budget");
    SubmitAddressSearch(s, false);
    log << "restore empty=" << tab->search_content_empty << " editor=" << IsWindow(s.hwndAddressEdit) << " textlen=" << GetWindowTextLengthW(s.hwndAddressEdit) << " composing=" << s.addressSearchComposing << " searching=" << s.addressSearching << std::endl;
    check(!tab->search_content_empty && tab->banner_title != l10n::Get(l10n::StringId::ContentIndexNoQuery),
          "same content can be searched again after clearing");
    SwitchAddressSearchMode(s, false);
    SubmitAddressSearch(s, false);
    std::wstring mode_query;
    app::ParsePulsePath(tab->current_path, nullptr, &mode_query);
    check(!app::SplitSearchQueryText(mode_query).content.present() && GetWindowTextLengthW(s.hwndAddressEdit) == 6,
          "name segment retains query and switches immediately");
    SwitchAddressSearchMode(s, true);
    SaveAddressSearchDraft(s); // History dismissal saves the newly selected mode first.
    SubmitAddressSearch(s, false);
    app::ParsePulsePath(tab->current_path, nullptr, &mode_query);
    check(app::SplitSearchQueryText(mode_query).content.present() && app::ParseSearchQuery(mode_query).name.empty(),
          "content switch after draft save does not retain a filename filter");
    s.addressSearchComposing = true;
    const auto composing_path = tab->current_path;
    SwitchAddressSearchMode(s, false);
    SubmitAddressSearch(s, false);
    check(tab->current_path == composing_path, "mode switch waits for IME composition");
    s.addressSearchComposing = false;
    SubmitAddressSearch(s, false);
    app::ParsePulsePath(tab->current_path, nullptr, &mode_query);
    check(!app::SplitSearchQueryText(mode_query).content.present(), "mode query applies after IME completion");
    auto vm = BuildVm(s, false);
    const float width = static_cast<float>(s.compositor.Width());
    const auto layout = ui::LayoutAddressSearch(s.renderer.AddressBarRect(width), s.scale);
    const auto window = D2D1::RectF(0, 0, width, static_cast<float>(s.compositor.Height()));
    auto hit = [&](D2D1_RECT_F rect) {
        return s.renderer.HitTest(vm, window, (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f).region;
    };
    check(hit(layout.name) == ui::HitTestResult::AddressSearchMode, "name segment hit target");
    check(hit(layout.content) == ui::HitTestResult::AddressSearchContent, "content segment hit target");
    check(hit(layout.options) == ui::HitTestResult::AddressSearchOptions, "separate options hit target");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

#endif
