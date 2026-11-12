#include "../shell_bridge/file_dialog_runtime.h"
#include "../shell_bridge/runtime_config.h"
#include <windowsx.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;
namespace config = pulse::shell_bridge::runtime;
using Microsoft::WRL::ComPtr;
using Query = HRESULT (WINAPI*)(PulseFileDialogRuntimeStatus*, uint32_t);
using PluginCreate = HRESULT (__cdecl*)(BOOL, IFileDialog**);
using DelayCount = LONG (__cdecl*)();
using WindowDpi = UINT (WINAPI*)(HWND);
constexpr auto kMachine = sizeof(void*) == 4 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
constexpr uint32_t kEntries = config::kCoCreateInstance | config::kCoCreateInstanceEx |
    config::kCoGetClassObject | config::kCreateProcessW | config::kSHBrowseForFolderW | config::kSHBrowseForFolderA;
unsigned checks = 0, failures = 0;
Query query = nullptr;
WindowDpi window_dpi = nullptr;
fs::path output, dependency;
std::wstring control;
void Check(bool condition, const char* label) {
    ++checks; failures += !condition;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
}
std::wstring Env(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, 0);
    value.resize(GetEnvironmentVariableW(name, value.data(), size)); return value;
}
fs::path Image() {
    std::wstring value(32768, 0);
    value.resize(GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()))); return value;
}
DWORD ParentPid() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD parent = 0;
    if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == GetCurrentProcessId()) { parent = entry.th32ParentProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return parent;
}
bool Status(PulseFileDialogRuntimeStatus& status) { return query && query(&status, sizeof(status)) == S_OK; }
bool Ready() {
    HMODULE module = nullptr;
    for (const auto* name : {sizeof(void*) == 4 ? L"PulseFileDialogRuntime32.dll" : L"PulseFileDialogRuntime64.dll",
        sizeof(void*) == 4 ? L"PulseFileDialogRuntimeUiProof32R8.dll" : L"PulseFileDialogRuntimeUiProof64R8.dll"}) {
        if ((module = GetModuleHandleW(name))) break;
    }
    query = module ? reinterpret_cast<Query>(GetProcAddress(module, "PulseFileDialogRuntimeQuery")) : nullptr;
    PulseFileDialogRuntimeStatus status;
    const auto deadline = GetTickCount64() + 10000;
    config::Ready receipt;
    bool receipt_read = false;
    do {
        if (Status(status) && status.ready && status.initialization == S_OK) {
            std::ifstream file(fs::path(control) / config::kReadyFile, std::ios::binary);
            receipt_read = static_cast<bool>(file.read(reinterpret_cast<char*>(&receipt), sizeof(receipt)));
            if (receipt_read) break;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    wchar_t path[32768]{};
    if (module) GetModuleFileNameW(module, path, 32768);
    std::printf("READY caller_pid=%lu parent_pid=%lu pointer_bytes=%zu status_pid=%u ready=%u init=%08lx entries=%u expected=%u receipt=%u receipt_pid=%u receipt_machine=%u receipt_entries=%u runtime=%ls control=%ls\n",
        GetCurrentProcessId(), ParentPid(), sizeof(void*), status.process_id, status.ready,
        static_cast<unsigned long>(status.initialization), status.installed_entries, kEntries, receipt_read,
        receipt.target_pid, receipt.machine, receipt.installed_entries, path, control.c_str());
    return Status(status) && status.ready && status.initialization == S_OK && status.process_id == GetCurrentProcessId() &&
        (status.installed_entries & kEntries) == kEntries && receipt_read && receipt.target_pid == GetCurrentProcessId() &&
        receipt.machine == kMachine && receipt.status == S_OK && (receipt.installed_entries & kEntries) == kEntries;
}
bool ModuleLoaded(DWORD pid, const fs::path& expected) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szModule, expected.filename().c_str()) != 0) continue;
        std::error_code error;
        found = fs::equivalent(fs::path(entry.szExePath), expected, error);
        if (found) {
            std::printf("MODULE pid=%lu actual=%ls expected=%ls same_file=1\n", pid, entry.szExePath, expected.c_str());
            break;
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot); return found;
}
struct Plugin {
    HMODULE module = nullptr;
    PluginCreate create = nullptr;
    DelayCount count = nullptr;
    ~Plugin() { if (module) FreeLibrary(module); }
    bool Load(const wchar_t* leaf) {
        const auto path = Image().parent_path() / leaf;
        Check(!GetModuleHandleW(leaf), "entry DLL is absent until after managed ready");
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        create = module ? reinterpret_cast<PluginCreate>(GetProcAddress(module, "PulseEntryCreate")) : nullptr;
        count = module ? reinterpret_cast<DelayCount>(GetProcAddress(module, "PulseEntryDelayResolutions")) : nullptr;
        Check(module && create && count, "real loader resolves exact undecorated plugin exports");
        return module && create && count;
    }
};
enum class Route { Ordinary, Dynamic, Delay, Plugin, Extended, Cached };
const char* Name(Route route) {
    switch (route) {
    case Route::Ordinary: return "ordinary-import";
    case Route::Dynamic: return "dynamic-GetProcAddress";
    case Route::Delay: return "delay-import";
    case Route::Plugin: return "late-plugin";
    case Route::Extended: return "CoCreateInstanceEx";
    case Route::Cached: return "cached-CoGetClassObject-factory";
    }
    return "unknown";
}
struct Activator {
    Plugin plugin, delay;
    decltype(&CoCreateInstance) dynamic = nullptr;
    ComPtr<IClassFactory> open_factory, save_factory;
    bool Initialize() {
        dynamic = reinterpret_cast<decltype(dynamic)>(GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance"));
        Check(dynamic != nullptr, "GetProcAddress resolves the actual system CoCreateInstance");
        if (!plugin.Load(L"PulseDialogEntryPlugin.dll") || !delay.Load(L"PulseDialogEntryDelay.dll")) return false;
        Check(delay.count() == 0, "delay import has not resolved before the first call");
        PulseFileDialogRuntimeStatus before, after;
        Status(before);
        const auto first = CoGetClassObject(CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&open_factory));
        const auto second = CoGetClassObject(CLSID_FileSaveDialog, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&save_factory));
        Status(after);
        std::printf("FACTORY acquired_after_ready=1 open=%08lx save=%08lx activation_delta=%llu lifetime=retained-across-object-release-and-multiple-Shows preexisting_process_injection=0\n",
            static_cast<unsigned long>(first), static_cast<unsigned long>(second), static_cast<unsigned long long>(after.activations - before.activations));
        Check(first == S_OK && second == S_OK && after.activations - before.activations == 2,
            "two actual CoGetClassObject entries return retained factories after ready");
        return dynamic && open_factory && save_factory;
    }
    HRESULT Create(Route route, bool save, IFileDialog** dialog) {
        const auto clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        switch (route) {
        case Route::Ordinary: return CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog));
        case Route::Dynamic: return dynamic(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog));
        case Route::Delay: return delay.create(save, dialog);
        case Route::Plugin: return plugin.create(save, dialog);
        case Route::Cached: return (save ? save_factory : open_factory)->CreateInstance(nullptr, IID_PPV_ARGS(dialog));
        case Route::Extended: {
            MULTI_QI result{&IID_IFileDialog, nullptr, E_PENDING};
            const auto hr = CoCreateInstanceEx(clsid, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, &result);
            if (SUCCEEDED(hr) && SUCCEEDED(result.hr) && result.pItf) *dialog = static_cast<IFileDialog*>(result.pItf);
            else if (result.pItf) result.pItf->Release();
            return FAILED(hr) ? hr : result.hr;
        }
        }
        return E_UNEXPECTED;
    }
};
bool Capture(HWND window, const fs::path& path) {
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;
    HDC dc = GetWindowDC(window), memory = dc ? CreateCompatibleDC(dc) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, width, height) : nullptr;
    if (!bitmap) { if (memory) DeleteDC(memory); if (dc) ReleaseDC(window, dc); return false; }
    const auto old = SelectObject(memory, bitmap);
    const BOOL captured = PrintWindow(window, memory, 2);
    SelectObject(memory, old);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const auto lines = GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    bool written = false;
    if (captured && lines == height) {
        BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        written = static_cast<bool>(file);
    }
    std::printf("CAPTURE pid=%lu width=%d height=%d print=%u lines=%d written=%u path=%ls\n",
        GetCurrentProcessId(), width, height, captured, lines, written, path.c_str());
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc); return written;
}
struct Ui {
    IFileDialog* dialog = nullptr;
    std::wstring title;
    fs::path screenshot;
    ULONGLONG started = 0, first = 0;
    DWORD pid = 0;
    bool cancel = false, seen = false, native = false, acted = false, expired = false, luma = false, ui_module = false, capture = false;
};
thread_local Ui* active = nullptr;
BOOL CALLBACK Inspect(HWND window, LPARAM parameter) {
    auto& ui = *reinterpret_cast<Ui*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (!IsWindowVisible(window) || pid != GetCurrentProcessId()) return TRUE;
    wchar_t title[256]{}, type[128]{};
    GetWindowTextW(window, title, 256); GetClassNameW(window, type, 128);
    if (ui.title != title) return TRUE;
    if (wcscmp(type, L"#32770") == 0) {
        ui.native = true; PostMessageW(window, WM_COMMAND, IDCANCEL, 0); return TRUE;
    }
    if (wcscmp(type, L"PulseFolderPickerWindow")) return TRUE;
    ui.seen = true; ui.pid = pid;
    if (!ui.first) ui.first = GetTickCount64();
    if (!ui.acted && GetTickCount64() - ui.first >= 600) {
        ui.acted = true;
        ui.luma = ModuleLoaded(pid, dependency / L"lumatext.dll");
        ui.ui_module = ModuleLoaded(pid, dependency / L"PulsePickerUiProof.dll");
        ui.capture = Capture(window, ui.screenshot);
        if (ui.cancel) ui.dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        else {
            const float scale = window_dpi(window) / 96.0f;
            RECT client{}; GetClientRect(window, &client);
            const int x = client.right - static_cast<int>(70 * scale), y = client.bottom - static_cast<int>(30 * scale);
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        }
    }
    return TRUE;
}
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!active) return;
    EnumWindows(Inspect, reinterpret_cast<LPARAM>(active));
    if (GetTickCount64() - active->started > 10000) {
        active->expired = true; active->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
void Run(Activator& activator, Route route, bool gui, bool child) {
    const auto directory = output / fs::path(Name(route));
    fs::create_directory(directory);
    const auto source = directory / L"source-中文.txt", saved = directory / L"saved-中文.txt";
    const auto payload = "B5 " + std::to_string(sizeof(void*) * 8) + "-bit " + Name(route) + " actual file consumption\n";
    { std::ofstream file(source, std::ios::binary); file << payload; }
    for (unsigned scenario = 0; scenario < (gui ? 4u : 2u); ++scenario) {
        const bool save = (scenario & 1) != 0, cancel = gui && scenario < 2;
        ComPtr<IFileDialog> dialog;
        PulseFileDialogRuntimeStatus before{}, created{}, after{};
        const bool status_before = Status(before);
        auto hr = activator.Create(route, save, &dialog);
        const bool status_created = Status(created);
        const uint64_t expected = route == Route::Cached ? 0u : 1u;
        const auto delta = created.activations - before.activations;
        const bool activation = status_before && status_created && hr == S_OK && dialog && delta == expected;
        std::printf("ENTRY route=%s pid=%lu parent_pid=%lu pointer_bytes=%zu child=%u save=%u hr=%08lx activation_delta=%llu expected=%llu\n",
            Name(route), GetCurrentProcessId(), ParentPid(), sizeof(void*), child, save, static_cast<unsigned long>(hr),
            static_cast<unsigned long long>(delta), static_cast<unsigned long long>(expected));
        Check(activation, "real API traverses installed Detours entry (cached CreateInstance retains wrapped factory)");
        if (!activation || !gui) continue;
        ComPtr<IShellItem> folder;
        hr = SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT |
            (save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN : FOS_FILEMUSTEXIST));
        if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
        if (SUCCEEDED(hr)) hr = dialog->SetFileName((save ? saved : source).filename().c_str());
        Ui ui; ui.dialog = dialog.Get(); ui.cancel = cancel;
        ui.title = L"B5 " + std::to_wstring(GetCurrentProcessId()) + L" " + fs::path(Name(route)).wstring() + L" " + std::to_wstring(scenario);
        ui.screenshot = directory / (std::to_wstring(scenario) + L".bmp");
        if (SUCCEEDED(hr)) hr = dialog->SetTitle(ui.title.c_str());
        active = &ui; ui.started = GetTickCount64();
        const auto timer = SUCCEEDED(hr) ? SetTimer(nullptr, 0, 40, Tick) : 0;
        if (timer) hr = dialog->Show(nullptr);
        if (timer) KillTimer(nullptr, timer);
        active = nullptr;
        ComPtr<IShellItem> result;
        const auto get = dialog->GetResult(&result);
        bool matches = false, consumed = false;
        if (!cancel && hr == S_OK && get == S_OK && result) {
            PWSTR value = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                const fs::path selected(value); CoTaskMemFree(value);
                matches = selected == (save ? saved : source);
                if (matches && save) { std::ofstream file(selected, std::ios::binary); file << payload; }
                if (matches) consumed = Read(selected) == payload;
            }
        }
        const bool status_after = Status(after);
        const bool pass = timer && ui.seen && !ui.native && !ui.expired && ui.luma && ui.ui_module && ui.capture &&
            status_after && after.admitted > created.admitted && after.rejected == created.rejected &&
            (cancel ? hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && FAILED(get) && !result && !fs::exists(saved) :
                hr == S_OK && matches && consumed);
        std::printf("UI route=%s caller_pid=%lu ui_pid=%lu pointer_bytes=%zu child=%u save=%u cancel=%u visible=%u native=%u luma=%u ui_module=%u capture=%u expired=%u hr=%08lx result=%08lx path=%u read_or_write_reopen=%u admitted_delta=%llu rejected_delta=%llu\n",
            Name(route), GetCurrentProcessId(), ui.pid, sizeof(void*), child, save, cancel, ui.seen, ui.native,
            ui.luma, ui.ui_module, ui.capture, ui.expired, static_cast<unsigned long>(hr), static_cast<unsigned long>(get), matches, consumed,
            static_cast<unsigned long long>(after.admitted - created.admitted), static_cast<unsigned long long>(after.rejected - created.rejected));
        Check(pass, "actual managed Pulse UI, full physical screenshot, file consumption or clean cancellation");
    }
}
void Child(bool gui) {
    const auto destination = output / L"child";
    auto command = L"\"" + Image().wstring() + L"\" " + (gui ? L"ui" : L"headless") + L" \"" + destination.wstring() + L"\" --child " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const bool created = CreateProcessW(Image().c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child) != FALSE;
    std::printf("CHILD parent_pid=%lu child_pid=%lu pointer_bytes=%zu actual_CreateProcessW=%u error=%lu\n",
        GetCurrentProcessId(), child.dwProcessId, sizeof(void*), created, created ? ERROR_SUCCESS : GetLastError());
    Check(created, "actual CreateProcessW launches a new fixture descendant");
    if (!created) return;
    const auto waited = WaitForSingleObject(child.hProcess, gui ? 90000 : 20000);
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(child.hProcess, &code);
    if (waited != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 124); WaitForSingleObject(child.hProcess, 5000); }
    std::printf("CHILD_EXIT parent_pid=%lu child_pid=%lu wait=%lu exit=%lu\n", GetCurrentProcessId(), child.dwProcessId, waited, code);
    Check(waited == WAIT_OBJECT_0 && code == 0, "new descendant independently verifies bound ready, entry activation, and requested UI matrix");
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
}
DWORD WINAPI Watchdog(void* stop) {
    if (WaitForSingleObject(stop, 180000) == WAIT_TIMEOUT) ExitProcess(124);
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 5) return 2;
    const bool gui = wcscmp(argv[1], L"ui") == 0, child = argc == 5 && wcscmp(argv[3], L"--child") == 0;
    if ((!gui && wcscmp(argv[1], L"headless")) || (argc == 5 && !child)) return 2;
    output = argv[2];
    if (!output.is_absolute() || fs::exists(output) || !fs::create_directory(output)) return 2;
    int file = -1;
    if (_wsopen_s(&file, (output / L"report.txt").c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_TEXT | _O_NOINHERIT,
        _SH_DENYWR, _S_IREAD | _S_IWRITE)) return 2;
    const int redirected = _dup2(file, _fileno(stdout)); _close(file);
    if (redirected || !SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))), HANDLE_FLAG_INHERIT, 0)) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Keep the project's Win8.1 declaration floor. Missing modern DPI APIs fail
    // the fixture instead of silently approving virtualized/cropped captures.
    const auto user32 = GetModuleHandleW(L"user32.dll");
    using ProcessDpiContext = BOOL (WINAPI*)(HANDLE);
    const auto process_dpi = reinterpret_cast<ProcessDpiContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    window_dpi = reinterpret_cast<WindowDpi>(GetProcAddress(user32, "GetDpiForWindow"));
    const bool dpi = process_dpi && window_dpi && process_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    std::printf("DPI process_api=%u window_api=%u success=%u context=-4 error=%lu\n",
        process_dpi != nullptr, window_dpi != nullptr, dpi, dpi ? ERROR_SUCCESS : GetLastError());
    Check(dpi,
        "DPI PER_MONITOR_AWARE_V2 succeeds (no virtualized-window screenshot approval)");
    control = Env(L"PULSE_DIALOG_CONTROL");
    dependency = fs::path(Env(L"PULSE_DIALOG_HOST")).parent_path();
    Check(Ready(), "actual injected runtime and PID/machine-bound ready receipt agree on all entry bits");
    if (child) Check(ParentPid() == wcstoul(argv[4], nullptr, 10), "descendant parent PID equals the actual creating fixture");
    if (failures) return 1;
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE watchdog = stop ? CreateThread(nullptr, 0, Watchdog, stop, 0, nullptr) : nullptr;
    if (!watchdog) return 2;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "native COM apartment initializes");
    if (SUCCEEDED(initialized)) {
        {
            Activator activator;
            if (activator.Initialize()) {
                if (child) Run(activator, Route::Ordinary, gui, true);
                else for (const auto route : {Route::Ordinary, Route::Dynamic, Route::Delay, Route::Plugin, Route::Extended, Route::Cached})
                    Run(activator, route, gui, false);
                Check(activator.delay.count() == (child ? 0 : 1), "delay helper performs exactly one real CoCreateInstance import resolution");
            }
        }
        if (!child) Child(gui);
        CoUninitialize();
    }
    std::printf("Checks: %u, failures: %u; mode=%s pid=%lu pointer_bytes=%zu child=%u real_API_only=1 registration_changed=0\n",
        checks, failures, gui ? "ui" : "headless", GetCurrentProcessId(), sizeof(void*), child);
    SetEvent(stop); WaitForSingleObject(watchdog, INFINITE); CloseHandle(watchdog); CloseHandle(stop);
    Sleep(500); // Keep the owned target alive while the production launcher consumes ready.
    return failures ? 1 : 0;
}
