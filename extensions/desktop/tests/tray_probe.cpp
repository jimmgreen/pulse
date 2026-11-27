// tray_probe.cpp - Diagnostic: what the Dock's tray scan sees on this machine (run at the user's integrity
// level, not from a sandbox). Prints the Win11 UIA notify items, NotifyIconSettings entries (exe, promoted,
// running), and the final DockTrayApp list with the resolved exe path and whether an icon would load.
// Usage: pulse_desktop_tray_probe.exe <out.txt>
#include "host/live/dock_system_provider.cpp"
#include <fstream>
using namespace pulse::desktop::live;

namespace {
std::string U8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
bool IconLoads(const std::wstring& exe) {
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(exe.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon) return false;
    DestroyIcon(sfi.hIcon);
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::ofstream out(argv[1], std::ios::binary);
    out << "\xEF\xBB\xBF";

    std::set<std::wstring> running;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        if (Process32FirstW(snap, &pe)) do {
            running.insert(ToLower(pe.szExeFile));
            if (std::wstring(ToLower(pe.szExeFile)).find(L"everything") != std::wstring::npos) {
                out << "process: " << U8(pe.szExeFile) << " pid=" << pe.th32ProcessID
                    << " path=" << U8(QueryExePathForPid(pe.th32ProcessID)) << "\n";
            }
        } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
    }

    const auto reg = LoadNotifyIconRegIndex();
    out << "== NotifyIconSettings (" << reg.all.size() << ")\n";
    for (const auto& e : reg.all) {
        const std::wstring base = ToLower(e.exe_path.substr(e.exe_path.find_last_of(L"\\/") + 1));
        out << (e.promoted ? "[promoted] " : "[hidden]   ") << (running.count(base) ? "[running] " : "[stopped] ")
            << U8(e.initial_tooltip) << " | " << U8(e.exe_path) << " | key " << U8(e.subkey) << "\n";
    }

    std::vector<DockTrayApp> uia;
    std::set<std::wstring> seen;
    ScanWindows11TrayViaUia(reg, uia, seen);
    out << "== UIA notify items (" << uia.size() << ")\n";
    for (const auto& a : uia) out << U8(a.title) << " | " << U8(a.exe_path) << "\n";

    std::vector<DockTrayApp> apps;
    QueryTrayApps({}, apps);
    out << "== Dock tray list (" << apps.size() << ")\n";
    for (const auto& a : apps) {
        out << (IconLoads(a.exe_path) ? "[icon]     " : "[fallback] ") << U8(a.title) << " | " << U8(a.exe_path)
            << " | pid " << a.pid << (a.from_toolbar ? " | toolbar" : "") << "\n";
    }
    CoUninitialize();
    return 0;
}
