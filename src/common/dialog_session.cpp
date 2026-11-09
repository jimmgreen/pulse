#include "process_token_restriction.h"
#include "dialog_admin_trust.h"
#include "dialog_session.h"
#include "../dialog_launcher/managed_launch_profile.h"
#include <objbase.h>
#include <fstream>
#include <memory>
#include <vector>
namespace pulse::dialog_session {
namespace fs = std::filesystem;
std::wstring AdminDrivePath(std::wstring path) {
    // Canonical emits extended drive paths. Do not normalize UNC or devices.
    if (path.size() >= 7 && path.compare(0, 4, LR"(\\?\)") == 0 &&
        ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
        path[5] == L':' && path[6] == L'\\') path.erase(0, 4);
    return path;
}

#ifdef _WIN64
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#endif
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring Canonical(const fs::path& path, bool directory) {
    if (!path.is_absolute()) return {};
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return {};
    std::wstring result(cfg::kPathUnits, 0);
    const auto count = GetFinalPathNameByHandleW(file.value, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool Same(const std::wstring& a, const std::wstring& b) { return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool ReadExact(const fs::path& path, void* data, DWORD bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{}; DWORD read = 0;
    return file.value != INVALID_HANDLE_VALUE && GetFileSizeEx(file.value, &size) && size.QuadPart == bytes &&
        ReadFile(file.value, data, bytes, &read, nullptr) && read == bytes;
}
bool WriteNew(const fs::path& path, const void* data, DWORD bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    return file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, data, bytes, &written, nullptr) &&
        written == bytes && FlushFileBuffers(file.value);
}
bool TokenData(HANDLE process, TOKEN_INFORMATION_CLASS type, std::vector<BYTE>& data) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD count = 0;
    GetTokenInformation(token.value, type, nullptr, 0, &count);
    if (!count || count > 65536) return false;
    data.resize(count);
    return GetTokenInformation(token.value, type, data.data(), count, &count) != FALSE;
}
bool AllowedProcess(HANDLE process, bool allow_elevated) {
    if (allow_elevated && !dialog_admin::IsElevated()) return false;
    Handle thread_token;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread_token.value) || GetLastError() != ERROR_NO_TOKEN) return false;
    Handle caller_token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &caller_token.value) ||
        pulse::security::CheckUnrestrictedToken(caller_token.value) != ERROR_SUCCESS) return false;
    Handle process_token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &process_token.value) || pulse::security::CheckUnrestrictedToken(process_token.value) != ERROR_SUCCESS) return false;
    TOKEN_ELEVATION elevation{}; DWORD returned = 0;
    if (!GetTokenInformation(process_token.value, TokenElevation, &elevation, sizeof(elevation), &returned) || bool(elevation.TokenIsElevated) != allow_elevated) return false;
    using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto query_machine = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT process_machine = 0, native_machine = 0;
    if (!query_machine || !query_machine(process, &process_machine, &native_machine) ||
        (process_machine ? process_machine : native_machine) != kMachine) return false;
    std::vector<BYTE> own_user, other_user, own_level, other_level, container;
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    const auto query_protection = reinterpret_cast<decltype(&GetProcessInformation)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
    // Optional dialog integration fails closed when the OS lacks this query.
    if (!query_protection || !query_protection(process, ProcessProtectionLevelInfo, &protection, sizeof(protection)) ||
        protection.ProtectionLevel != PROTECTION_LEVEL_NONE ||
        !TokenData(process, TokenIsAppContainer, container) || *reinterpret_cast<DWORD*>(container.data()) ||
        !TokenData(GetCurrentProcess(), TokenUser, own_user) || !TokenData(process, TokenUser, other_user) ||
        !TokenData(GetCurrentProcess(), TokenIntegrityLevel, own_level) || !TokenData(process, TokenIntegrityLevel, other_level)) return false;
    const auto integrity_sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(other_level.data())->Label.Sid;
    if (!IsValidSid(integrity_sid) || !*GetSidSubAuthorityCount(integrity_sid) ||
        *GetSidSubAuthority(integrity_sid, *GetSidSubAuthorityCount(integrity_sid) - 1) < SECURITY_MANDATORY_MEDIUM_RID) return false;
    return EqualSid(reinterpret_cast<TOKEN_USER*>(own_user.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(other_user.data())->User.Sid) &&
        EqualSid(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(own_level.data())->Label.Sid, reinterpret_cast<TOKEN_MANDATORY_LABEL*>(other_level.data())->Label.Sid);
}
bool Machine(const fs::path& file, WORD& machine) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const auto bytes = stream.tellg(); IMAGE_DOS_HEADER dos{};
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(&dos), sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<std::streamoff>(dos.e_lfanew) + static_cast<std::streamoff>(sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) > bytes) return false;
    stream.seekg(dos.e_lfanew);
    DWORD signature = 0; IMAGE_FILE_HEADER header{};
    if (!stream.read(reinterpret_cast<char*>(&signature), sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !stream.read(reinterpret_cast<char*>(&header), sizeof(header))) return false;
    machine = header.Machine; return true;
}
bool Created(HANDLE process, FILETIME& created) { FILETIME exit{}, kernel{}, user{}; return GetProcessTimes(process, &created, &exit, &kernel, &user) != FALSE; }
bool SameTime(FILETIME a, FILETIME b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }
bool ProcessIdentity(HANDLE process, const cfg::Session& session) {
    wchar_t image[cfg::kPathUnits]{}; DWORD count = ARRAYSIZE(image); FILETIME created{};
    return GetProcessId(process) == session.target_pid && Created(process, created) && SameTime(created, session.target_created) &&
        QueryFullProcessImageNameW(process, 0, image, &count) && Same(Canonical(image, false), session.target_path);
}
bool LoadSession(const fs::path& directory, const std::wstring& expected_launcher, cfg::Session& session) {
    if (dialog_admin::IsElevated() &&
        (dialog_admin::VerifyProtectedPath(AdminDrivePath(directory.wstring()), true) ||
         dialog_admin::VerifyProtectedPath(AdminDrivePath(directory.parent_path().wstring()), true) ||
         dialog_admin::VerifyProtectedPath(AdminDrivePath((directory / cfg::kSessionFile).wstring()), false))) return false;
    if (!ReadExact(directory / cfg::kSessionFile, &session, sizeof(session)) || session.magic != cfg::kConfigMagic ||
        session.version != cfg::kVersion || session.size != sizeof(session) || (session.reserved & ~cfg::kAdminSession) || session.machine != kMachine ||
        session.nonce[39] || session.host_path[cfg::kPathUnits - 1] || session.target_path[cfg::kPathUnits - 1] ||
        session.control_path[cfg::kPathUnits - 1] || session.launcher_path[cfg::kPathUnits - 1]) return false;
    if (session.reserved == cfg::kAdminSession &&
        (dialog_admin::VerifyProtectedPath(AdminDrivePath(directory.wstring()), true) ||
         dialog_admin::VerifyProtectedPath(AdminDrivePath(directory.parent_path().wstring()), true) ||
         dialog_admin::VerifyProtectedPath(AdminDrivePath((directory / cfg::kSessionFile).wstring()), false) ||
         dialog_admin::VerifyProtectedTree(AdminDrivePath(fs::path(expected_launcher).parent_path().wstring())))) return false;
    GUID guid{};
    return wcslen(session.nonce) == 38 && SUCCEEDED(CLSIDFromString(session.nonce, &guid)) &&
        directory.filename() == session.nonce && Same(Canonical(directory, true), session.control_path) && Same(Canonical(expected_launcher, false), session.launcher_path);
}
bool Ready(const fs::path& directory, const cfg::Session& session, cfg::Ready& ready) {
    return ReadExact(directory / cfg::kReadyFile, &ready, sizeof(ready)) && ready.magic == cfg::kReadyMagic &&
        ready.version == cfg::kVersion && ready.size == sizeof(ready) && !ready.reserved && !ready.nonce[39] &&
        ready.target_pid == session.target_pid && ready.machine == session.machine &&
        SameTime(ready.target_created, session.target_created) && wcscmp(ready.nonce, session.nonce) == 0;
}
DWORD Inspect(const std::wstring& directory, const std::wstring& expected_launcher, bool disable, Status& output) noexcept {
    output = {};
    try {
        auto session = std::make_unique<cfg::Session>();
        if (!LoadSession(directory, expected_launcher, *session)) return ERROR_INVALID_DATA;
        output.pid = session->target_pid;
        output.application = fs::path(session->target_path).filename().wstring();
        cfg::Ready ready;
        output.historical_handshake = Ready(directory, *session, ready);
        if (output.historical_handshake) { output.initialization = ready.status; output.entry_coverage = ready.installed_entries; }
        std::wstring control(session->control_path);
        if (control.starts_with(LR"(\\?\)")) control.erase(0,4);
        DWORD error = dialog_launcher::ReadDisableMarker(control, output.local_disabled);
        if (error) return error;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, session->target_pid));
        if (!process.value) {
            const DWORD open_error = GetLastError();
            return open_error == ERROR_INVALID_PARAMETER ? ERROR_SUCCESS : open_error;
        }
        if (!ProcessIdentity(process.value, *session)) return ERROR_INVALID_DATA;
        const DWORD alive = WaitForSingleObject(process.value, 0);
        if (alive == WAIT_OBJECT_0) return ERROR_SUCCESS;
        if (alive != WAIT_TIMEOUT) return GetLastError();
        output.process_alive = true;
        if (!AllowedProcess(process.value, session->reserved == cfg::kAdminSession)) return ERROR_ACCESS_DENIED;
        if (disable && !output.local_disabled) {
            if (!WriteNew(fs::path(directory) / cfg::kDisableFile, session->nonce,
                static_cast<DWORD>((wcslen(session->nonce)+1)*sizeof(wchar_t)))) {
                error = dialog_launcher::ReadDisableMarker(control, output.local_disabled);
                return error ? error : (output.local_disabled ? ERROR_SUCCESS : ERROR_WRITE_FAULT);
            }
            output.local_disabled = true;
        }
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
