#include "../ops/shell_command.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pulse::ops;
namespace {
DWORD create_error = ERROR_ELEVATION_REQUIRED, shell_error = ERROR_SUCCESS;
unsigned shell_calls = 0;
std::wstring actual_file, actual_args, actual_dir, actual_verb;
HWND actual_owner = nullptr;
bool terminal_sequence = false;
DWORD terminal_open_error = ERROR_SUCCESS;
DWORD actual_mask = 0;
bool actual_null_verb = false, require_default_action = false, force_shell_failure = false;
HANDLE shell_process = nullptr;
BOOL WINAPI CreateStub(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    SetLastError(create_error); return create_error == ERROR_SUCCESS;
}
BOOL WINAPI ShellStub(SHELLEXECUTEINFOW* info) {
    ++shell_calls;
    actual_file = info->lpFile ? info->lpFile : L"";
    actual_args = info->lpParameters ? info->lpParameters : L"";
    actual_dir = info->lpDirectory ? info->lpDirectory : L"";
    actual_verb = info->lpVerb ? info->lpVerb : L"";
    actual_owner = info->hwnd;
    actual_mask = info->fMask;
    actual_null_verb = info->lpVerb == nullptr;
    info->hProcess = shell_process;
    const DWORD error = require_default_action && info->lpVerb ? ERROR_NO_ASSOCIATION
        : terminal_sequence && actual_verb == L"open" ? terminal_open_error : shell_error;
    SetLastError(error); return !force_shell_failure && error == ERROR_SUCCESS;
}
BOOL WINAPI CreateHidden(LPCWSTR image, LPWSTR command, LPSECURITY_ATTRIBUTES a, LPSECURITY_ATTRIBUTES b,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    startup->dwFlags |= STARTF_USESHOWWINDOW; startup->wShowWindow = SW_HIDE;
    return CreateProcessW(image, command, a, b, inherit, flags, environment, directory, startup, process);
}
bool TestShellItemLaunch() {
    bool ok = true;
    auto check = [&](bool condition, const char* label) {
        ok &= condition;
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    };
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    const ShellCommandApi stub{CreateStub, ShellStub};
    shell_calls = 0;
    shell_error = ERROR_SUCCESS;
    terminal_sequence = false;
    force_shell_failure = false;
    require_default_action = true;
    auto result = LaunchShellItem(L"\\\\?\\C:\\测试 图片\\photo.png", L"", L"",
        L"\\\\?\\C:\\测试 图片", owner, stub);
    check(!result.error && shell_calls == 1 && actual_null_verb,
        "default association uses null verb even when the default has no open verb");
    check(actual_file == L"C:\\测试 图片\\photo.png" && actual_dir == L"C:\\测试 图片" &&
        actual_args.empty() && actual_owner == owner,
        "default item preserves Unicode, spaces, working directory and owner");
    check((actual_mask & SEE_MASK_NOASYNC) && (actual_mask & SEE_MASK_FLAG_NO_UI) &&
        (actual_mask & SEE_MASK_NOCLOSEPROCESS) &&
        (actual_mask & SEE_MASK_INVOKEIDLIST) == SEE_MASK_INVOKEIDLIST && result.mask == actual_mask,
        "no-message-pump worker completes the shell handoff and invokes the default action");
    require_default_action = false;
    result = LaunchShellItem(L"\\\\?\\UNC\\server\\share\\图 片.bmp", L"", L"", L"", owner, stub);
    check(!result.error && actual_file == L"\\\\server\\share\\图 片.bmp" && actual_dir.empty(),
        "UNC extended prefix is stripped without inventing a working directory");
    result = LaunchShellItem(L"C:\\图 片.jpg", L"edit", L"/safe", L"C:\\", owner, stub);
    check(!result.error && actual_verb == L"edit" && !actual_null_verb && actual_args == L"/safe" &&
        (actual_mask & SEE_MASK_INVOKEIDLIST) == 0,
        "an explicitly requested verb is preserved and not replaced by the default");
    const std::wstring arguments = L"\"C:\\测试 图片\\photo.png\"";
    result = LaunchShellItem(L"mspaint.exe", L"open", arguments, L"C:\\测试 图片", owner, stub);
    check(!result.error && actual_file == L"mspaint.exe" && actual_verb == L"open" && actual_args == arguments,
        "explicit open-with application preserves executable and original argument quoting");
    shell_error = ERROR_NO_ASSOCIATION;
    auto before = shell_calls;
    result = LaunchShellItem(L"C:\\photo.png", L"", L"", L"", owner, stub);
    check(result.error == ERROR_NO_ASSOCIATION && shell_calls == before + 1,
        "missing association is returned to the caller without silent success or retries");
    shell_error = ERROR_CANCELLED;
    before = shell_calls;
    result = LaunchShellItem(L"C:\\photo.png", L"", L"", L"", owner, stub);
    check(result.error == ERROR_CANCELLED && shell_calls == before + 1,
        "shell cancellation is preserved without a fallback or another launch");
    shell_error = ERROR_ACCESS_DENIED;
    result = LaunchShellItem(L"C:\\photo.png", L"", L"", L"", owner, stub);
    check(result.error == ERROR_ACCESS_DENIED, "provider Win32 error is captured before handle cleanup");
    shell_error = ERROR_SUCCESS;
    force_shell_failure = true;
    result = LaunchShellItem(L"C:\\photo.png", L"", L"", L"", owner, stub);
    check(result.error == ERROR_GEN_FAILURE, "false shell result with no error code cannot appear successful");
    force_shell_failure = false;
    before = shell_calls;
    result = LaunchShellItem(L"", L"", L"", L"", owner, stub);
    check(result.error == ERROR_INVALID_PARAMETER && shell_calls == before,
        "empty item is rejected before shell invocation");
    shell_process = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HANDLE returned_handle = shell_process;
    result = LaunchShellItem(L"C:\\photo.png", L"", L"", L"", owner, stub);
    shell_process = nullptr;
    DWORD flags = 0;
    check(returned_handle && !result.error && !GetHandleInformation(returned_handle, &flags) &&
        GetLastError() == ERROR_INVALID_HANDLE, "returned shell handle is closed after handoff");
    require_default_action = false;
    force_shell_failure = false;
    shell_error = ERROR_SUCCESS;
    shell_calls = 0;
    return ok;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--association-only")
        return TestShellItemLaunch() ? 0 : 1;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--child") {
        wchar_t directory[32768]{}; GetCurrentDirectoryW(ARRAYSIZE(directory), directory);
        std::wofstream output(argv[2]); output << argv[3] << L'\n' << directory;
        return output ? 0 : 1;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-elevation") {
        std::wstring command = L"\"" + std::wstring(argv[2]) + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        // Never execute the downloaded application, even if it did not request elevation.
        if (created) { TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        std::cout << "created=" << created << " win32_error=" << error << '\n';
        return !created && error == ERROR_ELEVATION_REQUIRED ? 0 : 1;
    }
    bool ok = true;
    auto check = [&](bool condition, const char* label) { ok &= condition;
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n'; };
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"shell-command-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"工具 folder");
    const auto image = root / L"工具 folder" / L"menu helper.exe";
    std::filesystem::copy_file(self, image);
    const std::wstring arguments = L"/Monitor \"C:\\测试 文件\\setup.exe\" /mode=\"two words\"";
    const std::wstring command = L"\"" + image.wstring() + L"\" " + arguments;
    ShellCommandParts parts;
    check(SplitShellCommand(command, parts) && parts.executable == image.wstring() && parts.arguments == arguments,
        "quoted Unicode image and original argument quoting preserved");
    check(SplitShellCommand(image.wstring() + L" " + arguments, parts) && parts.executable == image.wstring() && parts.arguments == arguments,
        "unquoted executable path containing spaces resolved without truncation");
    check(!SplitShellCommand(L"\"unfinished", parts) && !SplitShellCommand(L"", parts), "malformed command rejected");
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    const ShellCommandApi stub{CreateStub, ShellStub};
    auto result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(!result.error && result.create_error == ERROR_ELEVATION_REQUIRED && result.elevation_requested && shell_calls == 1,
        "error 740 delegates exactly once to Windows elevation");
    check(actual_file == image.wstring() && actual_args == arguments && actual_dir == root.wstring() &&
        actual_verb == L"runas" && actual_owner == owner, "elevation preserves image, arguments, working directory and owner");
    shell_error = ERROR_CANCELLED;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_CANCELLED && shell_calls == 2, "UAC cancellation is preserved with no retry");
    shell_error = ERROR_ACCESS_DENIED;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_ACCESS_DENIED && result.create_error == ERROR_ELEVATION_REQUIRED,
        "elevation launch failure preserves both diagnostic error codes");
    const auto calls = shell_calls;
    create_error = ERROR_SUCCESS;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(!result.error && !result.elevation_requested && shell_calls == calls, "ordinary executable does not request elevation");
    create_error = ERROR_FILE_NOT_FOUND;
    result = LaunchShellCommand(command, root.wstring(), owner, stub);
    check(result.error == ERROR_FILE_NOT_FOUND && shell_calls == calls, "unrelated failures never trigger elevation");
    terminal_sequence = true;
    terminal_open_error = ERROR_SUCCESS;
    shell_calls = 0;
    const std::wstring terminal_dir = L"\\\\?\\C:\\工具 folder\\";
    const auto terminal_args = TerminalCommandLine(terminal_dir);
    check(terminal_args == L"-d \"C:\\工具 folder\\\\\"" &&
        TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"" &&
        TerminalCommandLine(L"\\\\?\\UNC\\server\\share\\") == L"-d \"\\\\server\\share\\\\\"",
        "terminal arguments strip extended prefixes and quote trailing directory separators");
    auto terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(!terminal.error && !terminal.elevation_requested && shell_calls == 1 && actual_verb == L"open" &&
        actual_file == L"wt.exe" && actual_args == terminal_args && actual_dir == L"C:\\工具 folder\\" &&
        actual_owner == owner && (actual_mask & SEE_MASK_NOASYNC) && (actual_mask & SEE_MASK_FLAG_NO_UI),
        "terminal normal launch preserves shell alias, arguments, normalized directory and owner");
    terminal_open_error = ERROR_ELEVATION_REQUIRED;
    shell_error = ERROR_SUCCESS;
    terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(!terminal.error && terminal.open_error == ERROR_ELEVATION_REQUIRED && terminal.elevation_requested &&
        shell_calls == 3 && actual_verb == L"runas" && actual_args == terminal_args && actual_owner == owner,
        "terminal error 740 requests elevation exactly once with original launch context");
    shell_error = ERROR_CANCELLED;
    terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(terminal.error == ERROR_CANCELLED && terminal.elevation_requested && shell_calls == 5,
        "terminal elevation cancellation returns without retry");
    shell_error = ERROR_ACCESS_DENIED;
    terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(terminal.error == ERROR_ACCESS_DENIED && terminal.open_error == ERROR_ELEVATION_REQUIRED && shell_calls == 7,
        "terminal elevation failure preserves both diagnostic codes");
    terminal_open_error = ERROR_CANCELLED;
    terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(terminal.error == ERROR_CANCELLED && !terminal.elevation_requested && shell_calls == 8,
        "terminal normal shell cancellation does not prompt again");
    terminal_open_error = ERROR_FILE_NOT_FOUND;
    terminal = LaunchTerminal(L"wt.exe", terminal_args, terminal_dir, owner, stub);
    check(terminal.error == ERROR_FILE_NOT_FOUND && !terminal.elevation_requested && shell_calls == 9,
        "missing terminal returns actionable error without elevation or fallback");
    terminal_sequence = false;
    const auto marker = root / L"child.txt";
    const auto live = LaunchShellCommand(L"\"" + image.wstring() + L"\" --child \"" + marker.wstring() +
        L"\" \"two words\"", root.wstring(), nullptr, {CreateHidden, ShellStub});
    const auto deadline = GetTickCount64() + 5000;
    while (!std::filesystem::exists(marker) && GetTickCount64() < deadline) Sleep(10);
    // Wait for the child to finish writing before comparing its captured values.
    std::wstring argument, directory;
    while (GetTickCount64() < deadline) {
        std::wifstream input(marker); std::getline(input, argument); std::getline(input, directory);
        if (directory == root.wstring()) break;
        Sleep(10);
    }
    check(!live.error && argument == L"two words" && directory == root.wstring(), "real non-elevated child receives argument and selected working directory");
    std::wcout << L"[INFO] isolated fixture=" << root.wstring() << L'\n';
    ok = TestShellItemLaunch() && ok;
    return ok ? 0 : 1;
}
