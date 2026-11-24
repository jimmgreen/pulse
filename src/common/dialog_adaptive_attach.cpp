#include "dialog_adaptive_attach.h"
#include "json_value.h"
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace pulse::dialog_adaptive {
namespace {

constexpr size_t kMaxImageBytes = 512u * 1024u * 1024u;
constexpr size_t kMaxModules = 1024;
constexpr size_t kMaxScannedModules = 512;

std::wstring StripExtendedPrefix(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return path;
}

bool StartsWithDir(const std::wstring& path, const std::wstring& root) noexcept {
    if (root.empty()) return false;
    const auto p = StripExtendedPrefix(path);
    auto r = StripExtendedPrefix(root);
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    if (r.empty() || p.size() <= r.size()) return false;
    if (_wcsnicmp(p.c_str(), r.c_str(), r.size()) != 0) return false;
    return p[r.size()] == L'\\' || p[r.size()] == L'/';
}

std::wstring BasenameLower(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    for (auto& ch : name) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return name;
}

bool ContainsCaseInsensitive(const std::wstring& haystack, std::wstring_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (_wcsnicmp(haystack.data() + i, needle.data(), needle.size()) == 0) return true;
    }
    return false;
}

bool IsMfcRedistModule(const std::wstring& path, const std::wstring& windows_dir) noexcept {
    const auto name = BasenameLower(path);
    static constexpr std::wstring_view kMfc[] = {
        L"mfc140u.dll", L"mfc140.dll", L"mfcm140u.dll", L"mfcm140.dll",
        L"mfc120u.dll", L"mfc120.dll", L"mfc110u.dll", L"mfc110.dll",
        L"mfc100u.dll", L"mfc100.dll", L"mfc90u.dll", L"mfc90.dll",
        L"mfc80u.dll", L"mfc80.dll", L"mfc71u.dll", L"mfc71.dll",
        L"mfc42u.dll", L"mfc42.dll"
    };
    bool matched = false;
    for (auto candidate : kMfc) {
        if (name == candidate) { matched = true; break; }
    }
    if (!matched) return false;
    auto win = StripExtendedPrefix(windows_dir);
    while (!win.empty() && (win.back() == L'\\' || win.back() == L'/')) win.pop_back();
    return StartsWithDir(path, win + L"\\System32") ||
           StartsWithDir(path, win + L"\\SysWOW64") ||
           StartsWithDir(path, win + L"\\WinSxS");
}

uint64_t CurrentFiletime() noexcept {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t Fnv1a64(std::wstring_view text) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : text) {
        const uint16_t unit = static_cast<uint16_t>(ch);
        hash = (hash ^ (unit & 0xffu)) * 1099511628211ull;
        hash = (hash ^ (unit >> 8)) * 1099511628211ull;
    }
    return hash;
}

// Exact FILETIME persisted as a decimal string (JSON numbers lose precision).
bool ReadExactFiletime(const pulse::json::Value& entry, uint64_t& output) noexcept {
    const auto* field = entry.find("updated_ft");
    if (!field || !field->is_string()) return false;
    const auto& text = field->as_string();
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
    output = value;
    return true;
}

uint64_t LowerPathHash(const std::wstring& image_path) {
    std::wstring lower = StripExtendedPrefix(image_path);
    for (auto& ch : lower) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return Fnv1a64(lower);
}

bool ReadRemoteChecked(HANDLE process, uintptr_t base, size_t image_size, size_t rva, void* out, size_t count) noexcept {
    if (rva > image_size || count > image_size - rva || base > UINTPTR_MAX - rva - count) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + rva), out, count, &copied) && copied == count;
}

bool ReadRemoteAscii(HANDLE process, uintptr_t base, size_t image_size, size_t rva, std::array<char, 256>& out) noexcept {
    out.fill('\0');
    if (rva >= image_size) return false;
    const size_t max_len = std::min(out.size() - 1, image_size - rva);
    if (!max_len) return false;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + rva), out.data(), max_len, &copied) || copied == 0) {
        // Retry smaller chunk if crossing end of committed page.
        for (size_t i = 0; i < max_len; ++i) {
            if (!ReadRemoteChecked(process, base, image_size, rva + i, &out[i], 1)) return false;
            if (!out[i]) return true;
        }
        return false;
    }
    for (size_t i = 0; i < copied; ++i) {
        if (!out[i]) return true;
    }
    return false;
}

uint32_t ClassifyImportName(const char* name, bool allow_winrt) noexcept {
    if (!name || !name[0]) return kMatchNone;
    if (std::strcmp(name, "GetOpenFileNameW") == 0 ||
        std::strcmp(name, "GetSaveFileNameW") == 0 ||
        std::strcmp(name, "GetOpenFileNameA") == 0 ||
        std::strcmp(name, "GetSaveFileNameA") == 0 ||
        std::strcmp(name, "SHBrowseForFolderW") == 0 ||
        std::strcmp(name, "SHBrowseForFolderA") == 0) {
        return kMatchLegacyImports;
    }
    if (std::strcmp(name, "CoCreateInstance") == 0 ||
        std::strcmp(name, "CoCreateInstanceEx") == 0 ||
        std::strcmp(name, "CoGetClassObject") == 0 ||
        std::strcmp(name, "SHOpenFolderAndSelectItems") == 0 ||
        std::strcmp(name, "ShellExecuteW") == 0 ||
        std::strcmp(name, "ShellExecuteExW") == 0 ||
        std::strcmp(name, "ShellExecuteA") == 0 ||
        std::strcmp(name, "ShellExecuteExA") == 0) {
        return kMatchComDialog;
    }
    if (allow_winrt && std::strcmp(name, "RoGetActivationFactory") == 0) {
        return kMatchWinRt;
    }
    return kMatchNone;
}

uint32_t ScanRemoteModulePe(HANDLE process, uintptr_t base, WORD expected_machine, bool is_main_exe) noexcept {
    IMAGE_DOS_HEADER dos{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base), &dos, sizeof(dos), &read) || read != sizeof(dos)) return kMatchNone;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) return kMatchNone;

    IMAGE_NT_HEADERS nt{};
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + static_cast<size_t>(dos.e_lfanew)), &nt, sizeof(nt), &read) || read != sizeof(nt)) return kMatchNone;
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != expected_machine ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt.OptionalHeader.SizeOfImage < sizeof(nt) ||
        nt.OptionalHeader.SizeOfImage > kMaxImageBytes) {
        return kMatchNone;
    }

    const size_t size = nt.OptionalHeader.SizeOfImage;
    uint32_t found = kMatchNone;
    size_t total_thunks = 0;
    std::array<char, 256> name{};

    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        const auto imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress && imports.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR) &&
            imports.VirtualAddress < size && imports.Size <= size - imports.VirtualAddress &&
            imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) <= 2048) {
            for (size_t off = 0; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size; off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                IMAGE_IMPORT_DESCRIPTOR desc{};
                if (!ReadRemoteChecked(process, base, size, imports.VirtualAddress + off, &desc, sizeof(desc))) break;
                if (!desc.Name && !desc.FirstThunk && !desc.OriginalFirstThunk) break;
                const size_t int_rva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
                if (!int_rva || int_rva >= size) continue;
                for (size_t idx = 0; idx < 8192; ++idx) {
                    if (++total_thunks > 32768) return found;
                    IMAGE_THUNK_DATA thunk{};
                    const size_t delta = idx * sizeof(thunk);
                    if (delta > size - int_rva || !ReadRemoteChecked(process, base, size, int_rva + delta, &thunk, sizeof(thunk))) break;
                    if (!thunk.u1.AddressOfData) break;
                    if (IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal)) continue;
                    const size_t hint_rva = static_cast<size_t>(thunk.u1.AddressOfData);
                    if (hint_rva >= size || size - hint_rva <= sizeof(WORD)) continue;
                    if (ReadRemoteAscii(process, base, size, hint_rva + sizeof(WORD), name)) {
                        found |= ClassifyImportName(name.data(), is_main_exe);
                        if (found & kMatchLegacyImports) return found;
                    }
                }
            }
        }
    }

    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) {
        const auto delay = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay.VirtualAddress && delay.Size >= sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) &&
            delay.VirtualAddress < size && delay.Size <= size - delay.VirtualAddress &&
            delay.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= 2048) {
            for (size_t off = 0; off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= delay.Size; off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
                IMAGE_DELAYLOAD_DESCRIPTOR desc{};
                if (!ReadRemoteChecked(process, base, size, delay.VirtualAddress + off, &desc, sizeof(desc))) break;
                if (!desc.DllNameRVA && !desc.ImportNameTableRVA && !desc.ImportAddressTableRVA) break;
                if (!desc.Attributes.RvaBased || !desc.ImportNameTableRVA || desc.ImportNameTableRVA >= size) continue;
                for (size_t idx = 0; idx < 8192; ++idx) {
                    if (++total_thunks > 32768) return found;
                    IMAGE_THUNK_DATA thunk{};
                    const size_t delta = idx * sizeof(thunk);
                    if (delta > size - desc.ImportNameTableRVA ||
                        !ReadRemoteChecked(process, base, size, static_cast<size_t>(desc.ImportNameTableRVA) + delta, &thunk, sizeof(thunk))) break;
                    if (!thunk.u1.AddressOfData) break;
                    if (IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal)) continue;
                    const size_t hint_rva = static_cast<size_t>(thunk.u1.AddressOfData);
                    if (hint_rva >= size || size - hint_rva <= sizeof(WORD)) continue;
                    if (ReadRemoteAscii(process, base, size, hint_rva + sizeof(WORD), name)) {
                        found |= ClassifyImportName(name.data(), is_main_exe);
                        if (found & kMatchLegacyImports) return found;
                    }
                }
            }
        }
    }

    return found;
}

uint32_t ClassifyFrameworkModule(const std::wstring& lower_name) noexcept {
    if (lower_name == L"comdlg32.dll") {
        return kMatchLegacyImports;
    }
    if (lower_name == L"pulsedialogauto64.dll" || lower_name == L"pulsedialogauto32.dll") {
        return kMatchComDialog;
    }
    if (lower_name == L"system.windows.forms.dll" || lower_name == L"system.windows.forms.ni.dll" ||
        lower_name == L"wpfgfx_v0400.dll" || lower_name == L"wpfgfx_cor3.dll" ||
        lower_name == L"presentationnative_v0400.dll" || lower_name == L"presentationnative_cor3.dll") {
        return kMatchWinForms;
    }
    if (lower_name == L"microsoft.ui.xaml.dll" || lower_name == L"windows.ui.xaml.dll" ||
        lower_name == L"winrt.runtime.dll" || lower_name == L"windows.storage.pickers.dll" ||
        lower_name == L"microsoft.windows.storage.pickers.dll") {
        return kMatchWinRt;
    }
    if (lower_name == L"qwindows.dll" || lower_name == L"qt5widgets.dll" || lower_name == L"qt6widgets.dll" ||
        lower_name == L"qt5gui.dll" || lower_name == L"qt6gui.dll" || lower_name == L"qtgui4.dll" ||
        lower_name == L"flutter_windows.dll" || lower_name == L"awt.dll" || lower_name == L"glass.dll" ||
        lower_name == L"libcef.dll" || lower_name == L"webview2loader.dll" || lower_name == L"node.dll" ||
        lower_name == L"duilib.dll" || lower_name.starts_with(L"soui") ||
        lower_name == L"_tkinter.pyd" || lower_name.starts_with(L"wxmsw") || lower_name.starts_with(L"libgtk-") ||
        (lower_name.starts_with(L"vcl") && lower_name.ends_with(L".bpl"))) {
        return kMatchDynamicGui;
    }
    return kMatchNone;
}

bool IsExcludedWindowsAppsPackage(const std::wstring& image_path) noexcept {
    const auto p = StripExtendedPrefix(image_path);
    constexpr std::wstring_view kMarker = L"\\WindowsApps\\";
    size_t pos = std::wstring::npos;
    for (size_t i = 0; i + kMarker.size() <= p.size(); ++i) {
        if (_wcsnicmp(p.data() + i, kMarker.data(), kMarker.size()) == 0) {
            pos = i + kMarker.size();
            break;
        }
    }
    if (pos == std::wstring::npos) return false;
    const size_t slash = p.find_first_of(L"\\/", pos);
    const std::wstring_view pkg = (slash == std::wstring::npos)
        ? std::wstring_view(p).substr(pos)
        : std::wstring_view(p).substr(pos, slash - pos);
    static constexpr std::wstring_view kExcludedPkgPrefixes[] = {
        L"Microsoft.WindowsAppRuntime.",
        L"Microsoft.WidgetsPlatformRuntime_",
        L"MicrosoftWindows.Client.",
        L"Microsoft.SecHealthUI_",
        L"Microsoft.WindowsStore_",
        L"Microsoft.DesktopAppInstaller_",
        L"Microsoft.Xbox",
        L"Microsoft.Gaming",
        L"Microsoft.YourPhone_",
        L"Microsoft.VCLibs.",
        L"Microsoft.UI.Xaml.",
        L"Microsoft.NET.",
        L"Microsoft.MicrosoftEdge.",
        L"Microsoft.WindowsTerminal_",
        L"Microsoft.PowerShell_"
    };
    for (auto prefix : kExcludedPkgPrefixes) {
        if (pkg.size() >= prefix.size() && _wcsnicmp(pkg.data(), prefix.data(), prefix.size()) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool IsExcludedExecutable(const std::wstring& image_path,
                          const std::wstring& windows_dir,
                          const std::wstring& install_root) noexcept {
    if (image_path.empty()) return true;
    try {
        if (StartsWithDir(image_path, windows_dir)) return true;
        if (StartsWithDir(image_path, install_root)) return true;
        // Full-trust MSIX desktop apps (e.g. Windows 11 Paint, Notepad, Snipping Tool,
        // Store Win32 apps) live under \WindowsApps\; AppContainer sandboxes are still
        // rejected at token-binding time, while system/runtime/shell packages stay excluded.
        if (IsExcludedWindowsAppsPackage(image_path)) return true;
        if (ContainsCaseInsensitive(image_path, L"\\Windows Defender\\")) return true;

        const auto name = BasenameLower(image_path);
        static constexpr std::wstring_view kExactExcluded[] = {
            // Browsers (already covered by COM zero-injection; heavy multi-process sandboxes)
            L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
            L"vivaldi.exe", L"opera.exe", L"waterfox.exe", L"librewolf.exe",
            L"arc.exe", L"msedgewebview2.exe",
            // Anti-cheat, game overlays, security, consoles, system hosts
            L"beservice.exe", L"bedaisy.exe", L"vgc.exe", L"vgtray.exe",
            L"steamwebhelper.exe", L"epicwebhelper.exe", L"msmpeng.exe",
            L"windowsterminal.exe", L"openconsole.exe", L"conhost.exe",
            L"wt.exe", L"pwsh.exe", L"powershell.exe", L"cmd.exe",
            L"sechealthui.exe", L"winstore.app.exe", L"widgets.exe", L"widgetservice.exe",
            L"gamebar.exe", L"gamebarftserver.exe", L"xboxpcapp.exe",
            L"phoneexperiencehost.exe", L"appinstaller.exe", L"winget.exe",
            L"backgroundtaskhost.exe", L"runtimebroker.exe",
            // Pulse's own binaries (defense in depth)
            L"pulse.exe", L"pulse.ai.exe", L"pulse.content.exe", L"pulse.document.exe",
            L"pulse.index.exe", L"pulse.preview.exe", L"pulse_shell.exe", L"pulse_desktop_host.exe",
            L"pulse_elevated.exe", L"pulse_hotkey_broker.exe", L"pulse_integration.exe",
            L"pulsedialogadminauto.exe", L"pulsedialoglauncher.exe", L"pulsedialogautocontrol.exe"
        };
        for (auto ex : kExactExcluded) {
            if (name == ex) return true;
        }
        static constexpr std::wstring_view kPrefixExcluded[] = {
            L"easyanticheat", L"faceit", L"riotclient", L"valorant",
            L"securityhealth", L"antimalware"
        };
        for (auto prefix : kPrefixExcluded) {
            if (name.starts_with(prefix)) return true;
        }
        return false;
    } catch (...) {
        return true;
    }
}

DWORD ScanRemoteProcessForAdaptiveMatch(HANDLE process,
                                        DWORD pid,
                                        WORD expected_machine,
                                        const std::wstring& windows_dir,
                                        const std::wstring& install_root,
                                        uint32_t& match_mask) noexcept {
    match_mask = kMatchNone;
    if (!process || !pid) return ERROR_INVALID_PARAMETER;
    try {
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (unsigned retry = 0; retry < 8; ++retry) {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snapshot != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_BAD_LENGTH) return GetLastError();
            Sleep(retry < 3 ? 5 : 15);
        }
        if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
        struct CloseSnap { HANDLE h; ~CloseSnap() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } snap_guard{snapshot};

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot, &entry)) return GetLastError();

        size_t seen = 0;
        size_t scanned = 0;
        do {
            if (++seen > kMaxModules) break;
            const std::wstring path = entry.szExePath;
            const std::wstring lower_name = BasenameLower(path);
            match_mask |= ClassifyFrameworkModule(lower_name);

            const bool is_main_exe = (seen == 1);
            // Full-trust MSIX desktop apps under \WindowsApps\ (already filtered by
            // IsExcludedWindowsAppsPackage and unrestricted token checks) can inherit
            // stale per-package virtualized HKCU CLSID shadows; attach proactively on
            // startup so COM dialog hooks route directly to the live Pulse runtime.
            if (is_main_exe && ContainsCaseInsensitive(path, L"\\WindowsApps\\")) {
                match_mask |= kMatchWinRt;
                break;
            }
            const bool in_windows = StartsWithDir(path, windows_dir);
            const bool in_install = StartsWithDir(path, install_root);
            const bool scan_pe = !in_install && (!in_windows || IsMfcRedistModule(path, windows_dir));
            if (scan_pe && scanned < kMaxScannedModules) {
                ++scanned;
                match_mask |= ScanRemoteModulePe(process, reinterpret_cast<uintptr_t>(entry.modBaseAddr), expected_machine, is_main_exe);
            }
            if (match_mask & (kMatchLegacyImports | kMatchWinForms | kMatchWinRt | kMatchDynamicGui | kMatchComDialog)) {
                break;
            }
        } while (Module32NextW(snapshot, &entry));

        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}

bool IsNativeFileDialogWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || std::wcscmp(cls, L"#32770") != 0) return false;

    struct Probe {
        bool has_modern_view = false;
        bool has_browse_tree = false;
        bool has_ok_button = false;
        bool has_classic_list = false;
        bool has_classic_file_box = false;
        unsigned visited = 0;
    } probe{};

    EnumChildWindows(hwnd, [](HWND child, LPARAM raw) -> BOOL {
        auto& p = *reinterpret_cast<Probe*>(raw);
        if (++p.visited > 160) return FALSE;
        wchar_t name[64]{};
        if (!GetClassNameW(child, name, ARRAYSIZE(name))) return TRUE;
        const int id = GetDlgCtrlID(child);
        if (std::wcscmp(name, L"DUIViewWndClassName") == 0 ||
            std::wcscmp(name, L"SHELLDLL_DefView") == 0) {
            p.has_modern_view = true;
            return FALSE;
        }
        if (std::wcscmp(name, L"SysTreeView32") == 0 && (id == 0x3741 || id == 100 || id == 0x0064)) {
            p.has_browse_tree = true;
        }
        if (id == IDOK) p.has_ok_button = true;
        if (id == 0x0460 || id == 0x0461) p.has_classic_list = true; // lst1 / lst2
        if (id == 0x047c || id == 0x0480) p.has_classic_file_box = true; // cmb13 / edt1
        return TRUE;
    }, reinterpret_cast<LPARAM>(&probe));

    if (probe.has_modern_view) return true;
    if (probe.has_browse_tree && probe.has_ok_button) return true;
    if (probe.has_classic_list && probe.has_classic_file_box) return true;
    return false;
}

bool IsCrashExitCode(DWORD exit_code) noexcept {
    if (exit_code == 0 || exit_code == STILL_ACTIVE) return false;
    // Standard Windows NTSTATUS fatal error codes (0xC000xxxx) and breakpoint/guard exceptions.
    return (exit_code & 0xC0000000u) == 0xC0000000u ||
           exit_code == 0x80000003u ||
           exit_code == 0x80000004u;
}

std::string ComputeBinaryFingerprint(const std::wstring& image_path) noexcept {
    if (image_path.empty()) return {};
    try {
        HANDLE file = CreateFileW(image_path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION info{};
        const BOOL ok = GetFileInformationByHandle(file, &info);
        CloseHandle(file);
        if (!ok) return {};

        const uint64_t path_hash = LowerPathHash(image_path);
        const uint64_t file_size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const uint64_t write_ft = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%016llx-%llx-%llx",
            static_cast<unsigned long long>(path_hash),
            static_cast<unsigned long long>(file_size),
            static_cast<unsigned long long>(write_ft));
        return buf;
    } catch (...) {
        return {};
    }
}

std::string ComputeBinaryFingerprintV2(const std::wstring& image_path) noexcept {
    if (image_path.empty()) return {};
    try {
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, image_path.c_str(), &ignored);
        if (size && size <= 4u * 1024u * 1024u) {
            std::vector<BYTE> data(size);
            if (GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, image_path.c_str(), 0, size, data.data())) {
                VS_FIXEDFILEINFO* fixed = nullptr;
                UINT length = 0;
                if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) &&
                    fixed && length >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == VS_FFI_SIGNATURE) {
                    const uint64_t file_version = (static_cast<uint64_t>(fixed->dwFileVersionMS) << 32) | fixed->dwFileVersionLS;
                    const uint64_t product_version = (static_cast<uint64_t>(fixed->dwProductVersionMS) << 32) | fixed->dwProductVersionLS;
                    if (file_version || product_version) {
                        char buf[80]{};
                        std::snprintf(buf, sizeof(buf), "v2-%016llx-%016llx-%016llx",
                            static_cast<unsigned long long>(LowerPathHash(image_path)),
                            static_cast<unsigned long long>(file_version),
                            static_cast<unsigned long long>(product_version));
                        return buf;
                    }
                }
            }
        }
        return ComputeBinaryFingerprint(image_path);
    } catch (...) {
        return {};
    }
}

AdaptiveTargetStore::FileStamp AdaptiveTargetStore::Stamp(const std::filesystem::path& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return {};
    return {(static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
        (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow, true};
}

bool AdaptiveTargetStore::ReloadIfChanged(const std::filesystem::path& path) noexcept {
    const FileStamp now = Stamp(path);
    if (stamped_ && now == stamp_) return false;
    (void)Load(path);
    return true;
}

bool AdaptiveTargetStore::Load(const std::filesystem::path& path) noexcept {
    // Taken before reading: a write that lands during the read is seen next time.
    stamp_ = Stamp(path); stamped_ = true;
    try {
        learned_.clear();
        failures_.clear();
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (content.size() > 512u * 1024u) return false;
        pulse::json::Value root;
        if (!pulse::json::Parse(content, root) || !root.is_object()) return false;

        if (const auto* learned = root.find("learned"); learned && learned->is_object()) {
            for (size_t i = 0; i < learned->size() && learned_.size() < kMaxEntries; ++i) {
                const auto& key = learned->keys()[i];
                const auto& entry = learned->at(i);
                if (!entry.is_object() || key.empty()) continue;
                LearnedTarget item;
                item.basename = entry.get_string("basename");
                if (!ReadExactFiletime(entry, item.updated_ft)) {
                    int64_t ts = 0;
                    if (const auto* ft = entry.find("updated_ft"); ft && ft->get_int(ts, 0)) {
                        item.updated_ft = static_cast<uint64_t>(ts); // Legacy: eviction order only.
                    }
                }
                learned_[key] = std::move(item);
            }
        }
        if (const auto* failures = root.find("failures"); failures && failures->is_object()) {
            for (size_t i = 0; i < failures->size() && failures_.size() < kMaxEntries; ++i) {
                const auto& key = failures->keys()[i];
                const auto& entry = failures->at(i);
                if (!entry.is_object() || key.empty()) continue;
                BlockedTarget item;
                item.basename = entry.get_string("basename");
                int64_t count = 0;
                if (const auto* c = entry.find("failures"); c && c->get_int(count, 0, 1000)) {
                    item.failures = static_cast<uint32_t>(count);
                }
                item.blocked = entry.get_bool("blocked", item.failures >= kFailureThreshold);
                // Version-1 files stored a 53-bit-masked number, which is not a real
                // FILETIME. Without an exact time a block restarts its retry window now.
                if (!ReadExactFiletime(entry, item.updated_ft)) item.updated_ft = CurrentFiletime();
                failures_[key] = std::move(item);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool AdaptiveTargetStore::Save(const std::filesystem::path& path) const noexcept {
    try {
        pulse::json::Value root = pulse::json::Value::Object();
        root.set("version", 2);

        pulse::json::Value learned_obj = pulse::json::Value::Object();
        for (const auto& [key, item] : learned_) {
            pulse::json::Value entry = pulse::json::Value::Object();
            entry.set("basename", item.basename);
            entry.set("updated_ft", std::to_string(item.updated_ft));
            learned_obj.set(key, std::move(entry));
        }
        root.set("learned", std::move(learned_obj));

        pulse::json::Value failures_obj = pulse::json::Value::Object();
        for (const auto& [key, item] : failures_) {
            pulse::json::Value entry = pulse::json::Value::Object();
            entry.set("basename", item.basename);
            entry.set("failures", static_cast<int64_t>(item.failures));
            entry.set("blocked", item.blocked);
            entry.set("updated_ft", std::to_string(item.updated_ft));
            failures_obj.set(key, std::move(entry));
        }
        root.set("failures", std::move(failures_obj));

        const std::string text = pulse::json::Serialize(root);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto tmp = path;
        tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        {
            std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!output) return false;
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        // Our own write is not an external change.
        stamp_ = Stamp(path); stamped_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool AdaptiveTargetStore::IsLearned(const std::string& fingerprint) const noexcept {
    return !fingerprint.empty() && learned_.find(fingerprint) != learned_.end();
}

bool AdaptiveTargetStore::IsBlocked(const std::string& fingerprint) const noexcept {
    return IsBlockedAt(fingerprint, CurrentFiletime());
}

bool AdaptiveTargetStore::IsBlockedAt(const std::string& fingerprint, uint64_t now) const noexcept {
    if (fingerprint.empty()) return false;
    const auto it = failures_.find(fingerprint);
    if (it == failures_.end() || !it->second.blocked) return false;
    // A clock moved backwards keeps the block; only an elapsed window expires it.
    return now < it->second.updated_ft || now - it->second.updated_ft < kBlockedRetryAfter;
}

bool AdaptiveTargetStore::AdoptLegacyLearned(const std::string& fingerprint, const std::string& legacy) noexcept {
    if (fingerprint.empty() || legacy.empty() || fingerprint == legacy || IsBlocked(fingerprint)) return false;
    try {
        const auto old = learned_.find(legacy);
        if (old == learned_.end() || learned_.find(fingerprint) != learned_.end()) return false;
        LearnedTarget moved = std::move(old->second);
        learned_.erase(old);
        learned_[fingerprint] = std::move(moved);
        return true;
    } catch (...) {
        return false;
    }
}

bool AdaptiveTargetStore::RecordLearned(const std::string& fingerprint, std::wstring_view basename) noexcept {
    if (fingerprint.empty() || IsBlocked(fingerprint)) return false;
    try {
        const bool existed = learned_.find(fingerprint) != learned_.end();
        if (!existed && learned_.size() >= kMaxEntries) {
            auto oldest = std::min_element(learned_.begin(), learned_.end(),
                [](const auto& a, const auto& b) { return a.second.updated_ft < b.second.updated_ft; });
            if (oldest != learned_.end()) learned_.erase(oldest);
        }
        learned_[fingerprint] = LearnedTarget{pulse::json::Utf8(basename), CurrentFiletime()};
        return !existed;
    } catch (...) {
        return false;
    }
}

bool AdaptiveTargetStore::RecordFailure(const std::string& fingerprint, std::wstring_view basename, bool immediate_block) noexcept {
    if (fingerprint.empty()) return false;
    try {
        auto it = failures_.find(fingerprint);
        if (it == failures_.end() && failures_.size() >= kMaxEntries) {
            auto oldest = std::min_element(failures_.begin(), failures_.end(),
                [](const auto& a, const auto& b) { return a.second.updated_ft < b.second.updated_ft; });
            if (oldest != failures_.end()) failures_.erase(oldest);
        }
        auto& entry = failures_[fingerprint];
        const uint64_t now = CurrentFiletime();
        if (entry.blocked && !IsBlockedAt(fingerprint, now)) {
            // Expired breaker: the retry starts a fresh failure window.
            entry.blocked = false;
            entry.failures = 0;
        }
        entry.basename = pulse::json::Utf8(basename);
        entry.failures = std::min<uint32_t>(entry.failures + 1u, 1000u);
        entry.updated_ft = now;
        if (immediate_block || entry.failures >= kFailureThreshold) {
            entry.blocked = true;
            learned_.erase(fingerprint);
        }
        return entry.blocked;
    } catch (...) {
        return false;
    }
}

void AdaptiveTargetStore::ClearFailureCount(const std::string& fingerprint) noexcept {
    if (fingerprint.empty()) return;
    const auto it = failures_.find(fingerprint);
    if (it != failures_.end() && !it->second.blocked) {
        failures_.erase(it);
    }
}

void AdaptiveTargetStore::ClearBlocked() noexcept {
    failures_.clear();
}

std::vector<PausedTarget> AdaptiveTargetStore::PausedTargets(uint64_t now) const {
    std::vector<PausedTarget> paused;
    for (const auto& [key, entry] : failures_) {
        if (!IsBlockedAt(key, now)) continue;
        PausedTarget item;
        item.fingerprint = key; item.basename = entry.basename; item.failures = entry.failures;
        item.paused_ft = entry.updated_ft;
        item.retry_ft = entry.updated_ft > UINT64_MAX - kBlockedRetryAfter ? UINT64_MAX : entry.updated_ft + kBlockedRetryAfter;
        paused.push_back(std::move(item));
    }
    std::sort(paused.begin(), paused.end(), [](const PausedTarget& a, const PausedTarget& b) {
        return a.paused_ft != b.paused_ft ? a.paused_ft > b.paused_ft : a.fingerprint < b.fingerprint;
    });
    return paused;
}

size_t AdaptiveTargetStore::ResumeBasename(std::string_view basename) noexcept {
    if (basename.empty()) return 0;
    try {
        // Names are stored as UTF-8: compare as Unicode, ignoring case like the file system.
        const std::wstring wanted = pulse::json::Wide(std::string(basename));
        return static_cast<size_t>(std::erase_if(failures_, [&](const auto& item) {
            return item.second.blocked &&
                CompareStringOrdinal(pulse::json::Wide(item.second.basename).c_str(), -1, wanted.c_str(), -1, TRUE) == CSTR_EQUAL;
        }));
    } catch (...) {
        return 0;
    }
}

size_t AdaptiveTargetStore::BlockedCount() const noexcept {
    size_t count = 0;
    for (const auto& [_, entry] : failures_) {
        if (entry.blocked) ++count;
    }
    return count;
}

} // namespace pulse::dialog_adaptive
