#include "common/picker_layout_settings.h"
#include <cstdio>
#include <limits>
#include <cstring>
using namespace pulse::picker_layout;
int main() {
    const auto root=L"Software\\Pulse\\Tests\\PickerLayout-"+std::to_wstring(GetCurrentProcessId());
    int failures=0;
    auto check=[&](bool ok,const char* name) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures; };
    State original; original.icon_dip=112; original.has_placement=true; original.x=120; original.width=940; original.view_mode=4; original.sort=2; original.descending=true; original.column_widths[0]=240; original.hidden_places={L"C:\\hidden", L"D:\\other"};
    check(SaveForIdentity(root,L"C:\\host-a.exe",0,original),"save including canceled dialog state");
    State read;
    original.collapsed_mask = 192;
    check(SaveForIdentity(root,L"C:\\host-a.exe",0,original) &&
        LoadForIdentity(root,L"C:\\host-a.exe",0,read) && read.collapsed_mask == 192,
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
    return failures ? 1 : 0;
}

