#include "app_internal.h"
#include "extension_store_seven_zip.h"
#include "extensions/native/command_contributions.h"
#include "extensions/native/staging_adapter_host.h"
#include "../common/json_value.h"
#include "../ui/fluent_menu.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <windowsx.h>

namespace {
HWND probe_owner = nullptr;
bool hash_dialog = false;
std::wstring dialog_text;
void CALLBACK ProbeDialogTimer(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(hwnd, name, 80);
        if (!wcscmp(name, L"PulseFormDialogWindow") && IsWindowVisible(hwnd)) {
            hash_dialog = true;
            EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
                const int count = GetWindowTextLengthW(child);
                std::wstring text(static_cast<size_t>(count) + 1, L'\0');
                GetWindowTextW(child, text.data(), count + 1);
                text.resize(static_cast<size_t>(count));
                dialog_text += text + L"\n";
                return TRUE;
            }, 0);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, 0);
}
void CALLBACK ProbeMenuTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(probe_owner, timer);
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(hwnd, name, 80);
        if (!wcscmp(name, L"PulseFluentMenu") && IsWindowVisible(hwnd)) {
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(65, 40));
            PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(65, 40));
            PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(65, 40));
        }
        return TRUE;
    }, 0);
}
void CALLBACK ProbeTimeout(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(probe_owner, timer);
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[80]{}; GetClassNameW(hwnd,name,80);
        if (!wcscmp(name,L"PulseFluentMenu")) PostMessageW(hwnd,WM_KEYDOWN,VK_ESCAPE,0);
        return TRUE;
    },0);
}
bool Await(const std::function<bool()>& predicate) {
    const auto until = GetTickCount64() + 30000;
    do {
        if (predicate()) return true;
        MSG message{};
        for (int i=0;i<64 && PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++i) {
            if (message.message==WM_QUIT) {PostQuitMessage(static_cast<int>(message.wParam));return false;}
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    } while(GetTickCount64()<until);
    return predicate();
}
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};
}
}
int RunExtensionStoreOperationsTest(pulse::AppState& s, const wchar_t* directory) {
    using namespace pulse;
    namespace fs=std::filesystem;
    if(!s.isolatedTest || !directory) return 2;
    const fs::path root=fs::path(directory)/L"operations";
    fs::create_directories(root);
    std::ofstream log(root/L"operations.log");
    int failures=0;
    auto check=[&](bool ok,const char* what){log<<(ok?"[PASS] ":"[FAIL] ")<<what<<std::endl;if(!ok)++failures;return ok;};
    const auto fixture=root/(L"fixture-"+std::to_wstring(GetTickCount64()));
    fs::create_directories(fixture);
    std::ofstream(fixture/L"payload.txt",std::ios::binary)<<"abc";
    SetSevenZipState(s,"format","1"); SetSevenZipState(s,"level","1");
    SetSevenZipState(s,"mode","1"); SetSevenZipState(s,"location","0");
    SetSevenZipState(s,"placement0","true");
    for(int i=0;i<6;++i)SetSevenZipState(s,"cmd"+std::to_string(i),"true");
    CloseSettings(s);
    NavigateTo(s,fixture.wstring());
    const bool loaded = Await([&] {
        auto* tab = ActiveTab(s);
        std::error_code error;
        return tab && !tab->loading && tab->EntryCount() > 0 && fs::equivalent(fs::path(tab->current_path), fixture, error);
    });
    if (auto* tab = ActiveTab(s)) log << "navigation current=" << json::Utf8(tab->current_path)
        << " expected=" << json::Utf8(fixture.wstring()) << " loading=" << tab->loading << " entries=" << tab->EntryCount() << std::endl;
    if(!check(loaded,"real fixture directory loaded")) { OpenSettingsTab(s,8); return 1; }
    extensions::native::SetStagingConfirmObserver([](HWND window){PostMessageW(window,WM_KEYDOWN,VK_RETURN,0);});
    probe_owner=s.hwnd;
    struct Cleanup{AppState& s;~Cleanup(){extensions::native::SetStagingConfirmObserver({});KillTimer(s.hwnd,0x7345);KillTimer(s.hwnd,0x7346);KillTimer(s.hwnd,0x7347);extensions::native::SetStagingPickerObserver({});probe_owner=nullptr;}} cleanup{s};
    auto invoke=[&](const wchar_t* file,const wchar_t* id,bool expect_task=true){
        auto* tab=ActiveTab(s);bool selected=false;
        for(size_t i=0;tab && i<tab->EntryCount();++i)if(tab->EntryAt(i).name==file){tab->SelectOnly(static_cast<int>(i));selected=true;break;}
        if(!check(selected,"select actual fixture file"))return uint64_t{0};
        auto rows=extensions::native::ContextMenuCommandRows(s);
        ui::FluentMenuItem chosen;
        for(const auto& row:rows)if(extensions::native::CommandIdentity(s,row.command)==id)chosen=row;
        log<<"command="<<json::Utf8(id)<<" token="<<chosen.command<<std::endl;
        if(!check(chosen.command!=0 && EnsureMenu(s),"real plugin command is registered"))return uint64_t{0};
        // Keep the production row and click it through the real popup loop.
        chosen.group_start=chosen.group_end=true;
        const uint64_t previous=s.ops.Status().task_id;
        SetTimer(s.hwnd,0x7345,300,ProbeMenuTimer);
        SetTimer(s.hwnd,0x7347,5000,ProbeTimeout);
        const int command=s.menu->TrackPopup(POINT{120,120},{chosen});
        KillTimer(s.hwnd,0x7347);
        check(command==chosen.command,"mouse click returns the native command token");
        DispatchMenuCommand(s,command);
        if (!expect_task) return uint64_t{0};
        uint64_t task=0;
        Await([&]{const auto status=s.ops.Status();if(status.external && status.task_id!=previous)task=status.task_id;return task!=0;});
        check(task!=0,"native command creates an actual external task");
        return task;
    };
    auto finish=[&](uint64_t task){
        const bool done=task && Await([&]{const auto status=s.ops.QueryTask(task);return status && !status->active;});
        const auto status=s.ops.QueryTask(task);
        if(status) log<<"task="<<task<<" outcome="<<static_cast<int>(status->external_outcome)<<" bytes="<<status->transferred_bytes<<" error="<<json::Utf8(status->last_error)<<std::endl;
        return done && status && status->external_outcome==ops::ExternalOutcome::Completed;
    };
    const auto compressed=invoke(L"payload.txt",L"native:pulse.7zip:quick-compress");
    check(finish(compressed),"quick compression completes through plugin worker and host publication");
    const auto archive=fixture/L"payload.zip";
    check(fs::exists(archive)&&fs::file_size(archive)>3,"real zip output is published");
    ScheduleFolderRefresh(s);
    Await([&]{auto* tab=ActiveTab(s);for(size_t i=0;tab && i<tab->EntryCount();++i)if(tab->EntryAt(i).name==L"payload.zip")return true;return false;});
    check(finish(invoke(L"payload.zip",L"native:pulse.7zip:test-archive")),"actual archive test completes successfully");
    check(finish(invoke(L"payload.zip",L"native:pulse.7zip:extract")),"actual archive extraction completes successfully");
    check(ReadFile(fixture/L"payload"/L"payload.txt")=="abc","extracted bytes exactly match the source");
    hash_dialog=false;
    SetTimer(s.hwnd,0x7346,200,ProbeDialogTimer);
    const auto hashed=invoke(L"payload.txt",L"native:pulse.7zip:checksums");
    check(finish(hashed),"actual checksum task completes successfully");
    check(Await([&]{return hash_dialog;}),"checksum results dialog is displayed");
    check(dialog_text.find(L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")!=std::wstring::npos,
          "actual checksum result control contains known SHA-256 for abc");
    hash_dialog=false;
    invoke(L"payload.txt",L"native:pulse.7zip:add-to-archive",false);
    check(Await([&]{return hash_dialog;}),"add-to-archive opens real options dialog and cancel closes it");
    bool picker=false;
    extensions::native::SetStagingPickerObserver([&](HWND window){picker=true;PostMessageW(window,WM_CLOSE,0,0);});
    invoke(L"payload.zip",L"native:pulse.7zip:extract-to",false);
    check(picker,"extract-to opens real destination picker and cancel closes it");
    log<<"fixture="<<json::Utf8(fixture.wstring())<<"\nfailures="<<failures<<std::endl;
    OpenSettingsTab(s,8);
    return failures?1:0;
}
