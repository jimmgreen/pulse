// shell_verbs.cpp — See shell_verbs.h for the contract.
#include "shell_verbs.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace pulse::app {

namespace {

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring RegReadString(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring out(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, out.data(), &size) != ERROR_SUCCESS)
        return {};
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

bool RegValueExists(HKEY root, const std::wstring& subkey, const wchar_t* value) {
    return RegGetValueW(root, subkey.c_str(), value, RRF_RT_ANY, nullptr, nullptr,
                        nullptr) == ERROR_SUCCESS;
}

std::wstring StripMnemonics(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'&') {
            if (i + 1 < in.size() && in[i + 1] == L'&') {
                out += L'&';
                ++i;
            }
            continue;
        }
        out += in[i];
    }
    return out;
}

// "@shell32.dll,-8506" style indirect strings -> localized text.
std::wstring ResolveIndirect(const std::wstring& s) {
    if (s.empty() || s[0] != L'@') return s;
    wchar_t buf[512]{};
    if (SUCCEEDED(SHLoadIndirectString(s.c_str(), buf, ARRAYSIZE(buf), nullptr)) && buf[0])
        return buf;
    return {};
}

// Verbs that duplicate Pulse built-ins or are not user-facing.
bool IsSkippedStaticVerb(const std::wstring& lower) {
    return lower == L"open" || lower == L"opennew" || lower == L"openas" ||
           lower == L"printto" || lower == L"explore" || lower == L"find";
}

std::wstring KnownVerbDisplay(const std::wstring& lower) {
    if (lower == L"edit") return L"编辑";
    if (lower == L"print") return L"打印";
    if (lower == L"preview") return L"预览";
    if (lower == L"runas") return L"以管理员身份运行";
    return {};
}

// Collects HKCR\<key>\shell\* verbs into out.
void CollectVerbsFrom(const std::wstring& shell_key, std::vector<StaticVerb>& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, shell_key.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD name_len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        const std::wstring verb = name;
        const std::wstring lower = ToLower(verb);
        if (IsSkippedStaticVerb(lower)) continue;
        const std::wstring verb_key = shell_key + L"\\" + verb;
        // Hidden / shift-only / programmatic verbs stay hidden in Pulse too.
        if (RegValueExists(HKEY_CLASSES_ROOT, verb_key, L"LegacyDisable") ||
            RegValueExists(HKEY_CLASSES_ROOT, verb_key, L"ProgrammaticAccessOnly") ||
            RegValueExists(HKEY_CLASSES_ROOT, verb_key, L"Extended"))
            continue;
        // Only verbs that can actually execute.
        if (RegReadString(HKEY_CLASSES_ROOT, verb_key + L"\\command", nullptr).empty() &&
            RegReadString(HKEY_CLASSES_ROOT, verb_key + L"\\command", L"DelegateExecute")
                .empty())
            continue;
        std::wstring display = ResolveIndirect(
            RegReadString(HKEY_CLASSES_ROOT, verb_key, L"MUIVerb"));
        if (display.empty())
            display = RegReadString(HKEY_CLASSES_ROOT, verb_key, nullptr);
        if (!display.empty() && display[0] == L'@') display = ResolveIndirect(display);
        if (display.empty()) display = KnownVerbDisplay(lower);
        if (display.empty()) display = verb;
        StaticVerb v;
        v.verb = verb;
        v.display = StripMnemonics(display);
        out.push_back(std::move(v));
    }
    RegCloseKey(key);
}

// First token of a shell command line = executable path.
std::wstring CommandExePath(const std::wstring& command) {
    if (command.empty()) return {};
    std::wstring exe;
    if (command[0] == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return {};
        exe = command.substr(1, end - 1);
    } else {
        exe = command.substr(0, command.find(L' '));
    }
    wchar_t expanded[MAX_PATH]{};
    if (ExpandEnvironmentStringsW(exe.c_str(), expanded, ARRAYSIZE(expanded)) > 0)
        exe = expanded;
    return exe;
}

// FileDescription from the exe's version resource ("AutoCAD Application" …).
std::wstring ExeFriendlyName(const std::wstring& exe_path) {
    const DWORD size = GetFileVersionInfoSizeW(exe_path.c_str(), nullptr);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(exe_path.c_str(), 0, size, data.data())) return {};
    struct LangCp { WORD lang; WORD cp; };
    LangCp* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&translations), &bytes) ||
        bytes < sizeof(LangCp))
        return {};
    wchar_t query[64];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               translations[0].lang, translations[0].cp);
    wchar_t* desc = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&desc), &len) &&
        desc && len > 1)
        return std::wstring(desc);
    return {};
}

std::wstring ExeBaseName(const std::wstring& exe) {
    auto pos = exe.find_last_of(L"\\/");
    std::wstring name = pos == std::wstring::npos ? exe : exe.substr(pos + 1);
    pos = name.find_last_of(L'.');
    if (pos != std::wstring::npos) name.resize(pos);
    return name;
}

// Resolves "acad.exe" from OpenWithList to a launchable path + friendly name.
bool ResolveOpenWithApp(const std::wstring& exe_name, StaticVerb& out) {
    std::wstring path = CommandExePath(RegReadString(
        HKEY_CLASSES_ROOT, L"Applications\\" + exe_name + L"\\shell\\open\\command",
        nullptr));
    if (path.empty()) {
        path = RegReadString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe_name,
            nullptr);
    }
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    std::wstring friendly = ResolveIndirect(RegReadString(
        HKEY_CLASSES_ROOT, L"Applications\\" + exe_name, L"FriendlyAppName"));
    if (friendly.empty()) friendly = ExeFriendlyName(path);
    if (friendly.empty()) friendly = ExeBaseName(exe_name);
    out.display = L"用 " + friendly + L" 打开";
    out.app_path = path;
    return true;
}

void CollectOpenWith(const std::wstring& ext_lower, const std::wstring& default_exe_lower,
                     std::vector<StaticVerb>& out) {
    const std::wstring list_key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
        ext_lower + L"\\OpenWithList";
    const std::wstring mru = RegReadString(HKEY_CURRENT_USER, list_key, L"MRUList");
    size_t added = 0;
    for (wchar_t slot : mru) {
        if (added >= 3) break;
        const wchar_t value_name[2] = { slot, L'\0' };
        const std::wstring exe_name = RegReadString(HKEY_CURRENT_USER, list_key, value_name);
        if (exe_name.empty()) continue;
        StaticVerb v;
        if (!ResolveOpenWithApp(exe_name, v)) continue;
        if (!default_exe_lower.empty() && ToLower(v.app_path) == default_exe_lower)
            continue; // duplicates the built-in 打开 row
        v.verb = L"__openwith";
        out.push_back(std::move(v));
        ++added;
    }
}

} // namespace

std::vector<StaticVerb> EnumerateStaticVerbs(const std::wstring& ext) {
    std::vector<StaticVerb> out;
    if (ext.size() < 2 || ext[0] != L'.') return out;
    const std::wstring ext_lower = ToLower(ext);

    // UserChoice beats the HKCR default progid (matches Explorer).
    std::wstring progid = RegReadString(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
            ext_lower + L"\\UserChoice",
        L"ProgId");
    if (progid.empty())
        progid = RegReadString(HKEY_CLASSES_ROOT, ext_lower, nullptr);

    std::wstring default_exe_lower;
    if (!progid.empty()) {
        default_exe_lower = ToLower(CommandExePath(RegReadString(
            HKEY_CLASSES_ROOT, progid + L"\\shell\\open\\command", nullptr)));
        CollectVerbsFrom(progid + L"\\shell", out);
    }
    CollectVerbsFrom(L"SystemFileAssociations\\" + ext_lower + L"\\shell", out);

    CollectOpenWith(ext_lower, default_exe_lower, out);

    StaticVerb open_as;
    open_as.verb = L"openas";
    open_as.display = L"打开方式…";
    out.push_back(std::move(open_as));
    return out;
}

std::vector<StaticVerb> DedupeStaticVerbs(std::vector<StaticVerb> verbs,
                                          const std::vector<std::wstring>& builtin_texts,
                                          size_t cap) {
    std::vector<StaticVerb> out;
    std::vector<std::wstring> seen;
    for (const auto& b : builtin_texts) seen.push_back(ToLower(b));
    for (auto& v : verbs) {
        if (v.display.empty()) continue;
        const std::wstring key = ToLower(v.display);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(std::move(v));
        if (out.size() >= cap) break;
    }
    return out;
}

} // namespace pulse::app
