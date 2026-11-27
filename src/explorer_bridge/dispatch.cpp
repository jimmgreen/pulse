#include "dispatch.h"
#include "cold_start.h"
#include "../common/native_browse_launch.h"
#include <cwchar>
namespace pulse::explorer_bridge {
DWORD LaunchNativeExplorer(const browse::Request& request) noexcept {
    native_browse::Launch native_fallback;
    native_fallback.Prepare(request);
    try {
        const auto explorer = explorer_launch::SystemExplorer();
        if (explorer.empty()) return ERROR_PATH_NOT_FOUND;
        auto command = explorer_launch::Quote(explorer) +
            (request.intent == browse::Intent::Reveal ? L" /select, " : L" ") +
            explorer_launch::Quote(request.path);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        // Use the explicit system image with normal GUI visibility. Production
        // excludes this helper from process interception to prevent recursion.
        if (!CreateProcessW(explorer.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process))
            return GetLastError();
        native_fallback.Complete(true);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DirectRevealResult Send(const browse::Request& request) noexcept {
    const auto outcome = cold_start::Deliver(request);
    SetLastError(outcome.error);
    return outcome.result;
}
namespace {
template<class SendRequest>
DWORD DispatchImpl(int argc, const wchar_t* const* argv, SendRequest sender, NativeFallback fallback) noexcept {
    try {
        if ((argc != 4 && argc != 6) || !argv) return ERROR_INVALID_PARAMETER;
        for (int i = 0; i < argc; ++i) if (!argv[i]) return ERROR_INVALID_PARAMETER;
        explorer_launch::Request expected;
        if (argc == 6 && !wcscmp(argv[1], L"--explorer-launch-v1")) {
            if (!explorer_launch::Resolve(argv[4], argv[5], CREATE_UNICODE_ENVIRONMENT, expected) ||
                expected.path != argv[3] || (expected.select ? wcscmp(argv[2], L"select") : wcscmp(argv[2], L"folder")))
                return ERROR_INVALID_PARAMETER;
        } else if (argc == 4 && !wcscmp(argv[1], L"--browse-v1")) {
            const bool select = !wcscmp(argv[2], L"select");
            if (!select && wcscmp(argv[2], L"folder")) return ERROR_INVALID_PARAMETER;
            const auto explorer = explorer_launch::SystemExplorer();
            const auto command = explorer_launch::Quote(explorer) + (select ? L" /select, " : L" ") +
                explorer_launch::Quote(argv[3]);
            if (!explorer_launch::Resolve(explorer.c_str(), command.c_str(), CREATE_UNICODE_ENVIRONMENT, expected) ||
                expected.path != argv[3] || expected.select != select) return ERROR_NOT_SUPPORTED;
        } else return ERROR_INVALID_PARAMETER;
        const DWORD attributes = GetFileAttributesW(expected.path.c_str());
        const bool supported = attributes != INVALID_FILE_ATTRIBUTES &&
            (expected.select || (attributes & FILE_ATTRIBUTE_DIRECTORY));
        const browse::Request request{
            expected.select ? browse::Intent::Reveal : browse::Intent::OpenFolder, expected.path};
        SetLastError(ERROR_SUCCESS);
        const auto result = supported ? sender(request) : DirectRevealResult::Rejected;
        const DWORD error = GetLastError();
        if (result == DirectRevealResult::Accepted) return ERROR_SUCCESS;
        // The production caller explicitly opts into native recovery, including
        // uncertain ACKs. Never replay the Pulse request before that recovery.
        if (fallback) return fallback(request);
        if (result == DirectRevealResult::Indeterminate) return ERROR_TIMEOUT;
        if (result == DirectRevealResult::Unsupported) return ERROR_NOT_SUPPORTED;
        if (error) return error;
        return ERROR_NOT_READY;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
DWORD Dispatch(int argc, const wchar_t* const* argv, BrowseSender sender) noexcept {
    return Dispatch(argc, argv, sender, nullptr);
}
DWORD Dispatch(int argc, const wchar_t* const* argv, BrowseSender sender, NativeFallback fallback) noexcept {
    if (!sender) return ERROR_INVALID_PARAMETER;
    return DispatchImpl(argc, argv, sender, fallback);
}
DWORD Dispatch(int argc, const wchar_t* const* argv, Sender sender) noexcept {
    return Dispatch(argc, argv, sender, nullptr);
}
DWORD Dispatch(int argc, const wchar_t* const* argv, Sender sender, NativeFallback fallback) noexcept {
    if (!sender) return ERROR_INVALID_PARAMETER;
    return DispatchImpl(argc, argv, [&](const browse::Request& request) {
        return sender(explorer_launch::Request{request.intent == browse::Intent::Reveal, request.path});
    }, fallback);
}
}
