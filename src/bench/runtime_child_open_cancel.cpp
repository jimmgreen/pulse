#include "runtime_child_open_cancel.h"
#include "../shell_bridge/file_dialog_runtime.h"
#include <tlhelp32.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
namespace {
using Microsoft::WRL::ComPtr;
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
std::wstring Env(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring text(size, 0);
    const DWORD count = GetEnvironmentVariableW(name, text.data(), size);
    text.resize(count); return text;
}
std::wstring Canonical(const std::wstring& path) {
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file.value, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool Status(PulseFileDialogRuntimeStatus& status) {
#ifdef _WIN64
    const auto module = GetModuleHandleW(L"PulseFileDialogRuntime64.dll");
#else
    const auto module = GetModuleHandleW(L"PulseFileDialogRuntime32.dll");
#endif
    using Query = HRESULT (WINAPI*)(PulseFileDialogRuntimeStatus*, uint32_t);
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseFileDialogRuntimeQuery")) : nullptr;
    return query && query(&status, sizeof(status)) == S_OK;
}
struct Probe {
    IFileDialog* dialog = nullptr;
    HDESK desktop = nullptr;
    std::wstring host;
    FILETIME started{};
    ULONGLONG deadline = 0;
    Handle host_process;
    DWORD host_pid = 0;
    HWND picker = nullptr;
    unsigned visible_samples = 0;
    bool ambiguous = false, timed_out = false, close_sent = false;
    HRESULT close_result = E_PENDING;
};
thread_local Probe* active = nullptr;
bool IsOwnNewHost(DWORD pid, const Probe& probe, HANDLE& result) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    PROCESSENTRY32W entry{sizeof(entry)};
    bool own_child = false;
    if (snapshot.value != INVALID_HANDLE_VALUE && Process32FirstW(snapshot.value, &entry)) do {
        if (entry.th32ProcessID == pid) { own_child = entry.th32ParentProcessID == GetCurrentProcessId(); break; }
    } while (Process32NextW(snapshot.value, &entry));
    if (!own_child) return false;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
    if (!process.value) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    std::wstring image(32768, 0); DWORD count = static_cast<DWORD>(image.size());
    if (!GetProcessTimes(process.value, &created, &exited, &kernel, &user) || CompareFileTime(&created, &probe.started) < 0 ||
        !QueryFullProcessImageNameW(process.value, 0, image.data(), &count) ||
        WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) return false;
    image.resize(count);
    const auto canonical = Canonical(image);
    if (canonical.empty() || _wcsicmp(canonical.c_str(), probe.host.c_str()) != 0) return false;
    result = process.value; process.value = nullptr; return true;
}
BOOL CALLBACK Observe(HWND window, LPARAM parameter) {
    auto& probe = *reinterpret_cast<Probe*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t name[80]{};
    if (!GetClassNameW(window, name, 80) || wcscmp(name, L"PulseFolderPickerWindow") != 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    HANDLE process = nullptr;
    if (probe.host_pid == pid && probe.host_process.value && WaitForSingleObject(probe.host_process.value, 0) == WAIT_TIMEOUT) {
        if (probe.picker && probe.picker != window) probe.ambiguous = true;
        probe.picker = window; ++probe.visible_samples; return TRUE;
    }
    if (!IsOwnNewHost(pid, probe, process)) return TRUE;
    if (probe.host_pid) { probe.ambiguous = true; CloseHandle(process); return TRUE; }
    probe.host_pid = pid; probe.host_process.value = process; probe.picker = window; ++probe.visible_samples;
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active || active->close_sent) return;
    try {
        EnumDesktopWindows(active->desktop, Observe, reinterpret_cast<LPARAM>(active));
        const bool confirmed = active->visible_samples && !active->ambiguous;
        if (!confirmed && GetTickCount64() < active->deadline) return;
        active->timed_out = !confirmed;
        active->close_sent = true;
        active->close_result = active->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    } catch (...) {
        active->timed_out = true; active->close_sent = true;
        active->close_result = active->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
}
int RunRuntimeChildOpenCancel() {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool condition, const char* label) {
        ++checks; if (!condition) ++failures;
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    };
    PulseFileDialogRuntimeStatus before{};
    const auto ready_deadline = GetTickCount64() + 10000;
    while ((!Status(before) || !before.ready) && GetTickCount64() < ready_deadline) Sleep(10);
    if (!Status(before) || !before.ready) return 30;
    const auto host = Canonical(Env(L"PULSE_DIALOG_HOST"));
    const auto control = Env(L"PULSE_DIALOG_CONTROL");
    if (host.empty() || control.empty()) return 31;
    int report = -1;
    const auto report_path = std::filesystem::path(control) / L"open-cancel-report.log";
    if (_wsopen_s(&report, report_path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_TEXT | _O_NOINHERIT,
        _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0) return 35;
    const int redirected = _dup2(report, _fileno(stdout)); _close(report);
    if (redirected != 0 || !SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))),
        HANDLE_FLAG_INHERIT, 0)) return 36;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto nonce = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    // Non-user directory, exclusively created under this fixture's bound session.
    const auto scratch = std::filesystem::path(control) / (L"open-cancel-" + nonce);
    if (!CreateDirectoryW(scratch.c_str(), nullptr)) return 32;
    const auto desktop_name = L"PulseRuntimeOpenCancel-" + nonce;
    HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        if (desktop) CloseDesktop(desktop); RemoveDirectoryW(scratch.c_str()); return 33;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { SetThreadDesktop(previous); CloseDesktop(desktop); RemoveDirectoryW(scratch.c_str()); return 34; }
    SetLastError(ERROR_SUCCESS);
    HWND owner = CreateWindowExW(0, L"STATIC", L"Owned runtime cancel fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const DWORD owner_error = GetLastError();
    std::printf("PREP owner=%p create_error=%lu coinitialize=0x%08lX scratch_units=%zu extended_prefix=%u\n",
        static_cast<void*>(owner), owner_error, static_cast<unsigned long>(initialized), scratch.native().size(),
        static_cast<unsigned>(_wcsnicmp(scratch.c_str(), L"\\\\?\\", 4) == 0));
    check(owner != nullptr, "same-STA fixture owner creation succeeds");
    if (owner) ShowWindow(owner, SW_SHOWNOACTIVATE);
    Probe probe; probe.desktop = desktop; probe.host = host;
    {
        ComPtr<IFileOpenDialog> dialog;
        const HRESULT created = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        check(created == S_OK && dialog, "raw CoCreateInstance returns the runtime-wrapped default Open");
        ComPtr<IShellItem> folder;
        // The fixture is owned and local. Normalize only \\?\X:\ to X:\ for
        // this Shell call; never strip an extended UNC/device namespace.
        const auto filesystem_path = scratch.native();
        std::wstring shell_path = filesystem_path;
        const bool extended = shell_path.rfind(L"\\\\?\\", 0) == 0;
        const bool local_drive = shell_path.size() >= 7 &&
            ((shell_path[4] >= L'A' && shell_path[4] <= L'Z') || (shell_path[4] >= L'a' && shell_path[4] <= L'z')) &&
            shell_path[5] == L':' && shell_path[6] == L'\\';
        if (extended && local_drive) shell_path.erase(0, 4);
        const auto normalized_identity = Canonical(shell_path);
        const auto original_identity = Canonical(filesystem_path);
        const bool same_owned_directory = !normalized_identity.empty() && !original_identity.empty() &&
            _wcsicmp(normalized_identity.c_str(), original_identity.c_str()) == 0 && (!extended || local_drive);
        check(same_owned_directory, "Shell path normalization retains the exact owned local fixture directory");
        if (extended) {
            ComPtr<IShellItem> extended_item;
            const HRESULT extended_result = SHCreateItemFromParsingName(filesystem_path.c_str(), nullptr, IID_PPV_ARGS(&extended_item));
            std::printf("PREP original_extended_parse_hr=0x%08lX normalized_units=%zu\n",
                static_cast<unsigned long>(extended_result), shell_path.size());
        }
        const HRESULT folder_result = same_owned_directory ?
            SHCreateItemFromParsingName(shell_path.c_str(), nullptr, IID_PPV_ARGS(&folder)) : E_INVALIDARG;
        const bool set_attempted = dialog && folder_result == S_OK && folder;
        const HRESULT set_result = set_attempted ? dialog->SetFolder(folder.Get()) : E_UNEXPECTED;
        std::printf("PREP shell_item_hr=0x%08lX shell_item=%p set_attempted=%u set_folder_hr=0x%08lX\n",
            static_cast<unsigned long>(folder_result), static_cast<void*>(folder.Get()), static_cast<unsigned>(set_attempted),
            static_cast<unsigned long>(set_result));
        check(folder_result == S_OK && folder, "Shell parses the exact owned navigation directory");
        check(set_attempted && set_result == S_OK, "default Open accepts the owned directory through SetFolder");
        const bool configured = set_attempted && set_result == S_OK;
        check(configured && owner, "default Open uses only owned isolated directory and owner");
        if (configured && owner) {
            probe.dialog = dialog.Get(); probe.deadline = GetTickCount64() + 20000;
            GetSystemTimeAsFileTime(&probe.started);
            active = &probe;
            const UINT_PTR timer = SetTimer(nullptr, 0, 50, Tick);
            HRESULT shown = E_FAIL;
            if (timer) shown = dialog->Show(owner);
            if (timer) KillTimer(nullptr, timer);
            active = nullptr;
            check(timer && probe.visible_samples > 0 && probe.host_pid && !probe.ambiguous && !probe.timed_out,
                "actual visible Pulse picker belongs to a new exact-host child of this fixture");
            check(probe.close_sent && probe.close_result == S_OK && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED),
                "same-STA Close cancels the real host dialog with ERROR_CANCELLED");
            check(IsWindow(owner) && IsWindowEnabled(owner), "fixture owner is restored after host cancellation");
            check(probe.host_process.value && WaitForSingleObject(probe.host_process.value, 5000) == WAIT_OBJECT_0,
                "exact owned host exits before cancellation completes");
            check(!probe.picker || !IsWindow(probe.picker), "observed host window is gone");
            PulseFileDialogRuntimeStatus after{};
            check(Status(after) && after.admitted == before.admitted + 1 && after.rejected == before.rejected,
                "one bridge admission and no native second dialog fallback");
            std::printf("OPEN_CANCEL host_pid=%lu hwnd=%p samples=%u timeout=%u shown=0x%08lX close=0x%08lX\n",
                probe.host_pid, static_cast<void*>(probe.picker), probe.visible_samples, static_cast<unsigned>(probe.timed_out),
                static_cast<unsigned long>(shown), static_cast<unsigned long>(probe.close_result));
        }
    }
    if (owner) DestroyWindow(owner);
    CoUninitialize();
    check(SetThreadDesktop(previous) != FALSE && CloseDesktop(desktop) != FALSE, "private desktop released");
    check(RemoveDirectoryW(scratch.c_str()) != FALSE, "owned empty navigation fixture removed");
    std::printf("OPEN_CANCEL Checks: %u, failures: %u\n", checks, failures); std::fflush(stdout);
    return failures ? 1 : 0;
}
