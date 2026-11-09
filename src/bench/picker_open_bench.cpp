// Dialog open latency: Windows native FileOpenDialog vs Pulse in-process UI DLL
// vs Pulse remote host. Usage:
//   pulse_picker_open_bench native|inprocess|remote ABSOLUTE_PATH COUNT
// ABSOLUTE_PATH is the UI DLL (inprocess), pulse.exe (remote), or ignored (native).
// Runs on the current desktop: DWM only composes the input desktop.
#include "../picker_inprocess/picker_ui_api_v2.h"
#include "../shell_bridge/file_dialog_remote_provider.h"
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
LARGE_INTEGER frequency{};
int64_t Now() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
double Ms(int64_t from, int64_t to) { return to > from ? (to - from) * 1000.0 / frequency.QuadPart : -1.0; }

// First top-level picker window shown after arming, from any process.
std::atomic<int64_t> visible_at{0};
std::atomic<bool> armed{false};
std::atomic<HWND> visible_window{nullptr};
void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (!armed || object != OBJID_WINDOW || child != CHILDID_SELF || !hwnd || GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    wchar_t name[64]{};
    if (!GetClassNameW(hwnd, name, 64)) return;
    if (wcscmp(name, L"PulseFolderPickerWindow") != 0 && wcscmp(name, L"#32770") != 0) return;
    int64_t expected = 0;
    if (visible_at.compare_exchange_strong(expected, Now())) visible_window = hwnd;
}

struct Sample { double visible = -1, ready = -1, total = -1, init = -1; HRESULT hr = S_OK; };
struct PulseState { const PulsePickerUiV2Api* api = nullptr; PulsePickerUiV2Session session = 0; int64_t ready_at = 0; };
HRESULT WINAPI Callback(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<PulseState*>(context);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY && !state.ready_at) {
        state.ready_at = Now();
        state.api->Close(state.session, event->show_id, HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    return S_OK;
}

Sample ShowPulse(const PulsePickerUiV2Api& api, HWND owner, const std::wstring& folder) {
    Sample sample;
    visible_at = 0; armed = true;
    const int64_t start = Now();
    PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = 2;
    PulseState state; state.api = &api;
    sample.hr = api.Initialize(&init, &state.session);
    sample.init = Ms(start, Now());
    if (SUCCEEDED(sample.hr)) {
        PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
        config.owner = owner; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.initial_folder = {folder.c_str(), static_cast<uint32_t>(folder.size())};
        PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks);
        callbacks.callback = Callback; callbacks.context = &state;
        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
        PulsePickerUiV2Result result{}; result.size = sizeof(result);
        result.path = {path.data(), static_cast<uint32_t>(path.size()), 0};
        sample.hr = api.ValidateConfig(state.session, &config);
        if (SUCCEEDED(sample.hr)) sample.hr = api.Show(state.session, &config, &callbacks, &result);
        api.Shutdown(state.session);
    }
    const int64_t end = Now();
    armed = false;
    sample.visible = Ms(start, visible_at.load());
    sample.ready = Ms(start, state.ready_at);
    sample.total = Ms(start, end);
    return sample;
}

Sample ShowNative(IClassFactory* factory, HWND owner, const std::wstring& folder) {
    Sample sample;
    visible_at = 0; visible_window = nullptr; armed = true;
    const int64_t start = Now();
    IFileOpenDialog* dialog = nullptr;
    sample.hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(sample.hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) { dialog->SetFolder(item); item->Release(); }
        // Close as soon as the window is visible, matching Pulse's READY close.
        std::atomic<bool> stop{false};
        std::thread closer([&] {
            while (!stop && !visible_window.load()) Sleep(1);
            if (HWND window = visible_window.load()) PostMessageW(window, WM_CLOSE, 0, 0);
        });
        sample.hr = dialog->Show(owner);
        stop = true; closer.join();
        dialog->Release();
    }
    const int64_t end = Now();
    armed = false;
    sample.visible = Ms(start, visible_at.load());
    sample.total = Ms(start, end);
    return sample;
}

double Median(std::vector<double> values) {
    if (values.empty()) return -1;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { std::printf("Usage: pulse_picker_open_bench native|inprocess|remote ABSOLUTE_PATH COUNT\n"); return 2; }
    const std::wstring mode = argv[1];
    const int count = std::max(1, _wtoi(argv[3]));
    QueryPerformanceFrequency(&frequency);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 2;

    // Isolated fixture folder with a few files.
    const auto folder = std::filesystem::absolute(L"bench_data/picker-open-bench");
    std::filesystem::create_directories(folder);
    for (int i = 0; i < 20; ++i) {
        const auto file = folder / (L"file-" + std::to_wstring(i) + L".txt");
        if (!std::filesystem::exists(file)) std::ofstream(file) << "open bench fixture";
    }

    std::atomic<DWORD> hook_thread{0};
    HANDLE hook_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread hook([&] {
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        const HWINEVENTHOOK handle = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0, WINEVENT_OUTOFCONTEXT);
        hook_thread = GetCurrentThreadId();
        SetEvent(hook_ready);
        while (handle && GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        if (handle) UnhookWinEvent(handle);
    });
    WaitForSingleObject(hook_ready, INFINITE);
    CloseHandle(hook_ready);

    HWND owner = CreateWindowW(L"STATIC", L"Pulse open bench owner", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::vector<Sample> samples;
    std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> remote;
    HMODULE module = nullptr;
    IClassFactory* factory = nullptr;
    PulsePickerUiV2Api api{};
    HRESULT setup = E_INVALIDARG;
    const int64_t setup_start = Now();
    if (mode == L"native") {
        // Load the system implementation directly so a per-user COM override
        // (such as Pulse's own dialog interception) cannot replace it.
        module = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using GetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        const auto get = module ? reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
        setup = get ? get(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    } else if (mode == L"inprocess") {
        module = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
        const auto get = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        setup = get ? get(PULSE_PICKER_UI_V2_VERSION, sizeof(api), &api) : E_NOINTERFACE;
    } else if (mode == L"remote") {
        setup = pulse::shell_bridge::LoadFileDialogRemoteProvider(argv[2], remote, true);
        if (remote) api = remote->api;
    }
    const double setup_ms = Ms(setup_start, Now());
    if (FAILED(setup)) { std::printf("setup failed %08lX\n", static_cast<unsigned long>(setup)); return 2; }
    for (int i = 0; i < count; ++i) {
        samples.push_back(factory ? ShowNative(factory, owner, folder.wstring()) : ShowPulse(api, owner, folder.wstring()));
        const auto& s = samples.back();
        std::printf("%ls run=%d init_ms=%.1f visible_ms=%.1f ready_ms=%.1f total_ms=%.1f hr=%08lX\n", mode.c_str(), i + 1,
            s.init, s.visible, s.ready, s.total, static_cast<unsigned long>(s.hr));
        Sleep(300);
    }
    std::vector<double> warm_visible, warm_ready;
    for (size_t i = 1; i < samples.size(); ++i) { warm_visible.push_back(samples[i].visible); warm_ready.push_back(samples[i].ready); }
    std::printf("SUMMARY mode=%ls setup_ms=%.1f cold_visible_ms=%.1f cold_ready_ms=%.1f warm_visible_median_ms=%.1f warm_ready_median_ms=%.1f\n",
        mode.c_str(), setup_ms, samples[0].visible, samples[0].ready, Median(warm_visible), Median(warm_ready));
    if (factory) factory->Release();
    remote.reset();
    DestroyWindow(owner);
    PostThreadMessageW(hook_thread, WM_QUIT, 0, 0);
    hook.join();
    CoUninitialize();
    return 0;
}
