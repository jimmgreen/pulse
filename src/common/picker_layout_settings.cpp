#include "app_runtime_paths.h"
#include "picker_layout_settings.h"
#include "windows_compat.h"
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
namespace pulse::picker_layout {
namespace {
constexpr wchar_t kRoot[] = L"Software\\Pulse\\PickerLayout\\v1";
struct WireState {
    DWORD magic = 0x504C5931, version = 3, size = 0, count = 0;
    int view_mode = 0, sort = 0;
    DWORD descending = 0, maximized = 0, has_placement = 0;
    float sidebar_width = 196;
    unsigned collapsed_mask = 0;
    std::array<float, 5> column_widths{};
    wchar_t monitor[32]{};
    float x = 0, y = 0, width = 900, height = 650;
};
constexpr size_t kMaxBytes = 128 * 1024;
bool Valid(const State& s) {
    if (s.hidden_places.size() > 64) return false;
    size_t bytes = sizeof(WireState) + sizeof(float);
    for (size_t i = 0; i < s.hidden_places.size(); ++i) {
        const auto& path = s.hidden_places[i];
        if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos) return false;
        bytes += sizeof(DWORD) + path.size() * sizeof(wchar_t);
        if (bytes > kMaxBytes) return false;
        for (size_t j = 0; j < i; ++j)
            if (CompareStringOrdinal(path.c_str(), -1, s.hidden_places[j].c_str(), -1, TRUE) == CSTR_EQUAL) return false;
    }
    auto finite = [](float v) { return std::isfinite(v); };
    if (s.view_mode < 0 || s.view_mode > 7 || s.sort < 0 || s.sort > 3 || s.collapsed_mask > 255 ||
        !finite(s.sidebar_width) || s.sidebar_width < 100 || s.sidebar_width > 1000 ||
        !finite(s.icon_dip) || s.icon_dip < 0 || s.icon_dip > 10000) return false;
    for (float v : s.column_widths) if (!finite(v) || v < 0 || v > 10000) return false;
    return finite(s.x) && finite(s.y) && finite(s.width) && finite(s.height) &&
        std::abs(s.x) <= 100000 && std::abs(s.y) <= 100000 && s.width >= 100 && s.width <= 20000 &&
        s.height >= 100 && s.height <= 20000 && s.monitor[31] == 0;
}
std::wstring Identity() { return app_runtime_paths::StableEntryPath(); }
std::wstring Value(const std::wstring& id, int mode) { return std::to_wstring(mode) + L":" + id; }
struct MonitorSearch { const wchar_t* name; HMONITOR found = nullptr; };
BOOL CALLBACK FindMonitor(HMONITOR m, HDC, LPRECT, LPARAM p) { auto& s = *reinterpret_cast<MonitorSearch*>(p); MONITORINFOEXW i{}; i.cbSize=sizeof(i); if (GetMonitorInfoW(m,&i) && wcscmp(i.szDevice,s.name)==0) s.found=m; return TRUE; }
}
bool SaveForIdentity(const std::wstring& root,const std::wstring& id,int mode,const State& s) {
    if (id.empty() || mode < 0 || mode > 3 || !Valid(s)) return false;
    WireState wire{};
    wire.view_mode = s.view_mode;
    wire.sort = s.sort;
    wire.descending = s.descending;
    wire.maximized = s.maximized;
    wire.has_placement = s.has_placement;
    wire.sidebar_width = s.sidebar_width;
    wire.collapsed_mask = s.collapsed_mask;
    wire.column_widths = s.column_widths;
    wire.x = s.x;
    wire.y = s.y;
    wire.width = s.width;
    wire.height = s.height;
    memcpy(wire.monitor, s.monitor, sizeof(wire.monitor));
    wire.count = static_cast<DWORD>(s.hidden_places.size());
    std::vector<BYTE> data(sizeof(wire));
    for (const auto& path : s.hidden_places) {
        DWORD length = static_cast<DWORD>(path.size());
        size_t offset = data.size(); data.resize(offset + sizeof(length) + length * sizeof(wchar_t));
        memcpy(data.data() + offset, &length, sizeof(length));
        memcpy(data.data() + offset + sizeof(length), path.data(), length * sizeof(wchar_t));
    }
    // v3 appends the custom icon size after the unchanged v2 layout payload.
    const size_t icon_offset = data.size();
    data.resize(icon_offset + sizeof(s.icon_dip));
    memcpy(data.data() + icon_offset, &s.icon_dip, sizeof(s.icon_dip));
    wire.size = static_cast<DWORD>(data.size()); memcpy(data.data(), &wire, sizeof(wire));
    HKEY key{}; if (RegCreateKeyExW(HKEY_CURRENT_USER,root.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return false;
    auto name=Value(id,mode);
    auto result=RegSetValueExW(key,name.c_str(),0,REG_BINARY,data.data(),wire.size); RegCloseKey(key); return result==ERROR_SUCCESS;
}
bool LoadForIdentity(const std::wstring& root,const std::wstring& id,int mode,State& s) {
    if (id.empty() || mode < 0 || mode > 3) return false;
    DWORD size=0; auto name=Value(id,mode);
    if (RegGetValueW(HKEY_CURRENT_USER,root.c_str(),name.c_str(),RRF_RT_REG_BINARY,nullptr,nullptr,&size)!=ERROR_SUCCESS || size < sizeof(WireState) || size > kMaxBytes) return false;
    std::vector<BYTE> data(size);
    if (RegGetValueW(HKEY_CURRENT_USER,root.c_str(),name.c_str(),RRF_RT_REG_BINARY,nullptr,data.data(),&size)!=ERROR_SUCCESS || size!=data.size()) return false;
    WireState wire{}; memcpy(&wire,data.data(),sizeof(wire));
    if (wire.magic!=0x504C5931 || (wire.version!=2 && wire.version!=3) || wire.size!=size || wire.count>64 || wire.descending>1 || wire.maximized>1 || wire.has_placement>1) return false;
    State candidate;
    candidate.view_mode = wire.view_mode;
    candidate.sort = wire.sort;
    candidate.descending = wire.descending != 0;
    candidate.maximized = wire.maximized != 0;
    candidate.has_placement = wire.has_placement != 0;
    candidate.sidebar_width = wire.sidebar_width;
    candidate.collapsed_mask = wire.collapsed_mask;
    candidate.column_widths = wire.column_widths;
    candidate.x = wire.x;
    candidate.y = wire.y;
    candidate.width = wire.width;
    candidate.height = wire.height;
    memcpy(candidate.monitor,wire.monitor,sizeof(wire.monitor));
    size_t offset=sizeof(wire);
    for (DWORD i=0; i<wire.count; ++i) {
        if (size-offset<sizeof(DWORD)) return false;
        DWORD length=0; memcpy(&length,data.data()+offset,sizeof(length)); offset+=sizeof(length);
        if (!length || length>32767 || length>(size-offset)/sizeof(wchar_t)) return false;
        std::wstring path(length,L'\0'); memcpy(path.data(),data.data()+offset,length*sizeof(wchar_t));
        offset+=length*sizeof(wchar_t); candidate.hidden_places.push_back(std::move(path));
    }
    if (wire.version == 3) {
        if (size - offset != sizeof(candidate.icon_dip)) return false;
        memcpy(&candidate.icon_dip, data.data() + offset, sizeof(candidate.icon_dip));
        offset += sizeof(candidate.icon_dip);
    }
    if (offset!=size || !Valid(candidate)) return false;
    s=std::move(candidate); return true;
}
bool Load(int mode,State& s) { return LoadForIdentity(kRoot,Identity(),mode,s); }
bool Save(int mode,const State& s) { return SaveForIdentity(kRoot,Identity(),mode,s); }
bool RestorePlacement(HWND window,HWND owner,const State& s) {
    if (!s.has_placement || !Valid(s)) return false;
    MonitorSearch search{s.monitor}; EnumDisplayMonitors(nullptr,nullptr,FindMonitor,reinterpret_cast<LPARAM>(&search));
    HMONITOR monitor=search.found ? search.found : MonitorFromWindow(owner ? owner : window,MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(monitor,&info)) return false;
    UINT dpi_x=compat::WindowDpi(window), dpi_y=dpi_x;
    using DpiFn=HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    auto dpi_fn=reinterpret_cast<DpiFn>(compat::ScalingApi("GetDpiForMonitor"));
    if (dpi_fn) dpi_fn(monitor,MDT_EFFECTIVE_DPI,&dpi_x,&dpi_y);
    float scale=static_cast<float>(dpi_x ? dpi_x : 96)/96.0f;
    auto work=info.rcWork;
    const LONG w=std::min(static_cast<LONG>(std::max(640.0f,s.width)*scale),work.right-work.left);
    const LONG h=std::min(static_cast<LONG>(std::max(420.0f,s.height)*scale),work.bottom-work.top);
    int x=std::clamp(work.left+static_cast<int>(s.x*scale),work.left,work.right-w), y=std::clamp(work.top+static_cast<int>(s.y*scale),work.top,work.bottom-h);
    return SetWindowPos(window,nullptr,x,y,w,h,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE;
}
void CapturePlacement(HWND window,State& s) {
    WINDOWPLACEMENT placement{sizeof(placement)}; if (!GetWindowPlacement(window,&placement)) return;
    RECT rect=placement.rcNormalPosition; HMONITOR monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{}; info.cbSize=sizeof(info); if (!GetMonitorInfoW(monitor,&info)) return;
    // WINDOWPLACEMENT normal coordinates are in workspace coordinates for top-level windows.
    rect.left+=info.rcWork.left-info.rcMonitor.left; rect.right+=info.rcWork.left-info.rcMonitor.left;
    rect.top+=info.rcWork.top-info.rcMonitor.top; rect.bottom+=info.rcWork.top-info.rcMonitor.top;
    UINT dpi_x=compat::WindowDpi(window), dpi_y=dpi_x;
    using DpiFn=HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    auto dpi_fn=reinterpret_cast<DpiFn>(compat::ScalingApi("GetDpiForMonitor"));
    if (dpi_fn) dpi_fn(monitor,MDT_EFFECTIVE_DPI,&dpi_x,&dpi_y);
    float scale=static_cast<float>(dpi_x ? dpi_x : 96)/96.0f;
    s.x=(rect.left-info.rcWork.left)/scale; s.y=(rect.top-info.rcWork.top)/scale;
    s.width=(rect.right-rect.left)/scale; s.height=(rect.bottom-rect.top)/scale;
    s.maximized=IsZoomed(window)!=FALSE || (IsIconic(window) && (placement.flags & WPF_RESTORETOMAXIMIZED));
    s.has_placement=true; wcscpy_s(s.monitor,info.szDevice);
}
}

