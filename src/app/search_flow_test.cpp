#include "toolbar_tools.h"
#include "toolbar_view_order.h"
#include "app_internal.h"
#include "../ui/ui_renderer.h"
#include "app_input.h"
#include "app_runtime.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "vertical_tabs.h"
#include "../ui/toolbar_picker.h"
#include "search_query.h"
#include "../ui/address_search_layout.h"
#include "../ui/toolbar_layout.h"
#include "../ui/ui_renderer.h"
#include "../common/localization.h"
#include <fstream>
#include <filesystem>
#include <wincodec.h>
#include <dwmapi.h>

#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
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
    const auto layout = ui::LayoutAddressSearch(s.renderer.SearchBarRect(static_cast<float>(s.compositor.Width())), s.scale);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    const int x = static_cast<int>((layout.close.left + layout.close.right) * 0.5f);
    const int y = static_cast<int>((layout.close.top + layout.close.bottom) * 0.5f);
    // One trailing button: it clears a typed query first, then exits search.
    check(s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y)).region == ui::HitTestResult::AddressSearchClear,
        "trailing button clears a typed query");
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    check(s.addressSearching && GetWindowTextLengthW(s.hwndAddressEdit) == 0, "first click clears the query and keeps editing");
    vm = BuildVm(s, false);
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

int RunSidebarNarrowTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)}; int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok?"[PASS] ":"[FAIL] ")<<label<<std::endl;if(!ok)++failures;};
    if(!s.isolatedTest){check(false,"isolated instance required");return 1;}
    const auto prefs=s.appPrefs; const float scale_before=s.scale; const bool dark_before=s.darkMode; const auto theme_before=s.themeOverride;
    RECT previous{};GetWindowRect(s.hwnd,&previous);
    s.appPrefs.persist=false;s.appPrefs.sidebar_collapsed=false;
    auto click=[&](int x,int y){
        SendMessageW(s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
    };
    auto toggle=[&]{const auto r=s.renderer.SidebarToggleRect(static_cast<float>(s.compositor.Width()));
        click(static_cast<int>((r.left+r.right)/2),static_cast<int>((r.top+r.bottom)/2));};
    for(float scale:{1.25f,1.5f}) {
        s.scale=scale;s.renderer.SetScale(scale);
        for(int dip:{899,900}) {
            if(dip==899) CloseSidebarPeek(s);
            s.appPrefs.sidebar_collapsed=false;s.renderer.SetSidebarCollapsed(false,false);
            RECT client{},outer{};GetClientRect(s.hwnd,&client);GetWindowRect(s.hwnd,&outer);
            const int width=static_cast<int>(dip*scale+0.5f);
            SetWindowPos(s.hwnd,nullptr,0,0,width+(outer.right-outer.left)-(client.right-client.left),
                static_cast<int>(650*scale)+(outer.bottom-outer.top)-(client.bottom-client.top),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            s.scale=scale;s.renderer.SetScale(scale);s.compositor.RecreateTextFormats(scale);Render(s);
            const float w=static_cast<float>(s.compositor.Width()),h=static_cast<float>(s.compositor.Height());
            log<<"[CASE] dip="<<dip<<" scale="<<scale<<" actual_width="<<w<<std::endl;
            check(static_cast<int>(w)==width,"real window client width matches requested scale boundary");
            const auto button=s.renderer.SidebarToggleRect(w);
            check(button.right>button.left && button.left>=0,"sidebar toggle remains visible");
            if(dip==900) {
                check(!s.renderer.SidebarAutoCollapsed(w),"900 DIP is expanded by default");
                check(!s.renderer.SidebarPeek(),"resize to expanded 900 DIP dismisses old narrow overlay");
                toggle();check(s.appPrefs.sidebar_collapsed,"wide-window click retains preference collapse behavior");
                toggle();check(!s.appPrefs.sidebar_collapsed,"wide-window second click restores expanded preference");
                continue;
            }
            check(s.renderer.SidebarAutoCollapsed(w) && s.renderer.SidebarRailVisible(w),"899 DIP uses automatic rail");
            const auto content=s.renderer.ContentRect(w,h);
            toggle();Render(s);
            check(s.renderer.SidebarPeekVisible(w) && s.renderer.SidebarPeekPinned() && !s.appPrefs.sidebar_collapsed,
                "real narrow toggle opens pinned overlay without changing saved expanded preference");
            check(s.renderer.ContentRect(w,h).left==content.left && s.renderer.ContentRect(w,h).right==content.right,
                "overlay does not shift or squeeze file content");
            for(bool dark:{false,true}) {
                SetThemeMode(s,dark?2:1);
                // Capture the final theme, rather than an in-flight reveal of the previous frame.
                s.themeReveal.start=0;
                Render(s); Render(s); // Fill both flip-model buffers before reading buffer 0.
                check(BuildVm(s,false).dark==dark && s.darkMode==dark,
                    "snapshot view model and rendered theme match requested light or dark mode");
                const auto png=std::filesystem::path(output).parent_path()/
                    (std::wstring(L"sidebar-narrow-")+(scale==1.25f?L"125-":L"150-")+(dark?L"dark.png":L"light.png"));
                check(s.compositor.SaveSnapshot(png.c_str()),"capture actual narrow sidebar overlay");
                log<<"screenshot="<<png.string()<<std::endl;
                ui::ComPtr<IWICImagingFactory> wic;
                ui::ComPtr<IWICBitmapDecoder> decoder;
                ui::ComPtr<IWICBitmapFrameDecode> frame;
                ui::ComPtr<IWICFormatConverter> converted;
                BYTE pixels[5*5*4]{};
                const auto area=s.renderer.ContentRect(w,h);
                WICRect sample{static_cast<INT>(area.right-40*scale),static_cast<INT>(area.bottom-40*scale),5,5};
                const bool read=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))) &&
                    SUCCEEDED(wic->CreateDecoderFromFilename(png.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)) &&
                    SUCCEEDED(decoder->GetFrame(0,&frame)) && SUCCEEDED(wic->CreateFormatConverter(&converted)) &&
                    SUCCEEDED(converted->Initialize(frame.get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)) &&
                    SUCCEEDED(converted->CopyPixels(&sample,5*4,sizeof(pixels),pixels));
                unsigned brightness=0;
                for(int i=0;i<25;++i)brightness+=pixels[i*4]+pixels[i*4+1]+pixels[i*4+2];
                log<<"[PIXELS] dark="<<dark<<" mean_rgb="<<brightness/(25*3)<<std::endl;
                check(read && (dark ? brightness<25*3*128 : brightness>25*3*128),
                    "saved PNG content pixels match requested theme rather than previous swap buffer");
            }
            toggle();check(!s.renderer.SidebarPeek() && !s.appPrefs.sidebar_collapsed,"second narrow toggle closes overlay without preference change");
            toggle();
            click(static_cast<int>(w-30*scale),static_cast<int>(h-70*scale));
            check(!s.renderer.SidebarPeek() && !s.appPrefs.sidebar_collapsed,"outside click dismisses narrow overlay without preference change");
            const int rail_x=static_cast<int>(10*scale),rail_y=static_cast<int>(s.renderer.TitleBarHeight()+40*scale);
            SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(rail_x,rail_y));
            check(s.sidebarPeekArmAt!=0,"real mouse move on automatic rail arms hover peek");
            SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(w-10),rail_y));
            check(s.sidebarPeekArmAt==0,"leaving rail cancels hover arm");
            toggle();
            SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(w-10),rail_y));
            check(s.renderer.SidebarPeekPinned(),"outside mouse move preserves click-pinned overlay");
            SendMessageW(s.hwnd,WM_ACTIVATE,WA_INACTIVE,0);
            check(!s.renderer.SidebarPeek(),"deactivation dismisses pinned overlay");
            s.appPrefs.sidebar_collapsed=true;s.renderer.SetSidebarCollapsed(true,false);
            toggle();check(s.renderer.SidebarPeekPinned() && s.appPrefs.sidebar_collapsed,
                "narrow toggle opens overlay while preserving collapsed preference");
            toggle();check(!s.renderer.SidebarPeek() && s.appPrefs.sidebar_collapsed,
                "narrow toggle closes overlay while preserving collapsed preference");
            s.appPrefs.sidebar_collapsed=false;s.renderer.SetSidebarCollapsed(false,false);
            toggle();check(s.renderer.SidebarPeekPinned(),"prepare open overlay for resize boundary check");
        }
    }
    CloseSidebarPeek(s);s.appPrefs=prefs;s.scale=scale_before;s.darkMode=dark_before;s.themeOverride=theme_before;
    s.renderer.SetScale(scale_before);s.compositor.RecreateTextFormats(scale_before);s.renderer.SetSidebarCollapsed(prefs.sidebar_collapsed,false);
    SetWindowPos(s.hwnd,nullptr,previous.left,previous.top,previous.right-previous.left,previous.bottom-previous.top,SWP_NOZORDER|SWP_NOACTIVATE);
    Render(s);log<<"failures="<<failures<<std::endl;return failures?1:0;
}
namespace pulse::ui {
HWND ProbeToolbarPicker(const wchar_t*, POINT*, const wchar_t*);
bool ProbeToolbarPickerLayout(const wchar_t*, const wchar_t*);
}
namespace {
bool CaptureLiveMenu(HWND menu, const std::filesystem::path& path) {
    RECT r{};
    if (!IsWindowVisible(menu) || !GetWindowRect(menu, &r)) return false;
    const int w = r.right-r.left, h = r.bottom-r.top;
    if (w <= 0 || h <= 0) return false;
    DwmFlush();
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (memory && bitmap) {
        const auto previous = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(memory, previous);
        if (ok) {
            ui::ComPtr<IWICImagingFactory> factory;
            ui::ComPtr<IWICBitmap> source;
            ui::ComPtr<IWICStream> stream;
            ui::ComPtr<IWICBitmapEncoder> encoder;
            ui::ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) &&
                SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
                SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) &&
                SUCCEEDED(frame->SetPixelFormat(&format)) &&
                SUCCEEDED(frame->WriteSource(source.get(), nullptr)) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return ok;
}
struct ToolbarInteractionProbe {
    AppState* state = nullptr;
    std::filesystem::path screenshot;
    ULONGLONG started = 0;
    int phase = 0;
    int menu_choice = -1;
    bool menu_seen = false, picker_seen = false, dragged = false, captured = false;
    bool hover_processed = false;
    bool tooltip_cleared = false;
    bool layout_only = false, layout_passed = false;
    std::filesystem::path layout_report;
};
ToolbarInteractionProbe* interaction_probe = nullptr;
void CALLBACK DriveToolbarInteraction(HWND, UINT, UINT_PTR timer, DWORD) {
    auto& p = *interaction_probe;
    if (GetTickCount64()-p.started > 5000) {
        if (p.state->menu) p.state->menu->Dismiss();
        ui::CloseToolbarPicker(); KillTimer(nullptr,timer); return;
    }
    if (p.state->menu && p.state->menu->IsOpen() && !p.menu_seen) {
        HWND menu = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM arg)->BOOL {
            wchar_t title[32]{};
            GetWindowTextW(window,title,ARRAYSIZE(title));
            if (IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0)
                *reinterpret_cast<HWND*>(arg)=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(&menu));
        if (!menu) return;
        RECT bounds{}; GetClientRect(menu,&bounds);
        // This menu has exactly two ordinary rows, 4 DIP padding at each
        // end, and a 5 DIP separator. Derive row height from the real window
        // so the selected density and pre-existing pointer hover do not matter.
        const float scale=p.state->scale;
        const float row=(bounds.bottom-2*ui::FluentMenu::kShadowMargin-13*scale)/2;
        const int x=(bounds.right-bounds.left)/2;
        const int y=static_cast<int>(ui::FluentMenu::kShadowMargin+4*scale+
            row*(p.menu_choice+0.5f)+(p.menu_choice?5*scale:0));
        if (!p.screenshot.empty()) p.captured = CaptureLiveMenu(menu, p.screenshot);
        p.menu_seen = true;
        p.tooltip_cleared = p.state->tooltipText.empty() && p.state->hoverSince == 0;
        // Exercise the owner hover handler while the context menu's nested
        // message loop is active, then both actual menu rows before selection.
        const auto toggle=p.state->renderer.ToolbarLayoutAt(static_cast<float>(p.state->compositor.Width()),
            p.state->renderer.NewCommandRect(static_cast<float>(p.state->compositor.Width())).right-
            p.state->renderer.NewCommandRect(static_cast<float>(p.state->compositor.Width())).left);
        SendMessageW(p.state->hwnd,WM_MOUSEMOVE,0,MAKELPARAM(
            static_cast<int>((toggle.filter.left+toggle.filter.right)/2),static_cast<int>((toggle.filter.top+toggle.filter.bottom)/2)));
        RedrawWindow(p.state->hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
        for(int hovered:{0,1,0,1}) {
            const int hover_y=static_cast<int>(ui::FluentMenu::kShadowMargin+4*scale+row*(hovered+0.5f)+(hovered?5*scale:0));
            SendMessageW(menu,WM_MOUSEMOVE,0,MAKELPARAM(x,hover_y));
        }
        p.hover_processed=p.state->menu->IsOpen();
        PostMessageW(menu,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
        PostMessageW(menu,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        PostMessageW(menu,WM_LBUTTONUP,0,MAKELPARAM(x,y));
        return;
    }
    const HWND picker = ui::ProbeToolbarPicker(nullptr,nullptr,nullptr);
    if (!picker) return;
    p.picker_seen = true;
    if (p.layout_only) {
        KillTimer(nullptr, timer);
        p.layout_passed = ui::ProbeToolbarPickerLayout(p.layout_report.c_str(), p.screenshot.c_str());
        ui::CloseToolbarPicker();
        return;
    }
    if (p.menu_choice >= 0) { ui::CloseToolbarPicker(); return; }
    if (p.phase++ == 0) {
        SendMessageW(picker,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-120*20)),0);
        return;
    }
    if (p.phase == 2) {
        POINT from{},to{};
        if (ui::ProbeToolbarPicker(L"group",&from,nullptr) && ui::ProbeToolbarPicker(L"sort",&to,nullptr)) {
            SendMessageW(picker,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(from.x,from.y));
            SendMessageW(picker,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
            SendMessageW(picker,WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y));
            p.dragged = true;
        }
        return;
    }
    p.captured = ui::ProbeToolbarPicker(nullptr,nullptr,p.screenshot.c_str()) != nullptr;
    ui::CloseToolbarPicker();
}
}
namespace {
struct ToolbarMenuDragProbe {
    AppState* state=nullptr;
    bool drag=false, seen=false, below_threshold=false;
    ULONGLONG started=0;
    std::filesystem::path screenshot;
};
ToolbarMenuDragProbe* toolbar_menu_drag_probe=nullptr;
void CALLBACK DriveToolbarMenuDrag(HWND,UINT,UINT_PTR timer,DWORD) {
    auto& p=*toolbar_menu_drag_probe;
    if(GetTickCount64()-p.started>5000) {p.state->menu->Dismiss();KillTimer(nullptr,timer);return;}
    if(!p.state->menu->IsOpen())return;
    HWND menu=nullptr;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM arg)->BOOL {
        wchar_t title[32]{};GetWindowTextW(window,title,ARRAYSIZE(title));
        if(IsWindowVisible(window) && wcscmp(title,L"PulseMenu")==0)*reinterpret_cast<HWND*>(arg)=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&menu));
    if(!menu)return;
    KillTimer(nullptr,timer);p.seen=true;
    RECT bounds{};GetClientRect(menu,&bounds);
    const int x=(bounds.right-bounds.left)/2;
    const int y=static_cast<int>(ui::FluentMenu::kShadowMargin+16*p.state->scale);
    if(!p.screenshot.empty())CaptureLiveMenu(menu,p.screenshot);
    SendMessageW(menu,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    SendMessageW(menu,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+static_cast<int>(5*p.state->scale),y));
    p.below_threshold=p.state->menu->IsOpen() && p.state->menu->DraggedCommand()==0;
    if(p.drag)SendMessageW(menu,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+static_cast<int>(10*p.state->scale),y));
    else SendMessageW(menu,WM_LBUTTONUP,0,MAKELPARAM(x,y));
}
}
int RunToolbarInteractionTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)}; int failures=0;
    auto check=[&](bool ok,const char* what){log<<(ok?"[PASS] ":"[FAIL] ")<<what<<std::endl;if(!ok)++failures;};
    if (!s.isolatedTest) { check(false,"isolated instance required");return 1; }
    const auto prefs=s.appPrefs; const bool dark=s.darkMode;
    s.appPrefs.persist=false;
    s.appPrefs.folder_size_auto=true;
    if (GetEnvironmentVariableW(L"PULSE_TEST_TOOLBAR_LAYOUT_ONLY", nullptr, 0)) {
        ToolbarInteractionProbe probe;
        probe.state = &s;
        probe.started = GetTickCount64();
        probe.layout_only = true;
        const auto folder = std::filesystem::path(output).parent_path();
        probe.layout_report = folder / L"toolbar-picker-layout-metrics.txt";
        probe.screenshot = folder / L"toolbar-picker-layout";
        interaction_probe = &probe;
        const auto timer = SetTimer(nullptr, 0, 150, DriveToolbarInteraction);
        ShowToolbarCustomize(s);
        KillTimer(nullptr, timer);
        interaction_probe = nullptr;
        check(probe.picker_seen && probe.layout_passed, "picker labels, descriptions and work-area layout at 100/150/200 percent DPI");
        s.appPrefs = prefs;
        s.darkMode = dark;
        log << "failures=" << failures << std::endl;
        return failures ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_TOOLBAR_DRAG_ONLY", nullptr, 0)) {
        s.appPrefs.window_effect=L"none";
        s.appPrefs.background_image.clear();
        s.appPrefs.wallpaper_look=0;s.appPrefs.frame_transparency=0;s.appPrefs.menu_acrylic=false;
        auto settle = [&] {
            s.notification_toast.DismissFor(s.hwnd,WM_COMMAND);
            const auto until=GetTickCount64()+350;
            while(GetTickCount64()<until){Render(s);Sleep(16);}
            Render(s);
        };
        s.appPrefs.toolbar_show_sort=s.appPrefs.toolbar_show_filter=s.appPrefs.toolbar_show_group=true;
        s.appPrefs.toolbar_tools = {L"copy_path", L"terminal", L"|", L"properties", L"hidden"};
        Render(s);
        auto layout = [&] {
            const float width = static_cast<float>(s.compositor.Width());
            const float left = s.renderer.EffectiveSidebarWidth(width);
            return s.renderer.ToolbarLayoutAt(width, s.renderer.NewButtonWidthPx(width-left<600*s.scale), 0);
        };
        auto center = [](D2D1_RECT_F r) { return POINT{static_cast<LONG>((r.left+r.right)/2), static_cast<LONG>((r.top+r.bottom)/2)}; };
        auto original = s.appPrefs.toolbar_tools;
        auto first = center(layout().pins[0]);
        auto third = center(layout().pins[2]);
        SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(first.x, first.y));
        check(ToolbarPinDragging(), "ordinary toolbar press starts pending drag without picker");
        MoveToolbarPinDrag(s, POINT{first.x+static_cast<LONG>(5*s.scale),first.y});
        check(s.appPrefs.toolbar_tools==original, "movement below 6 DIP preserves click threshold");
        MoveToolbarPinDrag(s, third);
        check(s.appPrefs.toolbar_tools!=original, "drag crosses separator into another tool group");
        SendMessageW(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!ToolbarPinDragging() && s.appPrefs.toolbar_tools==original, "Escape restores original order");
        BeginToolbarPinDrag(s,0,first);MoveToolbarPinDrag(s,third);EndToolbarPinDrag(s,false);
        check(s.appPrefs.toolbar_tools!=original, "drop commits reordered tools");
        RestoreToolbarDrag(s);
        check(s.appPrefs.toolbar_tools==original, "restore snack action restores prior order");
        Render(s);
        BeginToolbarPinDrag(s,0,first);MoveToolbarPinDrag(s,center(layout().overflow));EndToolbarPinDrag(s,false);
        check(std::find(s.appPrefs.toolbar_tools.begin(),s.appPrefs.toolbar_tools.end(),L"copy_path")==s.appPrefs.toolbar_tools.end(), "drop on More removes the visible pin");
        bool found=false;
        for (const auto& item:BuildToolbarMoreItems(s)) found=found || item.drag_command==app::ToolbarToolIndex(L"copy_path")+1;
        check(found, "removed command remains runnable and draggable in More");
        auto screen=third;ClientToScreen(s.hwnd,&screen);
        BeginToolbarMenuDrag(s,app::ToolbarToolIndex(L"copy_path"),screen);
        MoveToolbarPinDrag(s,first);EndToolbarPinDrag(s,false);
        check(std::find(s.appPrefs.toolbar_tools.begin(),s.appPrefs.toolbar_tools.end(),L"copy_path")!=s.appPrefs.toolbar_tools.end(), "drag from More pins command again");
        RestoreToolbarDrag(s);
        s.appPrefs.toolbar_tools=original;Render(s);
        const auto before_right=layout();
        POINT right_drop{static_cast<LONG>(before_right.sort.left-16*s.scale),first.y};
        BeginToolbarPinDrag(s,0,first);MoveToolbarPinDrag(s,right_drop);EndToolbarPinDrag(s,false);Render(s);
        check(app::ToolbarRightFrom(s.appPrefs.toolbar_tools)>=0, "drop across central gap persists right tool group");
        const auto grouped=layout();
        const int right_slot=app::ToolbarRightFrom(s.appPrefs.toolbar_tools);
        check(right_slot>=0 && grouped.pins[right_slot].left>grouped.create.right+80*s.scale,
              "right tool group is visibly aligned beside view controls");
        RestoreToolbarDrag(s);Render(s);
        const auto split=layout().pins[2];
        POINT separator{static_cast<LONG>(split.left-6*s.scale),first.y};
        check(BeginToolbarSeparatorDrag(s,separator), "separator has independent drag target");
        MoveToolbarPinDrag(s,center(layout().pins[1]));EndToolbarPinDrag(s,false);
        check((app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools)&2)!=0, "separator moves without executing command");
        RestoreToolbarDrag(s);Render(s);
        const auto view_order=s.appPrefs.toolbar_view_order;
        BeginToolbarViewDrag(s,0,center(layout().sort));MoveToolbarPinDrag(s,center(layout().group));EndToolbarPinDrag(s,false);
        check(s.appPrefs.toolbar_view_order!=view_order,"right view controls reorder by direct drag");
        RestoreToolbarDrag(s);Render(s);
        check(s.appPrefs.toolbar_view_order==view_order,"restore reverts view control order");
        BeginToolbarViewDrag(s,0,center(layout().sort));MoveToolbarPinDrag(s,center(layout().pins[1]));EndToolbarPinDrag(s,false);Render(s);
        check(app::ToolbarViewSlots(s.appPrefs.toolbar_tools)[0]>=0,"sort drag across central gap persists left slot");
        check(layout().sort.left<layout().pins[2].left,"sort is rendered between left tool pins");
        check(app::NormalizeToolbarTools(s.appPrefs.toolbar_tools)==s.appPrefs.toolbar_tools, "layout markers survive preference normalization");
        const auto marker_tools=s.appPrefs.toolbar_tools;
        const auto marker_slot=app::ToolbarViewSlots(marker_tools);
        const auto sep_pin=layout().pins[2];
        BeginToolbarSeparatorDrag(s,POINT{static_cast<LONG>(sep_pin.left-6*s.scale),first.y});
        MoveToolbarPinDrag(s,center(layout().pins[1]));EndToolbarPinDrag(s,false);
        check(app::ToolbarViewSlots(s.appPrefs.toolbar_tools)==marker_slot &&
              app::ToolbarRightFrom(s.appPrefs.toolbar_tools)==app::ToolbarRightFrom(marker_tools),
              "separator drag preserves view and right-group position markers");
        s.appPrefs.toolbar_tools=marker_tools;Render(s);
        const auto left_png=std::filesystem::path(output).parent_path()/L"toolbar-sort-left.png";
        settle();
        check(s.compositor.SaveSnapshot(left_png.c_str()),"left group view control screenshot saved");
        RestoreToolbarDrag(s);Render(s);
        {
            const auto before=s.appPrefs.toolbar_tools;
            ToolbarMenuDragProbe probe;probe.state=&s;probe.drag=true;probe.started=GetTickCount64();
            probe.screenshot=std::filesystem::path(output).parent_path()/L"toolbar-more-grips.png";
            toolbar_menu_drag_probe=&probe;
            const auto timer=SetTimer(nullptr,0,150,DriveToolbarMenuDrag);
            const auto more_items=BuildToolbarMoreItems(s);
            ShowToolbarMore(s);
            KillTimer(nullptr,timer);toolbar_menu_drag_probe=nullptr;
            check(probe.seen && probe.below_threshold && s.menu->DraggedCommand()>0 && ToolbarPinDragging(),
                  "real More menu threshold hands capture to toolbar drag");
            SendMessageW(s.hwnd,WM_KEYDOWN,VK_ESCAPE,0);
            log<<"after menu Escape dragging="<<ToolbarPinDragging()<<" history="<<s.undoHistoryOpen<<" before=";
            auto ids=[&](const std::vector<std::wstring>& values) {
                for(const auto& id:values){for(wchar_t c:id)log<<static_cast<char>(c);log<<",";}
            };
            ids(before);log<<" after=";ids(s.appPrefs.toolbar_tools);log<<std::endl;
            check(!ToolbarPinDragging() && s.appPrefs.toolbar_tools==before,
                  "Escape after real More-menu handoff leaves hidden command unpinned");
            check(s.menu && s.menu->SaveDebugSnapshot(probe.screenshot.c_str(),more_items,0),
                  "More menu grip screenshot saved");
        }
        {
            ToolbarMenuDragProbe probe;probe.state=&s;probe.started=GetTickCount64();
            toolbar_menu_drag_probe=&probe;
            const auto timer=SetTimer(nullptr,0,150,DriveToolbarMenuDrag);
            ui::FluentMenuItem item;item.command=35099;item.text=L"Toolbar click fixture";item.drag_command=1;
            POINT anchor=first;ClientToScreen(s.hwnd,&anchor);
            const int result=s.menu->TrackPopup(anchor,{item});
            KillTimer(nullptr,timer);toolbar_menu_drag_probe=nullptr;
            check(probe.seen && probe.below_threshold && result==35099,
                  "real menu press and sub-threshold move still invoke clicked command");
        }
        const auto png=std::filesystem::path(output).parent_path()/L"toolbar-direct-drag.png";
        BeginToolbarPinDrag(s,0,center(layout().pins[0]));MoveToolbarPinDrag(s,center(layout().pins[2]));Render(s);
        settle();
        check(s.compositor.SaveSnapshot(png.c_str()), "direct toolbar drag screenshot saved");
        EndToolbarPinDrag(s,true);
        s.appPrefs=prefs;s.darkMode=dark;Render(s);
        log<<"failures="<<failures<<std::endl;return failures?1:0;
    }
    const auto folder_fixture=std::filesystem::path(output).parent_path()/L"toolbar-hover-folders";
    std::filesystem::create_directories(folder_fixture/L"nested"/L"child");
    { std::ofstream seed(folder_fixture/L"nested"/L"sample.txt");seed<<"isolated folder-size hover fixture"; }
    const auto previous_path=ActiveTab(s)->current_path;
    NavigateTo(s,folder_fixture.wstring());
    const auto deadline=GetTickCount64()+5000;
    while(GetTickCount64()<deadline) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        auto* tab=ActiveTab(s);
        if(tab && !tab->loading && tab->snapshot && !tab->snapshot->empty())break;
        Sleep(10);
    }
    check(ActiveTab(s) && !ActiveTab(s)->loading && ActiveTab(s)->snapshot && !ActiveTab(s)->snapshot->empty(),
        "isolated folder-size fixture is enumerated before nested popup render regression");
    s.appPrefs.toolbar_show_sort=s.appPrefs.toolbar_show_filter=s.appPrefs.toolbar_show_group=true;
    for (bool theme : {false,true}) {
        s.darkMode=theme;
        s.appPrefs.toolbar_view_order={L"sort",L"filter",L"group"};
        Render(s);
        ToolbarInteractionProbe probe; probe.state=&s;probe.started=GetTickCount64();
        probe.screenshot=std::filesystem::path(output).parent_path()/(theme?L"toolbar-picker-dark.png":L"toolbar-picker-light.png");
        interaction_probe=&probe;
        const auto timer=SetTimer(nullptr,0,450,DriveToolbarInteraction);
        ShowToolbarCustomize(s);
        KillTimer(nullptr,timer);interaction_probe=nullptr;
        check(probe.picker_seen && probe.dragged,"real picker receives group-to-sort mouse drag");
        check(!s.appPrefs.toolbar_view_order.empty() && s.appPrefs.toolbar_view_order.front()==L"group",
            "real drag callback updates toolbar view preference order");
        Render(s);
        const float w=static_cast<float>(s.compositor.Width());
        const auto create=s.renderer.NewCommandRect(w);
        const auto row=s.renderer.ToolbarLayoutAt(w,create.right-create.left);
        check(row.group.left<row.sort.left && row.sort.left<row.filter.left,
            "actual renderer follows dragged group sort filter order");
        check(probe.captured,"picker compositor captures real shadow surface for visual review");
        log<<"screenshot="<<probe.screenshot.string()<<std::endl;
    }
    s.darkMode=dark;
    for (int round=0;round<3;++round) for (int item=0;item<3;++item) {
        s.appPrefs.toolbar_show_sort=s.appPrefs.toolbar_show_filter=s.appPrefs.toolbar_show_group=true;
        for (int choice : {1,0}) {
            s.darkMode = choice==0;
            Render(s);
            const float w=static_cast<float>(s.compositor.Width());
            const auto create=s.renderer.NewCommandRect(w);
            const auto row=s.renderer.ToolbarLayoutAt(w,create.right-create.left);
            const auto r=item==0?row.sort:item==1?row.filter:row.group;
            s.hoverRegion = item==0?ui::HitTestResult::ToolbarSort:item==1?ui::HitTestResult::FilterBox:ui::HitTestResult::ToolbarGroup;
            s.hoverSince = GetTickCount64()-2000;
            s.tooltipText = TooltipForHover(s);
            check(!s.tooltipText.empty(),"real toolbar tooltip seeded before context menu" );
            Render(s);
            ToolbarInteractionProbe probe;probe.state=&s;probe.started=GetTickCount64();probe.menu_choice=choice;
            if (round==0 && item==1) probe.screenshot=std::filesystem::path(output).parent_path()/(choice==1?L"menu-live-light.png":L"menu-live-dark.png");
            interaction_probe=&probe;
            const auto timer=SetTimer(nullptr,0,250,DriveToolbarInteraction);
            log << "[PHASE] round=" << round << " right-click control=" << item << " choice=" << choice << std::endl;
            SendMessageW(s.hwnd,WM_RBUTTONUP,0,MAKELPARAM(static_cast<int>((r.left+r.right)/2),static_cast<int>((r.top+r.bottom)/2)));
            KillTimer(nullptr,timer);interaction_probe=nullptr;
            check(probe.menu_seen,"actual toolbar control right click opens context menu");
            if (!probe.screenshot.empty()) {
                check(probe.captured,"capture visible menu screen rectangle for visual review");
                log << "screenshot=" << probe.screenshot.string() << std::endl;
            }
            check(probe.tooltip_cleared,"toolbar tooltip cleared before context menu message loop" );
            check(probe.hover_processed,"owner and menu mouse hover handlers complete inside open context menu");
            if(choice==1)check(probe.picker_seen,"context customize item opens actual picker");
            else check(!(item==0?s.appPrefs.toolbar_show_sort:item==1?s.appPrefs.toolbar_show_filter:s.appPrefs.toolbar_show_group),
                "context remove item disables corresponding toolbar control");
        }
    }
    s.appPrefs=prefs;s.darkMode=dark;NavigateTo(s,previous_path);Render(s);
    log<<"failures="<<failures<<std::endl;return failures?1:0;
}
namespace {
AppState* toolbar_test_state = nullptr;
bool toolbar_path_opened = false;
void CALLBACK CloseToolbarPathMenu(HWND, UINT, UINT_PTR timer, DWORD) {
    auto* s = toolbar_test_state;
    if (s && s->menu && s->menu->IsOpen()) {
        toolbar_path_opened = s->addressEditing && !s->addressSearching;
        s->menu->Dismiss();
        KillTimer(nullptr, timer);
    }
}
}
int RunToolbarLayoutTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) { log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!ok) ++failures; };
    s.appPrefs.persist = false;
    auto* tab = ActiveTab(s);
    s.appPrefs.address_search_current = true;
    ui::WindowViewModel hint;
    FillAddressSearchView(s,hint);
    check(hint.address_search_placeholder.find(app::TabTitle(tab->current_path)) != std::wstring::npos,
        "search placeholder includes current directory");
    const auto original_path=tab->current_path;
    tab->current_path=L"C:\\Other folder";
    FillAddressSearchView(s,hint);
    check(hint.address_search_placeholder.find(L"Other folder") != std::wstring::npos,
        "search placeholder follows directory changes");
    s.appPrefs.address_search_current=false;
    FillAddressSearchView(s,hint);
    check(hint.address_search_placeholder.find(l10n::Get(l10n::StringId::SearchScopeAll)) != std::wstring::npos,
        "all-disk search placeholder reports actual scope");
    tab->current_path=original_path;
    s.appPrefs.address_search_current=true;
    tab->search_input_path = tab->current_path;
    tab->search_input_text = L"report";
    tab->search_input_current=true;
    tab->search_input_root=tab->current_path;
    BYTE keys[256]{}, control[256]{};
    GetKeyboardState(keys);
    control[VK_CONTROL]=0x80;
    SetKeyboardState(control);
    SendMessageW(s.hwnd,WM_KEYDOWN,L'K',0);
    SetKeyboardState(keys);
    check(s.addressSearching && s.addressEditing,"Ctrl+K focuses the toolbar search field");
    const auto vm = BuildVm(s, false);
    const float width = static_cast<float>(s.compositor.Width());
    for (float sidebar_width : {224.0f, 1200.0f}) {
    s.renderer.SetSidebarWidthDip(sidebar_width);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        s.renderer.SetScale(scale);
        for (float dip_width : {480.0f, 640.0f, 900.0f, 1280.0f}) {
            const float w = dip_width * scale;
            const auto bounds = D2D1::RectF(0, 0, w, 900 * scale);
            const auto address = s.renderer.AddressBarRect(w);
            const auto search = s.renderer.SearchBarRect(w);
            const auto sidebar = s.renderer.SidebarRect(w, bounds.bottom);
            const auto bar = s.renderer.ToolbarRect(w);
            const auto controls = ui::LayoutAddressSearch(search, scale);
            auto region = [&](D2D1_RECT_F r) {
                return s.renderer.HitTest(vm, bounds, (r.left+r.right)/2, (r.top+r.bottom)/2).region;
            };
            check(address.right < search.left && controls.input.right > controls.input.left,
                "separate address and search fields fit narrow windows and DPI");
            check(bar.left == sidebar.right && sidebar.top < bar.bottom && address.left > sidebar.right,
                "toolbar starts after full-height sidebar");
            const auto toolbar = ui::MakeToolbarLayout(w,scale,s.renderer.TitleBarHeight(),s.renderer.Margin(),
                s.renderer.NewCommandRect(w).right-s.renderer.NewCommandRect(w).left,sidebar.right,
                0.0f,s.renderer.SearchBarMinWidth());
            const ui::HitTestResult::Region navigation[]={ui::HitTestResult::NavBack,ui::HitTestResult::NavForward,
                ui::HitTestResult::NavUp,ui::HitTestResult::NavRefresh};
            bool navigation_ok=true;
            for(int i=0;i<4;++i) navigation_ok &= toolbar.navigation[i].left >= sidebar.right &&
                region(toolbar.navigation[i]) == navigation[i];
            check(navigation_ok,"all four navigation icons remain clickable to right of sidebar");
            const auto sidebar_hit=s.renderer.HitTest(vm,bounds,16*scale,sidebar.top+18*scale);
            check(sidebar_hit.region == ui::HitTestResult::SidebarItem,
                "sidebar first item above file area is not intercepted by toolbar");
            check(region(address) == ui::HitTestResult::AddressBar &&
                  region(controls.input) == ui::HitTestResult::AddressSearchInput,
                "address and search input have independent hit targets");
            check(region(controls.name) == ui::HitTestResult::AddressSearchMode &&
                  controls.content.right <= controls.content.left &&
                  region(controls.close) == ui::HitTestResult::AddressSearchClose,
                "search controls remain reachable across window sizes");
            const auto create = s.renderer.NewCommandRect(w);
            check(create.top > address.bottom && region(create) == ui::HitTestResult::NewButton,
                "second toolbar row and popup anchors match hit targets");
            const auto new_arrow = s.renderer.NewArrowRect(w);
            check(new_arrow.left > create.left && new_arrow.right == create.right &&
                  region(new_arrow) == ui::HitTestResult::NewButtonArrow &&
                  region(D2D1::RectF(create.left, create.top, new_arrow.left, create.bottom)) ==
                      ui::HitTestResult::NewButton,
                "split New button: chevron opens the menu, main part creates a folder");
            // Sort and Group drop-downs are sized to their labels, so each chevron
            // sits the same distance after its text; hit targets follow.
            const auto row = s.renderer.ToolbarLayoutAt(w, create.right - create.left);
            if (w - sidebar.right >= 700*scale) {
                const float sort_w = row.sort.right - row.sort.left;
                check(sort_w >= 60*scale && region(row.sort) == ui::HitTestResult::ToolbarSort,
                    "sort drop-down fits its label and stays clickable");
                if (row.group.right > row.group.left && sort_w < 120*scale) {
                    const float group_w = row.group.right - row.group.left;
                    const std::wstring& sort_label = l10n::Get(l10n::StringId::ToolbarSort);
                    const std::wstring& group_label = l10n::Get(l10n::StringId::ToolbarGroup);
                    bool cjk = sort_label.size() == group_label.size() && !sort_label.empty();
                    for (wchar_t c : sort_label + group_label) cjk = cjk && c >= 0x4E00 && c <= 0x9FFF;
                    const float diff = sort_w > group_w ? sort_w - group_w : group_w - sort_w;
                    check(region(row.group) == ui::HitTestResult::ToolbarGroup && (!cjk || diff < 1.0f),
                        "group drop-down matches sort: chevron right after the label");
                    // Filter has no chevron: same label length = 20 DIP (chevron + gap) narrower,
                    // unless the 64 DIP floor (kept above icon-only) applies.
                    const float filter_w = row.filter.right - row.filter.left;
                    const float chevron = sort_w - filter_w;
                    const bool at_floor = filter_w <= 64.0f*scale + 0.5f;
                    check(region(row.filter) == ui::HitTestResult::FilterBox && filter_w >= 60*scale &&
                          (!cjk || at_floor || (chevron > 19.0f*scale && chevron < 21.0f*scale)),
                        "filter fits its label like sort and group");
                }
            }
        }
    }
    }
    s.renderer.SetSidebarWidthDip(static_cast<float>(s.appPrefs.sidebar_width));
    s.renderer.SetScale(s.scale);
    Render(s);
    check(s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path()/L"search-separated.png").c_str()),
        "separate search screenshot captured");
    const auto address = s.renderer.AddressBarRect(width);
    toolbar_test_state = &s;
    toolbar_path_opened = false;
    const auto timer = SetTimer(nullptr, 0, 50, CloseToolbarPathMenu);
    SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(
        static_cast<int>(address.right-8*s.scale), static_cast<int>((address.top+address.bottom)/2)));
    KillTimer(nullptr, timer);
    toolbar_test_state = nullptr;
    check(toolbar_path_opened, "click address while searching opens path menu");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

int RunSearchFlowTest(AppState& s, const wchar_t* output) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_TOOLBAR_INTERACTION_ONLY", nullptr, 0)) return RunToolbarInteractionTest(s, output);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TOOLBAR_LAYOUT", nullptr, 0)) return RunToolbarLayoutTest(s, output);
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
    const auto layout = ui::LayoutAddressSearch(s.renderer.SearchBarRect(width), s.scale);
    const auto window = D2D1::RectF(0, 0, width, static_cast<float>(s.compositor.Height()));
    auto hit = [&](D2D1_RECT_F rect) {
        return s.renderer.HitTest(vm, window, (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f).region;
    };
    check(hit(layout.name) == ui::HitTestResult::AddressSearchMode, "mode chip hit target");
    check(layout.content.right <= layout.content.left, "mode is one toggle chip, not two segments");
    {
        const bool before = s.addressSearchContent;
        const int mx = static_cast<int>((layout.name.left + layout.name.right) * 0.5f);
        const int my = static_cast<int>((layout.name.top + layout.name.bottom) * 0.5f);
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mx, my));
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(mx, my));
        check(s.addressSearchContent != before, "mode chip toggles name/content");
        SwitchAddressSearchMode(s, before);
        vm = BuildVm(s, false);
    }
    check(hit(layout.options) == ui::HitTestResult::AddressSearchOptions, "separate options hit target");
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}

#endif