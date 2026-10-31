#include "../shell_bridge/file_dialog_runtime.h"
#include "../shell_bridge/runtime_child_injection.h"
#include "runtime_child_open_cancel.h"
#include <windows.h>
#include <filesystem>
#include <string>
#include <cstdio>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) {
    ++checks; if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
}
HMODULE RuntimeModule() {
#ifdef _WIN64
    return GetModuleHandleW(L"PulseFileDialogRuntime64.dll");
#else
    return GetModuleHandleW(L"PulseFileDialogRuntime32.dll");
#endif
}
bool Ready() {
    const auto module = RuntimeModule();
    using Query = HRESULT (WINAPI*)(PulseFileDialogRuntimeStatus*, uint32_t);
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseFileDialogRuntimeQuery")) : nullptr;
    PulseFileDialogRuntimeStatus status;
    return query && query(&status, sizeof(status)) == S_OK && status.ready && status.process_id == GetCurrentProcessId();
}
std::wstring Environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, 0);
    const DWORD count = GetEnvironmentVariableW(name, result.data(), size);
    result.resize(count); return result;
}
std::wstring Image() {
    std::wstring result(32768, 0);
    const DWORD count = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    result.resize(count); return result;
}
bool Spawn(const std::wstring& mode, DWORD flags, PROCESS_INFORMATION& process, void* environment = nullptr) {
    auto command = L"\"" + Image() + L"\" " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    return CreateProcessW(Image().c_str(), command.data(), nullptr, nullptr, FALSE, flags, environment,
        nullptr, &startup, &process) != FALSE;
}
void Finish(PROCESS_INFORMATION& process, const char* label, DWORD timeout = 15000) {
    const DWORD wait = WaitForSingleObject(process.hProcess, timeout);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &code);
    Check(wait == WAIT_OBJECT_0 && code == 0, label);
    // This executable owns these fixture children; production hook never kills.
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 90); WaitForSingleObject(process.hProcess, 5000); }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); process = {};
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--parent" && argc > 2) {
        if (argc != 3 || !std::filesystem::path(argv[2]).is_absolute()) return 2;
        int report = -1;
        if (_wsopen_s(&report, argv[2], _O_WRONLY | _O_CREAT | _O_EXCL | _O_TEXT | _O_NOINHERIT,
            _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0) return 2;
        const int redirected = _dup2(report, _fileno(stdout));
        _close(report);
        if (redirected != 0 || !SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))),
            HANDLE_FLAG_INHERIT, 0)) return 2;
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
    if (mode == L"--child-open-cancel") return RunRuntimeChildOpenCancel();
    if (mode == L"--child-native") return RuntimeModule() ? 10 : 0;
    if (mode == L"--child-ready" || mode == L"--child-env-a" || mode == L"--child-env-w" ||
        mode == L"--child-parent-death" || mode == L"--child-inherited" || mode == L"--child-revoke") {
        const auto deadline = GetTickCount64() + 10000;
        while (!Ready() && GetTickCount64() < deadline) Sleep(10);
        if (!Ready()) return 11;
        std::printf("CHILD_READY pid=%lu control=%ls\n", GetCurrentProcessId(), Environment(L"PULSE_DIALOG_CONTROL").c_str());
        if (mode == L"--child-ready") return 0;
        if (mode == L"--child-inherited") {
            if (argc != 3) return 23;
            const auto inherited = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10)));
            return WaitForSingleObject(inherited, 0) == WAIT_OBJECT_0 ? 0 : 24;
        }
        if (mode == L"--child-env-a") return Environment(L"PULSE_CHILD_ENV_CHECK") == L"explicit-ansi-exact" ? 0 : 15;
        if (mode == L"--child-env-w") return Environment(L"PULSE_CHILD_ENV_CHECK") == L"unicode-\x4e2d\U0001F642" ? 0 : 16;
        if (mode == L"--child-parent-death") {
            if (argc != 4) return 17;
            HANDLE ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
            HANDLE done_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
            if (!ready_event || !done_event) return 18;
            SetEvent(ready_event);
            const auto timeout = GetTickCount64() + 10000;
            while (Ready() && GetTickCount64() < timeout) Sleep(10);
            const bool revoked = !Ready();
            if (revoked) SetEvent(done_event);
            CloseHandle(ready_event); CloseHandle(done_event); return revoked ? 0 : 19;
        }
        if (argc != 3) return 12;
        HANDLE event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (!event) return 13;
        SetEvent(event);
        const auto revoked_deadline = GetTickCount64() + 10000;
        while (Ready() && GetTickCount64() < revoked_deadline) Sleep(10);
        const bool revoked = !Ready(); CloseHandle(event); return revoked ? 0 : 14;
    }
    if (mode == L"--middle-exit" && argc == 4) {
        const auto timeout = GetTickCount64() + 10000;
        while (!Ready() && GetTickCount64() < timeout) Sleep(10);
        if (!Ready()) return 20;
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        PROCESS_INFORMATION descendant{};
        if (!event || !Spawn(L"--child-parent-death \"" + std::wstring(argv[2]) + L"\" \"" + argv[3] + L"\"", 0, descendant)) return 21;
        const bool observed = WaitForSingleObject(event, 12000) == WAIT_OBJECT_0;
        CloseHandle(event); CloseHandle(descendant.hThread); CloseHandle(descendant.hProcess);
        return observed ? 0 : 22;
    }
    if (mode == L"--rollback" && argc == 3) {
        // Standalone, without a runtime-injected parent. Only this newly created
        // suspended fixture child is mutated; force failure after real Detours.
        Check(!RuntimeModule(), "rollback fixture parent is not intercepted");
        PROCESS_INFORMATION child{};
        if (!Spawn(L"--child-native", CREATE_SUSPENDED, child)) return 2;
        const UINT codepage = GetACP();
        BOOL replaced = FALSE;
        const DWORD conversion_flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        BOOL* replacement = codepage == CP_UTF8 ? nullptr : &replaced;
        const int count = WideCharToMultiByte(codepage, conversion_flags, argv[2], -1, nullptr, 0, nullptr, replacement);
        std::string dll(count > 0 ? static_cast<size_t>(count) : 0, 0);
        bool exact_path = count > 0 && !replaced && WideCharToMultiByte(codepage, conversion_flags, argv[2], -1,
            dll.data(), count, nullptr, replacement) != 0 && !replaced;
        std::wstring roundtrip(wcslen(argv[2]) + 1, 0);
        exact_path = exact_path && MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, dll.c_str(), -1,
            roundtrip.data(), static_cast<int>(roundtrip.size())) == static_cast<int>(roundtrip.size()) &&
            std::wstring(roundtrip.c_str()) == argv[2];
        Check(exact_path, "runtime DLL path converts losslessly for Detours");
        if (!exact_path) {
            ResumeThread(child.hThread); Finish(child, "unsupported DLL path preserves native fixture"); return 1;
        }
        const auto result = pulse::shell_bridge::runtime::InjectChildRuntime(child.hProcess, dll.c_str(), true);
        Check(!result.injected && result.restored && !result.rollback_failed && result.error != 0,
            "real post-update failure restores original header bytes and protections");
        Check(ResumeThread(child.hThread) == 1, "rollback preserves caller suspension until explicit resume");
        Finish(child, "restored child runs natively without runtime DLL");
    } else if (mode == L"--parent") {
        const auto deadline = GetTickCount64() + 10000;
        while (!Ready() && GetTickCount64() < deadline) Sleep(10);
        Check(Ready(), "launcher-bound fixture parent is ready");
        if (!Ready()) return 2;
        PROCESS_INFORMATION child{};
        if (Spawn(L"--child-ready", 0, child)) Finish(child, "new child receives distinct PID-bound ready runtime");
        else Check(false, "create ordinary fixture child");
        if (Spawn(L"--child-open-cancel", 0, child)) Finish(child, "child raw default Open shows real isolated Pulse UI and cancels cleanly", 45000);
        else Check(false, "create child Open cancellation fixture");
        if (Spawn(L"--child-ready", CREATE_SUSPENDED, child)) {
            Check(WaitForSingleObject(child.hProcess, 50) == WAIT_TIMEOUT, "caller-suspended child does not execute");
            Check(ResumeThread(child.hThread) == 1, "caller suspension is retained exactly once");
            Finish(child, "caller-suspended child starts with ready runtime after resume");
        } else Check(false, "create suspended fixture child");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE inherited = CreateEventW(&attributes, TRUE, TRUE, nullptr);
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> attribute_storage(bytes);
        auto* attribute_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        const bool initialized_attributes = inherited && bytes && InitializeProcThreadAttributeList(attribute_list, 1, 0, &bytes);
        const bool updated_attributes = initialized_attributes && UpdateProcThreadAttribute(attribute_list, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited, sizeof(inherited), nullptr, nullptr);
        Check(updated_attributes, "prepare owned extended startup handle-list fixture");
        if (updated_attributes) {
            STARTUPINFOEXW extended{}; extended.StartupInfo.cb = sizeof(extended); extended.lpAttributeList = attribute_list;
            auto command = L"\"" + Image() + L"\" --child-inherited " + std::to_wstring(reinterpret_cast<uintptr_t>(inherited));
            if (CreateProcessW(Image().c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr, &extended.StartupInfo, &child)) Finish(child, "opaque extended handle-list survives child routing");
            else Check(false, "create extended startup fixture child");
        }
        if (initialized_attributes) DeleteProcThreadAttributeList(attribute_list);
        if (inherited) CloseHandle(inherited);
        // Explicit A/W blocks preserve unrelated values; NULL must retain
        // inherited Unicode even when CREATE_UNICODE_ENVIRONMENT was absent.
        const auto old_value = Environment(L"PULSE_CHILD_ENV_CHECK");
        const std::wstring unicode_value = L"unicode-\x4e2d\U0001F642";
        Check(SetEnvironmentVariableW(L"PULSE_CHILD_ENV_CHECK", unicode_value.c_str()) != FALSE, "set private inherited Unicode fixture");
        if (Spawn(L"--child-env-w", 0, child)) Finish(child, "NULL environment preserves inherited Unicode values");
        else Check(false, "create NULL-environment child");
        std::wstring wide_block = L"SystemRoot=" + Environment(L"SystemRoot");
        wide_block.push_back(0); wide_block += L"PULSE_CHILD_ENV_CHECK=" + unicode_value;
        wide_block.push_back(0); wide_block.push_back(0);
        if (Spawn(L"--child-env-w", CREATE_UNICODE_ENVIRONMENT, child, wide_block.data())) Finish(child, "explicit Unicode environment remains exact");
        else Check(false, "create Unicode-environment child");
        std::string ansi_block = "PULSE_CHILD_ENV_CHECK=explicit-ansi-exact";
        ansi_block.push_back(0); ansi_block.push_back(0);
        if (Spawn(L"--child-env-a", 0, child, ansi_block.data())) Finish(child, "explicit ANSI environment remains exact");
        else Check(false, "create ANSI-environment child");
        Check(SetEnvironmentVariableW(L"PULSE_CHILD_ENV_CHECK", old_value.empty() ? nullptr : old_value.c_str()) != FALSE,
            "restore fixture process environment");
        const auto death_name = L"Local\\PulseChildDeath-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto done_name = death_name + L"-done";
        HANDLE death_ready = CreateEventW(nullptr, TRUE, FALSE, death_name.c_str());
        const bool fresh_ready = death_ready && GetLastError() != ERROR_ALREADY_EXISTS;
        HANDLE death_done = CreateEventW(nullptr, TRUE, FALSE, done_name.c_str());
        const bool fresh_done = death_done && GetLastError() != ERROR_ALREADY_EXISTS;
        if (!fresh_ready || !fresh_done) return 2;
        if (Spawn(L"--middle-exit \"" + death_name + L"\" \"" + done_name + L"\"", 0, child)) {
            Finish(child, "intermediate parent exits after its descendant is ready");
            Check(WaitForSingleObject(death_done, 12000) == WAIT_OBJECT_0, "parent death revokes already-ready descendant");
        } else Check(false, "create intermediate parent fixture");
        CloseHandle(death_ready); CloseHandle(death_done);
        STARTUPINFOW startup{sizeof(startup)};
        std::wstring missing = Image() + L".missing";
        SetLastError(ERROR_SUCCESS);
        const BOOL created = CreateProcessW(missing.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child);
        const DWORD error = GetLastError();
        Check(!created && error == ERROR_FILE_NOT_FOUND, "native creation failure and LastError preserved");
        HANDLE token = nullptr, restricted = nullptr;
        const bool restricted_ready = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token) &&
            CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted) &&
            ImpersonateLoggedOnUser(restricted);
        Check(restricted_ready, "fixture establishes unsupported impersonated thread");
        if (restricted_ready) {
            const bool launched = Spawn(L"--child-native", 0, child);
            const BOOL reverted = RevertToSelf();
            Check(reverted != FALSE, "fixture restores original thread identity");
            if (launched) Finish(child, "unsupported thread token preserves native child creation");
            else Check(false, "create native child under thread impersonation");
        }
        if (restricted) CloseHandle(restricted); if (token) CloseHandle(token);
        const auto event_name = L"Local\\PulseChildRevoke-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        if (!event || GetLastError() == ERROR_ALREADY_EXISTS) return 2;
        const bool launched = Spawn(L"--child-revoke \"" + event_name + L"\"", 0, child);
        Check(launched && WaitForSingleObject(event, 12000) == WAIT_OBJECT_0, "descendant ready before tree revocation");
        const auto marker = Environment(L"PULSE_DIALOG_CONTROL") + L"\\disable";
        HANDLE disable = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(disable != INVALID_HANDLE_VALUE, "fixture disables only its unique launcher session");
        if (disable != INVALID_HANDLE_VALUE) CloseHandle(disable);
        if (launched) Finish(child, "parent disable revokes an existing child runtime");
        CloseHandle(event);
        if (Spawn(L"--child-native", 0, child)) Finish(child, "disabled tree creates future children natively");
        else Check(false, "create child after disable");
    } else { std::printf("Use --parent through launcher, or --rollback absolute-runtime-dll\n"); return 2; }
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    if (std::fflush(stdout) != 0) return 2;
    return failures ? 1 : 0;
}
