#include "cloud_roots.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>

namespace pulse::app {
namespace {

constexpr wchar_t kExplorer[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
constexpr wchar_t kNonEnum[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\NonEnum";

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + L"\\" + b;
}

// REG_SZ / REG_EXPAND_SZ (expanded). Grows the buffer instead of asking for
// the size first, which proved unreliable for some keys.
bool ReadString(HKEY root, const std::wstring& subkey, const wchar_t* value, std::wstring& out) {
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        const LSTATUS status = RegGetValueW(root, subkey.c_str(), value,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer.data(), &bytes);
        if (status == ERROR_MORE_DATA) {
            buffer.assign(bytes / sizeof(wchar_t) + 64, L'\0');
            continue;
        }
        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return false;
        buffer.resize(bytes / sizeof(wchar_t));
        while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
        out = std::move(buffer);
        return !out.empty();
    }
    return false;
}

bool ReadDword(HKEY root, const std::wstring& subkey, const wchar_t* value, DWORD& out) {
    DWORD bytes = sizeof(out);
    return RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_DWORD, nullptr, &out, &bytes)
        == ERROR_SUCCESS;
}

bool KeyExists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

std::vector<std::wstring> SubkeyNames(HKEY root, const std::wstring& subkey) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return names;
    for (DWORD index = 0; index < 4096; ++index) {
        wchar_t name[256];
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status = RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) names.emplace_back(name, length);
    }
    RegCloseKey(key);
    return names;
}

// "@%SystemRoot%\\foo.dll,-101" style names resolve through the resource.
std::wstring ResolveDisplayString(const std::wstring& raw) {
    if (raw.empty() || raw.front() != L'@') return raw;
    wchar_t resolved[512]{};
    if (SUCCEEDED(SHLoadIndirectString(raw.c_str(), resolved, ARRAYSIZE(resolved), nullptr)) &&
        resolved[0])
        return resolved;
    return {};
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool ContainsNoCase(const std::wstring& haystack, const wchar_t* needle) {
    std::wstring h = haystack, n = needle;
    for (auto& c : h) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(std::towlower(c));
    return h.find(n) != std::wstring::npos;
}

std::wstring LastComponent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Where a delegate-folder CLSID points: TargetFolderPath or the FolderShortcut
// "Target" spelling. TargetKnownFolder is deliberately not followed: Windows'
// own NameSpace entries (Documents, Pictures, "This Device", ...) use it, and
// the OneDrive accounts come from their own key.
std::wstring TargetOf(HKEY root, const std::wstring& clsid_key) {
    const std::wstring bag = Join(clsid_key, L"Instance\\InitPropertyBag");
    std::wstring target;
    if (ReadString(root, bag, L"TargetFolderPath", target)) return target;
    if (ReadString(root, bag, L"Target", target)) return target;
    return {};
}

bool HiddenByPolicy(const CloudRegistry& registry, const std::wstring& clsid) {
    for (const auto& key : registry.nonenum_keys) {
        DWORD value = 0;
        if (ReadDword(key.root, key.path, clsid.c_str(), value) && (value & 1u)) return true;
    }
    return false;
}

} // namespace

CloudRegistry CloudRegistry::System() {
    CloudRegistry r;
    const std::wstring ns = Join(kExplorer, L"Desktop\\NameSpace");
    r.namespace_keys = { {HKEY_CURRENT_USER, ns}, {HKEY_LOCAL_MACHINE, ns} };
    const std::wstring pc = Join(kExplorer, L"MyComputer\\NameSpace");
    r.mycomputer_keys = { {HKEY_CURRENT_USER, pc}, {HKEY_LOCAL_MACHINE, pc} };
    r.clsid_keys = { {HKEY_CURRENT_USER, L"Software\\Classes\\CLSID"},
                     {HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID"} };
    r.nonenum_keys = { {HKEY_CURRENT_USER, kNonEnum}, {HKEY_LOCAL_MACHINE, kNonEnum} };
    r.sync_root_manager = {HKEY_LOCAL_MACHINE, Join(kExplorer, L"SyncRootManager")};
    r.user_sid = CurrentUserSidString();
    return r;
}

CloudRegistry CloudRegistry::Fixture(const std::wstring& prefix, const std::wstring& sid) {
    CloudRegistry r;
    r.namespace_keys = { {HKEY_CURRENT_USER, Join(prefix, L"NameSpace")} };
    r.mycomputer_keys = { {HKEY_CURRENT_USER, Join(prefix, L"MyComputer")} };
    r.clsid_keys = { {HKEY_CURRENT_USER, Join(prefix, L"CLSID")} };
    r.nonenum_keys = { {HKEY_CURRENT_USER, Join(prefix, L"NonEnum")} };
    r.sync_root_manager = {HKEY_CURRENT_USER, Join(prefix, L"SyncRootManager")};
    r.user_sid = sid;
    return r;
}

std::wstring NormalizeCloudRootPath(const std::wstring& raw) {
    if (raw.empty()) return {};
    std::wstring path = raw;
    if (path.find(L'%') != std::wstring::npos) {
        wchar_t expanded[MAX_PATH * 2]{};
        const DWORD n = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
        if (n > 0 && n <= ARRAYSIZE(expanded)) path = expanded;
    }
    for (auto& c : path) if (c == L'/') c = L'\\';
    // Account roots use extended paths; Shell registrations use ordinary paths.
    // Compare both in the same spelling, including extended UNC paths.
    if (_wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0)
        path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0)
        path.erase(0, 4);
    wchar_t full[MAX_PATH * 2]{};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    if (n > 0 && n < ARRAYSIZE(full)) path = full;
    // Relative or UNC-less junk never names a sync folder.
    if (path.size() < 3 || !(path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\'))) return {};
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool SameCloudRootPath(const std::wstring& a, const std::wstring& b) {
    return EqualsNoCase(NormalizeCloudRootPath(a), NormalizeCloudRootPath(b));
}

std::vector<CloudRoot> ReadNavigationPaneCloudRoots(const CloudRegistry& registry) {
    std::vector<CloudRoot> out;
    std::vector<std::wstring> seen;
    for (const auto& ns : registry.namespace_keys) {
        for (const auto& clsid : SubkeyNames(ns.root, ns.path)) {
            if (clsid.size() < 3 || clsid.front() != L'{') continue;
            if (std::any_of(seen.begin(), seen.end(),
                    [&](const std::wstring& s) { return EqualsNoCase(s, clsid); }))
                continue;
            seen.push_back(clsid);
            if (HiddenByPolicy(registry, clsid)) continue;
            for (const auto& classes : registry.clsid_keys) {
                const std::wstring key = Join(classes.path, clsid);
                if (!KeyExists(classes.root, key)) continue;
                // Cloud providers opt in with IsPinnedToNameSpaceTree = 1 (see
                // "Integrate a cloud storage provider"); Windows' own NameSpace
                // entries do not, and a 0 means the provider was switched off.
                DWORD pinned = 0;
                if (!ReadDword(classes.root, key, L"System.IsPinnedToNameSpaceTree", pinned) ||
                    pinned == 0)
                    break;
                const std::wstring path = NormalizeCloudRootPath(TargetOf(classes.root, key));
                if (path.empty()) break; // a shell extension, not a folder
                std::wstring label;
                std::wstring raw;
                if (ReadString(classes.root, key, L"LocalizedString", raw)) label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(classes.root, key, nullptr, raw)) label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(ns.root, Join(ns.path, clsid), nullptr, raw))
                    label = ResolveDisplayString(raw);
                if (label.empty()) label = LastComponent(path);
                DWORD order = 0x42;
                ReadDword(classes.root, key, L"SortOrderIndex", order);
                out.push_back({path, label, clsid, CloudRootSource::NavigationPane, order});
                break;
            }
        }
    }
    // Explorer orders its top-level nodes by SortOrderIndex, then by name.
    std::stable_sort(out.begin(), out.end(), [](const CloudRoot& a, const CloudRoot& b) {
        if (a.sort_order != b.sort_order) return a.sort_order < b.sort_order;
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return out;
}

std::vector<CloudRoot> ReadSyncRootCloudRoots(const CloudRegistry& registry) {
    std::vector<CloudRoot> out;
    if (registry.user_sid.empty()) return out;
    const auto& manager = registry.sync_root_manager;
    for (const auto& id : SubkeyNames(manager.root, manager.path)) {
        const std::wstring key = Join(manager.path, id);
        std::wstring raw;
        if (!ReadString(manager.root, Join(key, L"UserSyncRoots"), registry.user_sid.c_str(), raw))
            continue;
        const std::wstring path = NormalizeCloudRootPath(raw);
        if (path.empty()) continue;
        std::wstring label;
        if (ReadString(manager.root, key, L"DisplayNameResource", raw)) label = ResolveDisplayString(raw);
        // Ids read "Provider!SID!Account"; the provider part is the best fallback.
        if (label.empty()) label = id.substr(0, id.find(L'!'));
        if (label.empty()) label = LastComponent(path);
        out.push_back({path, label, id, CloudRootSource::SyncRoot});
    }
    return out;
}

void MergeCloudRoots(std::vector<CloudRoot>& into, std::vector<CloudRoot> more,
                     const std::function<bool(const std::wstring&)>& is_directory) {
    for (auto& root : more) {
        if (root.path.empty()) continue;
        const bool listed = std::any_of(into.begin(), into.end(),
            [&](const CloudRoot& r) { return SameCloudRootPath(r.path, root.path); });
        if (listed) continue;
        if (is_directory && !is_directory(root.path)) continue;
        into.push_back(std::move(root));
    }
}

namespace {
std::atomic<std::shared_ptr<const std::vector<std::wstring>>> g_cloud_files_roots;
} // namespace

void PublishCloudFilesRoots(std::vector<std::wstring> roots) {
    for (auto& root : roots) root = NormalizeCloudRootPath(root);
    roots.erase(std::remove(roots.begin(), roots.end(), std::wstring()), roots.end());
    g_cloud_files_roots.store(std::shared_ptr<const std::vector<std::wstring>>(
        std::make_shared<std::vector<std::wstring>>(std::move(roots))));
}

namespace {
// Pane paths may carry the long-path prefix; sync roots never do.
std::wstring WithoutLongPrefix(const std::wstring& raw) {
    if (raw.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + raw.substr(8);
    if (raw.rfind(L"\\\\?\\", 0) == 0) return raw.substr(4);
    return raw;
}
bool PathUnderRoot(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size() ||
        _wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}
std::atomic<std::shared_ptr<const std::vector<std::pair<std::wstring, std::wstring>>>> g_cloud_root_labels;
} // namespace

bool IsUnderCloudFilesRoot(const std::wstring& raw) {
    const auto roots = g_cloud_files_roots.load();
    if (!roots || roots->empty() || raw.size() < 3) return false;
    const std::wstring path = WithoutLongPrefix(raw);
    for (const auto& root : *roots)
        if (PathUnderRoot(path, root)) return true;
    return false;
}

void PublishCloudRootLabels(const std::vector<CloudRoot>& roots) {
    auto labels = std::make_shared<std::vector<std::pair<std::wstring, std::wstring>>>();
    for (const auto& root : roots) {
        std::wstring path = NormalizeCloudRootPath(root.path);
        if (!path.empty() && !root.label.empty()) labels->emplace_back(std::move(path), root.label);
    }
    g_cloud_root_labels.store(
        std::shared_ptr<const std::vector<std::pair<std::wstring, std::wstring>>>(std::move(labels)));
}

std::wstring CloudRootLabelFor(const std::wstring& raw) {
    const auto labels = g_cloud_root_labels.load();
    if (!labels || raw.size() < 3) return {};
    const std::wstring path = WithoutLongPrefix(raw);
    const std::pair<std::wstring, std::wstring>* best = nullptr;
    for (const auto& entry : *labels)
        if (PathUnderRoot(path, entry.first) && (!best || entry.first.size() > best->first.size())) best = &entry;
    return best ? best->second : std::wstring();
}

std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::wstring out;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes > 0) {
        std::vector<BYTE> buffer(bytes);
        if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(user->User.Sid, &text) && text) {
                out = text;
                LocalFree(text);
            }
        }
    }
    CloseHandle(token);
    return out;
}

uint32_t CloudRootColor(const CloudRoot& root) {
    const std::wstring key = root.id + L"|" + root.label;
    if (ContainsNoCase(key, L"onedrive")) return 0x2B88D8;
    if (ContainsNoCase(key, L"wps")) return 0x2F6FD6;
    if (ContainsNoCase(key, L"baidu") || key.find(L"\u767E\u5EA6") != std::wstring::npos) return 0x2F8CF0;
    if (ContainsNoCase(key, L"dropbox")) return 0x0061FE;
    if (ContainsNoCase(key, L"google")) return 0x1FA463;
    return 0x60A5FA;
}

namespace {

// InprocServer32 of Windows' own shell folders: %SystemRoot%\system32\*.dll,
// or a bare DLL name that loads from System32.
bool SystemServer(std::wstring server) {
    if (!server.empty() && server.front() == L'"') {
        server.erase(0, 1);
        const size_t quote = server.find(L'"');
        if (quote != std::wstring::npos) server.resize(quote);
    }
    if (server.find_first_of(L"\\/") == std::wstring::npos) return true;
    wchar_t windir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
    if (n == 0 || n >= ARRAYSIZE(windir)) return false;
    const std::wstring dir = std::wstring(windir) + L"\\";
    return server.size() > dir.size() && _wcsnicmp(server.c_str(), dir.c_str(), dir.size()) == 0;
}

uint8_t LauncherBrand(const std::wstring& clsid, const std::wstring& label) {
    if (EqualsNoCase(clsid, L"{679F137C-3162-45da-BE3C-2F9C3D093F64}") ||
        ContainsNoCase(label, L"Baidu") || ContainsNoCase(label, L"\x767E\x5EA6"))
        return 2;
    if (EqualsNoCase(clsid, L"{5FCD4425-CA3A-48F4-A57C-B8A75C32ACB1}") || ContainsNoCase(label, L"WPS"))
        return 3;
    return 1;
}

} // namespace

std::vector<ThisPcLauncher> ReadThisPcLaunchers(const CloudRegistry& registry) {
    std::vector<ThisPcLauncher> out;
    std::vector<std::wstring> seen;
    for (const auto& ns : registry.mycomputer_keys) {
        for (const auto& clsid : SubkeyNames(ns.root, ns.path)) {
            if (clsid.size() != 38 || clsid.front() != L'{' || clsid.back() != L'}') continue;
            if (std::any_of(seen.begin(), seen.end(),
                    [&](const std::wstring& s) { return EqualsNoCase(s, clsid); }))
                continue;
            seen.push_back(clsid);
            if (HiddenByPolicy(registry, clsid)) continue;
            for (const auto& classes : registry.clsid_keys) {
                const std::wstring key = Join(classes.path, clsid);
                if (!KeyExists(classes.root, key)) continue;
                std::wstring target = NormalizeCloudRootPath(TargetOf(classes.root, key));
                if (!target.empty()) {
                    const DWORD attrs = GetFileAttributesW(target.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) target.clear();
                }
                std::wstring server;
                const bool has_server = ReadString(classes.root, Join(key, L"InprocServer32"), nullptr, server);
                if (target.empty() && (!has_server || SystemServer(server))) break;  // Windows' own
                std::wstring label;
                std::wstring raw;
                if (ReadString(classes.root, key, L"LocalizedString", raw)) label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(classes.root, key, nullptr, raw)) label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(ns.root, Join(ns.path, clsid), nullptr, raw))
                    label = ResolveDisplayString(raw);
                if (label.empty() && !target.empty()) label = LastComponent(target);
                if (label.empty()) break;  // nothing to call it: Explorer shows no name either
                ThisPcLauncher launcher;
                launcher.clsid = clsid;
                launcher.label = std::move(label);
                launcher.target = std::move(target);
                launcher.brand = LauncherBrand(clsid, launcher.label);
                out.push_back(std::move(launcher));
                break;
            }
        }
    }
    return out;
}

std::vector<ThisPcLauncher> CurrentThisPcLaunchers() {
    // Isolated test instances read their private fixture instead.
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0) {
        wchar_t prefix[512]{};
        const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_CLOUD_REG", prefix, ARRAYSIZE(prefix));
        if (n > 0 && n < ARRAYSIZE(prefix))
            return ReadThisPcLaunchers(CloudRegistry::Fixture(prefix, L"S-1-5-21-pulse-test"));
    }
    return ReadThisPcLaunchers(CloudRegistry::System());
}

} // namespace pulse::app