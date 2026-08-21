// app_prefs.cpp — Persist general settings; sync 开机自启 with the Run key.
#include "app_prefs.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace pulse::app {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Pulse";

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? std::wstring(path, n) : L"";
}

} // namespace

namespace {

void DeleteStoredWallpapers(const std::wstring& dir) {
    static constexpr const wchar_t* kExt[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".jfif", L".img"
    };
    for (const wchar_t* ext : kExt)
        DeleteFileW((dir + L"\\wallpaper" + ext).c_str());
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

void AppPrefs::ResetToDefaults() {
    launch_on_startup = false;
    keep_running_on_close = false;
    open_folders_in_pulse = false;
    window_effect = L"mica-alt";
    background_image.clear();
    row_height = 34;
    tray_icon_size = 48;
    accent_rgb.clear();
}

std::wstring AppPrefs::ToJson() const {
    std::wstring escaped_effect;
    std::wstring escaped_image;
    pulse::json::Escape(window_effect, escaped_effect);
    pulse::json::Escape(background_image, escaped_image);
    std::wstring out = L"{\n  \"version\":2,\n  \"launch_on_startup\":";
    out += launch_on_startup ? L"true" : L"false";
    out += L",\n  \"keep_running_on_close\":";
    out += keep_running_on_close ? L"true" : L"false";
    out += L",\n  \"open_folders_in_pulse\":";
    out += open_folders_in_pulse ? L"true" : L"false";
    out += L",\n  \"window_effect\":\"";
    out += escaped_effect;
    out += L"\",\n  \"background_image\":\"";
    out += escaped_image;
    out += L"\",\n  \"row_height\":";
    out += std::to_wstring(row_height);
    out += L",\n  \"tray_icon_size\":";
    out += std::to_wstring(tray_icon_size);
    out += L",\n  \"accent_rgb\":\"";
    {
        std::wstring escaped_accent;
        pulse::json::Escape(accent_rgb, escaped_accent);
        out += escaped_accent;
    }
    out += L"\",\n  \"custom_tag_colors\":[";
    for (size_t i = 0; i < custom_tag_colors.size(); ++i) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", custom_tag_colors[i] & 0x00FFFFFFu);
        if (i > 0) out += L",";
        out += L"\"";
        out += hex;
        out += L"\"";
    }
    out += L"]\n}\n";
    return out;
}

bool AppPrefs::FromJson(const std::wstring& json) {
    if (json.empty()) return false;
    launch_on_startup = pulse::json::ExtractBool(json, L"launch_on_startup", false);
    keep_running_on_close = pulse::json::ExtractBool(json, L"keep_running_on_close", false);
    open_folders_in_pulse = pulse::json::ExtractBool(json, L"open_folders_in_pulse", false);
    window_effect = pulse::json::ExtractString(json, L"window_effect", L"mica-alt");
    if (window_effect == L"dwm-blur") window_effect = L"acrylic-material";
    else if (window_effect.empty()) window_effect = L"mica-alt";
    background_image = pulse::json::ExtractString(json, L"background_image");
    row_height = pulse::json::ExtractInt(json, L"row_height", 34);
    if (row_height < 24 || row_height > 48) row_height = 34;
    tray_icon_size = pulse::json::ExtractInt(json, L"tray_icon_size", 48);
    if (tray_icon_size < 32 || tray_icon_size > 64) tray_icon_size = 48;
    accent_rgb = pulse::json::ExtractString(json, L"accent_rgb");
    uint32_t accent_parsed = 0;
    if (!accent_rgb.empty() && ParseAccentRgb(accent_rgb, accent_parsed)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", accent_parsed);
        accent_rgb = hex;
    } else {
        accent_rgb.clear();
    }
    custom_tag_colors.clear();
    for (const std::wstring& entry :
         pulse::json::ExtractStringArray(json, L"custom_tag_colors")) {
        const wchar_t* text = entry.c_str();
        if (*text == L'#') ++text;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(text, &end, 16);
        if (end && *end == L'\0' && v <= 0xFFFFFFul && wcslen(text) == 6)
            custom_tag_colors.push_back(static_cast<uint32_t>(v));
    }
    return true;
}

bool AppPrefs::StoreBackgroundImage(const std::wstring& source_path) {
    if (source_path.empty()) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        background_image = source_path;
        return true;
    }
    const wchar_t* ext = PathFindExtensionW(source_path.c_str());
    std::wstring dest = dir + L"\\wallpaper";
    dest += (ext && ext[0]) ? ext : L".img";
    if (!SamePath(source_path, dest)) {
        DeleteStoredWallpapers(dir);
        if (!CopyFileW(source_path.c_str(), dest.c_str(), FALSE)) {
            background_image = source_path;
            return true;
        }
    }
    background_image = dest;
    return true;
}

void AppPrefs::ClearBackgroundImage() {
    const std::wstring dir = GetPulseDataDir();
    if (!dir.empty() && !background_image.empty()) {
        const std::wstring prefix = dir + L"\\wallpaper";
        if (background_image.size() >= prefix.size() &&
            CompareStringOrdinal(background_image.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
            DeleteFileW(background_image.c_str());
        }
    }
    background_image.clear();
}

bool AppPrefs::ReadLaunchOnStartup() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[MAX_PATH] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    return value[0] != 0;
}

bool AppPrefs::ApplyLaunchOnStartup(bool on) {
    launch_on_startup = on;
    if (!persist) return true;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG st = ERROR_SUCCESS;
    if (on) {
        const std::wstring exe = ExePath();
        if (exe.empty()) { RegCloseKey(key); return false; }
        const std::wstring cmd = L"\"" + exe + L"\"";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

std::wstring FolderOpenCommandLine(const std::wstring& exe) {
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" \"%1\"";
}

bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe) {
    if (command.empty() || exe.empty()) return false;
    size_t i = 0;
    while (i < command.size() && iswspace(command[i])) ++i;
    std::wstring token;
    if (i < command.size() && command[i] == L'"') {
        ++i;
        const size_t start = i;
        while (i < command.size() && command[i] != L'"') ++i;
        token = command.substr(start, i - start);
    } else {
        const size_t start = i;
        while (i < command.size() && !iswspace(command[i])) ++i;
        token = command.substr(start, i - start);
    }
    return !token.empty() &&
           CompareStringOrdinal(token.c_str(), -1, exe.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {

constexpr const wchar_t* kFolderOpenClasses[] = { L"Directory", L"Drive" };

std::wstring FolderOpenKey(const wchar_t* cls) {
    return std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\open";
}

std::wstring FolderShellKey(const wchar_t* cls) {
    return std::wstring(L"Software\\Classes\\") + cls + L"\\shell";
}

std::wstring ReadRegDefault(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return {};
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LONG st = RegQueryValueExW(h, nullptr, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(h);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    return value;
}

bool WriteFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring open = FolderOpenKey(cls);
    const std::wstring command = open + L"\\command";
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, command.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring line = FolderOpenCommandLine(exe);
    const LONG st = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(line.c_str()),
                                   static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    if (st != ERROR_SUCCESS) return false;
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, open.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t empty[] = L"";
    const LONG de = RegSetValueExW(h, L"DelegateExecute", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    RegCloseKey(h);
    if (de != ERROR_SUCCESS) return false;

    // HKLM Directory/Drive shell default is "none", so double-click never uses
    // the open verb and falls through to Folder → Explorer. Point HKCU at open.
    h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FolderShellKey(cls).c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t open_verb[] = L"open";
    const LONG def = RegSetValueExW(h, nullptr, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(open_verb),
                                    sizeof(open_verb));
    RegCloseKey(h);
    return def == ERROR_SUCCESS;
}

bool ClearFolderOpenClass(const wchar_t* cls, const std::wstring& exe) {
    const std::wstring command = ReadRegDefault(FolderOpenKey(cls) + L"\\command");
    if (!command.empty() && !FolderOpenCommandIsOurs(command, exe)) return true;
    SHDeleteKeyW(HKEY_CURRENT_USER, FolderOpenKey(cls).c_str());
    const std::wstring shell = FolderShellKey(cls);
    if (_wcsicmp(ReadRegDefault(shell).c_str(), L"open") == 0) {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, shell.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, nullptr);
            RegCloseKey(h);
        }
    }
    return true;
}

void NotifyAssocChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace

bool AppPrefs::ReadFolderOpen() const {
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    const std::wstring command =
        ReadRegDefault(FolderOpenKey(L"Directory") + L"\\command");
    return FolderOpenCommandIsOurs(command, exe);
}

bool AppPrefs::ApplyFolderOpen(bool on) {
    open_folders_in_pulse = on;
    if (!persist) return true;
    const std::wstring exe = ExePath();
    if (exe.empty()) return false;
    bool ok = true;
    for (const wchar_t* cls : kFolderOpenClasses) {
        if (on) ok = WriteFolderOpenClass(cls, exe) && ok;
        else ok = ClearFolderOpenClass(cls, exe) && ok;
    }
    NotifyAssocChanged();
    return ok;
}

bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept {
    const wchar_t* p = text.c_str();
    if (!p || !*p) return false;
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!iswxdigit(p[i])) return false;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(p, &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFFFFul) return false;
    rgb = static_cast<uint32_t>(v);
    return true;
}

bool AppPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        launch_on_startup = ReadLaunchOnStartup();
        open_folders_in_pulse = ReadFolderOpen();
        return false;
    }
    std::wstring json;
    if (ReadUtf8File(dir + L"\\app.json", json) && !json.empty())
        FromJson(json);
    launch_on_startup = ReadLaunchOnStartup();
    open_folders_in_pulse = ReadFolderOpen();
    // Repair older installs that wrote open\command but left shell default as none.
    if (persist && open_folders_in_pulse)
        ApplyFolderOpen(true);
    return true;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\app.json", ToJson());
}

} // namespace pulse::app
