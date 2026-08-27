#include "index_config.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
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

bool EnsureMachineDirectory(const std::wstring& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    // SYSTEM + Administrators full; Authenticated Users can list/read (logs,
    // diagnostics). Protected DACL still blocks unintended ProgramData inherit.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;AU)",
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
        path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        directory = CreateFileW(
            path.c_str(), READ_CONTROL,
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

std::vector<std::wstring> ExtractStringArray(const std::wstring& json,
                                             const std::wstring& key) {
    std::vector<std::wstring> out;
    const std::wstring marker = L"\"" + key + L"\"";
    size_t p = json.find(marker);
    if (p == std::wstring::npos) return out;
    p = json.find(L'[', p + marker.size());
    if (p == std::wstring::npos) return out;
    ++p;
    while (p < json.size()) {
        while (p < json.size() && iswspace(json[p])) ++p;
        if (p >= json.size() || json[p] == L']') break;
        if (json[p] != L'\"') return {};
        ++p;
        std::wstring value;
        while (p < json.size() && json[p] != L'\"') {
            if (json[p] != L'\\') {
                value.push_back(json[p++]);
                continue;
            }
            if (++p >= json.size()) return {};
            switch (json[p]) {
            case L'\"': value.push_back(L'\"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            default: value.push_back(json[p]); break;
            }
            ++p;
        }
        if (p >= json.size()) return {};
        ++p;
        out.push_back(std::move(value));
        while (p < json.size() && iswspace(json[p])) ++p;
        if (p < json.size() && json[p] == L',') ++p;
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
    out += L"]\n}\n";
    return out;
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
    if (!EnsureMachineDirectory(index)) return {};
    return index;
}

std::wstring UserIndexRoot() {
    std::wstring root = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (root.empty()) return {};
    const std::wstring pulse = root + L"\\Pulse";
    if (!EnsureDirectory(pulse)) return {};
    return pulse;
}

std::wstring MachineConfigPath() {
    const std::wstring root = MachineIndexRoot();
    return root.empty() ? L"" : root + L"\\config.json";
}

bool LoadMachineConfig(IndexConfig& config, std::wstring* error) {
    config = {};
    config.index_path = MachineIndexRoot();
    const std::wstring path = MachineConfigPath();
    if (path.empty()) {
        SetError(error, L"无法定位 ProgramData 索引目录");
        return false;
    }
    std::wstring json;
    if (!pulse::ReadUtf8File(path, json)) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
        SetError(error, L"无法读取索引配置");
        return false;
    }
    if (json.empty() || json.find(L'{') == std::wstring::npos) {
        SetError(error, L"索引配置为空或损坏");
        return false;
    }
    config.version = static_cast<uint32_t>((std::max)(1, pulse::json::ExtractInt(json, L"version", 1)));
    config.generation = static_cast<uint64_t>((std::max)(1, pulse::json::ExtractInt(json, L"generation", 1)));
    config.include_fixed_ntfs = pulse::json::ExtractBool(json, L"include_fixed_ntfs", true);
    config.include_removable_ntfs = pulse::json::ExtractBool(json, L"include_removable_ntfs", true);
    config.index_path = pulse::json::ExtractString(json, L"index_path", MachineIndexRoot());
    if (config.index_path.empty()) config.index_path = MachineIndexRoot();
    for (auto& id : ExtractStringArray(json, L"excluded_volume_ids"))
        config.excluded_volume_ids.insert(NormalizeVolumeId(std::move(id)));
    for (auto& path_value : ExtractStringArray(json, L"excluded_paths")) {
        std::replace(path_value.begin(), path_value.end(), L'/', L'\\');
        while (path_value.size() > 3 && path_value.back() == L'\\') path_value.pop_back();
        if (!path_value.empty()) config.excluded_paths.push_back(std::move(path_value));
    }
    std::sort(config.excluded_paths.begin(), config.excluded_paths.end(),
              [](const auto& a, const auto& b) {
                  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
              });
    config.excluded_paths.erase(std::unique(config.excluded_paths.begin(), config.excluded_paths.end(),
              [](const auto& a, const auto& b) {
                  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
              }), config.excluded_paths.end());
    return true;
}

bool SaveMachineConfig(const IndexConfig& config, std::wstring* error) {
    const std::wstring path = MachineConfigPath();
    if (path.empty()) {
        SetError(error, L"无法定位 ProgramData 索引目录");
        return false;
    }
    if (!pulse::WriteUtf8FileAtomic(path, ConfigJson(config))) {
        SetError(error, Win32Error(L"保存索引配置失败"));
        return false;
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
