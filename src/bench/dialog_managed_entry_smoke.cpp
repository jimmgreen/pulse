#include "../shell_bridge/file_dialog_runtime.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <cstring>
#include <tlhelp32.h>
#include <commdlg.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <cstdio>
namespace {
HANDLE report = INVALID_HANDLE_VALUE;
DWORD thread_id = 0;
IFileDialog* dialog = nullptr;
bool observed = false, expired = false;
ULONGLONG deadline = 0;
void Log(const char* text) { DWORD written = 0; WriteFile(report, text, static_cast<DWORD>(strlen(text)), &written, nullptr); FlushFileBuffers(report); }
BOOL CALLBACK Observe(HWND window, LPARAM) {
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != thread_id || process != GetCurrentProcessId()) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    if (IsWindowVisible(window) && wcscmp(name, L"PulseFolderPickerWindow") == 0) {
        observed = true;
        if (!dialog) PostMessageW(window, WM_CLOSE, 0, 0);
    }
    if (expired) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    expired = GetTickCount64() >= deadline;
    EnumThreadWindows(thread_id, Observe, 0);
    if (dialog && (observed || expired)) dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    if (expired) { Log("FAIL timeout\n"); ExitProcess(5); }
}
DWORD WINAPI Watchdog(void*) {
    Sleep(60000);
    Log("FAIL watchdog timeout\n");
    EnumThreadWindows(thread_id, [](HWND window, LPARAM) -> BOOL { PostMessageW(window, WM_CLOSE, 0, 0); return TRUE; }, 0);
    Sleep(1000); ExitProcess(6);
}
bool Ready(bool diagnostic = false) {
#ifdef _WIN64
    HMODULE module = GetModuleHandleW(L"PulseFileDialogRuntimeUiProof64R8.dll");
#else
    HMODULE module = GetModuleHandleW(L"PulseFileDialogRuntimeUiProof32R8.dll");
#endif
    using Query = HRESULT (WINAPI*)(PulseFileDialogRuntimeStatus*, uint32_t);
    auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseFileDialogRuntimeQuery")) : nullptr;
    PulseFileDialogRuntimeStatus status{};
    const HRESULT result = query ? query(&status, sizeof(status)) : E_NOINTERFACE;
    if (diagnostic) {
        char line[256]{};
        std::snprintf(line, sizeof(line), "INFO runtime module=%p query=%u hr=%08lx ready=%u initialization=%08lx size=%u version=%u\n", static_cast<void*>(module), query ? 1u : 0u, static_cast<unsigned long>(result), status.ready, static_cast<unsigned long>(status.initialization), status.size, status.version);
        Log(line);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        MODULEENTRY32W entry{sizeof(entry)};
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Module32FirstW(snapshot, &entry)) do {
                char name[2048]{};
                WideCharToMultiByte(CP_UTF8, 0, entry.szExePath, -1, name, sizeof(name), nullptr, nullptr);
                Log("INFO module "); Log(name); Log("\n");
            } while (Module32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
    }
    return result == S_OK && status.ready;
}
}
int wmain(int argc, wchar_t** argv) {
    const bool xmind = argc == 6 && wcscmp(argv[5], L"--xmind-open") == 0;
    const bool save = argc == 6 && wcscmp(argv[5], L"--save") == 0;
    const bool minimal_save = argc == 6 && wcscmp(argv[5], L"--minimal-save") == 0;
    if ((argc != 5 && !xmind && !save && !minimal_save) || wcscmp(argv[1], L"--report") || wcscmp(argv[3], L"--folder")) return 2;
    const std::filesystem::path output(argv[2]), folder(argv[4]);
    if (!output.is_absolute() || !folder.is_absolute() || !std::filesystem::is_directory(folder)) return 2;
    report = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report == INVALID_HANDLE_VALUE) return 3;
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{}; DWORD returned = 0;
    const bool elevation_known = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    if (token) CloseHandle(token);
    char elevation_line[96]{};
    std::snprintf(elevation_line, sizeof(elevation_line), "INFO elevation_known=%u elevated=%u save=%u\n", elevation_known ? 1u : 0u, elevation.TokenIsElevated, save ? 1u : 0u);
    Log(elevation_line);
    if (minimal_save) Log("INFO minimal_save=1\n");
    thread_id = GetCurrentThreadId();
    HANDLE watchdog = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (!watchdog) { CloseHandle(report); return 3; }
    CloseHandle(watchdog);
    const auto ready_deadline = GetTickCount64() + 15000;
    while (!Ready() && GetTickCount64() < ready_deadline) Sleep(20);
    if (!Ready(true)) { Log("FAIL runtime ready\n"); return 4; }
    Log("PASS runtime ready\n");
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 4;
    Microsoft::WRL::ComPtr<IFileDialog> open;
    Microsoft::WRL::ComPtr<IShellItem> item;
    bool ok = SUCCEEDED(CoCreateInstance((save || minimal_save) ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&open))) &&
        SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)));
    if (ok && minimal_save) {
        COMDLG_FILTERSPEC filters[]={{L"HTML",L"*.html"},{L"All",L"*.*"}};
        ok = SUCCEEDED(open->SetDefaultExtension(L"*.html")) && SUCCEEDED(open->SetFileTypes(2, filters)) &&
            SUCCEEDED(open->SetFileTypeIndex(1)) && SUCCEEDED(open->SetFileName(L"export.html")) &&
            SUCCEEDED(open->SetOptions(0x42u));
    } else if (ok && save) {
        COMDLG_FILTERSPEC filter{L"HTML", L"*.html"};
        FILEOPENDIALOGOPTIONS options{};
        ok = SUCCEEDED(open->GetOptions(&options)) && SUCCEEDED(open->SetOptions(options | FOS_FORCEFILESYSTEM)) &&
            SUCCEEDED(open->SetFileName(L"export.html")) && SUCCEEDED(open->SetFileTypes(1, &filter)) &&
            SUCCEEDED(open->SetDefaultExtension(L"html"));
    } else if (ok) ok = SUCCEEDED(open->SetOptions(xmind ? 0x1240u : FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT));
    if (ok && xmind) {
        COMDLG_FILTERSPEC filters[]={{L"XMind",L"*.xmind"},{L"All",L"*.*"}};
        FILEOPENDIALOGOPTIONS actual{};
        ok = SUCCEEDED(open->SetFileName(L"")) && SUCCEEDED(open->SetFileTypes(2,filters)) && SUCCEEDED(open->SetFileTypeIndex(1)) && SUCCEEDED(open->GetOptions(&actual)) && actual==0x1240u;
    }
    if (ok) ok = SUCCEEDED(open->SetFolder(item.Get()));
    if (ok) {
        dialog = open.Get(); observed = false; deadline = GetTickCount64() + 15000;
        const auto timer = SetTimer(nullptr, 0, 50, Tick);
        if (!timer) return 4;
        const HRESULT result = open->Show(nullptr);
        KillTimer(nullptr, timer); dialog = nullptr;
        Microsoft::WRL::ComPtr<IShellItem> selected;
        ok = observed && result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(open->GetResult(&selected));
    }
    Log(ok ? "PASS COM Pulse window and cancellation\n" : "FAIL COM Pulse window or cancellation\n");
    if (xmind || minimal_save) {
        open.Reset(); item.Reset(); CoUninitialize(); CloseHandle(report); report=INVALID_HANDLE_VALUE;
        return ok?0:1;
    }
    wchar_t buffer[32768]{};
    if (save) wcscpy_s(buffer, L"export.html");
    OPENFILENAMEW request{sizeof(request)};
    request.lpstrFile = buffer; request.nMaxFile = 32768; request.lpstrInitialDir = folder.c_str();
    request.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (save) {
        request.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        request.lpstrFilter = L"HTML\0*.html\0\0"; request.nFilterIndex = 1; request.lpstrDefExt = L"html";
    }
    observed = false; deadline = GetTickCount64() + 15000;
    const auto timer = SetTimer(nullptr, 0, 50, Tick);
    if (!timer) return 4;
    const BOOL result = save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request);
    const DWORD error = CommDlgExtendedError();
    KillTimer(nullptr, timer);
    const bool legacy_ok = observed && !result && !error && (save ? wcscmp(buffer, L"export.html") == 0 : !buffer[0]);
    Log(legacy_ok ? "PASS legacy Pulse window and cancellation\n" : "FAIL legacy Pulse window or cancellation\n");
    open.Reset(); item.Reset(); CoUninitialize();
    CloseHandle(report); report = INVALID_HANDLE_VALUE;
    return ok && legacy_ok ? 0 : 1;
}
