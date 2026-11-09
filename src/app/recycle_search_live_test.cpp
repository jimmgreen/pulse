#include "app_internal.h"
#include "app_navigation.h"
#include "app_hosted_edit.h"
#include "global_search_handoff.h"
#include "../fs/fs_recycle.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <shellapi.h>

namespace pulse {
int RunRecycleSearchLiveTest(AppState& s, const wchar_t* output) {
    namespace files = std::filesystem;
    const files::path report(output);
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const auto allowed = files::weakly_canonical(files::path(executable).parent_path().parent_path()/L"bench_data").wstring()+L"\\";
    const auto root = files::weakly_canonical(report.parent_path());
    const auto root_text = root.wstring()+L"\\";
    if (!s.isolatedTest || root_text.size()<=allowed.size() || _wcsnicmp(root_text.c_str(),allowed.c_str(),allowed.size())) return 2;
    std::ofstream log(report);
    int failures = 0;
    auto check = [&](bool ok, const char* label) { log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<std::endl; if (!ok) ++failures; };
    auto pump = [&](const std::function<bool()>& ready, DWORD timeout) {
        const auto until = GetTickCount64()+timeout;
        do {
            MSG msg{};
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                if (msg.message==WM_QUIT) return false;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (ready()) return true;
            Sleep(15);
        } while (GetTickCount64()<until);
        return ready();
    };
    const std::wstring token = L"pulse-recycle-probe-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto original = root/(token+L"-\u4e2d\u6587.txt");
    { std::ofstream file(original,std::ios::binary); file<<"Pulse isolated recycle search fixture"; }
    check(files::exists(original),"create isolated bench_data file");
    std::wstring source = original.wstring(); source.push_back(L'\0'); source.push_back(L'\0');
    SHFILEOPSTRUCTW deletion{};
    deletion.hwnd=s.hwnd; deletion.wFunc=FO_DELETE; deletion.pFrom=source.c_str();
    deletion.fFlags=FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT;
    const int deleted=SHFileOperationW(&deletion);
    check(deleted==0 && !deletion.fAnyOperationsAborted && !files::exists(original),"recycle only the generated test file through Windows");
    fs::DirEntry recycled;
    bool discovered=pump([&] {
        std::vector<fs::DirEntry> entries; fs::EnumerateRecycleBin(entries);
        for (const auto& entry:entries) if (entry.full_path==fs::NormalizePath(original.wstring()) && !entry.recycle_path.empty()) { recycled=entry; return true; }
        return false;
    },5000);
    check(discovered,"real current-user recycle bin exposes original name and payload identity");
    if (discovered) {
        s.appPrefs.persist=false;
        ShowWindow(s.hwnd,SW_SHOWNORMAL);
        ShowWindow(s.hwnd,SW_SHOWNORMAL); SetForegroundWindow(s.hwnd); SetFocus(s.hwnd);
        check(IsWindowVisible(s.hwnd)!=FALSE,"real main-window fixture is visible");
        auto matching = [&]() -> size_t {
            const auto* tab=ActiveTab(s);
            if (!tab || !IsAddressSearchResults(tab) || tab->loading || tab->search_retaining_results || !tab->snapshot) return SIZE_MAX;
            for (size_t i=0;i<tab->snapshot->size();++i)
                if ((*tab->snapshot)[i].recycle_path==recycled.recycle_path) return i;
            return SIZE_MAX;
        };
        for (const std::wstring origin : {std::wstring{},std::wstring(L"pulse:recycle")}) {
            NavigateTo(s,origin);
            pump([&] { const auto* tab=ActiveTab(s); return tab && tab->current_path==origin && tab->snapshot!=nullptr; },10000);
            s.searchHistoryOpen=true;
            ShowAddressSearch(s);
            s.searchHistoryOpen=false;
            check(s.addressSearching && s.hwndAddressEdit,"real search editor opens");
            s.addressSearchCurrent=false;
            SetWindowTextW(s.hwndAddressEdit,token.c_str());
            SubmitAddressSearch(s);
            const bool found_query=pump([&] { const auto* current=ActiveTab(s); return matching()!=SIZE_MAX && current && current->snapshot->size()==1; },15000);
            check(found_query, origin.empty() ? "top all-disk search finds recycled original filename" : "all-disk search launched from recycle view also finds file");
            {
                const auto* current=ActiveTab(s);
                log<<"[DIAG] search="<<(current && IsAddressSearchResults(current))<<" loading="<<(current && current->loading)
                   <<" content="<<s.addressSearchContent<<" edit_length="<<GetWindowTextLengthW(s.hwndAddressEdit)
                   <<" pending="<<s.pendingIndexSearches.size()<<" rows="<<(current && current->snapshot ? current->snapshot->size() : 0)<<std::endl;
                for (const auto& [id,request]:s.pendingIndexSearches)
                    log<<"[DIAG] id="<<id<<" local_ready="<<request.local_ready<<" recycle_ready="<<request.recycle_ready
                       <<" local_error="<<request.local.error<<" recycle_error="<<request.recycle.error
                       <<" local_hits="<<request.local.hits.size()<<" recycle_hits="<<request.recycle.hits.size()
                       <<" needle_length="<<request.query.needle.size()<<" scope_length="<<request.query.path_prefix.size()<<std::endl;
            }
            const auto* tab=ActiveTab(s);
            const size_t index=matching();
            check(tab && index!=SIZE_MAX && (*tab->snapshot)[index].name==recycled.name &&
                (*tab->snapshot)[index].full_path==fs::NormalizePath(original.wstring()),"search row preserves original name and recycle identity");
        }
        if (matching()!=SIZE_MAX) {
            auto vm=BuildVm(s); vm.window_effect=ui::WindowEffect::None; vm.backdrop_enabled=false;
            auto* dc=s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(s.darkMode ? 0x202020 : 0xFFFFFF));
            s.renderer.Render(vm,D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),ui::MakeTheme(s.darkMode,s.accentColor));
            check(SUCCEEDED(dc->EndDraw()) && s.compositor.SaveSnapshot((root/L"recycle-search-results.png").c_str()),
                "real main-window recycled search results screenshot captured");
        }
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_NAME",original.filename().c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_PATH",original.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_REPORT",(root/L"global-search-live.log").c_str());
        const auto probe=files::path(executable).parent_path()/L"pulse_global_search_window_fixture.exe";
        std::wstring command=L"\""+probe.wstring()+L"\"";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(probe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) {
            CloseHandle(process.hThread);
            const bool finished=pump([&] { return WaitForSingleObject(process.hProcess,0)==WAIT_OBJECT_0; },25000);
            DWORD code=1; GetExitCodeProcess(process.hProcess,&code);
            check(finished && code==0,"real global floating search finds fixture and hands off to recycle scope");
            CloseHandle(process.hProcess);
        } else check(false,"start isolated global floating search probe");
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_NAME",nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_PATH",nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_RECYCLE_REPORT",nullptr);
        SetForegroundWindow(s.hwnd); SetFocus(s.hwnd);
        GlobalSearchHandoff handoff;
        handoff.folder=L"pulse:recycle";
        handoff.query=L"\""+original.filename().wstring()+L"\"";
        ContinueGlobalSearchInMain(s,handoff);
        check(pump([&] { return matching()!=SIZE_MAX; },15000),
            "floating-search handoff reaches actual main-window recycle-scoped results");
        const auto found=matching();
        if (auto* tab=ActiveTab(s); tab && found!=SIZE_MAX) {
            tab->SelectOnly(static_cast<int>(found));
            OpenSelected(s);
            check(pump([&] { return files::exists(original) && !files::exists(recycled.recycle_path); },15000),
                "opening a recycled search result restores the fixture instead of launching its original path");
            check(pump([&] { return matching()==SIZE_MAX; },10000),
                "restored payload disappears from live search results");
        }
        // Exact fixture-only fallback cleanup if a product restore failed.
        if (!files::exists(original) && files::exists(recycled.recycle_path)) {
            fs::RecycleItem metadata;
            const auto index=fs::RecycleIndexPath(recycled.recycle_path);
            if (fs::ReadRecycleIndex(index,metadata) && metadata.original_path==original.wstring() &&
                MoveFileExW(recycled.recycle_path.c_str(),original.c_str(),MOVEFILE_WRITE_THROUGH))
                DeleteFileW(index.c_str());
        }
        check(files::exists(original) && !files::exists(recycled.recycle_path),"fixture restored and no generated payload remains in recycle bin");
    }
    log<<"Failures: "<<failures<<std::endl;
    return failures ? 1 : 0;
}
} // namespace pulse
