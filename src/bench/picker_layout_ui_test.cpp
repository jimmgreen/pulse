#include "../common/picker_layout_settings.h"
#include "../common/windows_compat.h"
#include "../picker_inprocess/picker_ui_api_v2.h"
#include <objbase.h>
#include <filesystem>
#include <cstdio>

namespace {
int failures=0, ready=0;
RECT expected{};
void Check(bool ok,const char* name) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; }
HRESULT WINAPI Callback(void*,const PulsePickerUiV2Event* event) {
    if (event->kind!=PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
    RECT actual{}; GetWindowRect(event->window,&actual);
    if (ready<3 && !EqualRect(&actual,&expected)) std::printf("rectangle actual=%ld,%ld,%ld,%ld expected=%ld,%ld,%ld,%ld\n",
        actual.left,actual.top,actual.right,actual.bottom,expected.left,expected.top,expected.right,expected.bottom);
    if(ready<3) Check(EqualRect(&actual,&expected)!=FALSE,"actual takeover restores saved screen rectangle");
    else Check(IsZoomed(event->window)!=FALSE,"actual takeover restores maximized state");
    if (ready==0) {
        SetWindowPos(event->window,nullptr,actual.left+12,actual.top+18,
            actual.right-actual.left+40,actual.bottom-actual.top+30,SWP_NOACTIVATE|SWP_NOZORDER);
        GetWindowRect(event->window,&expected);
    }
    if(ready==2) ShowWindow(event->window,SW_MAXIMIZE);
    ++ready;
    PostMessageW(event->window,WM_CLOSE,0,0);
    return S_OK;
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",nullptr);
    using SetDpi=DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_dpi=reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext"));
    if(set_dpi) set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 2;
    std::wstring identity(32768,L'\0'); identity.resize(GetModuleFileNameW(nullptr,identity.data(),static_cast<DWORD>(identity.size())));
    pulse::picker_layout::State old;
    if(pulse::picker_layout::Load(2,old)) { std::puts("Refusing to replace pre-existing fixture preferences"); return 2; }
    HWND owner=CreateWindowExW(0,L"STATIC",L"Isolated picker persistence fixture",WS_OVERLAPPEDWINDOW,160,160,800,520,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!owner) return 2;
    const float scale=static_cast<float>(pulse::compat::WindowDpi(owner))/96.0f;
    SetWindowPos(owner,nullptr,0,0,static_cast<int>(800*scale),static_cast<int>(520*scale),SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
    pulse::picker_layout::State saved;
    pulse::picker_layout::CapturePlacement(owner,saved);
    saved.view_mode=1; saved.sort=2; saved.descending=true; saved.sidebar_width=240;
    saved.collapsed_mask=5; saved.column_widths={210,130,120,0,0}; saved.hidden_places={argv[2]};
    Check(pulse::picker_layout::Save(2,saved),"seed isolated host layout");
    GetWindowRect(owner,&expected);
    HMODULE dll=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto get=dll?reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(dll,"PulsePickerUiGetApiV2")):nullptr;
    PulsePickerUiV2Api api{};
    Check(get && get(2,sizeof(api),&api)==S_OK,"load production picker API");
    if(get && api.Initialize) {
        PulsePickerUiV2Initialize init{}; init.size=sizeof(init); init.version=2;
        PulsePickerUiV2Session session=0;
        Check(api.Initialize(&init,&session)==S_OK,"initialize picker session");
        if(session) {
            const std::wstring folder=std::filesystem::absolute(argv[2]).wstring();
            PulsePickerUiV2Config config{}; config.size=sizeof(config); config.version=2;
            config.owner=owner; config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
            config.initial_folder={folder.c_str(),static_cast<uint32_t>(folder.size())};
            PulsePickerUiV2Callbacks callbacks{}; callbacks.size=sizeof(callbacks); callbacks.callback=Callback;
            for(int round=0;round<4;++round) {
                wchar_t path[32768]{}; PulsePickerUiV2Result result{}; result.size=sizeof(result); result.path={path,32768,0};
                const HRESULT hr=api.Show(session,&config,&callbacks,&result);
                Check(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED),"cancel actual takeover without selecting files");
                pulse::picker_layout::State restored;
                Check(pulse::picker_layout::Load(2,restored) && restored.has_placement && restored.view_mode==1 && restored.sort==2 &&
                    restored.descending && restored.sidebar_width==240 && restored.collapsed_mask==5 &&
                    restored.column_widths==saved.column_widths && restored.hidden_places==saved.hidden_places && restored.maximized==(round>=2),
                    "cancel persists restored view, sorting, sidebar, column widths and hidden places");
            }
            Check(api.Shutdown(session)==S_OK,"shutdown picker session");
        }
    }
    Check(ready==4,"four real visible picker sessions completed");
    if(dll) FreeLibrary(dll);
    DestroyWindow(owner);
    HKEY key{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Pulse\\PickerLayout\\v1",0,KEY_SET_VALUE,&key)==ERROR_SUCCESS) {
        const auto value=L"2:"+identity;
        Check(RegDeleteValueW(key,value.c_str())==ERROR_SUCCESS,"remove only this test executable's preference value"); RegCloseKey(key);
    }
    CoUninitialize(); return failures?1:0;
}
