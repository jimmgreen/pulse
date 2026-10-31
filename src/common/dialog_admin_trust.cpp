#include "dialog_admin_trust.h"
#include <aclapi.h>
#include <filesystem>
#include <sddl.h>
#include <vector>

namespace pulse::dialog_admin {
namespace {
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
struct Local {
    void *p = nullptr;
    ~Local() {
        if (p)
            LocalFree(p);
    }
};
bool Trusted(PSID sid) {
    if (!sid || !IsValidSid(sid))
        return false;
    BYTE system[SECURITY_MAX_SID_SIZE]{}, admins[SECURITY_MAX_SID_SIZE]{};
    DWORD a = sizeof(system), b = sizeof(admins);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &a) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &b))
        return false;
    if (EqualSid(sid, system) || EqualSid(sid, admins))
        return true;
    Local installer;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
                                reinterpret_cast<PSID *>(&installer.p)))
        return false;
    return EqualSid(sid, installer.p) != FALSE;
}
DWORD CheckAcl(PSID owner, PACL acl, bool ancestor) {
    if (!Trusted(owner) || !acl || !IsValidAcl(acl))
        return ERROR_ACCESS_DENIED;
    const DWORD prohibited = ancestor ? (DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER)
                                      : (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                         FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER);
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void *value = nullptr;
        if (!GetAce(acl, i, &value))
            return GetLastError();
        auto header = static_cast<ACE_HEADER *>(value);
        if (header->AceFlags & INHERIT_ONLY_ACE)
            continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return ERROR_ACCESS_DENIED;
        auto ace = static_cast<ACCESS_ALLOWED_ACE *>(value);
        if (header->AceSize < FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + 8)
            return ERROR_INVALID_ACL;
        PSID sid = &ace->SidStart;
        if (!IsValidSid(sid) ||
            GetLengthSid(sid) > static_cast<DWORD>(header->AceSize - FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart)))
            return ERROR_INVALID_SID;
        DWORD mask = ace->Mask;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        MapGenericMask(&mask, &mapping);
        if ((mask & prohibited) && !Trusted(sid))
            return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}
DWORD Object(const std::wstring &path, bool directory, bool ancestor) {
    Handle handle{CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.h == INVALID_HANDLE_VALUE)
        return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.h, &info))
        return GetLastError();
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return ERROR_REPARSE_TAG_INVALID;
    if (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return ERROR_DIRECTORY;
    PSID owner = nullptr;
    PACL acl = nullptr;
    Local descriptor;
    DWORD error =
        GetSecurityInfo(handle.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                        nullptr, &acl, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR *>(&descriptor.p));
    return error ? error : CheckAcl(owner, acl, ancestor);
}
DWORD Path(const std::wstring &path, bool directory) {
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path.find(L'\0') != path.npos || path.find(L'/', 0) != path.npos || path.find(L':', 2) != path.npos)
        return ERROR_INVALID_NAME;
    const auto drive = path.substr(0, 3);
    if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED)
        return ERROR_BAD_PATHNAME;
    if (path.size() > 3 && path.back() == L'\\')
        return ERROR_INVALID_NAME;
    DWORD error = Object(drive, path.size() == 3 ? directory : true, path.size() > 3);
    if (error)
        return error;
    for (size_t start = 3; start < path.size();) {
        size_t end = path.find(L'\\', start);
        if (end == path.npos)
            end = path.size();
        const auto component = path.substr(start, end - start);
        if (component.empty() || component == L"." || component == L".." || component.back() == L'.' ||
            component.back() == L' ')
            return ERROR_INVALID_NAME;
        const bool last = end == path.size();
        error = Object(path.substr(0, end), last ? directory : true, !last);
        if (error)
            return error;
        start = end + 1;
    }
    return ERROR_SUCCESS;
}
} // namespace
DWORD VerifyProtectedPath(const std::wstring &path, bool directory) noexcept {
    try {
        return Path(path, directory);
    } catch (const std::bad_alloc &) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}
DWORD VerifyProtectedTree(const std::wstring &root) noexcept {
    try {
        DWORD error = Path(root, true);
        if (error)
            return error;
        std::vector<std::wstring> pending{root};
        size_t count = 1;
        while (!pending.empty()) {
            auto directory = std::move(pending.back());
            pending.pop_back();
            WIN32_FIND_DATAW data{};
            HANDLE found = FindFirstFileW((directory + L"\\*").c_str(), &data);
            if (found == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND)
                    continue;
                return error;
            }
            struct Find {
                HANDLE h;
                ~Find() {
                    FindClose(h);
                }
            } close{found};
            do {
                if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L".."))
                    continue;
                if (++count > 4096)
                    return ERROR_TOO_MANY_NAMES;
                const auto child = directory + L"\\" + data.cFileName;
                const bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                error = Object(child, is_directory, false);
                if (error)
                    return error;
                if (is_directory)
                    pending.push_back(child);
            } while (FindNextFileW(found, &data));
            error = GetLastError();
            if (error != ERROR_NO_MORE_FILES)
                return error;
        }
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc &) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_INVALID_DATA;
    }
}
bool IsElevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}
} // namespace pulse::dialog_admin
