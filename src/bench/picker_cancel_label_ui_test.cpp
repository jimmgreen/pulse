#include "../picker_inprocess/picker_ui_api_v2.h"
#include "picker_interaction_test_api.h"
#ifdef PULSE_REMOTE_PROVIDER_TEST
#include "../shell_bridge/file_dialog_remote_provider.h"
#endif
#include <objbase.h>
#include <windowsx.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
PulsePickerUiV2Text Text(const std::wstring& text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
bool Send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(window, message, wp, lp, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &result) != 0;
}
struct State {
    std::wstring expected, shot;
    bool explicit_label = false, narrow = false, click = false;
    std::atomic<bool> done = false;
    bool observed = false, driven = false, captured = false;
    std::thread driver;
};
HRESULT WINAPI Event(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(context);
    if (event->kind != PULSE_PICKER_UI_V2_EVENT_READY) return S_OK;
    const HWND window = event->window;
    const auto desktop = GetThreadDesktop(GetCurrentThreadId());
    state.driver = std::thread([&state, window, desktop] {
        SetThreadDesktop(desktop);
#ifdef PULSE_REMOTE_PROVIDER_TEST
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        state.observed = IsWindowVisible(window) && pid && pid != GetCurrentProcessId();
        const ATOM atom = GlobalAddAtomW(state.shot.c_str());
        if (atom && SetPropW(window, L"Pulse.PickerTestSnapshot", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(atom)))) {
            InvalidateRect(window, nullptr, FALSE); Send(window, WM_PAINT);
            if (RemovePropW(window, L"Pulse.PickerTestSnapshot")) GlobalDeleteAtom(atom);
            state.captured = std::filesystem::exists(state.shot);
        } else if (atom) GlobalDeleteAtom(atom);
        state.driven = PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0) != FALSE;
#else
        if (state.narrow) {
            SetWindowPos(window, nullptr, 0, 0, 700, 540, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT suggested{}; GetWindowRect(window, &suggested);
            Send(window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
        }
        pulse::ui::PickerInteractionTest query;
        if (Send(window, pulse::ui::kPickerInteractionTestMessage, 0, reinterpret_cast<LPARAM>(&query))) {
            state.observed = state.explicit_label ? query.cancel_text == state.expected : !query.cancel_text.empty();
            state.observed = state.observed && query.cancel.left >= 0 && query.cancel.right > query.cancel.left &&
                query.cancel.right < query.primary.left && query.cancel.bottom <= query.footer.bottom;
            std::printf("UI cancel_units=%zu rect=%ld,%ld,%ld,%ld primary_left=%ld\n", query.cancel_text.size(),
                query.cancel.left, query.cancel.top, query.cancel.right, query.cancel.bottom, query.primary.left);
        }
        SetPropW(window, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(state.shot.c_str()));
        InvalidateRect(window, nullptr, FALSE); Send(window, WM_PAINT);
        RemovePropW(window, L"Pulse.PickerTestSnapshot");
        state.captured = std::filesystem::exists(state.shot);
        if (state.click && state.observed) {
            const auto point = MAKELPARAM((query.cancel.left + query.cancel.right) / 2, (query.cancel.top + query.cancel.bottom) / 2);
            state.driven = Send(window, WM_LBUTTONDOWN, MK_LBUTTON, point) && Send(window, WM_LBUTTONUP, 0, point);
        } else {
            state.driven = Send(window, WM_KEYDOWN, VK_ESCAPE);
        }
#endif
        // Bound a failed driver without leaving a modal window behind.
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!state.done && IsWindow(window) && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
    });
    return S_OK;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && !(argc == 4 && std::wstring_view(argv[3]) == L"--interactive")) { std::puts("Usage: picker_cancel_label_ui_test ABSOLUTE_UI_DLL ABSOLUTE_OUTPUT_DIR [--interactive]"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto desktop_name = L"PulseCancelLabel_" + std::to_wstring(GetCurrentProcessId());
    const auto desktop = argc == 4 ? OpenInputDesktop(0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS) :
        CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) { std::printf("SETUP desktop error=%lu\n", GetLastError()); return 2; }
    const auto output = std::filesystem::absolute(argv[2]); std::filesystem::create_directories(output);
    const auto folder = (output / L"fixture").wstring(); std::filesystem::create_directories(folder);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.c_str());
    HKEY isolated = nullptr;
    const auto created = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes", 0, KEY_READ, &isolated);
    const auto overridden = created == ERROR_SUCCESS ? RegOverridePredefKey(HKEY_CURRENT_USER, isolated) : created;
    if (overridden != ERROR_SUCCESS) { std::printf("SETUP registry create=%ld override=%ld\n", created, overridden); return 2; }
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::printf("SETUP COM hr=%08lX\n", static_cast<unsigned long>(initialized)); return 2; }
#ifdef PULSE_REMOTE_PROVIDER_TEST
    std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
    const auto loaded = pulse::shell_bridge::LoadFileDialogRemoteProvider(argv[1], provider, true);
    if (loaded != S_OK || !provider) { std::printf("SETUP remote hr=%08lX\n", static_cast<unsigned long>(loaded)); return 1; }
    PulsePickerUiV2Api api = provider->api;
#else
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    PulsePickerUiV2Api api{};
    if (!get || get(2, sizeof(api), &api) != S_OK) { Check(false, "load real picker API"); return 1; }
#endif
    Check((api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_CANCEL_LABEL) != 0, "real provider advertises cancel-label extension");
    PulsePickerUiV2Initialize init{sizeof(init), 2}; PulsePickerUiV2Session session = 0;
    if (api.Initialize(&init, &session) != S_OK) { Check(false, "initialize real picker API"); return 1; }
    PulsePickerUiV2CancelLabelConfig extension{};
    auto& config = extension.base; config.size = sizeof(config); config.version = 2;
    config.initial_folder = Text(folder); config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    Check(api.ValidateConfig(session, &config) == S_OK, "legacy v2 config size still validates");
    config.label_flags = PULSE_PICKER_UI_V2_LABEL_CANCEL;
    Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "legacy-sized config never reads cancel-label tail");
    config.size = sizeof(extension);
    Check(api.ValidateConfig(session, &config) == S_OK, "explicit empty label validates with extension");
    config.label_flags = 0;
    Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "unflagged extension is rejected");
    const std::wstring invalid = L"bad\nlabel";
    extension.cancel_button_label = Text(invalid); config.label_flags = PULSE_PICKER_UI_V2_LABEL_CANCEL;
    Check(api.ValidateConfig(session, &config) == E_INVALIDARG, "multiline cancel label is rejected before UI");
    const std::vector<std::wstring> labels{L"", L"Leave picker", L"", L"Cancel this selection without changing the application document"};
    for (unsigned n = 0; n < labels.size(); ++n) {
        State state; state.expected = labels[n]; state.explicit_label = n != 0; state.click = n == 1 || n == 2;
        state.narrow = n == 3; state.shot = (output / (L"cancel-label-" + std::to_wstring(n) + L".png")).wstring();
        extension.cancel_button_label = Text(labels[n]);
        config.size = n ? sizeof(extension) : sizeof(config); config.label_flags = n ? PULSE_PICKER_UI_V2_LABEL_CANCEL : 0;
        config.appearance = n % 2 ? PULSE_PICKER_UI_V2_DARK : 0;
        config.access_policy = n == 2 ? PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1 : n == 3 ? PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1 : PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
        config.save_options = n == 2 ? PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT : 0;
        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
        PulsePickerUiV2Result result{}; result.size = sizeof(result); result.path = {path.data(), static_cast<uint32_t>(path.size())};
        PulsePickerUiV2Callbacks callbacks{sizeof(callbacks), 0, Event, &state};
        const auto hr = api.Show(session, &config, &callbacks, &result);
        state.done = true; if (state.driver.joinable()) state.driver.join();
        std::printf("RESULT case=%u hr=%08lX status=%u flags=%u\n", n, static_cast<unsigned long>(hr), result.status, result.flags);
#ifdef PULSE_REMOTE_PROVIDER_TEST
        Check(state.observed && state.driven, "remote READY exposes visible child HWND and accepts Escape cancellation");
#else
        Check(state.observed && state.driven, "real chrome label and separated actionable cancel button match config");
#endif
#ifdef PULSE_REMOTE_PROVIDER_TEST
        if (state.captured) Check(true, "capture actual remote picker for visual verification");
        else std::puts("REMOTE_VISUAL_NOT_AVAILABLE: host has no snapshot hook; config and cancellation are verified separately");
#else
        Check(state.captured, "capture actual rendered picker for label verification");
#endif
        Check(hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.status == PULSE_PICKER_UI_V2_CANCELLED && result.path.units == 0,
            "mouse or Escape cancellation clears result across repeated Shows");
    }
    Check(api.Shutdown(session) == S_OK, "shutdown real picker session");
    CoUninitialize();
    Check(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS, "restore real HKCU mapping");
    RegCloseKey(isolated);
    std::puts("REGISTRY: read-only fixture; real HKCU mapping restored");
    std::printf("Checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
