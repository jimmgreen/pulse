#include "dialog_bridge_paths.h"
#include "dialog_admin_trust.h"
#include "json_value.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <filesystem>
#include <vector>

namespace pulse::dialog_bridge_paths {
namespace {

namespace fs = std::filesystem;

struct LocalFreeGuard {
    void* ptr = nullptr;
    ~LocalFreeGuard() {
        if (ptr) LocalFree(ptr);
    }
};

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

std::wstring KnownFolder(const KNOWNFOLDERID& id) noexcept {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return {};
    std::wstring result;
    try {
        result.assign(raw);
    } catch (...) {}
    CoTaskMemFree(raw);
    return result;
}

std::wstring StripTrailingSlash(std::wstring value) {
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    return value;
}

std::wstring StripExtendedPrefix(std::wstring value) {
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    return value;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), out.data(), needed, nullptr, nullptr);
    if (written != needed) return {};
    return out;
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), out.data(), needed);
    if (written != needed) return {};
    return out;
}

bool HasBridgeManifestOrPointer(const std::wstring& root) noexcept {
    if (root.empty()) return false;
    const auto active = root + L"\\" + kActivePointerFile;
    if (GetFileAttributesW(active.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const auto legacy = root + L"\\dialog-current.json";
    return GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES;
}

DWORD ApplyProtectedDacl(const std::wstring& directory, bool include_app_container) noexcept {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            ProtectedBridgeSddl(include_app_container), SDDL_REVISION_1, &sd, nullptr)) {
        return GetLastError();
    }
    LocalFreeGuard sd_guard{sd};
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) {
        return ERROR_INVALID_SECURITY_DESCR;
    }
    return SetNamedSecurityInfoW(
        const_cast<wchar_t*>(directory.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
}

} // namespace

std::wstring CommonDialogBridgeRoot(const std::wstring& override_root) noexcept {
    try {
        if (!override_root.empty()) return StripTrailingSlash(StripExtendedPrefix(override_root));
        const auto common = StripTrailingSlash(KnownFolder(FOLDERID_ProgramFilesCommon));
        if (common.empty()) return {};
        return common + L"\\" + kCommonRelativeSubdir;
    } catch (...) {
        return {};
    }
}

const wchar_t* ProtectedBridgeSddl(bool include_app_container) noexcept {
    return include_app_container
        ? L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)(A;OICI;GRGX;;;AC)"
        : L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)";
}

DWORD EnsureProtectedBridgeDirectory(const std::wstring& directory, bool include_app_container) noexcept {
    try {
        const auto clean = StripTrailingSlash(StripExtendedPrefix(directory));
        if (clean.size() < 4 || clean[1] != L':' || clean[2] != L'\\') return ERROR_INVALID_NAME;

        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                ProtectedBridgeSddl(include_app_container), SDDL_REVISION_1, &sd, nullptr)) {
            return GetLastError();
        }
        LocalFreeGuard sd_guard{sd};
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};

        // Create intermediate components safely without crossing reparse points.
        for (size_t pos = 3; pos <= clean.size(); ++pos) {
            if (pos != clean.size() && clean[pos] != L'\\') continue;
            const auto part = clean.substr(0, pos);
            const DWORD attrs = GetFileAttributesW(part.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(part.c_str(), &sa)) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS) return err;
                }
            } else {
                if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_REPARSE_TAG_INVALID;
                if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
            }
        }
        return ApplyProtectedDacl(clean, include_app_container);
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

DWORD WriteActiveBridgePointer(const std::wstring& bridge_root, const ActiveBridgePointer& pointer) noexcept {
    try {
        const auto root = StripTrailingSlash(StripExtendedPrefix(bridge_root));
        if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') return ERROR_INVALID_NAME;

        json::Value doc = json::Value::Object();
        doc.set("schema", static_cast<int64_t>(pointer.schema));
        doc.set("core_dir_x64", WideToUtf8(pointer.core_dir_x64));
        doc.set("core_dir_x86", WideToUtf8(pointer.core_dir_x86));
        doc.set("core_digest_x64", pointer.core_digest_x64);
        doc.set("core_digest_x86", pointer.core_digest_x86);
        doc.set("host_exe", WideToUtf8(pointer.host_exe));
        doc.set("install_root", WideToUtf8(pointer.install_root));
        doc.set("generation", static_cast<int64_t>(pointer.generation));

        const std::string encoded = json::Serialize(doc);
        const auto temp_path = root + L"\\" + kActivePointerFile + L"." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L".tmp";
        const auto final_path = root + L"\\" + kActivePointerFile;

        {
            HandleGuard file{CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.h == INVALID_HANDLE_VALUE) return GetLastError();
            DWORD written = 0;
            if (!WriteFile(file.h, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr) ||
                written != encoded.size() || !FlushFileBuffers(file.h)) {
                const DWORD err = GetLastError();
                DeleteFileW(temp_path.c_str());
                return err ? err : ERROR_WRITE_FAULT;
            }
        }
        if (!MoveFileExW(temp_path.c_str(), final_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD err = GetLastError();
            DeleteFileW(temp_path.c_str());
            return err;
        }
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

DWORD ReadActiveBridgePointer(const std::wstring& bridge_root, ActiveBridgePointer& output) noexcept {
    output = {};
    try {
        const auto root = StripTrailingSlash(StripExtendedPrefix(bridge_root));
        if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') return ERROR_INVALID_NAME;
        const auto path = root + L"\\" + kActivePointerFile;
        HandleGuard file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.h == INVALID_HANDLE_VALUE) return GetLastError();

        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file.h, &info) || !GetFileSizeEx(file.h, &size)) return GetLastError();
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_REPARSE_TAG_INVALID;
        if (size.QuadPart <= 0 || size.QuadPart > 65536) return ERROR_INVALID_DATA;

        std::string raw(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!ReadFile(file.h, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr) || read != raw.size()) {
            return GetLastError();
        }
        json::Value parsed;
        if (!json::Parse(raw, parsed) || !parsed.is_object()) return ERROR_INVALID_DATA;
        const auto* schema = parsed.find("schema");
        int64_t schema_val = 0;
        if (!schema || !schema->get_int(schema_val) || schema_val != 1) return ERROR_INVALID_DATA;

        auto read_wstr = [&](const char* key) -> std::wstring {
            const auto* f = parsed.find(key);
            return (f && f->is_string()) ? Utf8ToWide(f->as_string()) : std::wstring{};
        };
        auto read_str = [&](const char* key) -> std::string {
            const auto* f = parsed.find(key);
            return (f && f->is_string()) ? f->as_string() : std::string{};
        };

        output.schema = 1;
        output.core_dir_x64 = read_wstr("core_dir_x64");
        output.core_dir_x86 = read_wstr("core_dir_x86");
        output.core_digest_x64 = read_str("core_digest_x64");
        output.core_digest_x86 = read_str("core_digest_x86");
        output.host_exe = read_wstr("host_exe");
        output.install_root = read_wstr("install_root");
        int64_t gen_val = 0;
        if (const auto* gen = parsed.find("generation"); gen && gen->get_int(gen_val, 0)) {
            output.generation = static_cast<uint64_t>(gen_val);
        }
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}

std::wstring ResolveActiveBridgeRoot(const std::wstring& install_root,
                                     const std::wstring& override_common_root) noexcept {
    try {
        const auto common = CommonDialogBridgeRoot(override_common_root);
        if (!common.empty() && HasBridgeManifestOrPointer(common)) {
            if (!override_common_root.empty() || dialog_admin::VerifyProtectedPath(common, true) == ERROR_SUCCESS) {
                return common;
            }
        }
        return StripTrailingSlash(StripExtendedPrefix(install_root));
    } catch (...) {
        return install_root;
    }
}

bool IsUnderProgramFiles(const std::wstring& path) noexcept {
    try {
        const auto clean = StripTrailingSlash(StripExtendedPrefix(path));
        if (clean.empty()) return false;
        for (const auto& folder_id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesCommon, FOLDERID_ProgramFilesX86}) {
            const auto base = StripTrailingSlash(KnownFolder(folder_id));
            if (base.empty() || clean.size() <= base.size() + 1) continue;
            if (_wcsnicmp(clean.c_str(), base.c_str(), base.size()) == 0 && clean[base.size()] == L'\\') {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace pulse::dialog_bridge_paths
