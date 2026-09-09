// shell_verbs.cpp — See shell_verbs.h for the contract.
#include "shell_verbs.h"

#include "../ipc/ctx_menu_util.h"

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
bool IsSkippedStaticVerb(const std::wstring& lower, bool background) {
    if (lower == L"opennew" || lower == L"printto" || lower == L"find") return true;
    return ipc::IsBuiltinContextVerb(lower, background);
}

std::wstring KnownVerbDisplay(const std::wstring& lower) {
    if (lower == L"edit") return L"编辑";
    if (lower == L"print") return L"打印";
    if (lower == L"preview") return L"预览";
    if (lower == L"runas") return L"以管理员身份运行";
    return {};
}

bool FillStaticVerbFromKey(HKEY root, const std::wstring& verb_key, const std::wstring& verb,
                           bool allow_cascade, bool background, StaticVerb& out);
void CollectCascadeChildren(HKEY parent_root, const std::wstring& subcommands,
                            const std::wstring& ext_key, bool background,
                            std::vector<StaticVerb>& children);
void CollectVerbsFrom(HKEY root, const std::wstring& shell_key, std::vector<StaticVerb>& out,
                      bool allow_cascade, bool background,
                      std::unordered_set<std::wstring>* seen);

bool FillStaticVerbFromKey(HKEY root, const std::wstring& verb_key, const std::wstring& verb,
                           bool allow_cascade, bool background, StaticVerb& out) {
    const std::wstring lower = ToLower(verb);
    if (IsSkippedStaticVerb(lower, background)) return false;

    ipc::StaticVerbRegFlags flags;
    flags.legacy_disable = RegValueExists(root, verb_key, L"LegacyDisable");
    flags.programmatic = RegValueExists(root, verb_key, L"ProgrammaticAccessOnly");
    flags.extended = RegValueExists(root, verb_key, L"Extended");
    const std::wstring command = RegReadString(root, verb_key + L"\\command", nullptr);
    flags.has_command = !command.empty();
    flags.has_delegate_execute =
        !RegReadString(root, verb_key + L"\\command", L"DelegateExecute").empty();
    flags.has_explorer_command =
        !RegReadString(root, verb_key, L"ExplorerCommandHandler").empty();
    const std::wstring subcommands = RegReadString(root, verb_key, L"SubCommands");
    const std::wstring ext_key = RegReadString(root, verb_key, L"ExtendedSubCommandsKey");
    flags.has_subcommands = !subcommands.empty();
    flags.has_extended_subcommands_key = !ext_key.empty();
    if (!ipc::KeepStaticVerb(flags)) return false;

    std::wstring display = ResolveIndirect(RegReadString(root, verb_key, L"MUIVerb"));
    if (display.empty()) display = RegReadString(root, verb_key, nullptr);
    if (!display.empty() && display[0] == L'@') display = ResolveIndirect(display);
    if (display.empty()) display = KnownVerbDisplay(lower);
    if (display.empty()) display = verb;

    out.verb = verb;
    out.display = StripMnemonics(display);
    out.app_path.clear();
    out.command = command;
    out.children.clear();
    if (allow_cascade)
        CollectCascadeChildren(root, subcommands, ext_key, background, out.children);
    if ((flags.has_subcommands || flags.has_extended_subcommands_key) &&
        out.children.empty() && !flags.has_command)
        return false;
    return true;
}

void CollectCascadeChildren(HKEY parent_root, const std::wstring& subcommands,
                            const std::wstring& ext_key, bool background,
                            std::vector<StaticVerb>& children) {
    if (!subcommands.empty()) {
        std::wstring cur;
        auto flush = [&] {
            if (cur.empty()) return;
            StaticVerb child;
            const std::wstring store =
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\" +
                cur;
            if (FillStaticVerbFromKey(HKEY_LOCAL_MACHINE, store, cur, false, background, child) ||
                FillStaticVerbFromKey(HKEY_CURRENT_USER, store, cur, false, background, child))
                children.push_back(std::move(child));
            cur.clear();
        };
        for (wchar_t c : subcommands) {
            if (c == L' ' || c == L';' || c == L'\t') flush();
            else cur += c;
        }
        flush();
    }
    if (!ext_key.empty()) {
        std::wstring shell_key = ext_key;
        while (!shell_key.empty() && (shell_key.back() == L'\\' || shell_key.back() == L'/'))
            shell_key.pop_back();
        const std::wstring lower = ToLower(shell_key);
        if (lower.size() < 6 || lower.compare(lower.size() - 6, 6, L"\\shell") != 0)
            shell_key += L"\\shell";
        CollectVerbsFrom(parent_root, shell_key, children, false, background, nullptr);
    }
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [](const StaticVerb& child) {
                                      return child.command.empty() && child.app_path.empty();
                                  }),
                   children.end());
}

void CollectVerbsFrom(HKEY root, const std::wstring& shell_key, std::vector<StaticVerb>& out,
                      bool allow_cascade, bool background,
                      std::unordered_set<std::wstring>* seen) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, shell_key.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD name_len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        const std::wstring verb = name;
        const std::wstring lower = ToLower(verb);
        if (seen && !seen->insert(lower).second) continue;
        StaticVerb v;
        if (!FillStaticVerbFromKey(root, shell_key + L"\\" + verb, verb, allow_cascade,
                                   background, v)) {
            if (seen) seen->erase(lower);
            continue;
        }
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
    if (ipc::IsLocationVerbKey(ext)) {
        std::unordered_set<std::wstring> seen;
        const bool background = ext == ipc::kBackgroundVerbKey;
        if (background) {
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell", out, true,
                             true, &seen);
        } else {
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Directory\\shell", out, true, false, &seen);
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Folder\\shell", out, true, false, &seen);
            CollectVerbsFrom(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shell", out, true,
                             false, &seen);
            if (ext == ipc::kDriveVerbKey)
                CollectVerbsFrom(HKEY_CLASSES_ROOT, L"Drive\\shell", out, true, false, &seen);
        }
        return DedupeStaticVerbs(std::move(out), {}, 48);
    }
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
        CollectVerbsFrom(HKEY_CLASSES_ROOT, progid + L"\\shell", out, true, false, nullptr);
    }
    CollectVerbsFrom(HKEY_CLASSES_ROOT,
                     L"SystemFileAssociations\\" + ext_lower + L"\\shell", out, true, false,
                     nullptr);

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

namespace {

void PutU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4);
}
void PutW(std::vector<uint8_t>& buf, const std::wstring& s) {
    PutU32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data() + s.size()));
}
bool GetU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (p + 4 > end) return false;
    memcpy(&v, p, 4);
    p += 4;
    return true;
}
bool GetW(const uint8_t*& p, const uint8_t* end, std::wstring& s) {
    uint32_t n = 0;
    if (!GetU32(p, end, n) || n > 4096) return false;
    if (p + n * 2 > end) return false;
    s.assign(reinterpret_cast<const wchar_t*>(p), n);
    p += n * 2;
    return true;
}

constexpr uint32_t kStaticVerbCacheVersion = 2;

void PutVerb(std::vector<uint8_t>& buf, const StaticVerb& v, int depth) {
    PutW(buf, v.verb);
    PutW(buf, v.display);
    PutW(buf, v.app_path);
    PutW(buf, v.command);
    const uint32_t n = depth > 0 ? 0 : static_cast<uint32_t>(
        (std::min)(v.children.size(), static_cast<size_t>(ipc::kMaxSubmenuChildren)));
    PutU32(buf, n);
    for (uint32_t i = 0; i < n; ++i) PutVerb(buf, v.children[i], depth + 1);
}

bool GetVerb(const uint8_t*& p, const uint8_t* end, StaticVerb& v, int depth) {
    if (!GetW(p, end, v.verb) || !GetW(p, end, v.display) || !GetW(p, end, v.app_path) ||
        !GetW(p, end, v.command))
        return false;
    uint32_t n = 0;
    if (!GetU32(p, end, n) || n > 16 || (depth > 0 && n != 0)) return false;
    v.children.clear();
    v.children.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        StaticVerb child;
        if (!GetVerb(p, end, child, depth + 1)) return false;
        v.children.push_back(std::move(child));
    }
    return true;
}

} // namespace

std::wstring MachineStaticVerbCachePath() {
    wchar_t dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, dir))) return {};
    std::wstring path = std::wstring(dir) + L"\\Pulse";
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\shell-verbs.bin";
}

bool LoadMachineStaticVerbCache(std::unordered_map<std::wstring, std::vector<StaticVerb>>& out) {
    out.clear();
    const std::wstring path = MachineStaticVerbCachePath();
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 16 || sz.QuadPart > 32 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    const bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr) &&
        r == buf.size();
    CloseHandle(h);
    if (!ok) return false;
    const uint8_t* p = buf.data();
    const uint8_t* end = p + buf.size();
    if (p + 8 > end || memcmp(p, "PSVC", 4) != 0) return false;
    p += 4;
    uint32_t ver = 0, count = 0;
    if (!GetU32(p, end, ver) || ver != kStaticVerbCacheVersion || !GetU32(p, end, count) ||
        count > 8000)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::wstring ext;
        uint32_t nverb = 0;
        if (!GetW(p, end, ext) || !GetU32(p, end, nverb) || nverb > 64) return false;
        std::vector<StaticVerb> verbs;
        verbs.reserve(nverb);
        for (uint32_t k = 0; k < nverb; ++k) {
            StaticVerb v;
            if (!GetVerb(p, end, v, 0)) return false;
            verbs.push_back(std::move(v));
        }
        if (!ext.empty()) out.emplace(std::move(ext), std::move(verbs));
    }
    return true;
}

bool SaveMachineStaticVerbCache(
    const std::unordered_map<std::wstring, std::vector<StaticVerb>>& cache) {
    const std::wstring path = MachineStaticVerbCachePath();
    if (path.empty()) return false;
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), {'P', 'S', 'V', 'C'});
    PutU32(buf, kStaticVerbCacheVersion);
    PutU32(buf, static_cast<uint32_t>(cache.size()));
    for (const auto& [ext, verbs] : cache) {
        PutW(buf, ext);
        PutU32(buf, static_cast<uint32_t>(verbs.size()));
        for (const auto& v : verbs) PutVerb(buf, v, 0);
    }
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &w, nullptr) &&
        w == buf.size();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool SeedMachineStaticVerbCache() {
    std::unordered_map<std::wstring, std::vector<StaticVerb>> cache;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < 8000; ++i) {
        wchar_t name[128];
        DWORD name_len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        if (name_len < 2 || name[0] != L'.') continue;
        std::wstring ext = ToLower(name);
        auto verbs = EnumerateStaticVerbs(ext);
        if (!verbs.empty()) cache.emplace(std::move(ext), std::move(verbs));
    }
    RegCloseKey(key);
    return SaveMachineStaticVerbCache(cache);
}

} // namespace pulse::app
