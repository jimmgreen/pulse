#include "../explorer_bridge/dispatch.h"
#include <filesystem>
#include <cstdio>

namespace {
unsigned sent = 0;
pulse::browse::Request captured;
pulse::DirectRevealResult reply = pulse::DirectRevealResult::Accepted;
DWORD send_error = ERROR_SUCCESS;
unsigned native_calls = 0;
DWORD native_error = ERROR_SUCCESS;
pulse::browse::Request native_request;
pulse::DirectRevealResult Send(const pulse::browse::Request& request) noexcept {
    ++sent; captured = request; SetLastError(send_error); return reply;
}
DWORD Native(const pulse::browse::Request& request) noexcept {
    ++native_calls; native_request = request; return native_error;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto folder = std::filesystem::absolute(argv[1]).wstring();
    std::filesystem::create_directories(folder);
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const wchar_t* args[]{L"helper.exe", L"--browse-v1", L"folder", folder.c_str()};
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_SUCCESS && sent == 1 &&
          captured.intent == pulse::browse::Intent::OpenFolder && captured.path == folder,
          "browse folder dispatch delivers the shared typed request");
    args[2] = L"select";
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_SUCCESS && sent == 2 &&
          captured.intent == pulse::browse::Intent::Reveal,
          "browse selection dispatch preserves Reveal intent");
    reply = pulse::DirectRevealResult::Indeterminate;
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_TIMEOUT && sent == 3,
          "uncertain ACK returns real timeout without replay");
    reply = pulse::DirectRevealResult::Rejected; send_error = ERROR_ACCESS_DENIED;
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_ACCESS_DENIED && sent == 4,
          "cold-start validation error reaches the helper exit result");
    args[2] = L"execute";
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_INVALID_PARAMETER && sent == 4,
          "unknown browse intent is rejected before delivery");
    args[2] = L"folder"; args[3] = L"\\\\server\\share";
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_NOT_SUPPORTED && sent == 4,
          "unsupported UNC path is explicit and never delivered as success");
    args[3] = L"relative";
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_NOT_SUPPORTED && sent == 4,
          "unresolved relative browse path never becomes a native fallback");

    args[3] = folder.c_str();
    reply = pulse::DirectRevealResult::Accepted; send_error = ERROR_SUCCESS;
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_SUCCESS && sent == 1 && native_calls == 0,
          "accepted Pulse request never invokes native fallback");
    reply = pulse::DirectRevealResult::Rejected; send_error = ERROR_NOT_READY;
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_SUCCESS && sent == 1 && native_calls == 1 &&
          native_request.intent == pulse::browse::Intent::OpenFolder && native_request.path == folder,
          "rejected folder request invokes native fallback exactly once");
    args[2] = L"select";
    reply = pulse::DirectRevealResult::Indeterminate; send_error = ERROR_TIMEOUT;
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_SUCCESS && sent == 1 && native_calls == 1 &&
          native_request.intent == pulse::browse::Intent::Reveal && native_request.path == folder,
          "uncertain ACK falls back exactly once without another Pulse submission");
    reply = pulse::DirectRevealResult::Unsupported; send_error = ERROR_NOT_SUPPORTED;
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_SUCCESS && sent == 1 && native_calls == 1,
          "unsupported sender result falls back exactly once for a validated request");
    reply = pulse::DirectRevealResult::Rejected; send_error = ERROR_NOT_READY; native_error = ERROR_ACCESS_DENIED;
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_ACCESS_DENIED && sent == 1 && native_calls == 1,
          "native launch error replaces the Pulse transport error");
    native_error = ERROR_SUCCESS;
    args[2] = L"execute"; sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_INVALID_PARAMETER && sent == 0 && native_calls == 0,
          "malformed intent invokes neither sender nor native fallback");
    args[2] = L"folder"; args[3] = L"relative";
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_NOT_SUPPORTED && sent == 0 && native_calls == 0,
          "unresolved operand invokes neither sender nor native fallback");
    args[3] = L"C:\\bad\" /select, C:\\other";
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_NOT_SUPPORTED && sent == 0 && native_calls == 0,
          "command injection operand never reaches native fallback");
    args[3] = L"\\\\server\\share";
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_NOT_SUPPORTED && sent == 0 && native_calls == 0,
          "unsupported UNC input does not bypass CLI validation through native fallback");
    const auto explorer = pulse::explorer_launch::SystemExplorer();
    const auto original = pulse::explorer_launch::Quote(explorer) + L" " + pulse::explorer_launch::Quote(folder);
    const wchar_t* legacy[]{L"helper.exe", L"--explorer-launch-v1", L"select", folder.c_str(), explorer.c_str(), original.c_str()};
    check(pulse::explorer_bridge::Dispatch(6, legacy, Send, Native) == ERROR_INVALID_PARAMETER && sent == 0 && native_calls == 0,
          "legacy intent mismatch never reaches native fallback");
    legacy[2] = L"folder";
    check(pulse::explorer_bridge::Dispatch(6, legacy, Send, Native) == ERROR_SUCCESS && sent == 1 && native_calls == 1 &&
          native_request.path == folder && native_request.intent == pulse::browse::Intent::OpenFolder,
          "validated legacy CLI also uses one native recovery");
    const auto missing = folder + L"\\pulse-missing-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    args[2] = L"folder"; args[3] = missing.c_str(); sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send, Native) == ERROR_SUCCESS && sent == 0 && native_calls == 1 &&
          native_request.path == missing && native_request.intent == pulse::browse::Intent::OpenFolder,
          "valid but missing path reaches native handling once without Pulse submission");
    sent = 0; native_calls = 0;
    check(pulse::explorer_bridge::Dispatch(4, args, Send) == ERROR_NOT_READY && sent == 0 && native_calls == 0,
          "legacy three-argument overload preserves missing-path failure without fallback");
    std::printf("RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
