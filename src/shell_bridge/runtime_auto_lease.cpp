#include "runtime_auto_lease.h"
#include "../common/dialog_auto_binding.h"
#include "../common/dialog_admin_trust.h"
#include <aclapi.h>
#include <objbase.h>

namespace pulse::shell_bridge::runtime {
namespace {
DWORD VerifyOwnedByCurrentUser(HANDLE handle) noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
    struct CloseToken { HANDLE h; ~CloseToken() { if (h) CloseHandle(h); } } close_token{token};
    alignas(TOKEN_USER) BYTE user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD returned = 0;
    if (!GetTokenInformation(token, TokenUser, user_buf, sizeof(user_buf), &returned)) return GetLastError();
    PSID self_sid = reinterpret_cast<TOKEN_USER*>(user_buf)->User.Sid;
    PSID owner_sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD sec = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner_sid, nullptr, nullptr, nullptr, &sd);
    if (sec != ERROR_SUCCESS) return sec;
    struct FreeSd { PSECURITY_DESCRIPTOR p; ~FreeSd() { if (p) LocalFree(p); } } free_sd{sd};
    if (!owner_sid || !IsValidSid(owner_sid) || !EqualSid(owner_sid, self_sid)) return ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}
}
AutoLease::~AutoLease() {
    if (helper_) CloseHandle(helper_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}
bool AutoLease::Alive() const noexcept {
    return !helper_ || WaitForSingleObject(helper_, 0) == WAIT_TIMEOUT;
}
DWORD AutoLease::Open(const std::wstring& control, const std::wstring& nonce, bool required, bool user_mode) noexcept {
    if (file_ != INVALID_HANDLE_VALUE || helper_) return ERROR_ALREADY_INITIALIZED;
    try {
        if (user_mode && pulse::dialog_admin::IsElevated()) return ERROR_ACCESS_DENIED;
        const auto path = control + L"\\" + kAutoBindingFile;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND && !required ? ERROR_SUCCESS : error;
        }
        struct Guard { HANDLE value; ~Guard() { if (value) CloseHandle(value); } } held{file}, helper{nullptr};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) return GetLastError();
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_INVALID_DATA;
        DWORD error = ERROR_SUCCESS;
        if (user_mode) {
            error = VerifyOwnedByCurrentUser(file);
            if (!error) {
                HANDLE dir = CreateFileW(control.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (dir == INVALID_HANDLE_VALUE) return GetLastError();
                Guard dir_guard{dir};
                BY_HANDLE_FILE_INFORMATION dir_info{};
                if (!GetFileInformationByHandle(dir, &dir_info)) return GetLastError();
                if (!(dir_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (dir_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_INVALID_DATA;
                error = VerifyOwnedByCurrentUser(dir);
            }
        } else {
            error = pulse::dialog_admin::VerifyProtectedPath(path, false);
        }
        if (error) return error;
        namespace binding = pulse::dialog_auto_binding;
        binding::BindingV1 record{};
        LARGE_INTEGER size{};
        DWORD read = 0;
        if (!GetFileSizeEx(file, &size)) return GetLastError();
        if (size.QuadPart != sizeof(record)) return ERROR_BAD_LENGTH;
        if (!ReadFile(file, &record, sizeof(record), &read, nullptr)) return GetLastError();
        if (read != sizeof(record)) return ERROR_BAD_LENGTH;
        error = binding::ValidateFormat(record);
        if (error) return error;
        GUID expected{};
        if (FAILED(CLSIDFromString(nonce.c_str(), &expected)) || !IsEqualGUID(record.nonce, expected)) return ERROR_INVALID_DATA;
        helper.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, record.helper_pid);
        if (!helper.value) return GetLastError();
        binding::BindingV1 actual{};
        error = user_mode ?
            binding::CaptureCurrentWithHelperUser(helper.value, record.generation, expected, actual) :
            binding::CaptureCurrentWithHelper(helper.value, record.generation, expected, actual);
        if (error) return error;
        error = binding::MatchTarget(record, actual.target);
        if (error) return error;
        if (actual.helper_pid != record.helper_pid ||
            CompareFileTime(&actual.helper_created, &record.helper_created) != 0) return ERROR_INVALID_DATA;
        if (WaitForSingleObject(helper.value, 0) != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
        file_ = held.value; held.value = nullptr;
        helper_ = helper.value; helper.value = nullptr;
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
