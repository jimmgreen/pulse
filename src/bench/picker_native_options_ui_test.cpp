#include "../picker_inprocess/picker_ui_api_v2.h"
#include <shobjidl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
unsigned failures = 0;
void Check(bool value, const char* text) { std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text); std::fflush(stdout); if (!value) ++failures; }
PulsePickerUiV2Text Text(const std::wstring& text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
bool Send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0) {
    DWORD_PTR result = 0; return SendMessageTimeoutW(window, message, wp, lp, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &result) != 0;
}
bool Wait(const std::function<bool()>& ready, const std::atomic<bool>& done) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!done && std::chrono::steady_clock::now() < end) { if (ready()) return true; std::this_thread::sleep_for(10ms); }
    return ready();
}
HWND Confirmation() {
    HWND found = nullptr;
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        DWORD process = 0; GetWindowThreadProcessId(window, &process); wchar_t name[64]{}; GetClassNameW(window, name, 64);
        if (process == GetCurrentProcessId() && std::wstring_view(name) == L"PulseConfirmWindow" && IsWindowVisible(window)) {
            *reinterpret_cast<HWND*>(value) = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}
struct State {
    std::atomic<bool> done = false;
    std::atomic<unsigned> file_ok = 0, sharing = 0;
    std::thread driver;
    std::function<bool(HWND, State&)> drive;
    bool driven = false;
    uint32_t share_response = 1;
    ~State() { done = true; if (driver.joinable()) driver.join(); }
};
HRESULT WINAPI Event(void* opaque, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(opaque);
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_FILE_OK) ++state.file_ok;
    if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        const HWND window = event->window;
        const auto desktop = GetThreadDesktop(GetCurrentThreadId());
        state.driver = std::thread([&state, window, desktop] {
            SetThreadDesktop(desktop); state.driven = state.drive(window, state);
            if (const auto confirm = Confirmation()) PostMessageW(confirm, WM_CLOSE, 0, 0);
            if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
        });
    }
    return S_OK;
}
HRESULT WINAPI Overwrite(void*, const PulsePickerUiV2OverwriteEvent*, uint32_t* response) { *response = 1; return S_OK; }
HRESULT WINAPI Share(void* opaque, const PulsePickerUiV2OverwriteEvent*, uint32_t* response) {
    auto& state = *static_cast<State*>(opaque); ++state.sharing; *response = state.share_response; return S_OK;
}
struct Result { HRESULT hr = E_FAIL; uint32_t status = 0; std::wstring path; unsigned file_ok = 0, sharing = 0; bool driven = false; };
Result Run(PulsePickerUiV2Api& api, PulsePickerUiV2Session session, PulsePickerUiV2Config& config,
    const std::function<bool(HWND, State&)>& drive, uint32_t share_response = 1) {
    State state; state.drive = drive; state.share_response = share_response;
    PulsePickerUiV2Callbacks callbacks{sizeof(callbacks), 0, Event, &state, Overwrite, Share};
    std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
    PulsePickerUiV2Result result{}; result.size = sizeof(result); result.path = {path.data(), static_cast<uint32_t>(path.size())};
    const auto hr = api.Show(session, &config, &callbacks, &result);
    state.done = true; if (state.driver.joinable()) state.driver.join();
    std::printf("RESULT hr=%08lX status=%u error=%u file_ok=%u share=%u driven=%u\n", static_cast<unsigned long>(hr), result.status,
        result.win32_error, state.file_ok.load(), state.sharing.load(), state.driven);
    return {hr, result.status, path.data(), state.file_ok.load(), state.sharing.load(), state.driven};
}
bool Submit(HWND window) { return Send(GetDlgItem(window, 102), WM_KEYDOWN, VK_RETURN); }
bool SetName(HWND window, const wchar_t* text) { return Send(GetDlgItem(window, 102), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text)); }
bool ReadyAgain(HWND window, const State& state) { return Wait([&] { return IsWindowEnabled(GetDlgItem(window, 102)) != FALSE; }, state.done); }
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && !(argc == 4 && std::wstring_view(argv[3]) == L"--interactive")) { std::puts("Usage: picker_native_options_ui_test ABSOLUTE_UI_DLL ABSOLUTE_OUTPUT_DIR [--interactive]"); return 2; }
    if (argc == 3) {
        const auto desktop_name = L"PulseNativeOptions_" + std::to_wstring(GetCurrentProcessId());
        const auto desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) return 2;
    } else {
        const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS);
        if (!desktop || !SetThreadDesktop(desktop)) {
            std::printf("Input desktop attachment error=%lu\n", GetLastError()); return 2;
        }
    }
    const auto output = std::filesystem::path(argv[2]); std::filesystem::create_directories(output);
    const auto root = output / L"fixture"; std::filesystem::create_directories(root);
    std::ofstream(root / L"alpha.txt") << "Native options preview fixture\n";
    std::ofstream(root / L"blocked.bin") << "preserved";
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", output.c_str());
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<decltype(&PulsePickerUiGetApiV2)>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
    PulsePickerUiV2Api api{}; api.size = sizeof(api);
    if (!get || get(2, sizeof(api), &api) != S_OK) { Check(false, "load current real picker DLL API"); return 1; }
    Check((api.implemented_features & PULSE_PICKER_UI_V2_FEATURE_NATIVE_OPTIONS) != 0, "actual provider advertises implemented native option feature");
    PulsePickerUiV2Initialize initialize{sizeof(initialize), 2}; PulsePickerUiV2Session session = 0;
    if (api.Initialize(&initialize, &session) != S_OK) { Check(false, "initialize current real picker DLL"); return 1; }
    HWND owner = CreateWindowExW(0, L"STATIC", L"Native options fixture owner", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (argc == 4) {
        ShowWindow(owner, SW_HIDE); // Consume a launcher's STARTF_USESHOWWINDOW/SW_HIDE.
        ShowWindow(owner, SW_SHOWNOACTIVATE); UpdateWindow(owner);
    }
    const std::wstring folder = root.wstring(), initial = L"blocked.bin", label = L"Text", pattern = L"*.txt";
    PulsePickerUiV2Filter filter{sizeof(filter), 0, Text(label), Text(pattern)};
    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2; config.owner = owner;
    config.initial_folder = Text(folder); config.filename = Text(initial); config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
    config.filters = &filter; config.filter_count = 1;
    const std::wstring recycle = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
    auto virtual_config = config; virtual_config.initial_folder = Text(recycle);
    Check(FAILED(api.ValidateConfig(session, &virtual_config)), "zero native options rejects virtual initial folder");
    virtual_config.native_options = FOS_ALLNONSTORAGEITEMS;
    Check(api.ValidateConfig(session, &virtual_config) == S_OK, "all-nonstorage permits virtual recycle-bin initial folder");
    const auto submit_and_close = [](HWND window, State& state) { return Submit(window) && Wait([&] { return !IsWindow(window); }, state.done); };
    config.native_options = 0;
    auto ordinary = Run(api, session, config, submit_and_close);
    Check(ordinary.hr == S_OK && ordinary.driven && ordinary.path == (root / L"blocked.bin").wstring(),
        "zero native options preserves default advisory typed open filter");
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1; config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    ordinary = Run(api, session, config, submit_and_close);
    Check(ordinary.hr == S_OK && ordinary.driven && ordinary.path == (root / L"blocked.bin").wstring(),
        "zero native options preserves ordinary save explicit extension behavior");
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1; config.save_options = 0;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_STRICTFILETYPES;
    auto old = config; old.size -= sizeof(uint32_t);
    Check(api.ValidateConfig(session, &old) == E_INVALIDARG, "reject pre-native-options configuration size before reading its tail");
    const auto strict = [](HWND window, State& state) {
        if (!Submit(window) || !ReadyAgain(window, state)) return false;
        Check(state.file_ok == 0 && IsWindow(window), "strict mismatch stays open before FileOk");
        return SetName(window, L"alpha.txt") && Submit(window) && Wait([&] { return !IsWindow(window); }, state.done);
    };
    PulsePickerUiV2Callbacks old_callbacks{}; old_callbacks.size = static_cast<uint32_t>(offsetof(PulsePickerUiV2Callbacks, share_violation));
    PulsePickerUiV2Result invalid_result{}; invalid_result.size = sizeof(invalid_result);
    Check(api.Show(session, &config, &old_callbacks, &invalid_result) == E_INVALIDARG,
        "reject pre-share-callback size before reading its tail");
    auto result = Run(api, session, config, submit_and_close);
    Check(result.hr == S_OK && result.driven && result.path == (root / L"blocked.bin").wstring(), "strict flag preserves native advisory typed open filter");
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1; config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES;
    result = Run(api, session, config, strict);
    Check(result.hr == S_OK && result.driven && result.path == (root / L"alpha.txt").wstring(), "strict save rejects mismatched explicit extension");
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1; config.save_options = 0;
    const std::wstring alpha = L"alpha.txt"; config.filename = Text(alpha);
    config.native_options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_OKBUTTONNEEDSINTERACTION;
    result = Run(api, session, config, [](HWND window, State& state) {
        if (!Submit(window)) return false;
        Check(state.file_ok == 0 && IsWindow(window), "initial filename cannot bypass required interaction with Enter");
        return SetName(window, L"alpha.txt") && Submit(window) && Wait([&] { return !IsWindow(window); }, state.done);
    });
    Check(result.hr == S_OK && result.driven && result.file_ok == 1, "editing filename enables confirmation once");
    const std::wstring missing = L"missing.txt"; config.filename = Text(missing); config.open_options = PULSE_PICKER_UI_V2_OPEN_ALLOW_MISSING;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_CREATEPROMPT;
    const auto create = [](HWND window, State& state) {
        if (!Submit(window) || !Wait([] { return Confirmation() != nullptr; }, state.done)) return false;
        if (!Send(Confirmation(), WM_CLOSE) || !ReadyAgain(window, state)) return false;
        Check(state.file_ok == 0 && IsWindow(window), "declining create keeps picker open without FileOk");
        if (!Submit(window) || !Wait([] { return Confirmation() != nullptr; }, state.done)) return false;
        const auto prompt = Confirmation();
        return Send(prompt, WM_KEYDOWN, VK_TAB) && Send(prompt, WM_KEYDOWN, VK_RETURN) && Wait([&] { return !IsWindow(window); }, state.done);
    };
    result = Run(api, session, config, create);
    Check(result.hr == S_OK && result.driven && result.file_ok == 1 && !std::filesystem::exists(root / missing), "create prompt open consent returns missing name without creating it");
    config.open_options = 0; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
    config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT; config.native_options |= FOS_OVERWRITEPROMPT;
    result = Run(api, session, config, create);
    Check(result.hr == S_OK && result.driven && result.file_ok == 1 && !std::filesystem::exists(root / missing), "create prompt save consent survives access revalidation without duplicate prompt");
    const auto locked = root / L"alpha.txt";
    HANDLE lock = CreateFileW(locked.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "hold real sharing-conflict file");
    config.filename = Text(alpha); config.open_options = 0; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1; config.save_options = 0;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_SHAREAWARE;
    const auto sharing = [](HWND window, State& state) { return Submit(window) && Wait([&] { return !IsWindow(window); }, state.done); };
    result = Run(api, session, config, sharing);
    Check(result.hr == S_OK && result.driven && result.sharing == 1 && result.file_ok == 1, "share-aware open ACCEPT invokes conflict callback before FileOk");
    const auto reject_sharing = [](HWND window, State& state) {
        if (!Submit(window) || !Wait([&] { return state.sharing == 1; }, state.done) || !ReadyAgain(window, state)) return false;
        Check(state.file_ok == 0 && IsWindow(window), "unaccepted sharing conflict stays open before FileOk");
        return SetName(window, L"blocked.bin") && Submit(window) && Wait([&] { return !IsWindow(window); }, state.done);
    };
    for (const auto response : {0u, 2u}) {
        result = Run(api, session, config, reject_sharing, response);
        Check(result.hr == S_OK && result.driven && result.sharing == 1 && result.file_ok == 1 && result.path == (root / L"blocked.bin").wstring(),
            response == 0 ? "share-aware DEFAULT retains ordinary conflict handling" : "share-aware REFUSE permits a later valid selection");
    }
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1; config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_SHAREAWARE;
    result = Run(api, session, config, sharing);
    Check(result.hr == S_OK && result.driven && result.sharing == 1 && result.file_ok == 1, "share-aware save ACCEPT remains bound across final access revalidation");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1; config.save_options = 0;
    config.native_options = FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEPREVIEWPANEON | FOS_HIDEMRUPLACES | FOS_HIDEPINNEDPLACES;
    config.appearance = PULSE_PICKER_UI_V2_DARK;
    const std::wstring empty; config.filename = Text(empty);
    result = Run(api, session, config, [&](HWND window, State& state) {
        // Select a real file row; changing only the edit must not fabricate a selected preview.
        if (!Send(window, WM_KEYDOWN, VK_HOME)) return false;
        const bool preview = Wait([&] { return FindWindowExW(window, nullptr, L"Pulse.QuickPreview", nullptr) != nullptr; }, state.done);
        Check(preview, "forced preview owns an actual embedded preview content window");
        const auto shot = (output / L"native-options-dark-preview.png").wstring();
        SetPropW(window, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(shot.c_str())); Send(window, WM_PAINT); RemovePropW(window, L"Pulse.PickerTestSnapshot");
        Check(std::filesystem::exists(shot), "capture dark forced preview and hidden navigation layout");
        if (preview) {
            const auto child = FindWindowExW(window, nullptr, L"Pulse.QuickPreview", nullptr);
            const auto preview_shot = (output / L"native-options-dark-preview-content.png").wstring();
            Check(Wait([&] { return GetPropW(child, L"Pulse.PreviewTestContentReady") != nullptr; }, state.done),
                "actual selected file preview finishes decoding before capture");
            SetPropW(child, L"Pulse.PreviewTestSnapshot", const_cast<wchar_t*>(preview_shot.c_str())); Send(child, WM_PAINT); RemovePropW(child, L"Pulse.PreviewTestSnapshot");
            Check(std::filesystem::exists(preview_shot), "capture actual embedded preview rendered content");
        }
        if (preview) {
            RECT parent{}, child{}; GetClientRect(window, &parent); GetWindowRect(FindWindowExW(window, nullptr, L"Pulse.QuickPreview", nullptr), &child);
            POINT origin{child.left, child.top}; ScreenToClient(window, &origin);
            Check(origin.x > parent.right / 2 && origin.y >= 0 && origin.y + child.bottom - child.top < parent.bottom,
                "preview sidebar bounds fit beside file list and above footer");
        }
        RECT scaled{0, 0, 1500, 960};
        if (!Send(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&scaled))) return false;
        const auto scaled_shot = (output / L"native-options-dark-preview-144dpi.png").wstring();
        SetPropW(window, L"Pulse.PickerTestSnapshot", const_cast<wchar_t*>(scaled_shot.c_str())); Send(window, WM_PAINT); RemovePropW(window, L"Pulse.PickerTestSnapshot");
        Check(std::filesystem::exists(scaled_shot), "capture forced preview at 144 DPI");
        SetWindowPos(window, nullptr, 0, 0, 960, 700, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Check(Wait([&] { return !IsWindowVisible(FindWindowExW(window, nullptr, L"Pulse.QuickPreview", nullptr)); }, state.done),
            "narrow layout hides preview content before it overlaps the list");
        SetWindowPos(window, nullptr, 0, 0, 1500, 960, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Check(Wait([&] { return IsWindowVisible(FindWindowExW(window, nullptr, L"Pulse.QuickPreview", nullptr)) != FALSE; }, state.done),
            "widening restores selected embedded preview");
        return Send(window, WM_CLOSE);
    });
    Check(result.driven, "forced preview and hidden navigation case completes");
    Check(api.Shutdown(session) == S_OK, "shutdown actual picker cleanly"); DestroyWindow(owner);
    // The DLL is process-pinned while preview workers wind down, matching its production loader.
    CoUninitialize();
    std::printf("Failures=%u\n", failures); return failures ? 1 : 0;
}
