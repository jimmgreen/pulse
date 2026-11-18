#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::shell_open_entry {
inline constexpr wchar_t kArgument[] = L"--shell-open";
inline constexpr wchar_t kThisPc[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
inline constexpr wchar_t kRecycleBin[] = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

inline std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(slashes * (ch == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'\"') result += L'\\';
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

// Only an explicit, complete shell invocation authorizes native fallback.
inline std::wstring Parse(int argc, const wchar_t* const* argv) {
    if (argc != 3 || !argv || !argv[1] || !argv[2] || std::wstring_view(argv[1]) != kArgument) return {};
    std::wstring target(argv[2]);
    if (target == L"pulse:win-e" || target == kThisPc || target == kRecycleBin) return target;
    // A registry "%1" with a trailing separator reaches CommandLineToArgvW
    // with a literal final quote. Quotes cannot belong to a filesystem path.
    if (!target.empty() && target.back() == L'\"') target.pop_back();
    if (target.size() == 2 && target[1] == L':') target += L'\\';
    if (target.find(L'\"') != std::wstring::npos || target.size() >= 32768) return {};
    const bool drive = target.size() >= 3 && ((target[0] >= L'A' && target[0] <= L'Z') ||
        (target[0] >= L'a' && target[0] <= L'z')) && target[1] == L':' && (target[2] == L'\\' || target[2] == L'/');
    return drive || target.starts_with(L"\\\\") ? target : std::wstring();
}

inline std::wstring NativeCommand(const std::wstring& explorer, const std::wstring& target) {
    if (target.empty()) return {};
    return Quote(explorer) + L" /n,/e," + Quote(target == L"pulse:win-e" ? std::wstring(kThisPc) : target);
}

inline DWORD OpenNative(const std::wstring& target, decltype(&CreateProcessW) create = &CreateProcessW) {
    wchar_t directory[MAX_PATH]{};
    const UINT count = GetWindowsDirectoryW(directory, ARRAYSIZE(directory));
    if (!count || count >= ARRAYSIZE(directory)) return ERROR_PATH_NOT_FOUND;
    const std::wstring explorer = std::wstring(directory) + L"\\explorer.exe";
    auto command = NativeCommand(explorer, target);
    if (command.empty()) return ERROR_INVALID_PARAMETER;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!create(explorer.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return GetLastError();
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return ERROR_SUCCESS;
}
}
