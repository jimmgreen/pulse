// desktop_mount.cpp - Zero-injection desktop hierarchy prober, lossless Shell COM
// icon snapshot/restore, directory watcher, and mount host implementation.
#include "host/desktop_mount.h"
#include "ipc/desktop_ipc.h"
#include "../../../src/common/os_version.h"

#include <ole2.h>
#include <dwmapi.h>
#include <exdisp.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace pulse::desktop::mount {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr wchar_t kMountWindowClass[] = L"PulseDesktopSurfaceWindow";
constexpr UINT_PTR kTimerKeepAboveHost = 0x5D01;

int64_t NowUnixMs() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring PulseAppDataDirectory();

// Mount diagnostics: the takeover touches Explorer's windows, so every step is logged next to Pulse's other host logs.
void MountLog(const char* fmt, ...) noexcept {
    char line[512]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    std::error_code ec;
    const fs::path path = fs::path(PulseAppDataDirectory()) / L"pulse_desktop_host.log";
    if (fs::file_size(path, ec) > 256 * 1024) fs::remove(path, ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") == 0 && f) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, line);
        std::fclose(f);
    }
}

int64_t FileTimeToUnixMs(const FILETIME& ft) noexcept {
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (uli.QuadPart <= kEpochDiff100ns) return 0;
    return static_cast<int64_t>((uli.QuadPart - kEpochDiff100ns) / 10000ULL);
}

std::wstring ToLowerWide(std::wstring_view in) {
    std::wstring out(in);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return out;
}

struct EnumDefViewCtx {
    HWND defview_parent = nullptr;
    HWND defview = nullptr;
    HWND wallpaper_workerw = nullptr;
};

BOOL CALLBACK EnumWorkerWProc(HWND top, LPARAM lparam) noexcept {
    auto* ctx = reinterpret_cast<EnumDefViewCtx*>(lparam);
    wchar_t cls[64]{};
    if (GetClassNameW(top, cls, ARRAYSIZE(cls)) <= 0) return TRUE;
    if (wcscmp(cls, L"WorkerW") != 0) return TRUE;
    HWND dv = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    if (dv) {
        ctx->defview_parent = top;
        ctx->defview = dv;
        ctx->wallpaper_workerw = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lparam) noexcept {
    auto* out = reinterpret_cast<std::vector<model::MonitorWorkArea>*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    model::MonitorWorkArea area{};
    area.device_name = mi.szDevice;
    area.bounds = {mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top};
    area.work_area = {mi.rcWork.left, mi.rcWork.top,
                      mi.rcWork.right - mi.rcWork.left,
                      mi.rcWork.bottom - mi.rcWork.top};
    area.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT dpi_x = 96;
    UINT dpi_y = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    static HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    static auto get_dpi = shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    if (get_dpi && SUCCEEDED(get_dpi(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x > 0) {
        area.dpi = dpi_x;
    } else {
        area.dpi = 96;
    }
    out->push_back(std::move(area));
    return TRUE;
}

// Runs a callable inside an STA COM apartment (switching to a helper STA thread if caller is MTA).
template <typename Fn>
HRESULT RunInStaApartment(Fn&& fn) {
    const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(init_hr)) {
        HRESULT res = E_FAIL;
        try {
            res = fn();
        } catch (...) {
            res = E_UNEXPECTED;
        }
        CoUninitialize();
        return res;
    }
    if (init_hr == RPC_E_CHANGED_MODE) {
        HRESULT worker_res = E_FAIL;
        std::thread sta_worker([&] {
            const HRESULT thr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(thr_init)) {
                try {
                    worker_res = fn();
                } catch (...) {
                    worker_res = E_UNEXPECTED;
                }
                CoUninitialize();
            } else {
                worker_res = thr_init;
            }
        });
        sta_worker.join();
        return worker_res;
    }
    return init_hr;
}

HRESULT BindDesktopFolderView(ComPtr<IFolderView2>& out_folder_view, ComPtr<IShellFolder>& out_desktop_folder) {
    ComPtr<IShellWindows> shell_windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&shell_windows));
    if (FAILED(hr) || !shell_windows) return FAILED(hr) ? hr : E_NOINTERFACE;

    VARIANT v_desktop{};
    v_desktop.vt = VT_I4;
    v_desktop.lVal = CSIDL_DESKTOP;
    VARIANT v_empty{};
    VariantInit(&v_empty);
    long desktop_hwnd = 0;
    ComPtr<IDispatch> disp;
    hr = shell_windows->FindWindowSW(&v_desktop, &v_empty, SWC_DESKTOP, &desktop_hwnd, SWFO_NEEDDISPATCH, &disp);
    if (hr != S_OK || !disp) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    ComPtr<IServiceProvider> sp;
    hr = disp.As(&sp);
    if (FAILED(hr) || !sp) return hr;

    ComPtr<IShellBrowser> browser;
    hr = sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (FAILED(hr) || !browser) return hr;

    ComPtr<IShellView> shell_view;
    hr = browser->QueryActiveShellView(&shell_view);
    if (FAILED(hr) || !shell_view) return hr;

    hr = shell_view.As(&out_folder_view);
    if (FAILED(hr) || !out_folder_view) return hr;

    ComPtr<IPersistFolder2> persist_folder;
    if (SUCCEEDED(out_folder_view.As(&persist_folder)) && persist_folder) {
        PIDLIST_ABSOLUTE cur_pidl = nullptr;
        if (SUCCEEDED(persist_folder->GetCurFolder(&cur_pidl)) && cur_pidl) {
            SHBindToObject(nullptr, cur_pidl, nullptr, IID_PPV_ARGS(&out_desktop_folder));
            CoTaskMemFree(cur_pidl);
        }
    }
    if (!out_desktop_folder) {
        out_folder_view->GetFolder(IID_PPV_ARGS(&out_desktop_folder));
    }
    if (!out_desktop_folder) {
        hr = SHGetDesktopFolder(&out_desktop_folder);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

std::wstring GetShellItemName(IShellFolder* folder, PCUITEMID_CHILD pidl, SHGDNF flags) {
    if (!folder || !pidl) return {};
    STRRET sr{};
    if (FAILED(folder->GetDisplayNameOf(pidl, flags, &sr))) return {};
    wchar_t buf[2048]{};
    if (FAILED(StrRetToBufW(&sr, pidl, buf, ARRAYSIZE(buf)))) return {};
    return buf;
}

// Extracts top-level objects `{ ... }` inside a named JSON array `"array_key": [ ... ]`.
std::vector<std::string_view> SplitJsonObjectArray(std::string_view json, std::string_view array_key) {
    std::vector<std::string_view> objects;
    const std::string needle = "\"" + std::string(array_key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return objects;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string_view::npos) return objects;
    ++pos;

    bool in_str = false;
    bool esc = false;
    int brace_depth = 0;
    int bracket_depth = 1;
    size_t obj_start = 0;

    for (size_t i = pos; i < json.size() && bracket_depth > 0; ++i) {
        const char ch = json[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (ch == '\\') {
                esc = true;
            } else if (ch == '"') {
                in_str = false;
            }
            continue;
        }
        if (ch == '"') {
            in_str = true;
        } else if (ch == '[' && brace_depth == 0) {
            ++bracket_depth;
        } else if (ch == ']' && brace_depth == 0) {
            --bracket_depth;
        } else if (ch == '{') {
            if (brace_depth == 0) obj_start = i;
            ++brace_depth;
        } else if (ch == '}') {
            --brace_depth;
            if (brace_depth == 0) {
                objects.push_back(json.substr(obj_start, i - obj_start + 1));
            }
        }
    }
    return objects;
}

std::wstring PulseAppDataDirectory() {
    std::wstring dir = ResolveKnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        dir = temp;
    }
    fs::path p = fs::path(dir) / L"Pulse";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.wstring();
}

void ResolveShortcutTargetNoUi(IShellLinkW* link, IPersistFile* persist, const std::wstring& lnk_path,
                               std::wstring& out_target, bool& out_missing) {
    out_target.clear();
    out_missing = false;
    if (!link || !persist) return;
    if (FAILED(persist->Load(lnk_path.c_str(), STGM_READ))) return;

    wchar_t target_buf[MAX_PATH * 4]{};
    WIN32_FIND_DATAW wfd{};
    if (SUCCEEDED(link->GetPath(target_buf, ARRAYSIZE(target_buf), &wfd, SLGP_RAWPATH)) && target_buf[0] != L'\0') {
        wchar_t expanded[MAX_PATH * 4]{};
        const DWORD n = ExpandEnvironmentStringsW(target_buf, expanded, ARRAYSIZE(expanded));
        out_target = (n > 0 && n < ARRAYSIZE(expanded)) ? expanded : target_buf;
        if (out_target.size() >= 3 && out_target[1] == L':' &&
            (out_target[2] == L'\\' || out_target[2] == L'/')) {
            out_missing = (GetFileAttributesW(out_target.c_str()) == INVALID_FILE_ATTRIBUTES);
        }
    }
}

} // namespace

const char* HierarchyModeName(DesktopHierarchyMode mode) noexcept {
    switch (mode) {
    case DesktopHierarchyMode::ClassicWorkerW: return "ClassicWorkerW";
    case DesktopHierarchyMode::Win11_24H2_ProgmanChild: return "Win11_24H2_ProgmanChild";
    case DesktopHierarchyMode::FallbackProgman: return "FallbackProgman";
    default: return "NotFound";
    }
}

DesktopHierarchyInfo ProbeDesktopHierarchy() noexcept {
    DesktopHierarchyInfo info{};
    info.virtual_bounds = {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
        std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)),
    };

    info.progman = FindWindowW(L"Progman", nullptr);
    if (info.progman) {
        GetWindowThreadProcessId(info.progman, &info.explorer_pid);
        HWND direct_defview = FindWindowExW(info.progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (direct_defview) {
            info.defview_parent = info.progman;
            info.shelldll_defview = direct_defview;
            info.workerw_wallpaper = FindWindowExW(info.progman, nullptr, L"WorkerW", nullptr);
            info.mode = info.workerw_wallpaper ? DesktopHierarchyMode::Win11_24H2_ProgmanChild
                                               : DesktopHierarchyMode::FallbackProgman;
        }
    }

    if (!info.shelldll_defview) {
        EnumDefViewCtx ctx{};
        EnumWindows(&EnumWorkerWProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.defview) {
            info.defview_parent = ctx.defview_parent;
            info.shelldll_defview = ctx.defview;
            info.workerw_wallpaper = ctx.wallpaper_workerw;
            info.mode = DesktopHierarchyMode::ClassicWorkerW;
            if (info.explorer_pid == 0) {
                GetWindowThreadProcessId(ctx.defview, &info.explorer_pid);
            }
        }
    }

    if (info.shelldll_defview) {
        info.sys_listview32 = FindWindowExW(info.shelldll_defview, nullptr, L"SysListView32", nullptr);
        if (info.sys_listview32) {
            info.listview_visible = IsWindowVisible(info.sys_listview32) != FALSE;
        }
        return info;
    }

    // Read-only Shell COM fallback when called from a non-Default Win32 desktop where FindWindowW
    // cannot see Explorer's desktop windows directly, while CLSID_ShellWindows RPC reaches Explorer.
    RunInStaApartment([&]() -> HRESULT {
        ComPtr<IShellWindows> sw;
        HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&sw));
        if (FAILED(hr) || !sw) return hr;
        VARIANT v_desk{};
        v_desk.vt = VT_I4;
        v_desk.lVal = CSIDL_DESKTOP;
        VARIANT v_empty{};
        VariantInit(&v_empty);
        long desk_hwnd = 0;
        ComPtr<IDispatch> disp;
        if (sw->FindWindowSW(&v_desk, &v_empty, SWC_DESKTOP, &desk_hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) {
            return E_FAIL;
        }
        info.progman = reinterpret_cast<HWND>(static_cast<uintptr_t>(static_cast<uint32_t>(desk_hwnd)));
        info.defview_parent = info.progman;
        ComPtr<IServiceProvider> sp;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> sv;
        if (SUCCEEDED(disp.As(&sp)) && sp &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) && browser &&
            SUCCEEDED(browser->QueryActiveShellView(&sv)) && sv) {
            HWND view_hwnd = nullptr;
            if (SUCCEEDED(sv->GetWindow(&view_hwnd)) && view_hwnd) {
                info.shelldll_defview = view_hwnd;
                info.sys_listview32 = FindWindowExW(view_hwnd, nullptr, L"SysListView32", nullptr);
                if (info.sys_listview32) {
                    info.listview_visible = IsWindowVisible(info.sys_listview32) != FALSE;
                }
            }
        }
        if (info.progman || info.shelldll_defview) {
            info.mode = DesktopHierarchyMode::FallbackProgman;
            if (info.progman) {
                GetWindowThreadProcessId(info.progman, &info.explorer_pid);
            }
        }
        return S_OK;
    });
    return info;
}

std::vector<model::MonitorWorkArea> QueryMonitorWorkAreas() {
    std::vector<model::MonitorWorkArea> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    std::sort(monitors.begin(), monitors.end(), [](const model::MonitorWorkArea& a, const model::MonitorWorkArea& b) {
        if (a.primary != b.primary) return a.primary > b.primary;
        if (a.bounds.x != b.bounds.x) return a.bounds.x < b.bounds.x;
        return a.bounds.y < b.bounds.y;
    });
    if (monitors.empty()) {
        model::MonitorWorkArea fallback{};
        fallback.bounds = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            fallback.work_area = {wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top};
        } else {
            fallback.work_area = fallback.bounds;
        }
        monitors.push_back(std::move(fallback));
    }
    return monitors;
}

std::wstring DefaultSnapshotFilePath() {
    return (fs::path(PulseAppDataDirectory()) / L"desktop_icon_snapshot.json").wstring();
}

std::wstring DefaultLayoutStateFilePath() {
    return (fs::path(PulseAppDataDirectory()) / L"desktop_layout_state.json").wstring();
}

bool CaptureNativeIconSnapshot(NativeIconSnapshot& out_snapshot, HRESULT* out_hr) {
    out_snapshot = {};
    out_snapshot.captured_unix_ms = NowUnixMs();
    out_snapshot.monitors = QueryMonitorWorkAreas();

    const HRESULT hr = RunInStaApartment([&]() -> HRESULT {
        ComPtr<IFolderView2> folder_view;
        ComPtr<IShellFolder> desktop_folder;
        HRESULT bind_hr = BindDesktopFolderView(folder_view, desktop_folder);
        if (FAILED(bind_hr)) return bind_hr;

        FOLDERVIEWMODE view_mode = FVM_ICON;
        int icon_sz = 48;
        if (SUCCEEDED(folder_view->GetViewModeAndIconSize(&view_mode, &icon_sz))) {
            out_snapshot.view_mode = static_cast<uint32_t>(view_mode);
            out_snapshot.icon_size = icon_sz;
        }

        int count = 0;
        bind_hr = folder_view->ItemCount(SVGIO_ALLVIEW, &count);
        if (FAILED(bind_hr) || count < 0) return FAILED(bind_hr) ? bind_hr : E_FAIL;

        out_snapshot.items.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            PITEMID_CHILD pidl = nullptr;
            if (FAILED(folder_view->Item(i, &pidl)) || !pidl) continue;

            POINT pt{};
            const HRESULT pos_hr = folder_view->GetItemPosition(pidl, &pt);
            NativeIconEntry entry{};
            entry.parsing_name = GetShellItemName(desktop_folder.Get(), pidl, SHGDN_FORPARSING);
            entry.relative_name = GetShellItemName(desktop_folder.Get(), pidl, SHGDN_INFOLDER | SHGDN_FORPARSING);
            entry.display_name = GetShellItemName(desktop_folder.Get(), pidl, SHGDN_INFOLDER);
            if (SUCCEEDED(pos_hr)) {
                entry.position = {pt.x, pt.y};
            }
            CoTaskMemFree(pidl);

            if (!entry.parsing_name.empty() || !entry.relative_name.empty() || !entry.display_name.empty()) {
                out_snapshot.items.push_back(std::move(entry));
            }
        }
        return S_OK;
    });

    if (out_hr) *out_hr = hr;
    return SUCCEEDED(hr);
}

bool RestoreNativeIconSnapshot(const NativeIconSnapshot& snapshot, size_t* out_restored_count, HRESULT* out_hr) {
    if (out_restored_count) *out_restored_count = 0;
    if (snapshot.items.empty()) {
        if (out_hr) *out_hr = S_OK;
        return true;
    }

    size_t restored = 0;
    const HRESULT hr = RunInStaApartment([&]() -> HRESULT {
        ComPtr<IFolderView2> folder_view;
        ComPtr<IShellFolder> desktop_folder;
        HRESULT bind_hr = BindDesktopFolderView(folder_view, desktop_folder);
        if (FAILED(bind_hr)) return bind_hr;

        int live_count = 0;
        if (FAILED(folder_view->ItemCount(SVGIO_ALLVIEW, &live_count))) live_count = 0;

        struct LivePidl {
            PITEMID_CHILD pidl = nullptr;
            std::wstring parse_lower;
            std::wstring rel_lower;
            std::wstring disp_lower;
        };
        std::vector<LivePidl> live_items;
        live_items.reserve(static_cast<size_t>(std::max(0, live_count)));
        for (int i = 0; i < live_count; ++i) {
            PITEMID_CHILD pidl = nullptr;
            if (SUCCEEDED(folder_view->Item(i, &pidl)) && pidl) {
                LivePidl lp{};
                lp.pidl = pidl;
                lp.parse_lower = ToLowerWide(GetShellItemName(desktop_folder.Get(), pidl, SHGDN_FORPARSING));
                lp.rel_lower = ToLowerWide(GetShellItemName(desktop_folder.Get(), pidl, SHGDN_INFOLDER | SHGDN_FORPARSING));
                lp.disp_lower = ToLowerWide(GetShellItemName(desktop_folder.Get(), pidl, SHGDN_INFOLDER));
                live_items.push_back(std::move(lp));
            }
        }

        std::vector<PITEMID_CHILD> extra_allocated;
        std::vector<PCITEMID_CHILD> batch_pidls;
        std::vector<POINT> batch_points;
        batch_pidls.reserve(snapshot.items.size());
        batch_points.reserve(snapshot.items.size());

        for (const auto& entry : snapshot.items) {
            const std::wstring want_parse = ToLowerWide(entry.parsing_name);
            const std::wstring want_rel = ToLowerWide(entry.relative_name);
            const std::wstring want_disp = ToLowerWide(entry.display_name);

            PCITEMID_CHILD matched = nullptr;
            for (const auto& lp : live_items) {
                if ((!want_parse.empty() && lp.parse_lower == want_parse) ||
                    (!want_rel.empty() && lp.rel_lower == want_rel) ||
                    (!want_disp.empty() && lp.disp_lower == want_disp)) {
                    matched = lp.pidl;
                    break;
                }
            }
            if (!matched && !entry.relative_name.empty()) {
                PIDLIST_RELATIVE parsed = nullptr;
                std::wstring mutable_rel = entry.relative_name;
                if (SUCCEEDED(desktop_folder->ParseDisplayName(nullptr, nullptr, mutable_rel.data(),
                                                               nullptr, &parsed, nullptr)) && parsed) {
                    auto* child = reinterpret_cast<PITEMID_CHILD>(parsed);
                    extra_allocated.push_back(child);
                    matched = child;
                }
            }
            if (matched) {
                batch_pidls.push_back(matched);
                batch_points.push_back(POINT{entry.position.x, entry.position.y});
            }
        }

        HRESULT pos_hr = S_OK;
        if (!batch_pidls.empty()) {
            pos_hr = folder_view->SelectAndPositionItems(
                static_cast<UINT>(batch_pidls.size()),
                batch_pidls.data(),
                batch_points.data(),
                SVSI_POSITIONITEM);
            if (SUCCEEDED(pos_hr)) {
                restored = batch_pidls.size();
            }
        }

        for (auto& lp : live_items) {
            if (lp.pidl) CoTaskMemFree(lp.pidl);
        }
        for (PITEMID_CHILD p : extra_allocated) {
            if (p) CoTaskMemFree(p);
        }
        return pos_hr;
    });

    if (out_restored_count) *out_restored_count = restored;
    if (out_hr) *out_hr = hr;
    return SUCCEEDED(hr);
}

bool SaveIconSnapshotFile(const std::wstring& file_path, const NativeIconSnapshot& snapshot) {
    if (file_path.empty()) return false;
    std::error_code ec;
    const fs::path target(file_path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }

    std::string json = "{\n";
    json += "  \"version\": 1,\n";
    json += "  \"captured_unix_ms\": " + std::to_string(snapshot.captured_unix_ms) + ",\n";
    json += "  \"view_mode\": " + std::to_string(snapshot.view_mode) + ",\n";
    json += "  \"icon_size\": " + std::to_string(snapshot.icon_size) + ",\n";
    json += "  \"monitors\": [\n";
    for (size_t i = 0; i < snapshot.monitors.size(); ++i) {
        const auto& m = snapshot.monitors[i];
        json += "    {\"device\": " + ipc::JsonQuoteWide(m.device_name) +
                ", \"bx\": " + std::to_string(m.bounds.x) +
                ", \"by\": " + std::to_string(m.bounds.y) +
                ", \"bw\": " + std::to_string(m.bounds.w) +
                ", \"bh\": " + std::to_string(m.bounds.h) +
                ", \"wx\": " + std::to_string(m.work_area.x) +
                ", \"wy\": " + std::to_string(m.work_area.y) +
                ", \"ww\": " + std::to_string(m.work_area.w) +
                ", \"wh\": " + std::to_string(m.work_area.h) +
                ", \"dpi\": " + std::to_string(m.dpi) +
                ", \"primary\": " + (m.primary ? "true" : "false") + "}";
        if (i + 1 < snapshot.monitors.size()) json += ",";
        json += "\n";
    }
    json += "  ],\n";
    json += "  \"items\": [\n";
    for (size_t i = 0; i < snapshot.items.size(); ++i) {
        const auto& it = snapshot.items[i];
        json += "    {\"parsing_name\": " + ipc::JsonQuoteWide(it.parsing_name) +
                ", \"relative_name\": " + ipc::JsonQuoteWide(it.relative_name) +
                ", \"display_name\": " + ipc::JsonQuoteWide(it.display_name) +
                ", \"x\": " + std::to_string(it.position.x) +
                ", \"y\": " + std::to_string(it.position.y) + "}";
        if (i + 1 < snapshot.items.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n}\n";

    const std::wstring tmp_path = file_path + L".tmp";
    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    size_t offset = 0;
    bool write_ok = true;
    while (offset < json.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(json.size() - offset, 1u << 20));
        if (!WriteFile(h, json.data() + offset, chunk, &written, nullptr) || written == 0) {
            write_ok = false;
            break;
        }
        offset += written;
    }
    if (write_ok) {
        FlushFileBuffers(h);
    }
    CloseHandle(h);
    if (!write_ok) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }

    if (!MoveFileExW(tmp_path.c_str(), file_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

bool LoadIconSnapshotFile(const std::wstring& file_path, NativeIconSnapshot& out_snapshot) {
    out_snapshot = {};
    HANDLE h = CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string utf8(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != utf8.size()) return false;

    uint64_t ver = 0;
    if (!ipc::JsonFindUint64(utf8, "version", ver) || ver != 1) return false;
    ipc::JsonFindInt64(utf8, "captured_unix_ms", out_snapshot.captured_unix_ms);
    uint64_t vm = 0;
    if (ipc::JsonFindUint64(utf8, "view_mode", vm)) out_snapshot.view_mode = static_cast<uint32_t>(vm);
    int64_t isz = 48;
    if (ipc::JsonFindInt64(utf8, "icon_size", isz)) out_snapshot.icon_size = static_cast<int32_t>(isz);

    for (std::string_view m_obj : SplitJsonObjectArray(utf8, "monitors")) {
        model::MonitorWorkArea m{};
        ipc::JsonFindWide(m_obj, "device", m.device_name);
        int64_t bx = 0, by = 0, bw = 0, bh = 0, wx = 0, wy = 0, ww = 0, wh = 0;
        uint64_t dpi = 96;
        ipc::JsonFindInt64(m_obj, "bx", bx);
        ipc::JsonFindInt64(m_obj, "by", by);
        ipc::JsonFindInt64(m_obj, "bw", bw);
        ipc::JsonFindInt64(m_obj, "bh", bh);
        ipc::JsonFindInt64(m_obj, "wx", wx);
        ipc::JsonFindInt64(m_obj, "wy", wy);
        ipc::JsonFindInt64(m_obj, "ww", ww);
        ipc::JsonFindInt64(m_obj, "wh", wh);
        ipc::JsonFindUint64(m_obj, "dpi", dpi);
        ipc::JsonFindBool(m_obj, "primary", m.primary);
        m.bounds = {static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bw), static_cast<int>(bh)};
        m.work_area = {static_cast<int>(wx), static_cast<int>(wy), static_cast<int>(ww), static_cast<int>(wh)};
        m.dpi = static_cast<uint32_t>(dpi);
        out_snapshot.monitors.push_back(std::move(m));
    }

    for (std::string_view it_obj : SplitJsonObjectArray(utf8, "items")) {
        NativeIconEntry e{};
        ipc::JsonFindWide(it_obj, "parsing_name", e.parsing_name);
        ipc::JsonFindWide(it_obj, "relative_name", e.relative_name);
        ipc::JsonFindWide(it_obj, "display_name", e.display_name);
        int64_t x = 0, y = 0;
        ipc::JsonFindInt64(it_obj, "x", x);
        ipc::JsonFindInt64(it_obj, "y", y);
        e.position = {static_cast<int>(x), static_cast<int>(y)};
        out_snapshot.items.push_back(std::move(e));
    }
    return true;
}

std::wstring ResolveKnownFolder(const GUID& folder_id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder_id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) || !raw) {
        return {};
    }
    std::wstring out(raw);
    CoTaskMemFree(raw);
    return out;
}

namespace {

std::wstring TrimTrailingBackslashForWatch(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }
    return p;
}

void EnumerateSingleDirectoryInto(const std::wstring& raw_dir,
                                  const std::string& owner_group_id,
                                  bool check_subfolder_empty,
                                  size_t max_items,
                                  IShellLinkW* shell_link,
                                  IPersistFile* persist_file,
                                  const std::unordered_map<std::wstring, model::PointI>* pos_by_path,
                                  const std::unordered_map<std::wstring, model::PointI>* pos_by_name,
                                  std::unordered_set<std::wstring>& seen_paths,
                                  std::vector<model::DesktopItem>& items) {
    const std::wstring dir = TrimTrailingBackslashForWatch(raw_dir);
    if (dir.empty()) return;

    const std::wstring pattern = (dir.back() == L'\\' || dir.back() == L'/')
                                     ? (dir + L"*")
                                     : (dir + L"\\*");
    WIN32_FIND_DATAW wfd{};
    HANDLE hfind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &wfd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hfind == INVALID_HANDLE_VALUE) return;

    const std::wstring prefix = (dir.back() == L'\\' || dir.back() == L'/') ? dir : (dir + L"\\");
    do {
        if (items.size() >= max_items) break;
        if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;
        if ((wfd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) continue;
        if (_wcsicmp(wfd.cFileName, L"desktop.ini") == 0) continue;

        const std::wstring full = prefix + wfd.cFileName;
        const std::wstring full_lower = ToLowerWide(full);
        if (!seen_paths.insert(full_lower).second) continue;

        model::DesktopItem item{};
        item.path = full;
        item.mapped_owner_group_id = owner_group_id;
        item.is_folder = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (item.is_folder && check_subfolder_empty) {
            WIN32_FIND_DATAW child_wfd{};
            const std::wstring sub_pat = full + L"\\*";
            HANDLE hsub = FindFirstFileExW(sub_pat.c_str(), FindExInfoBasic, &child_wfd,
                                           FindExSearchNameMatch, nullptr, 0);
            bool has_entry = false;
            if (hsub != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(child_wfd.cFileName, L".") != 0 &&
                        wcscmp(child_wfd.cFileName, L"..") != 0) {
                        has_entry = true;
                        break;
                    }
                } while (FindNextFileW(hsub, &child_wfd));
                FindClose(hsub);
            }
            item.folder_is_empty = !has_entry;
        }
        item.extension = item.is_folder ? std::wstring{} : ToLowerWide(fs::path(wfd.cFileName).extension().wstring());
        item.is_shortcut = (item.extension == L".lnk" || item.extension == L".url");
        if (item.is_shortcut) {
            item.name = fs::path(wfd.cFileName).stem().wstring();
        } else {
            item.name = wfd.cFileName;
        }
        if (item.name.empty()) item.name = wfd.cFileName;

        ULARGE_INTEGER sz{};
        sz.LowPart = wfd.nFileSizeLow;
        sz.HighPart = wfd.nFileSizeHigh;
        item.size_bytes = item.is_folder ? 0 : sz.QuadPart;
        item.modified_unix_ms = FileTimeToUnixMs(wfd.ftLastWriteTime);

        if (item.extension == L".lnk" && shell_link != nullptr && persist_file != nullptr) {
            ResolveShortcutTargetNoUi(shell_link, persist_file, item.path,
                                      item.link_target, item.link_target_missing);
        }

        if (pos_by_path && pos_by_name) {
            auto it_pos = pos_by_path->find(full_lower);
            if (it_pos != pos_by_path->end()) {
                item.native_pos = it_pos->second;
                item.loose_pos = it_pos->second;
            } else {
                auto it_nm = pos_by_name->find(ToLowerWide(wfd.cFileName));
                if (it_nm == pos_by_name->end()) it_nm = pos_by_name->find(ToLowerWide(item.name));
                if (it_nm != pos_by_name->end()) {
                    item.native_pos = it_nm->second;
                    item.loose_pos = it_nm->second;
                }
            }
        }
        items.push_back(std::move(item));
    } while (FindNextFileW(hfind, &wfd));
    FindClose(hfind);
}

} // namespace

std::vector<model::DesktopItem> EnumerateDesktopItems(const NativeIconSnapshot* optional_snapshot) {
    std::unordered_map<std::wstring, model::PointI> pos_by_path;
    std::unordered_map<std::wstring, model::PointI> pos_by_name;
    if (optional_snapshot) {
        for (const auto& s : optional_snapshot->items) {
            if (!s.parsing_name.empty()) pos_by_path[ToLowerWide(s.parsing_name)] = s.position;
            if (!s.relative_name.empty()) pos_by_name[ToLowerWide(s.relative_name)] = s.position;
            if (!s.display_name.empty()) pos_by_name[ToLowerWide(s.display_name)] = s.position;
        }
    }

    std::vector<model::DesktopItem> items;
    RunInStaApartment([&]() -> HRESULT {
        ComPtr<IShellLinkW> shell_link;
        ComPtr<IPersistFile> persist_file;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
            shell_link.As(&persist_file);
        }

        const std::wstring dirs[2]{
            ResolveKnownFolder(FOLDERID_Desktop),
            ResolveKnownFolder(FOLDERID_PublicDesktop),
        };
        std::unordered_set<std::wstring> seen_paths;
        for (const auto& dir : dirs) {
            EnumerateSingleDirectoryInto(dir, {}, /*check_subfolder_empty=*/true,
                                         static_cast<size_t>(-1), shell_link.Get(), persist_file.Get(),
                                         &pos_by_path, &pos_by_name, seen_paths, items);
        }
        return S_OK;
    });
    return items;
}

std::vector<model::DesktopItem> EnumerateDirectoryItems(const std::wstring& dir_path,
                                                        const std::string& owner_group_id,
                                                        size_t max_items) {
    std::vector<model::DesktopItem> items;
    if (dir_path.empty() || max_items == 0) return items;
    const DWORD attrs = GetFileAttributesW(dir_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return items;
    }

    RunInStaApartment([&]() -> HRESULT {
        ComPtr<IShellLinkW> shell_link;
        ComPtr<IPersistFile> persist_file;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link)))) {
            shell_link.As(&persist_file);
        }
        std::unordered_set<std::wstring> seen_paths;
        EnumerateSingleDirectoryInto(dir_path, owner_group_id, /*check_subfolder_empty=*/false,
                                     max_items, shell_link.Get(), persist_file.Get(),
                                     nullptr, nullptr, seen_paths, items);
        return S_OK;
    });
    return items;
}

std::vector<MiniFolderItem> QueryFolderTopItems(const std::wstring& dir_path, size_t max_items) {
    std::vector<MiniFolderItem> items;
    if (dir_path.empty() || max_items == 0) return items;
    const std::wstring pattern = dir_path + L"\\*";
    WIN32_FIND_DATAW wfd{};
    HANDLE hfind = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &wfd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hfind == INVALID_HANDLE_VALUE) return items;
    do {
        if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;
        if ((wfd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) continue;
        MiniFolderItem item{};
        item.path = dir_path + L"\\" + wfd.cFileName;
        item.name = wfd.cFileName;
        item.is_folder = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!item.is_folder) {
            item.extension = ToLowerWide(fs::path(wfd.cFileName).extension().wstring());
        }
        ULARGE_INTEGER sz{};
        sz.LowPart = wfd.nFileSizeLow;
        sz.HighPart = wfd.nFileSizeHigh;
        item.size_bytes = sz.QuadPart;
        item.modified_unix_ms = FileTimeToUnixMs(wfd.ftLastWriteTime);
        items.push_back(std::move(item));
    } while (FindNextFileW(hfind, &wfd));
    FindClose(hfind);

    std::sort(items.begin(), items.end(), [](const MiniFolderItem& a, const MiniFolderItem& b) {
        return a.modified_unix_ms > b.modified_unix_ms;
    });
    if (items.size() > max_items) items.resize(max_items);
    return items;
}

RecycleBinStats QueryRecycleBinStats() noexcept {
    SHQUERYRBINFO rb{};
    rb.cbSize = sizeof(rb);
    RecycleBinStats out{};
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb))) {
        out.item_count = static_cast<uint64_t>(std::max<LONGLONG>(0, rb.i64NumItems));
        out.total_bytes = static_cast<uint64_t>(std::max<LONGLONG>(0, rb.i64Size));
    }
    return out;
}

DriveSpaceStats QueryDriveSpaceStats(const std::wstring& root_path) noexcept {
    DriveSpaceStats out{};
    out.root_path = root_path.empty() ? L"C:\\" : root_path;
    ULARGE_INTEGER free_avail{}, total{}, total_free{};
    if (GetDiskFreeSpaceExW(out.root_path.c_str(), &free_avail, &total, &total_free)) {
        out.free_bytes = free_avail.QuadPart;
        out.total_bytes = total.QuadPart;
    }
    return out;
}

// ---------------------------------------------------------------------------
// DesktopFolderWatcher
// ---------------------------------------------------------------------------
DesktopFolderWatcher::~DesktopFolderWatcher() {
    Stop();
}

bool DesktopFolderWatcher::Start(ChangeCallback on_changed, std::vector<std::wstring> extra_dirs) {
    Stop();
    std::vector<std::wstring> dirs;
    std::unordered_set<std::wstring> seen;
    auto push_dir = [&](const std::wstring& raw) {
        if (dirs.size() >= MAXIMUM_WAIT_OBJECTS - 1) return;
        const std::wstring norm = TrimTrailingBackslashForWatch(raw);
        if (norm.empty()) return;
        const std::wstring key = ToLowerWide(norm);
        if (!seen.insert(key).second) return;
        const DWORD attrs = GetFileAttributesW(norm.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            dirs.push_back(norm);
        }
    };
    push_dir(ResolveKnownFolder(FOLDERID_Desktop));
    push_dir(ResolveKnownFolder(FOLDERID_PublicDesktop));
    for (const auto& ed : extra_dirs) {
        push_dir(ed);
    }
    if (dirs.empty()) return false;

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) return false;
    on_changed_ = std::move(on_changed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, d = std::move(dirs)]() mutable { WatchThreadMain(std::move(d)); });
    return true;
}

void DesktopFolderWatcher::Stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (stop_event_) SetEvent(stop_event_);
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

void DesktopFolderWatcher::WatchThreadMain(std::vector<std::wstring> dirs) {
    struct WatchSlot {
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED ov{};
        alignas(DWORD) uint8_t buffer[16384]{};
        bool armed = false;
    };
    std::vector<WatchSlot> slots(dirs.size());
    std::vector<HANDLE> wait_handles;
    wait_handles.push_back(stop_event_);

    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

    for (size_t i = 0; i < dirs.size(); ++i) {
        slots[i].dir = CreateFileW(
            dirs[i].c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        slots[i].event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (slots[i].dir != INVALID_HANDLE_VALUE && slots[i].event) {
            slots[i].ov.hEvent = slots[i].event;
            DWORD unused = 0;
            if (ReadDirectoryChangesW(slots[i].dir, slots[i].buffer, sizeof(slots[i].buffer), FALSE,
                                      kFilter, &unused, &slots[i].ov, nullptr) ||
                GetLastError() == ERROR_IO_PENDING) {
                slots[i].armed = true;
                wait_handles.push_back(slots[i].event);
            }
        }
    }

    bool pending_debounce = false;
    ULONGLONG debounce_deadline = 0;

    while (running_.load(std::memory_order_acquire)) {
        DWORD timeout = INFINITE;
        if (pending_debounce) {
            const ULONGLONG now = GetTickCount64();
            timeout = now >= debounce_deadline ? 0u : static_cast<DWORD>(debounce_deadline - now);
        }
        const DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()),
                                                    wait_handles.data(), FALSE, timeout);
        if (waited == WAIT_OBJECT_0) break;
        if (waited == WAIT_TIMEOUT && pending_debounce) {
            pending_debounce = false;
            if (on_changed_ && running_.load(std::memory_order_acquire)) {
                try {
                    on_changed_();
                } catch (...) {
                }
            }
            continue;
        }
        if (waited > WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + wait_handles.size()) {
            HANDLE signaled = wait_handles[waited - WAIT_OBJECT_0];
            for (auto& s : slots) {
                if (s.event == signaled && s.armed) {
                    DWORD bytes = 0;
                    GetOverlappedResult(s.dir, &s.ov, &bytes, FALSE);
                    ResetEvent(s.event);
                    s.ov = {};
                    s.ov.hEvent = s.event;
                    DWORD unused = 0;
                    s.armed = ReadDirectoryChangesW(s.dir, s.buffer, sizeof(s.buffer), FALSE,
                                                    kFilter, &unused, &s.ov, nullptr) ||
                              GetLastError() == ERROR_IO_PENDING;
                    pending_debounce = true;
                    debounce_deadline = GetTickCount64() + 120;
                }
            }
        } else {
            break;
        }
    }

    for (auto& s : slots) {
        if (s.dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(s.dir, nullptr);
            CloseHandle(s.dir);
        }
        if (s.event) CloseHandle(s.event);
    }
}

// ---------------------------------------------------------------------------
// DesktopMountHost
// ---------------------------------------------------------------------------
DesktopMountHost::~DesktopMountHost() {
    Unmount(false);
}

// The layer is a top-level tool window kept directly above Explorer's desktop host: the window that owns
// SHELLDLL_DefView (Progman, or the WorkerW that live wallpapers split off). It never becomes a child of
// Explorer's windows, so it composes like any app window and takes input over the bare desktop.
bool DesktopMountHost::Mount(bool takeover_native_icons,
                             const std::wstring& snapshot_file_path,
                             MountHostCallbacks callbacks,
                             const wchar_t* takeover_reg_key) {
    Unmount(false);

    callbacks_ = std::move(callbacks);
    takeover_icons_ = takeover_native_icons;
    snapshot_path_ = snapshot_file_path.empty() ? DefaultSnapshotFilePath() : snapshot_file_path;
    takeover_reg_key_ = takeover_reg_key ? takeover_reg_key : desktop_takeover::kDefaultRecordKey;
    msg_taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &DesktopMountHost::StaticWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kMountWindowClass;
    RegisterClassExW(&wc);

    hierarchy_ = ProbeDesktopHierarchy();
    ComputeBounds();

    // Read-only: where Explorer had each icon, so loose icons start where the user left them.
    if (CaptureNativeIconSnapshot(initial_snapshot_)) {
        SaveIconSnapshotFile(snapshot_path_, initial_snapshot_);
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kMountWindowClass, L"Pulse Desktop Surface",
                            WS_POPUP, bounds_.x, bounds_.y, bounds_.w, bounds_.h, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    mounted_ = true;
    PlaceAboveHost();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Explorer's icons stay until the host has drawn its first frame (HideNativeIconsNow), so the desktop
    // is never empty while items are enumerated.
    SetTimer(hwnd_, kTimerKeepAboveHost, 300, nullptr);
    MountLog("mount mode=%s host=%p bounds=%d,%d %dx%d dpi=%u hid_icons=%d", HierarchyModeName(hierarchy_.mode),
             LayerHost(), bounds_.x, bounds_.y, bounds_.w, bounds_.h, dpi_, hid_icons_ ? 1 : 0);
    return true;
}

void DesktopMountHost::ComputeBounds() {
    // The primary monitor's work area: a window covering the whole monitor counts as a full-screen app.
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    bounds_ = {mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
    dpi_ = dx;
}

HWND DesktopMountHost::LayerHost() const noexcept {
    return hierarchy_.defview_parent ? hierarchy_.defview_parent : hierarchy_.progman;
}

// True only when nothing visible sits between the layer and the desktop host: anywhere higher would cover
// app windows, anywhere lower would hide the layer.
bool DesktopMountHost::IsAboveHost() const noexcept {
    const HWND host = LayerHost();
    if (!host || !hwnd_) return false;
    for (HWND w = GetWindow(hwnd_, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == host) return true;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (IsWindowVisible(w) && !cloaked) {
            RECT wr{};
            if (!GetWindowRect(w, &wr) || wr.right <= wr.left || wr.bottom <= wr.top) continue;
            return false;
        }
    }
    return false;
}

void DesktopMountHost::PlaceAboveHost() {
    HWND host = LayerHost();
    if (!host || !IsWindow(host) || !FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr)) {
        hierarchy_ = ProbeDesktopHierarchy();
        host = LayerHost();
    }
    if (!host || !hwnd_ || IsAboveHost()) return;
    const HWND above = GetWindow(host, GW_HWNDPREV);
    if (above == hwnd_) return;
    placing_ = true;
    SetWindowPos(hwnd_, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    placing_ = false;
}

// Hide the owned view while Explorer keeps its icons, positions and user preference.
void DesktopMountHost::HideNativeIcons() {
    if (!takeover_icons_) return;
    HWND list_view = desktop_takeover::FindNativeDesktopListView();
    if (!list_view || (GetWindowLongPtrW(list_view, GWL_STYLE) & WS_VISIBLE) == 0) return;

    if (pulse::os::TrueWindowsBuild() < 22000) {
        // On Windows 10, ToggleDesktopIcons (WM_COMMAND 0x7402) tells CDefView to hide
        // SysListView32 AND repaint Progman/WorkerW so stale icon pixels are cleared.
        if (!desktop_takeover::DesktopIconsHidden()) {
            if (!desktop_takeover::RecordActive(GetCurrentProcessId(), true, snapshot_path_, takeover_reg_key_.c_str())) {
                return;
            }
            hid_icons_ = desktop_takeover::ToggleDesktopIcons();
            if (hid_icons_) return;
            desktop_takeover::RecordIconsRestored(takeover_reg_key_.c_str());
        }
        if (!desktop_takeover::RecordActive(GetCurrentProcessId(), false, snapshot_path_, takeover_reg_key_.c_str())) {
            return;
        }
        hid_icons_ = desktop_takeover::HideNativeDesktopListView(list_view, takeover_reg_key_.c_str());
        if (HWND dv = desktop_takeover::FindNativeDesktopDefView()) {
            RedrawWindow(dv, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        return;
    }

    // Record ownership before hiding; asynchronous visibility changes never wait on Explorer's UI thread.
    if (!desktop_takeover::RecordActive(GetCurrentProcessId(), false, snapshot_path_, takeover_reg_key_.c_str())) return;
    hid_icons_ = desktop_takeover::HideNativeDesktopListView(list_view, takeover_reg_key_.c_str());
}

void DesktopMountHost::HideNativeIconsNow() {
    if (!mounted_ || icons_taken_) return;
    icons_taken_ = true;
    HideNativeIcons();
    MountLog("icons hidden=%d", hid_icons_ ? 1 : 0);
}

void DesktopMountHost::UpdateSurfaceBounds() {
    ComputeBounds();
    if (hwnd_) {
        placing_ = true;
        SetWindowPos(hwnd_, nullptr, bounds_.x, bounds_.y, bounds_.w, bounds_.h, SWP_NOACTIVATE | SWP_NOZORDER);
        placing_ = false;
    }
    if (callbacks_.on_geometry_changed) {
        callbacks_.on_geometry_changed(bounds_, dpi_);
    }
}

void DesktopMountHost::Unmount(bool /*restore_icon_positions*/) noexcept {
    const bool was_mounted = mounted_;
    mounted_ = false;

    if (hwnd_) {
        KillTimer(hwnd_, kTimerKeepAboveHost);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    // Icon positions never change: Explorer kept the icons where they were while they were hidden.
    if (was_mounted && takeover_icons_) {
        desktop_takeover::RestoreDesktopIcons(takeover_reg_key_.c_str());
        desktop_takeover::ClearActive(takeover_reg_key_.c_str(), false);
        hid_icons_ = false;
    }
    icons_taken_ = false;
}

model::RectI DesktopMountHost::SurfaceBoundsDip() const noexcept {
    const float s = DipScale();
    return {static_cast<int>(std::lround(bounds_.x / s)), static_cast<int>(std::lround(bounds_.y / s)),
            static_cast<int>(std::lround(bounds_.w / s)), static_cast<int>(std::lround(bounds_.h / s))};
}

void DesktopMountHost::ClientToScreenDip(POINT* pt) const noexcept {
    const float s = DipScale();
    pt->x += static_cast<LONG>(std::lround(bounds_.x / s));
    pt->y += static_cast<LONG>(std::lround(bounds_.y / s));
}

void DesktopMountHost::ScreenDipToClient(POINT* pt) const noexcept {
    const float s = DipScale();
    pt->x -= static_cast<LONG>(std::lround(bounds_.x / s));
    pt->y -= static_cast<LONG>(std::lround(bounds_.y / s));
}

POINT DesktopMountHost::ClientDipToScreenPx(POINT pt) const noexcept {
    const float s = DipScale();
    return POINT{bounds_.x + static_cast<LONG>(std::lround(pt.x * s)), bounds_.y + static_cast<LONG>(std::lround(pt.y * s))};
}

POINT DesktopMountHost::ScreenPxToClientDip(POINT pt) const noexcept {
    const float s = DipScale();
    return POINT{static_cast<LONG>(std::lround((pt.x - bounds_.x) / s)), static_cast<LONG>(std::lround((pt.y - bounds_.y) / s))};
}

LRESULT CALLBACK DesktopMountHost::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    DesktopMountHost* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<DesktopMountHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<DesktopMountHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

namespace {
bool IsClientMouseMessage(UINT msg) noexcept {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEHOVER:
        return true;
    default:
        return false;
    }
}
} // namespace

LRESULT DesktopMountHost::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
    if (msg == WM_WINDOWPOSCHANGING && mounted_) {
        auto* wp = reinterpret_cast<WINDOWPOS*>(lparam);
        if (wp) {
            // Only the layer moves its own z-order: activation or clicks never lift it over app windows,
            // and Show Desktop cannot hide it.
            if (!placing_) wp->flags |= SWP_NOZORDER;
            wp->flags &= ~SWP_HIDEWINDOW;
        }
        return 0;
    }
    if (msg == WM_SYSCOMMAND && mounted_) {
        const WPARAM cmd = wparam & 0xFFF0u;
        if (cmd == SC_MINIMIZE || cmd == SC_MAXIMIZE || cmd == SC_CLOSE) {
            return 0;
        }
    }
    if (msg == WM_SIZE && wparam == SIZE_MINIMIZED && mounted_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateSurfaceBounds();
        return 0;
    }
    if (msg == WM_TIMER && wparam == kTimerKeepAboveHost) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (!IsAboveHost()) PlaceAboveHost();
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg_taskbar_created_ != 0 && msg == msg_taskbar_created_ && mounted_) {
        // Explorer restarted: new desktop windows; the icon switch survives in Explorer's own setting.
        hierarchy_ = ProbeDesktopHierarchy();
        PlaceAboveHost();
        if (icons_taken_) HideNativeIcons();
        UpdateSurfaceBounds();
        MountLog("explorer restarted mode=%s host=%p", HierarchyModeName(hierarchy_.mode), LayerHost());
        if (callbacks_.on_explorer_restarted) callbacks_.on_explorer_restarted();
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE || msg == WM_DPICHANGED || (msg == WM_SETTINGCHANGE && wparam == SPI_SETWORKAREA)) {
        UpdateSurfaceBounds();
        return 0;
    }
    if (msg == WM_SETTINGCHANGE && wparam == SPI_SETDESKWALLPAPER) {
        if (mounted_) {
            hierarchy_ = ProbeDesktopHierarchy();
            PlaceAboveHost();
        }
        if (callbacks_.on_wallpaper_changed) callbacks_.on_wallpaper_changed();
    }

    if (callbacks_.on_message) {
        // The host works in DIPs relative to the layer; client mouse coordinates arrive here in pixels.
        LPARAM host_lparam = lparam;
        if (IsClientMouseMessage(msg)) {
            const float s = DipScale();
            const int x = static_cast<int>(std::lround(static_cast<short>(LOWORD(lparam)) / s));
            const int y = static_cast<int>(std::lround(static_cast<short>(HIWORD(lparam)) / s));
            host_lparam = MAKELPARAM(static_cast<WORD>(static_cast<short>(x)), static_cast<WORD>(static_cast<short>(y)));
        }
        bool handled = false;
        const LRESULT res = callbacks_.on_message(hwnd_, msg, wparam, host_lparam, handled);
        if (handled) return res;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

} // namespace pulse::desktop::mount
