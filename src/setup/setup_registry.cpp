#include "setup_registry.h"

#include "setup_common.h"

namespace pulse::setup {

std::optional<std::wstring> ReadString(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &h) != ERROR_SUCCESS) return std::nullopt;
    DWORD type = 0, size = 0;
    std::optional<std::wstring> out;
    if (RegQueryValueExW(h, value, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::wstring data(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(h, value, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size) ==
            ERROR_SUCCESS) {
            data.resize(size / sizeof(wchar_t));
            while (!data.empty() && data.back() == L'\0') data.pop_back();
            out = data;
        }
    }
    RegCloseKey(h);
    return out;
}

bool WriteString(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value, const std::wstring& data) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS s = RegSetValueExW(h, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                                     static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return s == ERROR_SUCCESS;
}

bool DeleteValue(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE | view, &h) != ERROR_SUCCESS) return true;
    const LSTATUS s = RegDeleteValueW(h, value);
    RegCloseKey(h);
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}

std::optional<PreviousInstall> FindPreviousInstall(const wchar_t* key) {
    const RegLocation order[] = {{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
                                 {HKEY_CURRENT_USER, KEY_WOW64_64KEY}, {HKEY_CURRENT_USER, KEY_WOW64_32KEY}};
    for (const auto& where : order) {
        auto dir = ReadString(where.root, where.view, key, L"Inno Setup: App Path");
        if (!dir || NormalizeDir(*dir).empty()) dir = ReadString(where.root, where.view, key, L"InstallLocation");
        if (!dir || NormalizeDir(*dir).empty()) continue;
        PreviousInstall p;
        p.where = where;
        p.dir = NormalizeDir(*dir);
        p.uninstall_string = ReadString(where.root, where.view, key, L"UninstallString").value_or(L"");
        p.inno = !ReadString(where.root, where.view, key, L"Pulse Setup: Version").has_value();
        if (auto t = ReadString(where.root, where.view, key, L"Inno Setup: Selected Tasks")) {
            Tasks tasks;
            ApplyRememberedTaskList(*t, tasks);
            p.tasks = tasks;
        }
        return p;
    }
    return std::nullopt;
}

bool WriteUninstallEntry(const RegLocation& where, const wchar_t* key, const UninstallEntry& e, std::wstring& error) {
    HKEY h = nullptr;
    LSTATUS s = RegCreateKeyExW(where.root, key, 0, nullptr, 0, KEY_SET_VALUE | where.view, nullptr, &h, nullptr);
    if (s != ERROR_SUCCESS) { error = L"Cannot create uninstall key: " + Win32Error(s); return false; }
    bool ok = true;
    auto sz = [&](const wchar_t* name, const std::wstring& v) {
        ok &= RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                             static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    auto dw = [&](const wchar_t* name, DWORD v) {
        ok &= RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
    };
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t date[16];
    swprintf_s(date, L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay);
    // "1.0.53" -> 1, 0 (swscanf would pull ~30 KB of CRT input code into the stub)
    unsigned major = 0, minor = 0, *part = &major;
    for (wchar_t c : e.version) {
        if (c == L'.') { if (part == &minor) break; part = &minor; }
        else if (c >= L'0' && c <= L'9') *part = *part * 10 + static_cast<unsigned>(c - L'0');
    }

    sz(L"DisplayName", L"Pulse " + e.version + e.display_suffix);
    sz(L"DisplayVersion", e.version);
    sz(L"DisplayIcon", e.dir + L"\\pulse.exe");
    sz(L"InstallLocation", e.dir + L"\\");
    // Kept under the Inno names: older Pulse installers and the updater read them.
    sz(L"Inno Setup: App Path", e.dir);
    sz(L"Inno Setup: Selected Tasks", e.tasks);
    sz(L"Pulse Setup: Version", e.version);
    sz(L"UninstallString", Quote(e.uninstaller) + e.uninstall_args);
    sz(L"QuietUninstallString", Quote(e.uninstaller) + e.uninstall_args + L" /VERYSILENT");
    sz(L"InstallDate", date);
    dw(L"MajorVersion", major);
    dw(L"MinorVersion", minor);
    dw(L"NoModify", 1);
    dw(L"NoRepair", 1);
    dw(L"EstimatedSize", static_cast<DWORD>(e.estimated_kb));
    // Inno-only values that would now describe a missing unins000.dat.
    for (const wchar_t* stale : {L"Inno Setup: Setup Version", L"Inno Setup: Icon Group", L"Inno Setup: User",
                                 L"Inno Setup: Deselected Tasks", L"Inno Setup: Language"})
        RegDeleteValueW(h, stale);
    RegCloseKey(h);
    if (!ok) error = L"Cannot write uninstall values";
    return ok;
}

}  // namespace pulse::setup
