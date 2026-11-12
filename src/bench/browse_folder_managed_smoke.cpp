#include "../shell_bridge/file_dialog_runtime.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

namespace {
HANDLE report = INVALID_HANDLE_VALUE;
DWORD thread_id = 0;
HWND owner = nullptr, observed_window = nullptr;
std::wstring fixture, title;
bool cancelling = false, observed = false, timed_out = false;
unsigned phase = 0, calls = 0;
ULONGLONG deadline = 0, phase_tick = 0;
struct Case { unsigned ordinal; UINT flags; bool ansi, cancel; };
constexpr std::array<UINT, 3> modes{BIF_RETURNONLYFSDIRS, BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE,
    BIF_RETURNONLYFSDIRS | BIF_USENEWUI};
void Log(const std::string& text) {
    DWORD written = 0;
    WriteFile(report, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(report);
}
std::string Utf8(const std::wstring& value) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, 0);
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string Json(const std::wstring& value) {
    std::string result = "\"";
    for (const unsigned char byte : Utf8(value)) {
        if (byte == '\\' || byte == '"') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 32) { char escape[8]{}; std::snprintf(escape, sizeof(escape), "\\u%04x", byte); result += escape; }
        else result += static_cast<char>(byte);
    }
    return result + '"';
}
std::wstring Environment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    std::wstring value(count, 0);
    const DWORD read = GetEnvironmentVariableW(name, value.data(), count);
    if (!read || read >= count) return {};
    value.resize(read); return value;
}
bool Ready(bool log) {
#ifdef _WIN64
    HMODULE module = GetModuleHandleW(L"PulseFileDialogRuntimeUiProof64R8.dll");
    if (!module) module = GetModuleHandleW(L"PulseFileDialogRuntime64.dll");
#else
    HMODULE module = GetModuleHandleW(L"PulseFileDialogRuntimeUiProof32R8.dll");
    if (!module) module = GetModuleHandleW(L"PulseFileDialogRuntime32.dll");
#endif
    using Query = HRESULT (WINAPI*)(PulseFileDialogRuntimeStatus*, uint32_t);
    auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseFileDialogRuntimeQuery")) : nullptr;
    PulseFileDialogRuntimeStatus status{};
    const HRESULT hr = query ? query(&status, sizeof(status)) : E_NOINTERFACE;
    if (log) {
        wchar_t path[32768]{}; if (module) GetModuleFileNameW(module, path, 32768);
        char line[256]{};
        std::snprintf(line, sizeof(line), "RUNTIME pid=%lu arch=%u query_hr=%08lX initialization=%08lX ready=%u coverage=%u\n",
            GetCurrentProcessId(), static_cast<unsigned>(sizeof(void*) * 8), static_cast<unsigned long>(hr), static_cast<unsigned long>(status.initialization), status.ready, status.installed_entries);
        Log(line); Log("RUNTIME_PATH " + Utf8(path) + "\n");
    }
    return hr == S_OK && status.ready && (status.installed_entries & 48u) == 48u;
}
BOOL CALLBACK FindOwnPicker(HWND window, LPARAM) {
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != thread_id || process != GetCurrentProcessId() ||
        !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != owner) return TRUE;
    wchar_t caption[256]{}, name[128]{};
    GetWindowTextW(window, caption, 256); GetClassNameW(window, name, 128);
    if (title != caption || std::wcscmp(name, L"PulseFolderPickerWindow")) return TRUE;
    observed = true; observed_window = window;
    if (cancelling) { if (!phase) { PostMessageW(window, WM_CLOSE, 0, 0); phase = 1; } return TRUE; }
    if (!phase) {
        if (!phase_tick) { phase_tick = GetTickCount64(); return TRUE; }
        if (GetTickCount64() - phase_tick < 500) return TRUE;
        SendMessageW(window, WM_SYSKEYDOWN, 'D', static_cast<LPARAM>(1u << 29));
        const HWND address = GetDlgItem(window, 101);
        if (address && IsWindowVisible(address) && IsWindowEnabled(address)) {
            SetWindowTextW(address, fixture.c_str());
            PostMessageW(address, WM_KEYDOWN, VK_RETURN, 0);
            phase = 1; phase_tick = GetTickCount64();
        }
    } else if (phase == 1 && GetTickCount64() - phase_tick >= 1800) {
        const HWND filename = GetDlgItem(window, 102);
        if (filename) { PostMessageW(filename, WM_KEYDOWN, VK_RETURN, 0); phase = 2; }
    }
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (GetTickCount64() >= deadline) {
        timed_out = true;
        EnumThreadWindows(thread_id, [](HWND window, LPARAM) -> BOOL {
            if (window != owner) PostMessageW(window, WM_CLOSE, 0, 0); return TRUE;
        }, 0);
        return;
    }
    EnumThreadWindows(thread_id, FindOwnPicker, 0);
}
DWORD WINAPI Watchdog(void*) {
    Sleep(180000);
    Log("FAIL managed browse watchdog\n"); ExitProcess(6);
}
bool Consume(PIDLIST_ABSOLUTE pidl) {
    if (!pidl) return false;
    wchar_t selected[32768]{};
    const bool matches = SHGetPathFromIDListEx(pidl, selected, 32768, GPFIDL_DEFAULT) && _wcsicmp(selected, fixture.c_str()) == 0;
    bool consumed = false;
    if (matches) {
        const auto path = std::filesystem::path(fixture) / L"probe.txt";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[16]{}; DWORD read = 0;
            consumed = ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read == 5 && std::memcmp(buffer, "probe", 5) == 0;
            CloseHandle(file);
        }
    }
    CoTaskMemFree(pidl);
    return consumed;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 7 || std::wcscmp(argv[1], L"--report") || std::wcscmp(argv[3], L"--folder") || std::wcscmp(argv[5], L"--expected")) return 2;
    const std::filesystem::path output(argv[2]), folder(argv[4]), expected_file(argv[6]);
    if (!output.is_absolute() || !folder.is_absolute() || !expected_file.is_absolute() || !std::filesystem::is_directory(folder)) return 2;
    fixture = folder.wstring();
    report = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report == INVALID_HANDLE_VALUE) return 3;
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture.c_str());
    const auto control = Environment(L"PULSE_DIALOG_CONTROL");
    Log("CONTROL " + Utf8(control) + "\nNONCE " + Utf8(Environment(L"PULSE_DIALOG_NONCE")) + "\n");
    std::array<Case, 12> planned{}; unsigned count = 0;
    for (const UINT flags : modes) for (const bool ansi : {false, true}) for (const bool cancel : {false, true}) {
        planned[count] = {count + 1, flags, ansi, cancel}; ++count;
    }
    std::string manifest = "{\"schema\":1,\"host\":\"browse_folder_managed_smoke\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"process_arch\":" + std::to_string(sizeof(void*) * 8) + ",\"planned_call_count\":12,\"route\":\"managed\",\"control\":" + Json(control) + ",\"calls\":[";
    for (const auto& test : planned) {
        if (test.ordinal > 1) manifest += ',';
        manifest += "{\"ordinal\":" + std::to_string(test.ordinal) + ",\"entry_family\":\"SHBROWSE_" + (test.ansi ? "A" : "W") +
            "\",\"flags\":" + std::to_string(test.flags) + ",\"terminal_status\":\"" + (test.cancel ? "cancelled" : "accepted") + "\"}";
    }
    manifest += "]}\n";
    HANDLE expected = CreateFileW(expected_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (expected == INVALID_HANDLE_VALUE || !WriteFile(expected, manifest.data(), static_cast<DWORD>(manifest.size()), &written, nullptr) || written != manifest.size()) return 3;
    CloseHandle(expected);
    Log("PLAN call_count=12 written_before_first_Show=1\n");
    const auto ready_deadline = GetTickCount64() + 20000;
    while (!Ready(false) && GetTickCount64() < ready_deadline) Sleep(20);
    if (!Ready(true)) { Log("FAIL production runtime ready or W/A coverage\n"); return 4; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 4;
    thread_id = GetCurrentThreadId();
    owner = CreateWindowExW(0, L"STATIC", L"Pulse managed Browse smoke owner", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return 4;
    HANDLE watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (!watchdog) return 4; CloseHandle(watchdog);
    SHFILEINFOW native{};
    if (!SHGetFileInfoW(fixture.c_str(), 0, &native, sizeof(native), SHGFI_DISPLAYNAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) return 4;
    const int ansi_count = WideCharToMultiByte(CP_ACP, 0, native.szDisplayName, -1, nullptr, 0, nullptr, nullptr);
    std::string native_ansi(ansi_count, 0);
    if (!ansi_count || !WideCharToMultiByte(CP_ACP, 0, native.szDisplayName, -1, native_ansi.data(), ansi_count, nullptr, nullptr)) return 4;
    native_ansi.pop_back();
    unsigned failures = 0;
    for (const auto& test : planned) {
        title = L"Pulse Managed Browse " + std::to_wstring(GetCurrentProcessId()) + L" Case " + std::to_wstring(test.ordinal);
        cancelling = test.cancel; observed = false; timed_out = false; observed_window = nullptr; phase = 0; phase_tick = 0;
        deadline = GetTickCount64() + 20000;
        Log("CASE_BEGIN ordinal=" + std::to_string(test.ordinal) + " family=SHBROWSE_" + (test.ansi ? "A" : "W") +
            " flags=" + std::to_string(test.flags) + " action=" + (test.cancel ? "cancelled" : "accepted") + " tick=" + std::to_string(GetTickCount64()) + "\n");
        const UINT_PTR timer = SetTimer(nullptr, 0, 50, Tick);
        if (!timer) return 4;
        PIDLIST_ABSOLUTE pidl = nullptr; bool fields = false;
        if (test.ansi) {
            char display[MAX_PATH] = "sentinel";
            const auto ansi_title = Utf8(title); // The generated title is ASCII in every ACP.
            BROWSEINFOA input{}; input.hwndOwner = owner; input.pszDisplayName = display;
            input.lpszTitle = ansi_title.c_str(); input.ulFlags = test.flags; input.iImage = -777;
            pidl = SHBrowseForFolderA(&input);
            fields = test.cancel ? std::string(display) == "sentinel" && input.iImage == -777 :
                std::string(display) == native_ansi && input.iImage == native.iIcon;
        } else {
            wchar_t display[MAX_PATH] = L"sentinel";
            BROWSEINFOW input{}; input.hwndOwner = owner; input.pszDisplayName = display;
            input.lpszTitle = title.c_str(); input.ulFlags = test.flags; input.iImage = -777;
            pidl = SHBrowseForFolderW(&input);
            fields = test.cancel ? std::wstring(display) == L"sentinel" && input.iImage == -777 :
                std::wstring(display) == native.szDisplayName && input.iImage == native.iIcon;
        }
        KillTimer(nullptr, timer); ++calls;
        const bool accepted = pidl != nullptr;
        const bool consumed = accepted && Consume(pidl);
        const bool ok = observed && !timed_out && fields && (test.cancel ? !accepted : consumed);
        if (!ok) ++failures;
        Log(std::string(ok ? "PASS" : "FAIL") + " CASE_RESULT ordinal=" + std::to_string(test.ordinal) + " observed_pulse=" + std::to_string(observed) +
            " accepted=" + std::to_string(accepted) + " consumed=" + std::to_string(consumed) + " fields=" + std::to_string(fields) + " timeout=" + std::to_string(timed_out) + "\n");
        if (!ok) break;
    }
    DestroyWindow(owner); CoUninitialize();
    Log("MANAGED_BROWSE planned=12 calls=" + std::to_string(calls) + " failures=" + std::to_string(failures) + "\n");
    CloseHandle(report); report = INVALID_HANDLE_VALUE;
    return failures || calls != 12 ? 1 : 0;
}
