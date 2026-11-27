#include "common/picker_layout_settings.h"
#include <cstdio>
#include <limits>
#include <cstring>
#include <filesystem>
using namespace pulse::picker_layout;
int wmain(int argc, wchar_t** argv) {
    // A process-local HKCU override exercises the real registry implementation
    // without writing installed preferences or relying on the user's hive ACL.
    HKEY hive = nullptr;
    if (argc > 1) {
        const auto file = std::filesystem::absolute(argv[1]);
        const auto loaded = RegLoadAppKeyW(file.c_str(), &hive, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
        const auto overridden = loaded == ERROR_SUCCESS ? RegOverridePredefKey(HKEY_CURRENT_USER, hive) : loaded;
        std::printf("ISOLATED_HIVE_LOAD=%ld OVERRIDE=%ld\n", loaded, overridden);
        if (overridden != ERROR_SUCCESS) { if (hive) RegCloseKey(hive); return 2; }
    }
    const auto root=L"Software\\Pulse\\Tests\\PickerLayout-"+std::to_wstring(GetCurrentProcessId());
    HKEY probe=nullptr;
    const auto registry_probe=RegCreateKeyExW(HKEY_CURRENT_USER,root.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&probe,nullptr);
    std::printf("ISOLATED_REGISTRY_CREATE=%ld\n",registry_probe);
    if(probe)RegCloseKey(probe);
    int failures=0;
    auto check=[&](bool ok,const char* name) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; };
    check(LoadFlagsForIdentity(root,L"host-a",2)==0,"remove-after-open defaults off in isolated host");
    check(SaveFlagsForIdentity(root,L"host-a",2,kFlagPreviewPane|kFlagRemoveTrayAfterOpen),"remember remove-after-open without replacing preview flag");
    check(LoadFlagsForIdentity(root,L"host-a",2)==(kFlagPreviewPane|kFlagRemoveTrayAfterOpen),"host flags roundtrip");
    check(LoadFlagsForIdentity(root,L"host-b",2)==0 && LoadFlagsForIdentity(root,L"host-a",3)==0,"other programs and Save mode remain independent");
    check(SaveFlagsForIdentity(root,L"host-a",2,kFlagPreviewPane)&&LoadFlagsForIdentity(root,L"host-a",2)==kFlagPreviewPane,"turning remove-after-open off preserves preview setting");
    check(SaveFlagsForIdentity(root,L"host-a",2,kFlagPreviewPaneHidden)&&LoadFlagsForIdentity(root,L"host-a",2)==kFlagPreviewPaneHidden,"preview-hidden flag roundtrips cleanly");
    check(LoadAppPinnedForIdentity(root,L"host-a").empty(),"app pinned places default empty");
    check(SaveAppPinnedForIdentity(root,L"host-a",{L"C:\\PinA",L"D:\\PinB"}) &&
          LoadAppPinnedForIdentity(root,L"host-a")==std::vector<std::wstring>{L"C:\\PinA",L"D:\\PinB"} &&
          LoadAppPinnedForIdentity(root,L"host-b").empty(),"app pinned places roundtrip with per-app isolation");
    check(!SaveAppPinnedForIdentity(root,L"host-a",{L"C:\\PinA",L"c:\\pina"}),"duplicate app pinned paths rejected");
    check(SaveAppPinnedForIdentity(root,L"host-a",{}) && LoadAppPinnedForIdentity(root,L"host-a").empty(),"clearing app pinned places persists");
    check(!SaveFlagsForIdentity(root,L"",2,2)&&!SaveFlagsForIdentity(root,L"host-a",4,2),"invalid host and mode reject persistence");
    constexpr int save_mode = 3;
    constexpr uint32_t existing_flags = kFlagPreviewPane | kFlagRemoveTrayAfterOpen;
    constexpr uint32_t followup_flags = kFlagShowAfterSave | kFlagCopyAfterSave;
    check(LoadFlagsForIdentity(root,L"save-host-a",save_mode)==0 &&
        LoadFlagsForIdentity(root,L"save-host-b",save_mode)==0,
        "save follow-up flags default off for independent callers");
    check(SaveFlagsForIdentity(root,L"save-host-a",save_mode,existing_flags),
        "seed existing preview and tray flags before save follow-up toggles");
    auto save_flags = LoadFlagsForIdentity(root,L"save-host-a",save_mode);
    check(SaveFlagsForIdentity(root,L"save-host-a",save_mode,save_flags|followup_flags) &&
        LoadFlagsForIdentity(root,L"save-host-a",save_mode)==(existing_flags|followup_flags),
        "show-after-save and copy-after-save roundtrip while retaining existing flags");
    check(LoadFlagsForIdentity(root,L"save-host-b",save_mode)==0 &&
        LoadFlagsForIdentity(root,L"save-host-a",0)==0 && LoadFlagsForIdentity(root,L"save-host-a",2)==0,
        "save follow-up settings stay isolated from other callers and picker modes");
    check(SaveFlagsForIdentity(root,L"save-host-b",save_mode,kFlagCopyAfterSave) &&
        LoadFlagsForIdentity(root,L"save-host-b",save_mode)==kFlagCopyAfterSave &&
        LoadFlagsForIdentity(root,L"save-host-a",save_mode)==(existing_flags|followup_flags),
        "second caller independently remembers copy-only without changing first caller");
    save_flags = LoadFlagsForIdentity(root,L"save-host-a",save_mode);
    check(SaveFlagsForIdentity(root,L"save-host-a",save_mode,save_flags&~kFlagShowAfterSave) &&
        LoadFlagsForIdentity(root,L"save-host-a",save_mode)==(existing_flags|kFlagCopyAfterSave),
        "disabling show-after-save preserves copy-after-save preview and tray flags");
    save_flags = LoadFlagsForIdentity(root,L"save-host-a",save_mode);
    check(SaveFlagsForIdentity(root,L"save-host-a",save_mode,save_flags&~kFlagCopyAfterSave) &&
        LoadFlagsForIdentity(root,L"save-host-a",save_mode)==existing_flags &&
        LoadFlagsForIdentity(root,L"save-host-b",save_mode)==kFlagCopyAfterSave,
        "disabling both follow-ups preserves existing flags and other caller choice");
    State original; original.icon_dip=112; original.has_placement=true; original.x=120; original.width=940; original.view_mode=4; original.sort=2; original.descending=true; original.column_widths[0]=240; original.hidden_places={L"C:\\hidden", L"D:\\other"};
    check(SaveForIdentity(root,L"C:\\host-a.exe",0,original),"save including canceled dialog state");
    State read;
    original.collapsed_mask = 448;
    check(SaveForIdentity(root,L"C:\\host-a.exe",0,original) &&
        LoadForIdentity(root,L"C:\\host-a.exe",0,read) && read.collapsed_mask == 448,
        "shared favorites and workspace collapse state persists");
    check(LoadForIdentity(root,L"C:\\host-a.exe",0,read) && read.view_mode==4 && read.descending && read.column_widths[0]==240 && read.hidden_places==original.hidden_places && read.icon_dip==112 && read.has_placement && read.x==120 && read.width==940,"round trip including custom icon size and placement");
    check(!LoadForIdentity(root,L"C:\\host-b.exe",0,read) && !LoadForIdentity(root,L"C:\\host-a.exe",1,read),"host and mode isolation");
    State invalid=original; invalid.view_mode=8;
    check(!SaveForIdentity(root,L"a",0,invalid),"unknown enum rejected");
    invalid=original; invalid.sidebar_width=std::numeric_limits<float>::quiet_NaN();
    check(!SaveForIdentity(root,L"a",0,invalid),"nonfinite rejected");
    invalid=original; invalid.icon_dip=std::numeric_limits<float>::quiet_NaN();
    check(!SaveForIdentity(root,L"a",0,invalid),"nonfinite icon size rejected");
    invalid.icon_dip=-1;
    check(!SaveForIdentity(root,L"a",0,invalid),"negative icon size rejected");
    invalid.icon_dip=10001;
    check(!SaveForIdentity(root,L"a",0,invalid),"unbounded icon size rejected");
    State preset=original; preset.icon_dip=0;
    check(SaveForIdentity(root,L"preset",0,preset) && LoadForIdentity(root,L"preset",0,read) && read.icon_dip==0,
        "preset icon size round trip");
    // Construct the legacy payload by removing only v3's appended float.
    const wchar_t* stored=L"0:C:\\host-a.exe";
    DWORD bytes=0;
    bool legacy=RegGetValueW(HKEY_CURRENT_USER,root.c_str(),stored,RRF_RT_REG_BINARY,nullptr,nullptr,&bytes)==ERROR_SUCCESS;
    std::vector<BYTE> payload(bytes);
    legacy=legacy && RegGetValueW(HKEY_CURRENT_USER,root.c_str(),stored,RRF_RT_REG_BINARY,nullptr,payload.data(),&bytes)==ERROR_SUCCESS;
    if(legacy && payload.size()>=4*sizeof(DWORD)+sizeof(float)) {
        payload.resize(payload.size()-sizeof(float));
        DWORD version=2, length=static_cast<DWORD>(payload.size());
        memcpy(payload.data()+sizeof(DWORD),&version,sizeof(version));
        memcpy(payload.data()+2*sizeof(DWORD),&length,sizeof(length));
        HKEY legacy_key{};
        legacy=RegOpenKeyExW(HKEY_CURRENT_USER,root.c_str(),0,KEY_SET_VALUE,&legacy_key)==ERROR_SUCCESS;
        if(legacy) { legacy=RegSetValueExW(legacy_key,L"0:legacy",0,REG_BINARY,payload.data(),length)==ERROR_SUCCESS; RegCloseKey(legacy_key); }
    } else legacy=false;
    read.icon_dip=256;
    check(legacy && LoadForIdentity(root,L"legacy",0,read) && read.icon_dip==0 && read.view_mode==original.view_mode &&
        read.column_widths==original.column_widths && read.hidden_places==original.hidden_places &&
        read.has_placement && read.x==original.x && read.width==original.width,
        "v2 keeps placement columns and hidden places with preset icon size");
    invalid=original; invalid.hidden_places={L"C:\\hidden",L"c:\\HIDDEN"};
    check(!SaveForIdentity(root,L"a",0,invalid),"duplicate hidden paths rejected");
    invalid.hidden_places={std::wstring(L"a\0b",3)};
    check(!SaveForIdentity(root,L"a",0,invalid),"embedded NUL rejected");
    HKEY key{}; RegOpenKeyExW(HKEY_CURRENT_USER,root.c_str(),0,KEY_SET_VALUE,&key);
    DWORD bad=0; RegSetValueExW(key,L"0:bad",0,REG_BINARY,reinterpret_cast<BYTE*>(&bad),sizeof(bad)); RegCloseKey(key);
    check(!LoadForIdentity(root,L"bad",0,read),"malformed schema rejected");
    HWND window=CreateWindowExW(0,L"STATIC",L"Picker layout test",WS_OVERLAPPEDWINDOW,100,100,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"fixture window");
    if(window) {
        State geometry; CapturePlacement(window,geometry); geometry.x=90000; geometry.y=-90000;
        check(geometry.has_placement && RestorePlacement(window,nullptr,geometry),"restore offscreen placement");
        RECT rect{}; GetWindowRect(window,&rect); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info);
        check(rect.left>=info.rcWork.left && rect.top>=info.rcWork.top && rect.right<=info.rcWork.right && rect.bottom<=info.rcWork.bottom,"placement clamped to workarea");
        DestroyWindow(window);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER,root.c_str());
    if (hive) { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(hive); }
    std::printf("FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}

