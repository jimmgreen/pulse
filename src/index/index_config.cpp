#include "index_config.h"
#include "index_directory_security.h"
#include "../common/config_json.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <iterator>
#include <aclapi.h>
#include <cwctype>
#include <sddl.h>
#include <shlobj.h>
#include <sstream>
#include <windows.h>

namespace pulse::index {
namespace {

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool EnsureMachineDirectory(const std::wstring& path, bool user_readable = true) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // SYSTEM + Administrators full; Authenticated Users can list/read (logs,
    // diagnostics). Protected DACL still blocks unintended ProgramData inherit.
    // Index data lists every name on the indexed volumes, so it is never
    // user-readable; users query it through the filtered pipe instead (#66).
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            user_readable ? L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;AU)"
                          : L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr))
        return false;

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (!created && create_error != ERROR_ALREADY_EXISTS) {
        LocalFree(descriptor);
        return false;
    }

    HANDLE directory = CreateFileW(
        path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        directory = CreateFileW(
            path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            LocalFree(descriptor);
            return false;
        }
    }

    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool valid_directory = GetFileInformationByHandleEx(
        directory, FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE &&
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;

    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    BOOL owner_defaulted = FALSE;
    BOOL group_defaulted = FALSE;
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    const bool have_security =
        GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) != FALSE &&
        GetSecurityDescriptorGroup(descriptor, &group, &group_defaulted) != FALSE &&
        GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) != FALSE &&
        dacl_present;
    const DWORD security_error = valid_directory && have_security
        ? SetSecurityInfo(directory, SE_FILE_OBJECT,
              OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              owner, group, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;

    const DWORD access_error = GetLastError();
    CloseHandle(directory);
    LocalFree(descriptor);
    // A normal user can validate an existing service-owned directory but
    // cannot rewrite its owner/DACL. The service/installer will repair drift.
    return valid_directory &&
        (security_error == ERROR_SUCCESS || access_error == ERROR_ACCESS_DENIED);
}

std::wstring KnownFolder(int csidl) {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path))) return {};
    return path;
}

void SetError(std::wstring* error, const std::wstring& text) {
    if (error) *error = text;
}

std::wstring Win32Error(const wchar_t* operation) {
    const DWORD code = GetLastError();
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring out = operation;
    out += L"（" + std::to_wstring(code) + L"）";
    if (message) {
        while (*message && iswspace(message[wcslen(message) - 1]))
            message[wcslen(message) - 1] = 0;
        out += L"：";
        out += message;
        LocalFree(message);
    }
    return out;
}

std::wstring ConfigJson(const IndexConfig& config) {
    std::wstring escaped_path;
    pulse::json::Escape(config.index_path, escaped_path);
    std::vector<std::wstring> excluded(config.excluded_volume_ids.begin(),
                                       config.excluded_volume_ids.end());
    std::sort(excluded.begin(), excluded.end());
    std::vector<std::wstring> excluded_paths = config.excluded_paths;
    std::sort(excluded_paths.begin(), excluded_paths.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out = L"{\n  \"version\":" + std::to_wstring(config.version) +
                       L",\n  \"generation\":" + std::to_wstring(config.generation) +
                       L",\n  \"include_fixed_ntfs\":" +
                       (config.include_fixed_ntfs ? L"true" : L"false") +
                       L",\n  \"include_removable_ntfs\":" +
                       (config.include_removable_ntfs ? L"true" : L"false") +
                       L",\n  \"index_path\":\"" + escaped_path +
                       L"\",\n  \"excluded_volume_ids\":[";
    for (size_t i = 0; i < excluded.size(); ++i) {
        std::wstring escaped;
        pulse::json::Escape(excluded[i], escaped);
        if (i) out += L",";
        out += L"\n    \"" + escaped + L"\"";
    }
    if (!excluded.empty()) out += L"\n  ";
    out += L"],\n  \"excluded_paths\":[";
    for (size_t i = 0; i < excluded_paths.size(); ++i) {
        std::wstring escaped;
        pulse::json::Escape(excluded_paths[i], escaped);
        if (i) out += L",";
        out += L"\n    \"" + escaped + L"\"";
    }
    if (!excluded_paths.empty()) out += L"\n  ";
    out += L"],\n  \"exclude_system\":";
    out += config.exclude_system ? L"true" : L"false";
    out += L",\n  \"system_exclusion_groups\":[";
    for (size_t i = 0; i < config.system_groups.size(); ++i) {
        if (i) out += L",";
        out += L"\"" + config.system_groups[i] + L"\"";
    }
    // Groups this release offered; later groups start at their default.
    out += L"],\n  \"system_exclusion_known\":[";
    for (size_t i = 0; i < std::size(kSystemExclusionGroups); ++i) {
        if (i) out += L",";
        out += L"\"" + std::wstring(kSystemExclusionGroups[i]) + L"\"";
    }
    out += L"]\n}\n";
    return out;
}

void AddProfileFolders(const std::wstring& suffix, std::vector<std::wstring>& out) {
    HKEY list = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0,
            KEY_READ | KEY_WOW64_64KEY, &list) != ERROR_SUCCESS) return;
    wchar_t sid[256]{};
    for (DWORD i = 0;; ++i) {
        DWORD sid_len = ARRAYSIZE(sid);
        if (RegEnumKeyExW(list, i, sid, &sid_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t raw[MAX_PATH]{};
        DWORD bytes = sizeof(raw) - sizeof(wchar_t);
        if (RegGetValueW(list, sid, L"ProfileImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                nullptr, raw, &bytes) != ERROR_SUCCESS) continue;
        wchar_t expanded[MAX_PATH]{};
        const DWORD n = ExpandEnvironmentStringsW(raw, expanded, ARRAYSIZE(expanded));
        if (!n || n > ARRAYSIZE(expanded) || expanded[0] == 0 || expanded[1] != L':') continue;
        out.push_back(std::wstring(expanded) + suffix);
    }
    RegCloseKey(list);
}

bool IsNtfs(const std::wstring& fs) {
    return CompareStringOrdinal(fs.c_str(), -1, L"NTFS", -1, TRUE) == CSTR_EQUAL;
}

} // namespace

bool IndexConfig::IsExcluded(const std::wstring& id) const {
    return excluded_volume_ids.contains(NormalizeVolumeId(id));
}

bool IndexConfig::IsPathExcluded(std::wstring_view path) const {
    for (const auto& excluded : excluded_paths) {
        if (excluded.empty() || path.size() < excluded.size()) continue;
        if (CompareStringOrdinal(path.data(), static_cast<int>(excluded.size()),
                                 excluded.data(), static_cast<int>(excluded.size()), TRUE) !=
            CSTR_EQUAL) continue;
        if (path.size() == excluded.size() || path[excluded.size()] == L'\\' ||
            path[excluded.size()] == L'/') return true;
    }
    return false;
}

bool IsSystemExclusionGroup(std::wstring_view group) {
    return std::any_of(std::begin(kSystemExclusionGroups), std::end(kSystemExclusionGroups),
                       [&](const wchar_t* known) { return group == known; });
}

bool HidesNodeModules(const IndexConfig& config) {
    return config.exclude_system &&
        std::find(config.system_groups.begin(), config.system_groups.end(), L"node_modules") !=
            config.system_groups.end();
}

std::vector<std::wstring> SystemExclusionPaths(const IndexConfig& config) {
    std::vector<std::wstring> out;
    if (!config.exclude_system) return out;
    wchar_t windows[MAX_PATH]{};
    const UINT n = GetSystemWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (!n || n >= ARRAYSIZE(windows) || windows[1] != L':') return out;
    const std::wstring windir = windows;
    const std::wstring drive = windir.substr(0, 2);
    auto enabled = [&](const wchar_t* group) {
        return std::find(config.system_groups.begin(), config.system_groups.end(), group) !=
            config.system_groups.end();
    };
    if (enabled(L"windows")) out.push_back(windir);
    if (enabled(L"temp")) {
        out.push_back(windir + L"\\Temp");
        AddProfileFolders(L"\\AppData\\Local\\Temp", out);
        AddProfileFolders(L"\\AppData\\Local\\Microsoft\\Windows\\INetCache", out);
        AddProfileFolders(L"\\AppData\\Local\\CrashDumps", out);
    }
    if (enabled(L"old")) {
        for (const wchar_t* name : {L"\\Windows.old", L"\\$WINDOWS.~BT", L"\\$Windows.~WS", L"\\$WinREAgent"})
            out.push_back(drive + name);
    }
    if (enabled(L"programdata")) {
        const std::wstring data = KnownFolder(CSIDL_COMMON_APPDATA);
        if (data.size() > 3) out.push_back(data);
    }
    return out;
}

std::wstring NormalizeVolumeId(std::wstring id) {
    while (!id.empty() && iswspace(id.back())) id.pop_back();
    size_t first = 0;
    while (first < id.size() && iswspace(id[first])) ++first;
    if (first) id.erase(0, first);
    std::transform(id.begin(), id.end(), id.begin(), towupper);
    return id;
}

std::wstring MachineDataRoot() {
    std::wstring root = KnownFolder(CSIDL_COMMON_APPDATA);
    if (root.empty()) return {};
    const std::wstring pulse = root + L"\\Pulse";
    if (!EnsureMachineDirectory(pulse)) return {};
    return pulse;
}

std::wstring MachineIndexRoot() {
    const std::wstring pulse = MachineDataRoot();
    if (pulse.empty()) return {};
    const std::wstring index = pulse + L"\\Index";
    if (!EnsureMachineDirectory(index, false)) return {};
    return index;
}

bool ProtectIndexDirectory(const std::wstring& path) {
    return PreparePrivateIndexDirectory(std::filesystem::path(path));
}

std::wstring UserIndexRoot() {
    std::wstring root = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (root.empty()) return {};
    const std::wstring pulse = root + L"\\Pulse";
    if (!EnsureDirectory(pulse)) return {};
    return pulse;
}

std::wstring MachineConfigPath() {
    const std::wstring root = MachineDataRoot();
    return root.empty() ? L"" : root + L"\\index-config.json";
}

bool LoadIndexConfigFrom(const std::wstring& path, const std::wstring& default_index_path,
                         IndexConfig& config, std::wstring* error) {
    IndexConfig loaded;
    loaded.index_path = default_index_path;
    std::wstring json;
    if (!pulse::ReadUtf8File(path, json)) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                config = std::move(loaded);
                return true;
            }
        }
        config.load_failed = true;
        SetError(error, L"无法读取索引配置");
        return false;
    }
    if (!pulse::json::ValidConfigObject(json)) {
        config.load_failed = true;
        SetError(error, L"索引配置为空或损坏");
        return false;
    }
    loaded.version = static_cast<uint32_t>((std::max)(1, pulse::json::ExtractInt(json, L"version", 1)));
    loaded.generation = static_cast<uint64_t>((std::max)(1, pulse::json::ExtractInt(json, L"generation", 1)));
    loaded.include_fixed_ntfs = pulse::json::ExtractBool(json, L"include_fixed_ntfs", true);
    loaded.include_removable_ntfs = pulse::json::ExtractBool(json, L"include_removable_ntfs", true);
    loaded.index_path = pulse::json::ExtractString(json, L"index_path", default_index_path);
    if (loaded.index_path.empty()) loaded.index_path = default_index_path;
    for (auto& id : pulse::json::ExtractStringArray(json, L"excluded_volume_ids"))
        loaded.excluded_volume_ids.insert(NormalizeVolumeId(std::move(id)));
    for (auto& path_value : pulse::json::ExtractStringArray(json, L"excluded_paths")) {
        std::replace(path_value.begin(), path_value.end(), L'/', L'\\');
        while (path_value.size() > 3 && path_value.back() == L'\\') path_value.pop_back();
        if (!path_value.empty()) loaded.excluded_paths.push_back(std::move(path_value));
    }
    std::sort(loaded.excluded_paths.begin(), loaded.excluded_paths.end(),
              [](const auto& a, const auto& b) {
                  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
              });
    loaded.excluded_paths.erase(std::unique(loaded.excluded_paths.begin(), loaded.excluded_paths.end(),
              [](const auto& a, const auto& b) {
                  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
              }), loaded.excluded_paths.end());
    loaded.exclude_system = pulse::json::ExtractBool(json, L"exclude_system", true);
    loaded.system_groups_saved = json.find(L"\"system_exclusion_groups\"") != std::wstring::npos;
    if (loaded.system_groups_saved) {
        loaded.system_groups.clear();
        for (auto& group : pulse::json::ExtractStringArray(json, L"system_exclusion_groups"))
            if (IsSystemExclusionGroup(group) &&
                std::find(loaded.system_groups.begin(), loaded.system_groups.end(), group) ==
                    loaded.system_groups.end())
                loaded.system_groups.push_back(std::move(group));
        // A group introduced after this file was written was never offered to
        // the user, so it keeps its default. Before "system_exclusion_known"
        // existed, node_modules was always hidden.
        std::vector<std::wstring> known;
        if (json.find(L"\"system_exclusion_known\"") != std::wstring::npos)
            known = pulse::json::ExtractStringArray(json, L"system_exclusion_known");
        else
            known = {L"windows", L"temp", L"old", L"programdata"};
        const IndexConfig defaults;
        for (const auto& group : defaults.system_groups) {
            if (std::find(known.begin(), known.end(), group) == known.end() &&
                std::find(loaded.system_groups.begin(), loaded.system_groups.end(), group) ==
                    loaded.system_groups.end())
                loaded.system_groups.push_back(group);
        }
    }
    config = std::move(loaded);
    return true;
}

bool LoadMachineConfig(IndexConfig& config, std::wstring* error) {
    const std::wstring root = MachineDataRoot();
    std::wstring path = MachineConfigPath();
    if (path.empty()) {
        config.load_failed = true;
        SetError(error, L"无法定位 ProgramData 索引目录");
        return false;
    }
    // Older releases kept machine configuration inside the default data directory.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        path = root + L"\\Index\\config.json";
    return LoadIndexConfigFrom(path, root + L"\\Index", config, error);
}

bool SaveMachineConfig(const IndexConfig& config, std::wstring* error) {
    if (config.load_failed) {
        SetError(error, L"索引配置读取失败，禁止覆盖");
        return false;
    }
    const std::wstring path = MachineConfigPath();
    if (path.empty()) {
        SetError(error, L"无法定位 ProgramData 索引目录");
        return false;
    }
    if (!pulse::WriteUtf8FileAtomic(path, ConfigJson(config))) {
        SetError(error, Win32Error(L"保存索引配置失败"));
        return false;
    }
    const std::wstring legacy_root = MachineDataRoot() + L"\\Index";
    const DWORD attributes = GetFileAttributesW(legacy_root.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // The new configuration is durable before removing the legacy copy.
        DeleteFileW((legacy_root + L"\\config.json").c_str());
        if (CompareStringOrdinal(config.index_path.c_str(), -1, legacy_root.c_str(), -1, TRUE) != CSTR_EQUAL)
            RemoveDirectoryW(legacy_root.c_str());
    }
    return true;
}

bool ConfigureVolume(const std::wstring& id, bool enabled, std::wstring* error) {
    const std::wstring normalized = NormalizeVolumeId(id);
    if (normalized.empty() || normalized.rfind(L"\\\\?\\VOLUME{", 0) != 0) {
        SetError(error, L"无效的卷标识");
        return false;
    }
    IndexConfig config;
    if (!LoadMachineConfig(config, error)) return false;
    if (enabled) config.excluded_volume_ids.erase(normalized);
    else config.excluded_volume_ids.insert(normalized);
    ++config.generation;
    return SaveMachineConfig(config, error);
}

bool ConfigureIndexPath(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        SetError(error, L"索引路径不能为空");
        return false;
    }
    if (!EnsureDirectory(path)) {
        SetError(error, Win32Error(L"无法创建索引目录"));
        return false;
    }
    PrivateIndexDirectoryLock destination_lock;
    if (!destination_lock.Acquire(std::filesystem::path(path)) || !ProtectIndexDirectory(path)) {
        SetError(error, L"索引目录权限不安全，请使用空的独立目录。原索引配置已保留。");
        return false;
    }
    IndexConfig config;
    if (!LoadMachineConfig(config, error)) return false;
    config.index_path = path;
    ++config.generation;
    return SaveMachineConfig(config, error);
}

bool ConfigureExcludePath(const std::wstring& path, bool enabled, std::wstring* error) {
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    while (normalized.size() > 3 && normalized.back() == L'\\') normalized.pop_back();
    if (normalized.size() <= 3 || normalized[1] != L':') {
        SetError(error, L"排除项必须是本地文件夹路径");
        return false;
    }
    IndexConfig config;
    if (!LoadMachineConfig(config, error)) return false;
    auto it = std::find_if(config.excluded_paths.begin(), config.excluded_paths.end(),
                           [&](const auto& value) {
                               return CompareStringOrdinal(value.c_str(), -1,
                                                           normalized.c_str(), -1, TRUE) == CSTR_EQUAL;
                           });
    if (enabled) {
        if (it == config.excluded_paths.end()) config.excluded_paths.push_back(normalized);
    } else if (it != config.excluded_paths.end()) {
        config.excluded_paths.erase(it);
    }
    ++config.generation;
    return SaveMachineConfig(config, error);
}

bool ConfigureSystemExclusion(const std::wstring& group, bool enabled, std::wstring* error) {
    if (group != L"all" && !IsSystemExclusionGroup(group)) {
        SetError(error, L"未知的系统文件夹分组");
        return false;
    }
    IndexConfig config;
    if (!LoadMachineConfig(config, error)) return false;
    if (group == L"all") {
        config.exclude_system = enabled;
    } else {
        auto it = std::find(config.system_groups.begin(), config.system_groups.end(), group);
        if (enabled && it == config.system_groups.end()) config.system_groups.push_back(group);
        else if (!enabled && it != config.system_groups.end()) config.system_groups.erase(it);
    }
    ++config.generation;
    return SaveMachineConfig(config, error);
}

std::vector<VolumeInfo> EnumerateLocalVolumes(const IndexConfig& config) {
    std::vector<VolumeInfo> out;
    wchar_t drives[512]{};
    const DWORD n = GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    if (!n || n >= ARRAYSIZE(drives)) return out;
    for (const wchar_t* p = drives; *p; p += wcslen(p) + 1) {
        const UINT type = GetDriveTypeW(p);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        VolumeInfo info;
        info.mount_point = p;
        info.kind = type == DRIVE_FIXED ? VolumeKind::Fixed : VolumeKind::Removable;
        wchar_t volume_name[MAX_PATH]{};
        if (GetVolumeNameForVolumeMountPointW(p, volume_name, ARRAYSIZE(volume_name)))
            info.id = NormalizeVolumeId(volume_name);
        else
            info.id = NormalizeVolumeId(std::wstring(L"DRIVE:") + p[0]);
        wchar_t label[MAX_PATH]{};
        wchar_t fs[MAX_PATH]{};
        DWORD serial = 0;
        info.online = GetVolumeInformationW(p, label, ARRAYSIZE(label), &serial, nullptr,
                                            nullptr, fs, ARRAYSIZE(fs)) != FALSE;
        if (info.online) {
            info.label = label;
            info.file_system = fs;
        }
        info.supported = info.online && IsNtfs(info.file_system);
        const bool auto_include = info.kind == VolumeKind::Fixed
            ? config.include_fixed_ntfs : config.include_removable_ntfs;
        info.enabled = info.supported && auto_include && !config.IsExcluded(info.id);
        info.state = !info.online ? L"离线" : !info.supported ? L"非 NTFS" :
                     info.enabled ? L"等待索引" : L"已排除";
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const VolumeInfo& a, const VolumeInfo& b) {
        return CompareStringOrdinal(a.mount_point.c_str(), -1,
                                    b.mount_point.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
}

} // namespace pulse::index
