#include "runtime_child_session.h"
#include "../common/process_token_restriction.h"
#include "runtime_parent_ready.h"
#include "runtime_config.h"
#include <aclapi.h>
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <vector>

namespace pulse::shell_bridge::runtime {
namespace {
namespace fs = std::filesystem;
#ifdef _WIN64
constexpr USHORT kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr USHORT kMachine = IMAGE_FILE_MACHINE_I386;
#endif
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Local { void* value = nullptr; ~Local() { if (value) LocalFree(value); } };
DWORD Error() { const DWORD error = GetLastError(); return error ? error : ERROR_GEN_FAILURE; }
bool Same(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
DWORD NoImpersonation() {
    Handle token;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value)) return ERROR_ACCESS_DENIED;
    const DWORD error = GetLastError();
    return error == ERROR_NO_TOKEN ? ERROR_SUCCESS : error;
}
DWORD TokenData(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE>& data) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (!size || size > 65536) return Error();
    data.resize(size);
    return GetTokenInformation(token, kind, data.data(), size, &size) ? ERROR_SUCCESS : Error();
}
DWORD Allowed(HANDLE process) {
    if (const DWORD error = NoImpersonation()) return error;
    Handle own, other;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own.value) ||
        !OpenProcessToken(process, TOKEN_QUERY, &other.value)) return Error();
    TOKEN_ELEVATION elevation{}; DWORD bytes = 0, container = 0;
    if (!GetTokenInformation(other.value, TokenElevation, &elevation, sizeof(elevation), &bytes) ||
        !GetTokenInformation(other.value, TokenIsAppContainer, &container, sizeof(container), &bytes)) return Error();
    if (const DWORD error = security::CheckUnrestrictedToken(other.value)) return error;
    if (elevation.TokenIsElevated || container) return ERROR_ACCESS_DENIED;
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    const auto query_protection = reinterpret_cast<decltype(&GetProcessInformation)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
    if (!query_protection) return ERROR_NOT_SUPPORTED;
    if (!query_protection(process, ProcessProtectionLevelInfo, &protection, sizeof(protection))) return Error();
    if (protection.ProtectionLevel != PROTECTION_LEVEL_NONE) return ERROR_ACCESS_DENIED;
    using MachineFn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto machine_fn = reinterpret_cast<MachineFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT machine = 0, native = 0;
    if (!machine_fn) return ERROR_NOT_SUPPORTED;
    if (!machine_fn(process, &machine, &native)) return Error();
    if ((machine ? machine : native) != kMachine) return ERROR_BAD_EXE_FORMAT;
    std::vector<BYTE> own_user, user, own_level, level;
    DWORD error = TokenData(own.value, TokenUser, own_user);
    if (!error) error = TokenData(other.value, TokenUser, user);
    if (!error) error = TokenData(own.value, TokenIntegrityLevel, own_level);
    if (!error) error = TokenData(other.value, TokenIntegrityLevel, level);
    if (error) return error;
    const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(level.data())->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid) ||
        *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) < SECURITY_MANDATORY_MEDIUM_RID ||
        !EqualSid(reinterpret_cast<TOKEN_USER*>(own_user.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) ||
        !EqualSid(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(own_level.data())->Label.Sid, sid)) return ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}
DWORD Canonical(const std::wstring& path, bool directory, std::wstring& output) {
    output.clear();
    if (!fs::path(path).is_absolute()) return ERROR_INVALID_NAME;
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return Error();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)) return Error();
    if (((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return ERROR_INVALID_DATA;
    output.resize(kPathUnits);
    const DWORD count = GetFinalPathNameByHandleW(file.value, output.data(), static_cast<DWORD>(output.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= output.size()) { output.clear(); return count ? ERROR_FILENAME_EXCED_RANGE : Error(); }
    output.resize(count);
    return ERROR_SUCCESS;
}
DWORD SecureDirectory(const std::wstring& path, Handle& lock) {
    lock.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock.value == INVALID_HANDLE_VALUE) return Error();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(lock.value, &info)) return Error();
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return ERROR_NOT_SUPPORTED;
    Handle token; std::vector<BYTE> user;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return Error();
    if (const DWORD error = TokenData(token.value, TokenUser, user)) return error;
    PSID owner = nullptr; PACL acl = nullptr; Local descriptor;
    const DWORD error = GetSecurityInfo(lock.value, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value));
    if (error) return error;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    const auto user_sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    if (!owner || !EqualSid(owner, user_sid) || !acl ||
        !GetSecurityDescriptorControl(descriptor.value, &control, &revision) || !(control & SE_DACL_PROTECTED)) return ERROR_ACCESS_DENIED;
    BYTE system[SECURITY_MAX_SID_SIZE]{}; DWORD size = sizeof(system);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &size)) return Error();
    bool has_user = false, has_system = false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) return Error();
        const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) return ERROR_ACCESS_DENIED;
        const auto sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
        if (!IsValidSid(sid)) return ERROR_INVALID_DATA;
        if (EqualSid(sid, user_sid)) has_user = true;
        else if (EqualSid(sid, system)) has_system = true;
        else return ERROR_ACCESS_DENIED;
    }
    return has_user && has_system ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
DWORD ReadSession(const std::wstring& path, Session& value) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return Error();
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{}; DWORD read = 0;
    if (!GetFileInformationByHandle(file.value, &info) || !GetFileSizeEx(file.value, &size)) return Error();
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || size.QuadPart != static_cast<LONGLONG>(sizeof(value))) return ERROR_INVALID_DATA;
    if (!ReadFile(file.value, &value, sizeof(value), &read, nullptr)) return Error();
    if (read != sizeof(value) || value.magic != kConfigMagic || value.version != kVersion || value.size != sizeof(value) ||
        value.reserved || value.machine != kMachine || value.nonce[39] || value.host_path[kPathUnits - 1] ||
        value.control_path[kPathUnits - 1] || value.target_path[kPathUnits - 1] || value.launcher_path[kPathUnits - 1]) return ERROR_INVALID_DATA;
    GUID guid{};
    return wcslen(value.nonce) == 38 && SUCCEEDED(CLSIDFromString(value.nonce, &guid)) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD WriteSession(const std::wstring& path, const Session& value) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return Error();
    DWORD written = 0;
    if (!WriteFile(file.value, &value, sizeof(value), &written, nullptr) || !FlushFileBuffers(file.value)) return Error();
    return written == sizeof(value) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
DWORD Identity(HANDLE process, const Session& session) {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (GetProcessId(process) != session.target_pid) return ERROR_INVALID_DATA;
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return Error();
    if (CompareFileTime(&created, &session.target_created)) return ERROR_INVALID_DATA;
    const DWORD wait = WaitForSingleObject(process, 0);
    if (wait == WAIT_FAILED) return Error();
    if (wait != WAIT_TIMEOUT) return ERROR_PROCESS_ABORTED;
    std::wstring path(kPathUnits, 0), canonical; DWORD count = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &count)) return Error();
    path.resize(count);
    if (const DWORD error = Canonical(path, false, canonical)) return error;
    return Same(canonical, session.target_path) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD CheckSessionDirectory(const std::wstring& directory, const Session& session) {
    std::wstring canonical;
    if (const DWORD error = Canonical(directory, true, canonical)) return error;
    if (!Same(canonical, session.control_path) || fs::path(canonical).filename() != session.nonce) return ERROR_INVALID_DATA;
    if (const DWORD error = Canonical(session.host_path, false, canonical)) return error;
    if (!Same(canonical, session.host_path)) return ERROR_INVALID_DATA;
    if (const DWORD error = Canonical(session.launcher_path, false, canonical)) return error;
    return Same(canonical, session.launcher_path) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD Enabled(const std::wstring& directory) {
    if (GetFileAttributesW((directory + L"\\" + kDisableFile).c_str()) != INVALID_FILE_ATTRIBUTES) return ERROR_CANCELLED;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}
DWORD Chain(std::wstring directory, bool require_ready_ancestors = false) {
    std::vector<std::wstring> visited;
    for (unsigned depth = 0; depth != 16; ++depth) {
        for (const auto& previous : visited) if (Same(directory, previous)) return ERROR_CIRCULAR_DEPENDENCY;
        visited.push_back(directory);
        Handle lock;
        if (const DWORD error = SecureDirectory(directory, lock)) return error;
        auto session = std::make_unique<Session>();
        if (const DWORD error = ReadSession(directory + L"\\" + kSessionFile, *session)) return error;
        if (const DWORD error = CheckSessionDirectory(directory, *session)) return error;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, session->target_pid));
        if (!process.value) return Error();
        if (const DWORD error = Identity(process.value, *session)) return error;
        if (const DWORD error = Allowed(process.value)) return error;
        if (const DWORD error = Enabled(directory)) return error;
        if (require_ready_ancestors && depth) {
            if (const DWORD error = ValidateRuntimeReadyReceipt(directory, *session)) return error;
        }
        auto parent = std::make_unique<Session>();
        const DWORD read = ReadSession(directory + L"\\" + kParentSessionFile, *parent);
        if (read == ERROR_FILE_NOT_FOUND) {
            return _wcsicmp(fs::path(directory).parent_path().filename().c_str(), L"children") == 0 ? ERROR_INVALID_DATA : ERROR_SUCCESS;
        }
        if (read) return read;
        const fs::path expected = fs::path(parent->control_path) / L"children" / session->nonce;
        if (!Same(directory, expected.wstring()) || !Same(session->host_path, parent->host_path) ||
            !Same(session->launcher_path, parent->launcher_path)) return ERROR_INVALID_DATA;
        auto actual = std::make_unique<Session>();
        if (const DWORD error = ReadSession(std::wstring(parent->control_path) + L"\\" + kSessionFile, *actual)) return error;
        if (std::memcmp(actual.get(), parent.get(), sizeof(Session))) return ERROR_INVALID_DATA;
        directory = parent->control_path;
    }
    return ERROR_TOO_MANY_LINKS;
}
DWORD Descriptor(Local& descriptor) {
    Handle token; std::vector<BYTE> user;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return Error();
    if (const DWORD error = TokenData(token.value, TokenUser, user)) return error;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return Error();
    Local memory{sid};
    const auto sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;" + std::wstring(sid) + L")";
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) ? ERROR_SUCCESS : Error();
}
bool PulseKey(std::wstring_view entry) {
    for (const auto key : {L"PULSE_DIALOG_HOST=", L"PULSE_DIALOG_NONCE=", L"PULSE_DIALOG_CONTROL="}) {
        const auto count = wcslen(key);
        if (entry.size() >= count && CompareStringOrdinal(entry.data(), static_cast<int>(count), key, static_cast<int>(count), TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}
bool PulseKey(std::string_view entry) {
    for (const auto key : {"PULSE_DIALOG_HOST=", "PULSE_DIALOG_NONCE=", "PULSE_DIALOG_CONTROL="}) {
        const auto count = strlen(key);
        if (entry.size() >= count && _strnicmp(entry.data(), key, count) == 0) return true;
    }
    return false;
}
// Bound and probe readable memory; never dereference a malformed caller block.
template<class Char> DWORD CopyEnvironment(LPCVOID source, std::vector<Char>& output) {
    constexpr size_t limit = 1024 * 1024;
    auto cursor = reinterpret_cast<uintptr_t>(source);
    bool zero = false;
    while (output.size() < limit / sizeof(Char)) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return ERROR_BAD_ENVIRONMENT;
        const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (end <= cursor) return ERROR_BAD_ENVIRONMENT;
        const size_t available = static_cast<size_t>(end - cursor) / sizeof(Char);
        const size_t count = std::min({available, size_t{4096}, limit / sizeof(Char) - output.size()});
        if (!count) return ERROR_BAD_ENVIRONMENT;
        std::vector<Char> chunk(count); SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(cursor), chunk.data(), count * sizeof(Char), &read) ||
            read != count * sizeof(Char)) return ERROR_BAD_ENVIRONMENT;
        for (const auto c : chunk) {
            output.push_back(c);
            if (!c && zero) return ERROR_SUCCESS;
            zero = c == 0;
        }
        cursor += count * sizeof(Char);
    }
    return ERROR_BAD_ENVIRONMENT;
}
DWORD Ansi(const std::wstring& input, std::string& output) {
    const bool utf8 = GetACP() == CP_UTF8; BOOL used = FALSE;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int count = WideCharToMultiByte(CP_ACP, flags, input.c_str(), -1, nullptr, 0, nullptr, utf8 ? nullptr : &used);
    if (!count || used) return ERROR_NO_UNICODE_TRANSLATION;
    output.resize(static_cast<size_t>(count));
    if (!WideCharToMultiByte(CP_ACP, flags, input.c_str(), -1, output.data(), count, nullptr, utf8 ? nullptr : &used) || used) return ERROR_NO_UNICODE_TRANSLATION;
    std::wstring roundtrip(input.size() + 1, 0);
    if (MultiByteToWideChar(CP_ACP, 0, output.c_str(), -1, roundtrip.data(), static_cast<int>(roundtrip.size())) != static_cast<int>(input.size() + 1) ||
        input != roundtrip.c_str()) return ERROR_NO_UNICODE_TRANSLATION;
    output.pop_back(); return ERROR_SUCCESS;
}
template<class Char> void ReplaceEnvironment(std::vector<Char>& block, const std::vector<std::basic_string<Char>>& added) {
    std::vector<std::basic_string<Char>> entries;
    for (size_t start = 0; start < block.size() && block[start];) {
        const std::basic_string<Char> entry(block.data() + start);
        start += entry.size() + 1;
        if (!PulseKey(std::basic_string_view<Char>(entry))) entries.push_back(entry);
    }
    entries.insert(entries.end(), added.begin(), added.end());
    // Preserve duplicate caller entries' relative order and their exact bytes.
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if constexpr (sizeof(Char) == sizeof(wchar_t)) return _wcsicmp(a.c_str(), b.c_str()) < 0;
        else return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    block.clear();
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
    block.push_back(0);
}
}

struct ChildSessionPreparation::State {
    std::unique_ptr<Session> session = std::make_unique<Session>();
    std::wstring control, parent_control;
    Handle parent_lock, children_lock, directory_lock;
    std::vector<char> ansi;
    std::vector<wchar_t> unicode;
    FILETIME prepared{};
    DWORD environment_flags = 0;
    bool bound = false;
};
ChildSessionPreparation::ChildSessionPreparation() noexcept = default;
ChildSessionPreparation::~ChildSessionPreparation() = default;
ChildSessionPreparation::ChildSessionPreparation(ChildSessionPreparation&&) noexcept = default;
ChildSessionPreparation& ChildSessionPreparation::operator=(ChildSessionPreparation&&) noexcept = default;
LPVOID ChildSessionPreparation::Environment() noexcept {
    if (!state_) return nullptr;
    return state_->unicode.empty() ? static_cast<LPVOID>(state_->ansi.data()) : static_cast<LPVOID>(state_->unicode.data());
}
DWORD ChildSessionPreparation::EnvironmentFlags() const noexcept { return state_ ? state_->environment_flags : 0; }
const std::wstring& ChildSessionPreparation::ControlPath() const noexcept {
    static const std::wstring empty;
    return state_ ? state_->control : empty;
}
DWORD ValidateBoundChildRouting(const std::wstring& control, const std::wstring& host,
                                const std::wstring& nonce) noexcept {
    try {
        if (const DWORD error = Allowed(GetCurrentProcess())) return error;
        if (const DWORD error = Chain(control)) return error;
        auto session = std::make_unique<Session>();
        if (const DWORD error = ReadSession(control + L"\\" + kSessionFile, *session)) return error;
        if (nonce != session->nonce || !Same(host, session->host_path)) return ERROR_INVALID_DATA;
        return Identity(GetCurrentProcess(), *session);
    } catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { return ERROR_INVALID_DATA; }
}
DWORD ValidateReadyParentChain(const std::wstring& control) noexcept {
    try { if (const DWORD error = NoImpersonation()) return error; return Chain(control, true); }
    catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { return ERROR_INVALID_DATA; }
}
DWORD ValidateParentChain(const std::wstring& control) noexcept {
    try { if (const DWORD error = NoImpersonation()) return error; return Chain(control); }
    catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { return ERROR_INVALID_DATA; }
}
DWORD PrepareChildSession(const std::wstring& parent_control, LPCVOID environment, DWORD flags,
                          ChildSessionPreparation& output, ChildSessionDiagnostic& diagnostic) noexcept {
    diagnostic = {ChildSessionStage::Parent, ERROR_SUCCESS};
    output = ChildSessionPreparation{};
    auto fail = [&](DWORD error) { diagnostic.error = error; return error; };
    try {
        if (const DWORD error = Allowed(GetCurrentProcess())) return fail(error);
        if (const DWORD error = Chain(parent_control)) return fail(error);
        auto state = std::make_unique<ChildSessionPreparation::State>();
        GetSystemTimeAsFileTime(&state->prepared);
        state->environment_flags = environment ? flags : (flags | CREATE_UNICODE_ENVIRONMENT);
        if (const DWORD error = Canonical(parent_control, true, state->parent_control)) return fail(error);
        if (const DWORD error = SecureDirectory(state->parent_control, state->parent_lock)) return fail(error);
        auto parent = std::make_unique<Session>();
        if (const DWORD error = ReadSession(state->parent_control + L"\\" + kSessionFile, *parent)) return fail(error);
        if (const DWORD error = Identity(GetCurrentProcess(), *parent)) return fail(error);
        diagnostic.stage = ChildSessionStage::Security;
        Local descriptor;
        if (const DWORD error = Descriptor(descriptor)) return fail(error);
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor.value, FALSE};
        diagnostic.stage = ChildSessionStage::Directory;
        const auto children = state->parent_control + L"\\children";
        if (!CreateDirectoryW(children.c_str(), &security) && GetLastError() != ERROR_ALREADY_EXISTS) return fail(Error());
        if (const DWORD error = SecureDirectory(children, state->children_lock)) return fail(error);
        GUID guid{}; const HRESULT hr = CoCreateGuid(&guid);
        if (FAILED(hr)) return fail(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
        if (!StringFromGUID2(guid, state->session->nonce, ARRAYSIZE(state->session->nonce))) return fail(ERROR_INVALID_DATA);
        state->control = children + L"\\" + state->session->nonce;
        if (!CreateDirectoryW(state->control.c_str(), &security)) return fail(Error());
        diagnostic.retained_control = state->control;
        if (const DWORD error = SecureDirectory(state->control, state->directory_lock)) return fail(error);
        if (const DWORD error = WriteSession(state->control + L"\\" + kParentSessionFile, *parent)) return fail(error);
        state->session->machine = kMachine;
        if (wcscpy_s(state->session->host_path, parent->host_path) || wcscpy_s(state->session->launcher_path, parent->launcher_path) ||
            wcscpy_s(state->session->control_path, state->control.c_str())) return fail(ERROR_FILENAME_EXCED_RANGE);
        diagnostic.stage = ChildSessionStage::Environment;
        const std::vector<std::wstring> additions{L"PULSE_DIALOG_HOST=" + std::wstring(parent->host_path),
            L"PULSE_DIALOG_NONCE=" + std::wstring(state->session->nonce), L"PULSE_DIALOG_CONTROL=" + state->control};
        DWORD error = ERROR_SUCCESS;
        if (state->environment_flags & CREATE_UNICODE_ENVIRONMENT) {
            LPWCH inherited = environment ? nullptr : GetEnvironmentStringsW();
            if (!environment && !inherited) return fail(Error());
            try { error = CopyEnvironment(environment ? environment : inherited, state->unicode); }
            catch (...) { if (inherited) FreeEnvironmentStringsW(inherited); throw; }
            if (inherited) FreeEnvironmentStringsW(inherited);
            if (!error) ReplaceEnvironment(state->unicode, additions);
        } else {
            // NULL inheritance was normalized to Unicode above; this branch
            // copies an explicit ANSI block only, without an ACP roundtrip.
            if (!environment) return fail(ERROR_BAD_ENVIRONMENT);
            error = CopyEnvironment(environment, state->ansi);
            std::vector<std::string> encoded;
            for (const auto& value : additions) { std::string item; if (!error) error = Ansi(value, item); encoded.push_back(std::move(item)); }
            if (!error) ReplaceEnvironment(state->ansi, encoded);
            if (state->ansi.size() > 32767) error = ERROR_BAD_ENVIRONMENT;
        }
        if (error) return fail(error);
        output.state_ = std::move(state);
        diagnostic.stage = ChildSessionStage::Complete;
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) { return fail(ERROR_NOT_ENOUGH_MEMORY); }
    catch (...) { return fail(ERROR_INVALID_DATA); }
}
DWORD ChildSessionPreparation::Bind(HANDLE process, DWORD pid, ChildSessionDiagnostic& diagnostic) noexcept {
    diagnostic = {ChildSessionStage::Identity, ERROR_SUCCESS};
    auto fail = [&](DWORD error) { diagnostic.error = error; return error; };
    try {
        if (!state_ || state_->bound || !pid || !process || process == INVALID_HANDLE_VALUE || pid == GetCurrentProcessId() || GetProcessId(process) != pid)
            return fail(ERROR_INVALID_PARAMETER);
        diagnostic.retained_control = state_->control;
        if (const DWORD error = Allowed(process)) return fail(error);
        if (const DWORD error = Chain(state_->parent_control)) return fail(error);
        auto& session = *state_->session;
        FILETIME exit{}, kernel{}, user{};
        if (!GetProcessTimes(process, &session.target_created, &exit, &kernel, &user)) return fail(Error());
        if (CompareFileTime(&session.target_created, &state_->prepared) < 0) return fail(ERROR_INVALID_DATA);
        std::wstring image(kPathUnits, 0), canonical; DWORD count = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process, 0, image.data(), &count)) return fail(Error());
        image.resize(count);
        if (const DWORD error = Canonical(image, false, canonical)) return fail(error);
        // Do not instrument our own host/launcher when a bridge starts them.
        // Compare bound canonical images, never basename or command-line text.
        if (Same(canonical, session.host_path) || Same(canonical, session.launcher_path)) return fail(ERROR_NOT_SUPPORTED);
        if (wcscpy_s(session.target_path, canonical.c_str())) return fail(ERROR_FILENAME_EXCED_RANGE);
        session.target_pid = pid;
        if (const DWORD error = Identity(process, session)) return fail(error);
        diagnostic.stage = ChildSessionStage::Binding;
        if (const DWORD error = WriteSession(state_->control + L"\\" + kSessionFile, session)) return fail(error);
        state_->bound = true;
        diagnostic.stage = ChildSessionStage::Complete;
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) { return fail(ERROR_NOT_ENOUGH_MEMORY); }
    catch (...) { return fail(ERROR_INVALID_DATA); }
}
}
