#include "shell_command.h"
#include "shell_command_template.h"
#include "../common/path_utils.h"
#include <vector>

namespace pulse::ops {
namespace {
bool Space(wchar_t c) { return c == L' ' || c == L'\t'; }
bool ResolveExecutable(std::wstring candidate, std::wstring& resolved) {
    if (candidate.empty() || candidate.find(L'"') != std::wstring::npos) return false;
    std::vector<wchar_t> buffer(32768);
    // Resolve against the caller's search rules, before ShellExecute applies
    // the selected item's working directory to relative executable names.
    DWORD length = SearchPathW(nullptr, candidate.c_str(), L".exe", static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) return false;
    resolved.assign(buffer.data(), length);
    const DWORD attributes = GetFileAttributesW(resolved.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
}
bool SplitShellCommand(std::wstring_view command, ShellCommandParts& parts) {
    parts = {};
    size_t start = 0;
    while (start < command.size() && Space(command[start])) ++start;
    if (start == command.size() || command.find_first_of(L"\r\n\0", 0, 3) != std::wstring_view::npos) return false;
    size_t end = start;
    if (command[start] == L'"') {
        end = command.find(L'"', start + 1);
        if (end == std::wstring_view::npos || end == start + 1 ||
            (end + 1 < command.size() && !Space(command[end + 1]))) return false;
        if (!ResolveExecutable(std::wstring(command.substr(start + 1, end - start - 1)), parts.executable)) return false;
        ++end;
    } else {
        // CreateProcess tries each whitespace-delimited prefix for unquoted
        // paths containing spaces. Use the same first-existing-image order.
        for (; end <= command.size(); ++end) {
            if (end < command.size() && !Space(command[end])) continue;
            if (ResolveExecutable(std::wstring(command.substr(start, end - start)), parts.executable)) break;
        }
        if (parts.executable.empty()) return false;
    }
    while (end < command.size() && Space(command[end])) ++end;
    parts.arguments.assign(command.substr(end));
    return true;
}
ShellCommandResult LaunchShellCommand(const std::wstring& command, const std::wstring& directory,
    HWND owner, const ShellCommandApi& api) {
    if (command.empty()) return {ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, false};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    const auto working = pulse::path::StripExtendedPathPrefix(directory);
    const wchar_t* working_ptr = working.empty() ? nullptr : working.c_str();
    if (api.create_process(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, working_ptr, &startup, &process)) {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        return {};
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ELEVATION_REQUIRED) return {error, error, false};
    ShellCommandParts parts;
    if (!SplitShellCommand(command, parts)) return {ERROR_BAD_PATHNAME, error, false};
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = parts.executable.c_str();
    execute.lpParameters = parts.arguments.empty() ? nullptr : parts.arguments.c_str();
    execute.lpDirectory = working_ptr;
    execute.nShow = SW_SHOWNORMAL;
    if (!api.shell_execute(&execute)) return {GetLastError(), error, true};
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return {ERROR_SUCCESS, error, true};
}

DWORD LaunchShellNew(const std::wstring& directory, const std::wstring& stem,
    const std::wstring& extension, const std::wstring& command, const ShellCommandApi& api) {
    if (directory.empty() || stem.empty() || extension.empty()) return ERROR_INVALID_PARAMETER;
    const bool shortcut = path::EqualInsensitive(extension, L".lnk");
    const std::wstring working = path::StripExtendedPathPrefix(directory);
    if (working.starts_with(L"\\\\?\\") || working.starts_with(L"\\??\\"))
        return ERROR_NOT_SUPPORTED;
    std::wstring image, line;
    if (shortcut) {
        wchar_t system[MAX_PATH]{};
        const UINT count = GetSystemDirectoryW(system, ARRAYSIZE(system));
        if (!count) return GetLastError();
        if (count >= ARRAYSIZE(system)) return ERROR_INSUFFICIENT_BUFFER;
        image = std::wstring(system, count) + L"\\rundll32.exe";
        line = pulse::QuoteWindowsArgument(image) + L" " +
            pulse::QuoteWindowsArgument(std::wstring(system, count) + L"\\appwiz.cpl") + L",NewLinkHere ";
    } else {
        if (command.empty()) return ERROR_INVALID_PARAMETER;
        const DWORD capacity = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
        if (!capacity) return GetLastError();
        if (capacity > 32767) return ERROR_FILENAME_EXCED_RANGE;
        line.resize(capacity);
        const DWORD written = ExpandEnvironmentStringsW(command.c_str(), line.data(), capacity);
        if (!written) return GetLastError();
        if (written > capacity) return ERROR_INSUFFICIENT_BUFFER;
        line.resize(written - 1);
    }

    std::wstring full;
    BY_HANDLE_FILE_INFORMATION created{};
    bool identified = false;
    for (unsigned i = 1;; ++i) {
        full = directory;
        if (!full.ends_with(L"\\")) full += L'\\';
        full += stem;
        if (i > 1) full += L" (" + std::to_wstring(i) + L")";
        full += extension;
        if (!shortcut) {
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) return error;
            break;
        }
        // The legacy wizard opens with an extended path but cannot save from
        // it. Reject oversized paths before creating a zero-byte placeholder.
        if (path::StripExtendedPathPrefix(full).size() >= MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
        const HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            identified = GetFileInformationByHandle(file, &created) != FALSE;
            CloseHandle(file);
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            if (error == ERROR_ACCESS_DENIED && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            return error;
        }
    }

    if (shortcut) {
        // Rundll passes the remainder verbatim: NewLinkHere expects an
        // unquoted, literal path, including spaces and percent signs.
        line += path::StripExtendedPathPrefix(full);
    } else {
        bool replaced = false;
        line = ExpandShellCommand(line, full, &replaced);
        if (!replaced) line += L" " + pulse::QuoteWindowsArgument(path::StripExtendedPathPrefix(full));
    }
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (api.create_process(image.empty() ? nullptr : image.c_str(), line.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, working.c_str(), &startup, &process)) {
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        return ERROR_SUCCESS;
    }

    const DWORD error = GetLastError();
    if (identified) {
        // Launch failed. Remove only the still-empty file we created, not a
        // shortcut another process might have written or put in its place.
        const HANDLE file = CreateFileW(full.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(file, &info) && !info.nFileSizeLow && !info.nFileSizeHigh &&
                info.dwVolumeSerialNumber == created.dwVolumeSerialNumber &&
                info.nFileIndexHigh == created.nFileIndexHigh && info.nFileIndexLow == created.nFileIndexLow) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
            }
            CloseHandle(file);
        }
    }
    return error;
}

ShellItemResult LaunchShellItem(const std::wstring& file, const std::wstring& verb,
    const std::wstring& arguments, const std::wstring& directory, HWND owner,
    const ShellCommandApi& api) {
    if (file.empty()) return {ERROR_INVALID_PARAMETER};
    const auto shell_file = pulse::path::StripExtendedPathPrefix(file);
    const auto working = pulse::path::StripExtendedPathPrefix(directory);
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    // This STA worker waits on a condition variable, not a message pump.
    // DDE/COM association handoff must finish before it goes idle again.
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    if (verb.empty()) execute.fMask |= SEE_MASK_INVOKEIDLIST;
    execute.hwnd = owner;
    execute.lpVerb = verb.empty() ? nullptr : verb.c_str();
    execute.lpFile = shell_file.c_str();
    execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    execute.lpDirectory = working.empty() ? nullptr : working.c_str();
    execute.nShow = SW_SHOWNORMAL;
    SetLastError(ERROR_SUCCESS);
    const BOOL accepted = api.shell_execute(&execute);
    const DWORD reported = accepted ? ERROR_SUCCESS : GetLastError();
    const DWORD error = accepted ? ERROR_SUCCESS : (reported ? reported : ERROR_GEN_FAILURE);
    const DWORD process_id = accepted && execute.hProcess ? GetProcessId(execute.hProcess) : 0;
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return {error, process_id, execute.fMask};
}
TerminalLaunchResult LaunchTerminal(const std::wstring& executable, const std::wstring& arguments,
    const std::wstring& directory, HWND owner, const ShellCommandApi& api) {
    if (executable.empty()) return {ERROR_INVALID_PARAMETER, ERROR_INVALID_PARAMETER, false};
    const auto working = pulse::path::StripExtendedPathPrefix(directory);
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"open";
    execute.lpFile = executable.c_str();
    execute.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    execute.lpDirectory = working.empty() ? nullptr : working.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (api.shell_execute(&execute)) {
        if (execute.hProcess) CloseHandle(execute.hProcess);
        return {};
    }
    const DWORD error = GetLastError();
    // The normal shell launch may already prompt. Only retry a reported 740;
    // cancellation and other failures must not cause another UAC prompt.
    if (error != ERROR_ELEVATION_REQUIRED) return {error, error, false};
    execute.lpVerb = L"runas";
    execute.hProcess = nullptr;
    if (!api.shell_execute(&execute)) return {GetLastError(), error, true};
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return {ERROR_SUCCESS, error, true};
}

TerminalLaunchResult LaunchTerminalOrShell(const std::wstring& wt_arguments, const std::wstring& directory,
    HWND owner, const ShellCommandApi& api, std::wstring* launched) {
    // Only "the program is not there" moves on; a cancelled UAC prompt or a
    // bad folder is the user's answer and stops here.
    const auto missing = [](DWORD error) {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_ASSOCIATION;
    };
    if (launched) *launched = L"wt.exe";
    auto result = LaunchTerminal(L"wt.exe", wt_arguments, directory, owner, api);
    if (!missing(result.error)) return result;
    for (const wchar_t* shell : {L"pwsh.exe", L"powershell.exe", L"cmd.exe"}) {
        if (launched) *launched = shell;
        result = LaunchTerminal(shell, {}, directory, owner, api);
        if (!missing(result.error)) return result;
    }
    return result;
}

std::wstring QuoteWindowsArgument(std::wstring_view value) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(c);
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(c);
        }
        slashes = 0;
    }
    // Backslashes immediately before a closing quote must be doubled.
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring TerminalCommandLine(const std::wstring& dir) {
    return L"-d " + QuoteWindowsArgument(pulse::path::StripExtendedPathPrefix(dir));
}

}
