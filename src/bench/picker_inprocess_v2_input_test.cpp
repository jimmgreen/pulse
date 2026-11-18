#include "../picker_inprocess/picker_ui_api_v2.h"
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {
struct DpiApi {
    using GetThread = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using SetThread = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);
    using Equal = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    GetThread get_thread = nullptr;
    SetThread set_thread = nullptr;
    Equal equal = nullptr;
    DpiApi() {
        const HMODULE user = GetModuleHandleW(L"user32.dll");
        if (!user) return;
        get_thread = reinterpret_cast<GetThread>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
        set_thread = reinterpret_cast<SetThread>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
        equal = reinterpret_cast<Equal>(GetProcAddress(user, "AreDpiAwarenessContextsEqual"));
    }
    bool Available() const { return get_thread && set_thread && equal; }
};
DpiApi dpi;
unsigned checks = 0, failures = 0;
constexpr ULONG_PTR kMarker = 0x56324950;
void Check(bool ok, const char* label) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); }
bool Owned(HWND window, DWORD thread) {
    DWORD pid = 0;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &pid) == thread && pid == GetCurrentProcessId();
}
struct State {
    PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    DWORD thread = 0;
    std::wstring expected, expected_second;
    bool manual = false;
    uint32_t expected_count = 1;
    HANDLE ready = nullptr, veto = nullptr, ended = nullptr;
    DPI_AWARENESS_CONTEXT caller_dpi = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<unsigned> keyups{0}, accepts{0}, releases_before_accept{0};
    std::atomic<bool> valid{true}, input_ok{false}, same_veto{false};
    uint64_t show = 0, first_generation = 0, accepted_generation = 0;
};
thread_local State* observing = nullptr;
LRESULT CALLBACK Observe(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE && observing) {
        const auto* message = reinterpret_cast<const MSG*>(lp);
        if (message->message == WM_KEYUP && message->wParam == VK_RETURN &&
            static_cast<ULONG_PTR>(GetMessageExtraInfo()) == kMarker) ++observing->keyups;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
HRESULT WINAPI Event(void* opaque, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<State*>(opaque);
    try {
        if (!event || event->size != sizeof(*event)) { state.valid = false; return E_INVALIDARG; }
        const bool identity = GetCurrentThreadId() == state.thread && Owned(event->window, state.thread) &&
            dpi.equal(dpi.get_thread(), state.caller_dpi);
        if (!identity) state.valid = false;
        HWND actual = nullptr;
        const HRESULT window_hr = state.api->GetWindow(state.session, event->show_id, &actual);
        if (window_hr != S_OK || actual != event->window) state.valid = false;
        std::printf("EVENT kind=%u show=%llu generation=%llu thread=%lu hwnd=%p identity=%d\n", event->kind,
            event->show_id, event->generation, GetCurrentThreadId(), static_cast<void*>(event->window), identity);
        if (event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
            if (state.window.load()) state.valid = false;
            state.show = event->show_id; state.window = event->window; SetEvent(state.ready);
            return S_OK;
        }
        if (event->kind != PULSE_PICKER_UI_V2_EVENT_FILE_OK) return S_OK;
        if (event->show_id != state.show || event->window != state.window.load()) state.valid = false;
        PulsePickerUiV2Snapshot size{}; size.size = sizeof(size);
        HRESULT hr = state.api->Query(state.session, event->show_id, event->generation, &size);
        const bool lengths = size.folder.units <= PULSE_PICKER_UI_V2_MAX_UNITS && size.filename.units <= PULSE_PICKER_UI_V2_MAX_UNITS &&
            size.selection.units <= PULSE_PICKER_UI_V2_MAX_UNITS && size.candidate.units <= PULSE_PICKER_UI_V2_MAX_UNITS;
        if (hr != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || !lengths) { state.valid = false; return E_UNEXPECTED; }
        std::vector<wchar_t> folder(size.folder.units + 1), filename(size.filename.units + 1),
            selection(size.selection.units + 1), candidate(size.candidate.units + 1);
        std::vector<wchar_t> selections(size.selection_paths.units + 1), candidates(size.candidate_paths.units + 1);
        PulsePickerUiV2Snapshot snapshot{}; snapshot.size = sizeof(snapshot);
        snapshot.selection_paths = {selections.data(), static_cast<uint32_t>(selections.size()), 0};
        snapshot.candidate_paths = {candidates.data(), static_cast<uint32_t>(candidates.size()), 0};
        snapshot.folder = {folder.data(), static_cast<uint32_t>(folder.size()), 0};
        snapshot.filename = {filename.data(), static_cast<uint32_t>(filename.size()), 0};
        snapshot.selection = {selection.data(), static_cast<uint32_t>(selection.size()), 0};
        snapshot.candidate = {candidate.data(), static_cast<uint32_t>(candidate.size()), 0};
        hr = state.api->Query(state.session, event->show_id, event->generation, &snapshot);
        const bool candidate_ok = hr == S_OK && snapshot.show_id == event->show_id && snapshot.generation == event->generation &&
            snapshot.window == event->window && (snapshot.flags & PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY) &&
            std::wstring(candidate.data()) == state.expected;
        if (!candidate_ok) state.valid = false;
        if (state.manual && snapshot.candidate_count != state.expected_count) state.valid = false;
        const unsigned count = ++state.accepts;
        std::printf("FILE_OK count=%u query=%08lX candidate_exact=%d keyups=%u\n", count,
            static_cast<unsigned long>(hr), candidate_ok, state.keyups.load());
        if (count == 1) { state.first_generation = event->generation; SetEvent(state.veto); return S_FALSE; }
        state.releases_before_accept = state.keyups.load(); state.accepted_generation = event->generation;
        if (count != 2 || event->generation <= state.first_generation || (!state.manual && !state.releases_before_accept.load())) state.valid = false;
        if (state.manual) state.same_veto = count == 2 && event->window == state.window.load() && IsWindow(event->window);
        return state.valid ? S_OK : E_UNEXPECTED;
    } catch (...) { state.valid = false; return E_UNEXPECTED; }
}
bool Enter(State& state, unsigned attempt, ULONGLONG deadline) {
    const HWND window = state.window.load();
    // A background-launched test does not necessarily receive foreground rights.
    // Activate only its own verified picker, then retain the strict input gate.
    if (Owned(window, state.thread) && IsWindowVisible(window) && GetForegroundWindow() != window) {
        const BOOL activated = SetForegroundWindow(window);
        std::printf("INPUT own-foreground activation=%d\n", activated);
    }
    const ULONGLONG foreground_deadline = (std::min)(deadline, GetTickCount64() + 4000);
    while (Owned(window, state.thread) && GetForegroundWindow() != window && GetTickCount64() < foreground_deadline &&
        WaitForSingleObject(state.ended, 0) != WAIT_OBJECT_0) Sleep(10);
    if (!Owned(window, state.thread) || !IsWindowVisible(window) || GetForegroundWindow() != window ||
        GetTickCount64() >= deadline || WaitForSingleObject(state.ended, 0) == WAIT_OBJECT_0) {
        std::printf("INPUT attempt=%u refused: own exact HWND is not foreground/visible or budget ended\n", attempt); return false;
    }
    INPUT input[2]{};
    for (auto& value : input) { value.type = INPUT_KEYBOARD; value.ki.wVk = VK_RETURN; value.ki.dwExtraInfo = kMarker; }
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(2, input, sizeof(INPUT));
    const DWORD error = sent == 2 ? ERROR_SUCCESS : GetLastError();
    std::printf("INPUT attempt=%u actual_SendInput_count=%u error=%lu\n", attempt, sent, error);
    if (sent == 1 && Owned(window, state.thread) && GetForegroundWindow() == window) {
        const UINT released = SendInput(1, &input[1], sizeof(INPUT));
        std::printf("INPUT partial-send own-foreground release=%u (attempt remains failed)\n", released);
    }
    return sent == 2;
}
void Drive(State& state) {
    const ULONGLONG deadline = GetTickCount64() + 20000;
    const DWORD waited = WaitForSingleObject(state.ready, 8000);
    std::printf("DRIVER ready_wait=%lu\n", waited);
    if (waited == WAIT_OBJECT_0 && Enter(state, 1, deadline)) {
        while (GetTickCount64() < deadline && WaitForSingleObject(state.ended, 0) != WAIT_OBJECT_0 &&
            (WaitForSingleObject(state.veto, 0) != WAIT_OBJECT_0 || state.keyups.load() < 1)) Sleep(10);
        const HWND window = state.window.load();
        state.same_veto = WaitForSingleObject(state.veto, 0) == WAIT_OBJECT_0 && state.keyups.load() >= 1 &&
            Owned(window, state.thread) && IsWindowVisible(window);
        if (state.same_veto && GetTickCount64() < deadline) state.input_ok = Enter(state, 2, deadline);
    }
    if (!state.input_ok && Owned(state.window.load(), state.thread) && WaitForSingleObject(state.ended, 0) != WAIT_OBJECT_0) {
        // Cleanup only, never used as input evidence or acceptance.
        PostMessageW(state.window.load(), WM_CLOSE, 0, 0);
    }
}
int Run(const wchar_t* dll, bool folders, bool manual, bool multiple) {
    if (!std::filesystem::path(dll).is_absolute()) return 2;
    Check(dpi.Available(), "required caller DPI APIs dynamically available");
    if (!dpi.Available()) return 1;
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"picker-v2-input-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(directory.parent_path());
    if (!CreateDirectoryW(directory.c_str(), nullptr)) { std::printf("fixture root error=%lu\n", GetLastError()); return 2; }
    const auto file = directory / L"selection.txt";
    if (folders) {
        if (!CreateDirectoryW(file.c_str(), nullptr)) { std::printf("fixture directory error=%lu\n", GetLastError()); RemoveDirectoryW(directory.c_str()); return 2; }
    } else {
        HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (created == INVALID_HANDLE_VALUE) { RemoveDirectoryW(directory.c_str()); return 2; }
        CloseHandle(created);
    }
    const auto second = directory / L"selection2.txt";
    if (folders && manual && !CreateDirectoryW(second.c_str(), nullptr)) return 2;
    const auto prior_dpi = dpi.set_thread(DPI_AWARENESS_CONTEXT_UNAWARE);
    Check(prior_dpi != nullptr, "capture caller DPI context");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "caller STA on current interactive desktop");
    if (SUCCEEDED(com)) {
        HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(module != nullptr, "load explicit absolute UI DLL");
        using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
        auto get = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        PulsePickerUiV2Api api{};
        const HRESULT api_hr = get ? get(2, sizeof(api), &api) : E_NOINTERFACE;
        const bool valid = api_hr == S_OK && api.size == sizeof(api) && api.version == 2 && api.Initialize && api.ValidateConfig &&
            api.Show && api.Query && api.GetWindow && api.Close && api.Shutdown;
        Check(valid, "v2 table; internal proof only, required_capabilities=0");
        bool unload_safe = true;
        if (valid) {
            PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = 2;
            PulsePickerUiV2Session session = 0;
            const HRESULT initialized = api.Initialize(&init, &session);
            Check(initialized == S_OK && session, "initialize session");
            if (initialized == S_OK && session) {
                unload_safe = false;
                HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse owned v2 input proof", WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, 400, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "own test owner");
                if (owner) {
                    ShowWindow(owner, SW_SHOW); SetForegroundWindow(owner); // Only our owner; no AttachThreadInput.
                    State state; state.api = &api; state.session = session; state.thread = GetCurrentThreadId();
                    state.expected = file.wstring(); state.expected_second = second.wstring(); state.manual = manual; state.expected_count = multiple ? 2u : 1u; state.caller_dpi = dpi.get_thread();
                    state.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr); state.veto = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    state.ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    Check(state.ready && state.veto && state.ended, "owned driver synchronization");
                    const std::wstring folder = directory.wstring(), filename = L"selection.txt", title = L"Pulse v2 real input proof";
                    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2; config.owner = owner;
                    if (folders && manual) config.open_options = PULSE_PICKER_UI_V2_OPEN_ALLOW_MULTI_SELECT;
                    config.required_capabilities = 0; config.access_policy = folders ? PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1 : PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                    config.initial_folder = {folder.c_str(), static_cast<uint32_t>(folder.size())};
                    config.filename = {filename.c_str(), static_cast<uint32_t>(filename.size())};
                    config.title = {title.c_str(), static_cast<uint32_t>(title.size())};
                    const HRESULT validated = api.ValidateConfig(session, &config);
                    Check(validated == S_OK, "validate proof config");
                    if (state.ready && state.veto && state.ended && validated == S_OK) {
                        observing = &state;
                        HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, Observe, nullptr, state.thread);
                        Check(hook != nullptr, "observe genuine marker Return key releases on Show STA");
                        if (hook) {
                            PulsePickerUiV2Callbacks callbacks{}; callbacks.size = sizeof(callbacks); callbacks.callback = Event; callbacks.context = &state;
                            std::vector<wchar_t> buffer(PULSE_PICKER_UI_V2_MAX_UNITS + 1);
                            std::vector<wchar_t> packed(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
                            PulsePickerUiV2Result result{}; result.size = sizeof(result); result.paths = {packed.data(), static_cast<uint32_t>(packed.size()), 0}; result.path = {buffer.data(), static_cast<uint32_t>(buffer.size()), 0};
                            std::thread driver([&] { if (!manual) Drive(state); else if (WaitForSingleObject(state.ended, 120000) != WAIT_OBJECT_0 && Owned(state.window.load(), state.thread)) PostMessageW(state.window.load(), WM_CLOSE, 0, 0); });
                            const HRESULT shown = api.Show(session, &config, &callbacks, &result);
                            std::printf("RESULT path_count=%u packed_units=%u\n", result.path_count, result.paths.units);
                            SetEvent(state.ended); driver.join();
                            const unsigned at_return = state.keyups.load();
                            const ULONGLONG release_deadline = GetTickCount64() + 250;
                            while (state.keyups.load() < 2 && GetTickCount64() < release_deadline) {
                                MSG message{};
                                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                                    TranslateMessage(&message); DispatchMessageW(&message);
                                } else MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                            }
                            Check(UnhookWindowsHookEx(hook) != FALSE, "remove own thread message hook"); observing = nullptr;
                            std::printf("RESULT Show=%08lX status=%u callbacks=%u releases_at_return=%u releases_after_pump=%u before_accept=%u\n",
                                static_cast<unsigned long>(shown), result.status, state.accepts.load(), at_return, state.keyups.load(), state.releases_before_accept.load());
                            if (!manual) {
                                Check(state.input_ok && state.keyups.load() >= 2, "both real SendInput releases retrieved; no synthetic KeyUp");
                                Check(state.releases_before_accept.load() >= 1, "first release observed before second acceptance");
                            } else {
                                Check(result.path_count == state.expected_count, "manual exact accepted directory count");
                                const std::wstring expected_packed = state.expected + std::wstring(1, L'\0') + (multiple ? state.expected_second + std::wstring(1, L'\0') : L"");
                                Check(result.paths.units == expected_packed.size() && std::equal(expected_packed.begin(), expected_packed.end(), packed.begin()), "manual exact accepted directory paths");
                            }
                            Check(state.same_veto && state.accepts.load() == 2, "S_FALSE retains same own window until second input");
                            Check(state.valid, "callback candidate exact fixture, snapshot generation HWND and caller DPI valid");
                            Check(shown == S_OK && result.terminal_hr == S_OK && result.status == PULSE_PICKER_UI_V2_ACCEPTED &&
                                result.show_id == state.show && result.generation == state.accepted_generation &&
                                std::wstring(buffer.data()) == state.expected, "second FileOk commits precise validated generation and fixture");
                            Check((result.flags & PULSE_PICKER_UI_V2_UI_SHOWN) != 0, "actual UI_SHOWN");
                            Check(IsWindowEnabled(owner) && !IsWindow(state.window.load()), "owner restored and own picker destroyed");
                            Check(dpi.equal(dpi.get_thread(), state.caller_dpi), "caller DPI restored after Show");
                        }
                        observing = nullptr;
                    }
                    if (state.ready) CloseHandle(state.ready); if (state.veto) CloseHandle(state.veto); if (state.ended) CloseHandle(state.ended);
                    DestroyWindow(owner);
                }
                const HRESULT shutdown = api.Shutdown(session); unload_safe = shutdown == S_OK;
                Check(unload_safe, "Shutdown releases owned session");
            }
        }
        if (module && unload_safe) FreeLibrary(module);
        CoUninitialize();
    }
    if (prior_dpi) dpi.set_thread(prior_dpi);
    Check((folders ? RemoveDirectoryW(file.c_str()) : DeleteFileW(file.c_str())) != FALSE, "delete own fixture");
    if (folders && manual) Check(RemoveDirectoryW(second.c_str()) != FALSE, "delete second owned directory");
    Check(RemoveDirectoryW(directory.c_str()) != FALSE, "delete own empty fixture directory");
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) { std::printf("Usage: picker_inprocess_v2_input_test ABSOLUTE_UI_DLL\n"); return 2; }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!done) return 2;
    std::thread watchdog([done, argc] {
        if (WaitForSingleObject(done, argc == 4 ? 150000 : 30000) != WAIT_OBJECT_0) {
            std::printf("WATCHDOG own input proof exceeded 30 seconds; fixture retained, not a pass\n");
            std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    int result = 2;
    try { result = Run(argv[1], argc >= 3 && std::wstring_view(argv[2]) == L"--folders", argc == 4 && (std::wstring_view(argv[3]) == L"--manual" || std::wstring_view(argv[3]) == L"--manual-multi"), argc == 4 && std::wstring_view(argv[3]) == L"--manual-multi"); } catch (...) { Check(false, "exception; owned fixture retained"); }
    SetEvent(done); watchdog.join(); CloseHandle(done);
    std::printf("Checks=%u failures=%u result=%d\n", checks, failures, result); std::fflush(stdout);
    return result;
}
