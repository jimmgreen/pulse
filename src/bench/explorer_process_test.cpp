#include "../shell_bridge/explorer_process_route.h"
#include "../explorer_bridge/dispatch.h"
#include <cstdio>
#include <shellapi.h>
#include <filesystem>
namespace {
bool enabled = true;
bool TestGate() noexcept { return enabled; }
unsigned calls = 0, sender_calls = 0;
bool create_ok = true;
std::wstring captured;
LPSTARTUPINFOW captured_startup = nullptr;
DWORD captured_flags = 0;
BOOL captured_inherit = TRUE;
bool captured_native_scope = false;
DWORD captured_incoming_error = 0;
LPSECURITY_ATTRIBUTES captured_attributes = nullptr;
pulse::DirectRevealResult reply = pulse::DirectRevealResult::Accepted;
BOOL WINAPI TestNative(LPCWSTR, LPWSTR command, LPSECURITY_ATTRIBUTES attributes, LPSECURITY_ATTRIBUTES, BOOL inherit,
    DWORD flags, LPVOID, LPCWSTR, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION info) {
    captured_native_scope = pulse::shell_bridge::explorer_process::NativeFallbackActive();
    captured_incoming_error = GetLastError();
    ++calls; captured = command; captured_startup = startup; captured_flags = flags; captured_inherit = inherit; captured_attributes = attributes;
    if (create_ok) { *info = {}; info->dwProcessId = 4242; }
    SetLastError(create_ok ? 123 : ERROR_FILE_NOT_FOUND);
    return create_ok;
}
pulse::DirectRevealResult Send(const pulse::explorer_launch::Request&) noexcept { ++sender_calls; return reply; }

}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using namespace pulse::shell_bridge::explorer_process;
    if (argc != 4) { std::puts("usage: test BRIDGE_EXE FIXTURE_DIRECTORY BRIDGE_SHA256"); return 2; }
    unsigned count = 0, failed = 0;
    auto check = [&](bool ok, const char* name) { ++count; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; };
    std::filesystem::create_directories(argv[2]);
    std::wstring folder = std::filesystem::absolute(argv[2]).wstring();
    const auto explorer = explorer_launch::SystemExplorer();
    auto command = explorer_launch::Quote(explorer) + L" " + explorer_launch::Quote(folder);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION info{}; BOOL result = FALSE;
    auto route = [&](DWORD flags = CREATE_UNICODE_ENVIRONMENT) {
        return TryRoute(TestNative, explorer.c_str(), command.data(), nullptr, nullptr, TRUE, flags,
            nullptr, folder.c_str(), &startup, &info, result);
    };
    Disable(); SetLastError(777);
    check(!route() && calls == 0 && GetLastError() == 777, "unconfigured route leaves native untouched and preserves LastError");
    check(Configure(nullptr, TestGate) == ERROR_INVALID_PARAMETER, "invalid owner cannot configure production bridge");
    std::string digest;
    for (const wchar_t ch : std::wstring(argv[3])) {
        if (ch > 127) return 2;
        digest.push_back(static_cast<char>(ch));
    }
    check(VerifyBridgeForTest(argv[1], digest) == 0, "held helper PE and SHA256 match deployment binding");
    check(VerifyBridgeForTest(argv[1], std::string(64, '0')) == ERROR_CRC, "wrong helper SHA256 rejected");
    check(VerifyBridgeForTest(argv[1], "invalid") == ERROR_INVALID_DATA, "malformed helper SHA256 rejected");
    check(ConfigureForTest(argv[1], TestGate) == 0, "test-only configuration holds actual bridge file");
    check(route() && result && info.dwProcessId == 4242 && calls == 1 && GetLastError() == 123,
        "matched call returns exact native creation result and LastError");
    int rewritten_count = 0; auto rewritten = CommandLineToArgvW(captured.c_str(), &rewritten_count);
    check(rewritten && rewritten_count == 4 && wcscmp(rewritten[1], L"--browse-v1") == 0 &&
        rewritten[3] == folder, "process adapter emits the common normalized browse request");
    if (rewritten) LocalFree(rewritten);
    BYTE descriptors[31]{}; startup.cbReserved2 = sizeof(descriptors); startup.lpReserved2 = descriptors;
    check(route() && result && captured_startup == &startup && captured_startup->cbReserved2 == 31 &&
        captured_startup->lpReserved2 == descriptors, "Node CRT descriptor inheritance is forwarded unchanged");
    check(route(CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT) && result &&
        captured_flags == (CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT) &&
        captured_startup->lpReserved2 == descriptors, "Electron flags and CRT descriptors reach real helper creation unchanged");
    startup.cbReserved2 = 0; startup.lpReserved2 = nullptr;
    check(ConfigureForTest(folder + L"\\missing-bridge.exe", TestGate) != 0 && route() && result,
        "failed candidate reconfiguration preserves the previously verified route");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    check(TryRoute(TestNative, explorer.c_str(), command.data(), &attributes, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, nullptr, folder.c_str(), &startup, &info, result) && result &&
        !captured_inherit && captured_attributes == &attributes, "ordinary non-inheriting caller and security attributes retain native child semantics");
    const auto saved_command = command;
    command = explorer_launch::Quote(explorer) + L" /select, " + explorer_launch::Quote(folder);
    check(route() && result && captured.find(L" --browse-v1 select ") != std::wstring::npos,
        "split select argv reaches unified reveal intent");
    command = saved_command;
    enabled = false; const unsigned before = calls;
    check(!route() && calls == before, "gate revocation bypasses redirect"); enabled = true;
    check(!route(CREATE_SUSPENDED) && calls == before, "unsupported Explorer suspension delegates unchanged to native");
    check(!route(EXTENDED_STARTUPINFO_PRESENT) && calls == before, "unsupported attribute contract delegates unchanged to native");
    SetClassificationFailureForTest(true); SetLastError(555);
    check(!route() && GetLastError() == 555 && calls == before,
        "classification allocation failure permits original native call and preserves incoming error");
    SetClassificationFailureForTest(false);
    create_ok = false; SetLastError(888);
    check(!route() && calls == before + 1 && GetLastError() == 888 && command == saved_command,
        "bridge creation failure returns untouched original request for native fallback");
    create_ok = true;
    check(TestNative(explorer.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
        nullptr, folder.c_str(), &startup, &info) && captured == saved_command && calls == before + 2,
        "caller fallback invokes original Explorer once with original command");
    SetLastError(777);
    {
        NativeFallbackScope scope;
        check(NativeFallbackActive() && !route() && GetLastError() == 777,
            "native scope prevents process reinterception and preserves LastError");
        { NativeFallbackScope nested; check(NativeFallbackActive(), "native bypass is nesting-safe"); }
        check(NativeFallbackActive(), "nested native scope retains outer bypass");
    }
    check(!NativeFallbackActive() && GetLastError() == 777, "native scope restores routing without changing LastError");
    RevokeForTest(ERROR_ACCESS_DISABLED_BY_POLICY);
    const auto revoked_calls = calls;
    check(!IsReady() && QueryStatus().error == ERROR_ACCESS_DISABLED_BY_POLICY && !route() && calls == revoked_calls,
        "revoked configuration delegates original native operation");
    Disable(); check(!route() && QueryStatus().readiness == Readiness::Disabled, "explicit disable restores native policy");
    check(ConfigureForTest(folder + L"\\missing-bridge.exe", TestGate) != 0 &&
        QueryStatus().readiness == Readiness::Unavailable, "failed initial setup is observable as unavailable");
    const auto unavailable_calls = calls;
    check(!route() && calls == unavailable_calls, "unavailable bridge delegates original native operation");
    command = explorer_launch::Quote(explorer) + L" shell:Downloads";
    check(!route() && calls == unavailable_calls, "unsupported namespace delegates original native operation");
    command = explorer_launch::Quote(explorer);
    check(!route() && calls == unavailable_calls, "no-operand desktop shell startup remains native");
    command = saved_command;
    wchar_t self[32768]{};
    const auto self_length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const auto self_calls = calls;
    check(self_length && self_length < ARRAYSIZE(self) && ConfigureForTest(self, TestGate) == 0 &&
        !route() && calls == self_calls, "configured helper process never redirects its own native recovery back to itself");
    Disable();
    const auto native_before = calls;
    create_ok = true; SetLastError(9876);
    const auto native_ok = FallbackCreateProcess(TestNative, explorer.c_str(), command.data(), &attributes,
        nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, folder.c_str(), &startup, &info);
    const auto native_success_error = GetLastError();
    check(native_ok && calls == native_before + 1 && captured == command &&
        captured_flags == CREATE_NEW_PROCESS_GROUP && captured_startup == &startup &&
        !captured_inherit && captured_attributes == &attributes && captured_native_scope &&
        captured_incoming_error == 9876 && native_success_error == 123 && !NativeFallbackActive(),
        "original native fallback called once with exact arguments, scoped bypass and LastError");
    create_ok = false; SetLastError(8765);
    check(!FallbackCreateProcess(TestNative, explorer.c_str(), command.data(), nullptr, nullptr, TRUE,
        0, nullptr, nullptr, &startup, &info) && GetLastError() == ERROR_FILE_NOT_FOUND &&
        calls == native_before + 2 && captured_incoming_error == 8765 && !NativeFallbackActive(),
        "native failure and error are returned without retry or leaked bypass");
    create_ok = true;
    auto unrelated_command = L"unrelated.exe "+explorer_launch::Quote(folder);
    SetLastError(7654);
    check(FallbackCreateProcess(TestNative, L"unrelated.exe", unrelated_command.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &startup, &info) && calls == native_before + 3 &&
        !captured_native_scope && captured_incoming_error == 7654 && GetLastError() == 123,
        "unrelated native creation is not wrapped in a browsing bypass");
    const wchar_t* args[]{argv[1], L"--explorer-launch-v1", L"folder", folder.c_str(), explorer.c_str(), command.c_str()};
    check(explorer_bridge::Dispatch(6,args,Send)==ERROR_SUCCESS && sender_calls==1,
        "accepted dispatch succeeds without any Explorer fallback");
    reply=DirectRevealResult::Rejected;
    check(explorer_bridge::Dispatch(6,args,Send)==ERROR_NOT_READY && sender_calls==2,
        "explicit rejection returns not-ready without launching Explorer");
    reply=DirectRevealResult::Indeterminate;
    check(explorer_bridge::Dispatch(6,args,Send)==ERROR_TIMEOUT && sender_calls==3,
        "ambiguous ACK returns timeout without launching Explorer");
    args[3]=L"C:\\different";
    check(explorer_bridge::Dispatch(6,args,Send)==ERROR_INVALID_PARAMETER && sender_calls==3,
        "tampered helper operands rejected before delivery");
    std::printf("RESULT tests=%u failed=%u\n",count,failed);return failed?1:0;
}
