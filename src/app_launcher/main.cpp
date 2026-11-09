#include "../common/dialog_bundle.h"
#include "../common/app_identity.h"
#include "../common/runtime_log.h"
#include "../common/shell_open_entry.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int LaunchFailed(DWORD error, const std::wstring& shell_target, uint64_t phase) {
    const DWORD native_error = shell_target.empty() ? ERROR_NOT_SUPPORTED : pulse::shell_open_entry::OpenNative(shell_target);
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        pulse::diagnostics::runtime::Initialize(std::wstring(local) + L"\\Pulse", "launcher");
        CoTaskMemFree(local);
        pulse::diagnostics::runtime::Event("shell_entry_launch_failed", {{"phase", phase}, {"error", error},
            {"native_attempted", shell_target.empty() ? 0u : 1u}, {"native_error", native_error}},
            pulse::diagnostics::runtime::Level::Error);
        pulse::diagnostics::runtime::Shutdown();
    }
    return static_cast<int>(!shell_target.empty() && !native_error ? ERROR_SUCCESS : error);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show) {
    pulse::app_identity::SetProcess();
    std::wstring tail = arguments ? arguments : L"";
    bool wait = false;
    constexpr wchar_t wait_flag[] = L"--launcher-wait";
    if (tail == wait_flag || tail.starts_with(std::wstring(wait_flag) + L" ")) {
        wait = true; tail.erase(0, std::size(wait_flag) - 1);
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW((L"pulse.exe " + tail).c_str(), &argc);
    const auto shell_target = pulse::shell_open_entry::Parse(argc, argv);
    for (int i = 1; argv && i < argc; ++i)
        if (wcscmp(argv[i], L"--ai") == 0 || wcscmp(argv[i], L"--mcp") == 0 || wcscmp(argv[i], L"--mcp-stdio") == 0 ||
            wcscmp(argv[i], L"--seed-shell-verbs") == 0 || wcscmp(argv[i], L"--shot") == 0 ||
            wcscmp(argv[i], L"--selftest") == 0 || wcscmp(argv[i], L"--test-instance") == 0) wait = true;
    if (argv) LocalFree(argv);
    std::wstring image(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!size || size >= image.size()) return LaunchFailed(ERROR_BAD_PATHNAME, shell_target, 0);
    image.resize(size);
    pulse::dialog_bundle::BundleLease lease;
    const auto root = std::filesystem::path(image).parent_path().wstring();
    DWORD error = pulse::dialog_bundle::ResolveInstalledBundle(root, lease);
    // The setup pointer transaction can briefly have no current file. Never
    // fall back to another executable or retry an invalid manifest/pointer.
    for (DWORD delay = 10; error == ERROR_FILE_NOT_FOUND && delay <= 160; delay *= 2) {
        Sleep(delay);
        error = pulse::dialog_bundle::ResolveInstalledBundle(root, lease);
    }
    if (error) return LaunchFailed(error, shell_target, 1);
    // GUI launches release the stable entry promptly. Scripts can explicitly
    // wait; redirected streams and the stdio AI entry always preserve lifetime.
    std::wstring command = L"\"" + lease.Host() + L"\"";
    if (!tail.empty()) command += L" " + tail;
    STARTUPINFOEXW extended{};
    auto& startup = extended.StartupInfo;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(show);
    HANDLE inherited[3]{};
    const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        HANDLE original = GetStdHandle(kinds[i]);
        if (original && original != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &inherited[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            any = true;
            const DWORD type = GetFileType(original);
            if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) wait = true;
        }
    }
    if (any) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = inherited[0]; startup.hStdOutput = inherited[1]; startup.hStdError = inherited[2];
    }
    std::vector<HANDLE> handle_list;
    std::vector<unsigned char> attributes;
    if (any) {
        for (HANDLE handle : inherited) if (handle) handle_list.push_back(handle);
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes.resize(bytes);
        extended.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(extended.lpAttributeList, 1, 0, &bytes)) {
            const DWORD failure = GetLastError();
            for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
            return LaunchFailed(failure, shell_target, 2);
        }
        if (!UpdateProcThreadAttribute(extended.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handle_list.data(), handle_list.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD failure = GetLastError();
            DeleteProcThreadAttributeList(extended.lpAttributeList);
            for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
            return LaunchFailed(failure, shell_target, 2);
        }
        startup.cb = sizeof(extended);
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(lease.Host().c_str(), command.data(), nullptr, nullptr,
        any, any ? EXTENDED_STARTUPINFO_PRESENT : 0, nullptr, nullptr, &startup, &process);
    const DWORD launch_error = created ? ERROR_SUCCESS : GetLastError();
    if (any) DeleteProcThreadAttributeList(extended.lpAttributeList);
    for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
    if (!created) return LaunchFailed(launch_error, shell_target, 3);
    // Once the host exists its delivery outcome is unknown. Never open Explorer
    // on a wait timeout or host exit: it may already have dispatched this request.
    CloseHandle(process.hThread);
    DWORD result = ERROR_SUCCESS;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        if (!GetExitCodeProcess(process.hProcess, &result)) result = GetLastError();
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}
