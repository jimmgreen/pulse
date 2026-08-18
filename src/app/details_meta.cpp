// details_meta.cpp — See details_meta.h. No COM needed (advapi32/win32 only).
#include "details_meta.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>

namespace pulse::app {
namespace {

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

std::wstring LookupSid(PSID sid) {
    if (!sid) return {};
    wchar_t name[256]{}, domain[256]{};
    DWORD name_len = static_cast<DWORD>(std::size(name));
    DWORD domain_len = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use{};
    if (!LookupAccountSidW(nullptr, sid, name, &name_len, domain, &domain_len, &use))
        return {};
    if (domain_len > 0) return std::wstring(domain) + L"\\" + name;
    return name;
}

void FetchOwner(const std::wstring& shell_path, DetailsMeta& out) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(shell_path.c_str(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr,
                              nullptr, &sd) == ERROR_SUCCESS) {
        out.owner = LookupSid(sid);
        if (sd) LocalFree(sd);
    }
}

void FetchPermissions(const std::wstring& shell_path, bool is_dir, DetailsMeta& out) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed);
    TOKEN_USER* user = needed && GetTokenInformation(token, TokenUser, buffer.data(),
                                                     needed, &needed)
        ? reinterpret_cast<TOKEN_USER*>(buffer.data()) : nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (user && GetNamedSecurityInfoW(shell_path.c_str(), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &dacl, nullptr, &sd) == ERROR_SUCCESS) {
        if (!dacl) {
            out.permissions = L"完全控制"; // NULL DACL grants everyone full access
        } else {
            TRUSTEE_W trustee{};
            trustee.TrusteeForm = TRUSTEE_IS_SID;
            trustee.TrusteeType = TRUSTEE_IS_USER;
            trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);
            ACCESS_MASK mask = 0;
            if (GetEffectiveRightsFromAclW(dacl, &trustee, &mask) == ERROR_SUCCESS)
                out.permissions = FormatAccessMask(mask, is_dir);
        }
        if (sd) LocalFree(sd);
    }
    CloseHandle(token);
}

void FetchVolume(const std::wstring& shell_path, DetailsMeta& out) {
    wchar_t root[MAX_PATH]{};
    if (!GetVolumePathNameW(shell_path.c_str(), root, ARRAYSIZE(root))) return;
    wchar_t volume_name[MAX_PATH]{}, fs_name[MAX_PATH]{};
    if (GetVolumeInformationW(root, volume_name, ARRAYSIZE(volume_name), nullptr,
                              nullptr, nullptr, fs_name, ARRAYSIZE(fs_name))) {
        out.file_system = fs_name;
        std::wstring letter = root;
        while (!letter.empty() && letter.back() == L'\\') letter.pop_back();
        out.drive = volume_name[0] ? std::wstring(volume_name) + L" (" + letter + L")"
                                   : letter;
    }
    ULARGE_INTEGER free_avail{}, total{};
    if (GetDiskFreeSpaceExW(root, &free_avail, &total, nullptr) && total.QuadPart > 0) {
        const uint64_t pct = free_avail.QuadPart * 100 / total.QuadPart;
        wchar_t text[96];
        swprintf_s(text, L"%s / %s (%llu%%)",
                   pulse::format::ByteSize(free_avail.QuadPart).c_str(),
                   pulse::format::ByteSize(total.QuadPart).c_str(),
                   static_cast<unsigned long long>(pct));
        out.free_space = text;
    }
}

} // namespace

std::wstring FormatAccessMask(unsigned int mask, bool is_dir) {
    if ((mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS) return L"完全控制";
    // winnt.h has FILE_GENERIC_READ/WRITE/EXECUTE but no FILE_GENERIC_MODIFY.
    const bool read = (mask & FILE_GENERIC_READ) == FILE_GENERIC_READ;
    const bool write = (mask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE;
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += L"、";
        out += part;
    };
    if (read && write) add(L"修改");
    if ((mask & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE) add(L"读取和执行");
    if (is_dir && (mask & FILE_LIST_DIRECTORY) == FILE_LIST_DIRECTORY)
        add(L"列出文件夹内容");
    if (read && !write) add(L"读取");
    if (write && !read) add(L"写入");
    if (mask & DELETE) add(L"删除");
    if (out.empty()) add(L"特殊权限");
    return out;
}

DetailsMeta FetchDetailsMeta(const std::wstring& path) {
    DetailsMeta out;
    const std::wstring shell_path = ShellPath(path);
    const DWORD attrs = GetFileAttributesW(shell_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return out;
    FetchOwner(shell_path, out);
    FetchPermissions(shell_path, (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, out);
    FetchVolume(shell_path, out);
    return out;
}

} // namespace pulse::app
