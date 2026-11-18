#include "../common/shell_open_entry.h"
#include <shellapi.h>
#include <iostream>

namespace {
int failed = 0, native_calls = 0;
std::wstring native_exe, native_command;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failed;
}
BOOL WINAPI FailCreate(LPCWSTR exe, LPWSTR command, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL inherit, DWORD flags, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    ++native_calls; native_exe = exe; native_command = command;
    Check(!inherit && flags == 0, "native launch does not inherit caller handles");
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
std::wstring Parse(const std::wstring& command) {
    int argc = 0;
    auto argv = CommandLineToArgvW(command.c_str(), &argc);
    const auto target = pulse::shell_open_entry::Parse(argc, argv);
    if (argv) LocalFree(argv);
    return target;
}
}
int main() {
    namespace entry = pulse::shell_open_entry;
    for (const auto& target : {std::wstring(L"C:\\"), std::wstring(L"C:\\space 中文\\"),
        std::wstring(L"\\\\server\\share\\folder"), std::wstring(L"\\\\?\\C:\\long\\folder"),
        std::wstring(entry::kThisPc), std::wstring(entry::kRecycleBin), std::wstring(L"pulse:win-e")}) {
        Check(Parse(L"pulse.exe --shell-open " + entry::Quote(target)) == target, "explicit target round trips Windows quoting");
        const auto command = entry::NativeCommand(L"C:\\Windows\\explorer.exe", target);
        const auto expected = target == L"pulse:win-e" ? std::wstring(entry::kThisPc) : target;
        Check(command == L"\"C:\\Windows\\explorer.exe\" /n,/e," + entry::Quote(expected),
            "native browse uses exact target and bypasses default verb");
    }
    Check(Parse(L"pulse.exe --shell-open \"C:\\\"") == L"C:\\", "registry drive-root escaping is repaired");
    Check(Parse(L"pulse.exe --shell-open \"\\\\server\\share\\\"") == L"\\\\server\\share", "registry UNC trailing separator escaping is repaired");
    for (const auto* command : {L"pulse.exe C:\\folder", L"pulse.exe --shell-open", L"pulse.exe --shell-open C:\\folder extra",
        L"pulse.exe --shell-open relative", L"pulse.exe --shell-open pulse:unknown", L"pulse.exe --shell-open ::{unknown}"})
        Check(Parse(command).empty(), "ordinary or malformed command cannot authorize fallback");
    Check(entry::OpenNative(L"C:\\private fixture", &FailCreate) == ERROR_ACCESS_DENIED && native_calls == 1,
        "native failure is returned with exactly one attempt");
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    Check(native_exe == std::wstring(windows) + L"\\explorer.exe" &&
        native_command == entry::NativeCommand(native_exe, L"C:\\private fixture"),
        "native process uses absolute Windows Explorer path");
    Check(entry::OpenNative(L"", &FailCreate) == ERROR_INVALID_PARAMETER && native_calls == 1,
        "empty target never starts a native process");
    return failed ? 1 : 0;
}
