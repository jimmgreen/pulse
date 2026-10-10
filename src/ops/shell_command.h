#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <string_view>

namespace pulse::ops {
struct ShellCommandParts { std::wstring executable, arguments; };
bool SplitShellCommand(std::wstring_view command, ShellCommandParts& parts);
struct ShellCommandResult {
    DWORD error = ERROR_SUCCESS;
    DWORD create_error = ERROR_SUCCESS;
    bool elevation_requested = false;
};
struct ShellCommandApi {
    decltype(&CreateProcessW) create_process = ::CreateProcessW;
    decltype(&ShellExecuteExW) shell_execute = ::ShellExecuteExW;
};
ShellCommandResult LaunchShellCommand(const std::wstring& command, const std::wstring& directory,
    HWND owner, const ShellCommandApi& api = {});
struct ShellItemResult {
    DWORD error = ERROR_SUCCESS;
    DWORD process_id = 0; // zero is valid for a delegated/packaged activation
    DWORD mask = 0;
};
// Empty verb means the registered default action, not a hard-coded "open".
// The caller owns COM initialization; handoff completes on its worker thread.
ShellItemResult LaunchShellItem(const std::wstring& file, const std::wstring& verb,
    const std::wstring& arguments, const std::wstring& directory, HWND owner,
    const ShellCommandApi& api = {});
struct TerminalLaunchResult {
    DWORD error = ERROR_SUCCESS;
    DWORD open_error = ERROR_SUCCESS;
    bool elevation_requested = false;
};
// CRT-compatible, always-quoted single argument; doubles backslashes before
// embedded/closing quotes. Does not interpret shell or template metacharacters.
std::wstring QuoteWindowsArgument(std::wstring_view value);
std::wstring TerminalCommandLine(const std::wstring& directory);
TerminalLaunchResult LaunchTerminal(const std::wstring& executable, const std::wstring& arguments,
    const std::wstring& directory, HWND owner, const ShellCommandApi& api = {});
}
