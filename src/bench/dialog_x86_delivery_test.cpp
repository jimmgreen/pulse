#include "../common/dialog_auto_registration.h"
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr const char* kArchitecture = sizeof(void*) == 4 ? "x86" : "x64";
IFileDialog* active = nullptr;
DWORD thread_id = 0;
bool seen = false, native_seen = false, clicked = false, expired = false, cancel = false, luma_loaded = false;
ULONGLONG start = 0, first = 0;
std::filesystem::path screenshot;
std::filesystem::path dependency_directory;
std::wstring expected_title;
DWORD observed_pid = 0;
bool ModuleLoaded(DWORD pid, const std::filesystem::path& expected) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExePath, expected.c_str()) == 0) { found = true; break; }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}
struct IsolatedRoot {
    HKEY root = nullptr;
    bool overridden = false;
    std::wstring path;
    DWORD Cleanup() {
        if (overridden) { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); overridden = false; }
        if (root) { RegCloseKey(root); root = nullptr; }
        if (path.empty()) return ERROR_SUCCESS;
        const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        if (!result) path.clear();
        return result;
    }
    ~IsolatedRoot() { Cleanup(); }
};
void Capture(HWND window) {
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetWindowDC(window), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    auto old = SelectObject(memory, bitmap);
    const BOOL captured = PrintWindow(window, memory, 2);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    SelectObject(memory, old);
    const auto lines = GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    if (captured && lines == height) {
        BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        std::ofstream out(screenshot, std::ios::binary); out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader)); out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    }
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
}
BOOL CALLBACK Inspect(HWND window, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[256]{}; GetWindowTextW(window, title, 256);
    if (expected_title != title) return TRUE;
    if (pid != GetCurrentProcessId()) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t image[32768]{}; DWORD units = 32768;
        const bool expected = process && QueryFullProcessImageNameW(process, 0, image, &units) &&
            _wcsicmp(image, (dependency_directory / L"pulse.exe").c_str()) == 0;
        if (process) CloseHandle(process);
        if (!expected) return TRUE;
    }
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (wcscmp(name, L"#32770") == 0) {
        native_seen = true; PostMessageW(window, WM_COMMAND, IDCANCEL, 0); return TRUE;
    }
    if (wcscmp(name, L"PulseFolderPickerWindow")) return TRUE;
    seen = true;
    observed_pid = pid;
    if (!first) first = GetTickCount64();
    if (!clicked && GetTickCount64() - first >= 1000) {
        luma_loaded = ModuleLoaded(pid, dependency_directory / L"lumatext.dll");
        Capture(window);
        clicked = true;
        if (cancel) active->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        else {
            using Dpi = UINT(WINAPI*)(HWND);
            auto dpi = reinterpret_cast<Dpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
            const float scale = dpi ? dpi(window) / 96.0f : 1.0f;
            RECT client{}; GetClientRect(window, &client);
            const int x = client.right - static_cast<int>(70 * scale), y = client.bottom - static_cast<int>(30 * scale);
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        }
    }
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    EnumWindows(Inspect, 0);
    if (GetTickCount64() - start > 12000) {
        expired = true; if (active) active->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
DWORD WINAPI Watchdog(void* context) {
    if (WaitForSingleObject(context, 60000) == WAIT_TIMEOUT) ExitProcess(124);
    return 0;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
}
int wmain(int argc, wchar_t** argv) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        std::printf("[FAIL] test process DPI awareness error=%lu\n", GetLastError());
        return 2;
    }
    std::printf("[INFO] test process DPI awareness=PER_MONITOR_AWARE_V2 pointer_bytes=%zu\n", sizeof(void*));
    const bool registered = argc == 4 && wcscmp(argv[3], L"--registered") == 0;
    if (argc != 3 && !registered) return 2;
    const std::filesystem::path dll(argv[1]), output(argv[2]);
    if (!dll.is_absolute() || !output.is_absolute() || std::filesystem::exists(output)) return 2;
    std::filesystem::create_directories(output);
    const auto source = output / L"sample-中文.txt", saved = output / L"saved-中文.txt";
    const std::string payload = std::string("Pulse ") + kArchitecture + " file consumption\n";
    { std::ofstream file(source, std::ios::binary); file << payload; }
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr), watchdog = stop ? CreateThread(nullptr, 0, Watchdog, stop, 0, nullptr) : nullptr;
    if (!watchdog) return 2;
    IsolatedRoot isolation;
    DWORD error = ERROR_SUCCESS;
    if (!registered) {
        isolation.path = L"Software\\PulseX86DeliveryTest_" + std::to_wstring(GetCurrentProcessId());
        error = RegCreateKeyExW(HKEY_CURRENT_USER, isolation.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolation.root, nullptr);
        if (!error) { error = RegOverridePredefKey(HKEY_CURRENT_USER, isolation.root); isolation.overridden = !error; }
        if (error) return 2;
        error = pulse::dialog_auto::Enable(dll.wstring());
    }
    HKEY config = nullptr;
    if (!registered && !error) error = RegOpenKeyExW(HKEY_CURRENT_USER, pulse::dialog_auto::ConfigKey, 0, KEY_SET_VALUE | pulse::dialog_auto::RegistryView, &config);
    const DWORD host = 0;
    if (!registered && !error) error = RegSetValueExW(config, L"Host", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&host), sizeof(host));
    if (config) RegCloseKey(config);
    if (error) return 2;
    const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE module = registered ? nullptr : LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    using Entry = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto entry = module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    int failures = FAILED(apartment) || (!registered && !entry);
    dependency_directory = dll.parent_path();
    for (int scenario = 0; !failures && scenario < 4; ++scenario) {
        const bool save = scenario % 2 != 0;
        cancel = scenario < 2; seen = native_seen = clicked = expired = luma_loaded = false; first = 0;
        ComPtr<IClassFactory> factory; ComPtr<IFileDialog> dialog; ComPtr<IShellItem> folder;
        auto hr = registered ? CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)) :
            entry(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
        if (!registered && SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
        const bool expected_shim = ModuleLoaded(GetCurrentProcessId(), dll);
        if (!expected_shim) { std::printf("[FAIL] requested shim absent after %s activation\n", registered ? "registered COM" : "direct factory"); ++failures; break; }
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(output.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
            (save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN : FOS_FILEMUSTEXIST));
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
        if (SUCCEEDED(hr)) hr = dialog->SetFileName((save ? saved : source).filename().c_str());
        expected_title = L"Pulse " + std::to_wstring(sizeof(void*) * 8) + L"-bit delivery verification " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(scenario);
        if (SUCCEEDED(hr)) hr = dialog->SetTitle(expected_title.c_str());
        screenshot = output / (std::wstring(save ? L"save" : L"open") + (cancel ? L"-cancel.bmp" : L"-consume.bmp"));
        active = dialog.Get(); thread_id = GetCurrentThreadId(); start = GetTickCount64();
        const auto timer = SUCCEEDED(hr) ? SetTimer(nullptr, 0, 50, Tick) : 0;
        if (timer) hr = dialog->Show(nullptr);
        if (timer) KillTimer(nullptr, timer);
        active = nullptr;
        ComPtr<IShellItem> result; const auto get_result = dialog ? dialog->GetResult(&result) : E_FAIL;
        bool consumed = false, path_matches = false;
        if (!cancel && SUCCEEDED(hr) && SUCCEEDED(get_result) && result) {
            PWSTR text = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &text))) {
                const std::filesystem::path selected(text); CoTaskMemFree(text);
                path_matches = selected == (save ? saved : source);
                if (path_matches && save) { std::ofstream file(selected, std::ios::binary); file << payload; }
                if (path_matches) consumed = Read(selected) == payload;
            }
        }
        const bool pass = timer && seen && !native_seen && !expired && luma_loaded && std::filesystem::exists(screenshot) &&
            (cancel ? hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(get_result) && !result : hr == S_OK && path_matches && consumed);
        std::printf("[%s] %s %s save=%u cancel=%u visible=%u native=%u luma=%u expired=%u hr=%08lx path=%u consumed=%u pointer_bytes=%zu ui_pid=%lu caller_pid=%lu\n",
            pass ? "PASS" : "FAIL", kArchitecture, registered ? "registered COM" : "direct-factory (not COM registry activation)", save, cancel, seen, native_seen, luma_loaded, expired, static_cast<unsigned long>(hr), path_matches, consumed, sizeof(void*), observed_pid, GetCurrentProcessId());
        failures += !pass;
    }
    if (!registered) {
        error = pulse::dialog_auto::Disable();
        if (error || pulse::dialog_auto::Inspect().enabled) ++failures;
    }
    if (module) FreeLibrary(module);
    if (SUCCEEDED(apartment)) CoUninitialize();
    const auto cleanup = isolation.Cleanup();
    failures += cleanup != ERROR_SUCCESS;
    SetEvent(stop); WaitForSingleObject(watchdog, INFINITE); CloseHandle(watchdog); CloseHandle(stop);
    std::printf("[%s] %s error=%lu cleanup=%lu\n", failures ? "FAIL" : "PASS", registered ? "registered mode leaves registration unchanged" : "isolated registration restored", error, cleanup);
    return failures ? 1 : 0;
}
