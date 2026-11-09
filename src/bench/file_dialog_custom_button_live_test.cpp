#include "../shell_bridge/file_dialog_wrapper.h"
#ifdef PULSE_REMOTE_PROVIDER_TEST
#include "../shell_bridge/file_dialog_remote_provider.h"
#else
#include "../shell_bridge/file_dialog_ui_provider.h"
#endif
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace {
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
constexpr DWORD group_id = 45127, button_id = 57291;
constexpr HRESULT close_result = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x629);
constexpr ULONG_PTR input_marker = 0x50424342;
unsigned checks = 0, failures = 0;
void Check(bool value, const char* text) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text); }
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct State {
    IUnknown* identity = nullptr; // Main retains wrapper until after Unadvise.
    DWORD thread = GetCurrentThreadId(), cookies[2]{};
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> input_ok{false};
    std::atomic<ULONG_PTR> expected_key{0}, acknowledged_key{0};
    unsigned input_sequence = 0;
    HANDLE ready = nullptr, finished = nullptr;
    unsigned clicks[2]{}, sequence = 0;
    bool identity_ok = true, id_ok = true, ordered = true;
    HRESULT unadvise = E_PENDING, closed = E_PENDING;
};
thread_local State* observing_input = nullptr;
LRESULT CALLBACK ObserveInput(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE && observing_input) {
        const auto* message = reinterpret_cast<const MSG*>(lp);
        const auto marker = static_cast<ULONG_PTR>(GetMessageExtraInfo());
        if (message->message == WM_KEYUP && marker > input_marker &&
            marker == observing_input->expected_key.load()) {
            observing_input->acknowledged_key.store(marker);
            std::printf("INPUT_ACK key=%llu marker=%llu\n",
                static_cast<unsigned long long>(message->wParam), static_cast<unsigned long long>(marker));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
bool Owned(HWND window, DWORD thread) {
    DWORD pid = 0;
#ifdef PULSE_REMOTE_PROVIDER_TEST
    (void)thread;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &pid) && pid != GetCurrentProcessId();
#else
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &pid) == thread && pid == GetCurrentProcessId();
#endif
}
class Sink final : public IFileDialogEvents, public IFileDialogControlEvents {
    std::atomic<ULONG> refs_{1}; State& state_; unsigned index_;
    bool Identity(IUnknown* from) {
        ComPtr<IUnknown> identity;
        return from && SUCCEEDED(from->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()))) && identity.Get() == state_.identity;
    }
public:
    Sink(State& state, unsigned index) : state_(state), index_(index) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER; *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) *output = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents) *output = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --refs_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override {
        state_.identity_ok &= Identity(dialog) && GetCurrentThreadId() == state_.thread;
        ComPtr<IOleWindow> ole; HWND window = nullptr;
        const HRESULT queried = dialog->QueryInterface(IID_PPV_ARGS(ole.GetAddressOf()));
        const HRESULT got = SUCCEEDED(queried) ? ole->GetWindow(&window) : queried;
        std::printf("TYPE sink=%u GetWindow=%08lX hwnd=%p\n", index_ + 1, static_cast<unsigned long>(got), static_cast<void*>(window));
        if (got == S_OK && Owned(window, state_.thread)) { state_.window = window; SetEvent(state_.ready); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* customize, DWORD id) override {
        ++state_.clicks[index_];
        state_.identity_ok &= Identity(customize) && GetCurrentThreadId() == state_.thread;
        state_.id_ok &= id == button_id;
        state_.ordered &= ++state_.sequence == index_ + 1;
        std::printf("BUTTON sink=%u id=%lu sequence=%u identity=%d\n", index_ + 1, id, state_.sequence, state_.identity_ok);
        if (index_ == 0) return E_FAIL; // Control-event HRESULT must not suppress the next sink.
        ComPtr<IFileDialog> dialog;
        const HRESULT qi = customize->QueryInterface(IID_PPV_ARGS(dialog.GetAddressOf()));
        if (FAILED(qi)) return qi;
        state_.unadvise = dialog->Unadvise(state_.cookies[1]);
        state_.closed = dialog->Close(close_result);
        std::printf("SECOND self_Unadvise=%08lX Close=%08lX\n", static_cast<unsigned long>(state_.unadvise), static_cast<unsigned long>(state_.closed));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { Check(false, "custom button must not submit a file"); return S_FALSE; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* result) override { if (!result) return E_POINTER; *result = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* result) override { if (!result) return E_POINTER; *result = FDEOR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }
};
bool Key(State& state, WORD key, ULONGLONG deadline) {
    const HWND window = state.window.load();
    if (!Owned(window, state.thread) || GetForegroundWindow() != window ||
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) return false;
    const ULONG_PTR marker = input_marker + ++state.input_sequence;
    state.expected_key.store(marker);
    INPUT keys[2]{};
    for (auto& input : keys) { input.type = INPUT_KEYBOARD; input.ki.wVk = key; input.ki.dwExtraInfo = marker; }
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(2, keys, sizeof(INPUT));
    std::printf("SEND key=%u sent=%u target=%p\n", static_cast<unsigned>(key), sent, static_cast<void*>(window));
    // Release a partially delivered key only to this still-owned foreground.
    // A partial send remains a failure, regardless of safety release success.
    if (sent == 1 && Owned(window, state.thread) && GetForegroundWindow() == window) SendInput(1, &keys[1], sizeof(INPUT));
    if (sent != 2) return false;
    if (key != VK_TAB) return true; // Space can close Show on keydown.
    while (state.acknowledged_key.load() != marker && GetTickCount64() < deadline &&
        WaitForSingleObject(state.finished, 0) == WAIT_TIMEOUT) Sleep(1);
    const bool consumed = state.acknowledged_key.load() == marker;
    if (!consumed) std::printf("DRIVER Tab key release not consumed within original budget\n");
    return consumed;
}
void Drive(State& state) {
    HANDLE waits[]{state.ready, state.finished};
    if (WaitForMultipleObjects(2, waits, FALSE, 8000) != WAIT_OBJECT_0) return;
    const auto deadline = GetTickCount64() + 8000;
    bool prepared = false, activation_attempted = false;
    while (GetTickCount64() < deadline && WaitForSingleObject(state.finished, 0) == WAIT_TIMEOUT) {
        const HWND window = state.window.load();
        GUITHREADINFO info{sizeof(info)};
        const HWND edit = Owned(window, state.thread) ? GetDlgItem(window, 102) : nullptr;
        if (!activation_attempted && edit && IsWindowVisible(window)) {
            activation_attempted = true;
            const BOOL activated = SetForegroundWindow(window);
            std::printf("DRIVER own-foreground activation=%d\n", activated);
        }
        if (edit && IsWindowEnabled(edit) && IsWindowVisible(edit) && GetForegroundWindow() == window &&
            GetGUIThreadInfo(state.thread, &info) && info.hwndFocus == edit) { prepared = true; break; }
        // Foreground activation may focus the top-level window. Navigate using
        // real input until the observed filename edit has focus; never assume it.
        if (edit && IsWindowEnabled(edit) && IsWindowVisible(edit) && GetForegroundWindow() == window) {
            if (!Key(state, VK_TAB, deadline)) break;
            // The own Show STA has consumed this Tab before focus is sampled again.
        } else Sleep(25);
    }
    if (!prepared) {
        const HWND window = state.window.load();
        GUITHREADINFO info{sizeof(info)};
        const BOOL got = GetGUIThreadInfo(state.thread, &info);
        std::printf("DRIVER not-ready owned=%d visible=%d foreground=%d got_focus=%d edit=%p focus=%p\n",
            Owned(window, state.thread), IsWindowVisible(window), GetForegroundWindow() == window,
            got, static_cast<void*>(GetDlgItem(window, 102)), static_cast<void*>(info.hwndFocus));
    }
    // Single custom button: filename -> filter -> custom button. Button identity
    // is proven by the actual COM callback, never inferred solely from Tab count.
    bool sent = prepared && Key(state, VK_TAB, deadline);
    if (sent) sent = Key(state, VK_TAB, deadline);
    if (sent) sent = Key(state, VK_SPACE, deadline);
    state.input_ok = sent;
    if (WaitForSingleObject(state.finished, 6000) != WAIT_OBJECT_0) {
        const HWND window = state.window.load();
        if (Owned(window, state.thread)) PostMessageW(window, WM_CLOSE, 0, 0);
        std::printf("DRIVER bounded cleanup after missing button completion (not success)\n");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    const bool manual = argc == 3 && std::wstring(argv[2]) == L"--manual";
    const bool deferred = argc == 3 && std::wstring(argv[2]) == L"--deferred";
    const bool empty_name = argc == 3 && std::wstring(argv[2]) == L"--empty-name";
    if ((argc != 2 && !manual && !deferred && !empty_name) || !std::filesystem::path(argv[1]).is_absolute()) return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Handle complete(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!complete.value) return 2;
    std::thread watchdog([&] {
        if (WaitForSingleObject(complete.value, manual ? 90000 : 30000) != WAIT_OBJECT_0) {
            std::printf("[FAIL] own-process watchdog; fixture retained\n"); std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    const HRESULT com = OleInitialize(nullptr);
    try {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"custom-button-live-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root.parent_path());
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw GetLastError();
        const auto file = root / L"owned.txt";
        HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(created != INVALID_HANDLE_VALUE, "unique fixture file created");
        if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
        Check(SUCCEEDED(com), "calling STA initialized");
        if (SUCCEEDED(com) && created != INVALID_HANDLE_VALUE) {
            std::shared_ptr<const FileDialogInprocessProvider> provider;
#ifdef PULSE_REMOTE_PROVIDER_TEST
            const HRESULT loaded = LoadFileDialogRemoteProvider(argv[1], provider, true);
#else
            const HRESULT loaded = LoadFileDialogUiProvider(argv[1], provider, true);
#endif
            std::printf("PROVIDER hr=%08lX\n", static_cast<unsigned long>(loaded));
            Check(loaded == S_OK && provider, "real proof provider loaded with explicit test gate");
            if (loaded == S_OK && provider) {
                Check(provider->allow_unverified_proof && provider->api.capabilities == 0, "real cap-zero UI is explicitly test-only");
                HWND owner = CreateWindowW(L"STATIC", L"Pulse custom button live verification", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "owned visible caller window");
                State state; Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)), finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                state.ready = ready.value; state.finished = finished.value;
                unsigned native_calls = 0, gate_calls = 0;
                FileDialogBridgeServices services; services.executable = argv[1]; services.inprocess_provider = provider;
                services.access_policy = AccessPolicy::LocalOpenV1;
                services.validation_verified = [&](const ContractDecision& decision) {
                    ++gate_calls;
                    return decision.disposition == Disposition::Prepared && decision.validation_category == ValidationCategory::LocalOpenAccessV1 &&
                        decision.supported_access_policy == AccessPolicy::LocalOpenV1;
                };
                services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_UNEXPECTED; };
                bool published = false;
                unsigned resolutions = 0;
                if (deferred) {
                    const auto ready_services = services;
                    services.inprocess_provider.reset();
                    services.validation_verified = {};
                    services.access_policy = AccessPolicy::None;
                    services.resolve_on_show = [&, ready_services](FileDialogBridgeServices& output) {
                        ++resolutions;
                        if (!published) return false;
                        output = ready_services; return true;
                    };
                }
                ComPtr<IFileDialog> dialog;
                HRESULT hr = CreatePulseFileDialog(DialogFactory::Open, services, IID_PPV_ARGS(dialog.GetAddressOf()));
                Check(hr == S_OK && dialog, "explicit real COM wrapper created");
                if (hr == S_OK && dialog && owner && ready.value && finished.value) {
                    ComPtr<IUnknown> identity; hr = dialog.As(&identity); state.identity = identity.Get();
                    ComPtr<IShellItem> folder;
                    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()));
                    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
                    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
                    if (SUCCEEDED(hr)) hr = dialog->SetFileName(empty_name ? L"" : L"owned.txt");
                    ComPtr<IFileDialogCustomize> custom;
                    if (SUCCEEDED(hr)) hr = dialog.As(&custom);
                    if (SUCCEEDED(hr)) hr = custom->StartVisualGroup(group_id, L"Live custom controls");
                    if (SUCCEEDED(hr)) hr = custom->AddPushButton(button_id, L"Verify custom callback");
                    if (SUCCEEDED(hr)) hr = custom->EndVisualGroup();
                    Check(hr == S_OK, "arbitrary group/button IDs configured through native-mirrored COM");
                    ComPtr<IFileDialogEvents> sinks[2];
                    for (unsigned i = 0; SUCCEEDED(hr) && i != 2; ++i) {
                        sinks[i].Attach(new Sink(state, i)); hr = dialog->Advise(sinks[i].Get(), &state.cookies[i]);
                        Check(hr == S_OK, "Advise real dual-interface sink");
                    }
                    if (hr == S_OK) {
                        std::printf("MODE %s: select Verify custom callback; expected two sinks then custom Close\n", manual ? "manual (90s)" : "SendInput (30s)");
                        observing_input = &state;
                        HHOOK input_hook = manual ? nullptr : SetWindowsHookExW(WH_GETMESSAGE, ObserveInput, nullptr, GetCurrentThreadId());
                        if (!manual) Check(input_hook != nullptr, "observe only own STA genuine input consumption");
                        std::thread driver;
                        if (!manual) driver = std::thread([&] { Drive(state); });
                        if (deferred) Check(resolutions == 0, "early native setters and subscriptions retain pending resolver" );
                        published = true;
                        const HRESULT shown = dialog->Show(owner);
                        if (deferred) Check(resolutions == 1, "real UI Show binds published provider exactly once" );
                        SetEvent(finished.value);
                        if (driver.joinable()) driver.join();
                        if (input_hook) Check(UnhookWindowsHookEx(input_hook) != FALSE, "remove own input observation hook" );
                        observing_input = nullptr;
                        std::printf("SHOW hr=%08lX gate=%u native=%u clicks=[%u,%u]\n", static_cast<unsigned long>(shown), gate_calls,
                            native_calls, state.clicks[0], state.clicks[1]);
                        if (!manual) Check(state.input_ok, "real SendInput navigation dispatched only to owned foreground");
                        Check(gate_calls == 1 && native_calls == 0, "real inprocess path with no native replay");
                        Check(state.identity_ok && state.id_ok && state.ordered && state.clicks[0] == 1 && state.clicks[1] == 1,
                            "first E_FAIL still reaches second sink with exact wrapper identity and button ID");
                        Check(state.unadvise == S_OK && state.closed == S_OK && shown == close_result, "self Unadvise survives and custom Close HRESULT is exact");
                        ComPtr<IShellItem> result;
                        Check(FAILED(dialog->GetResult(result.GetAddressOf())) && !result, "custom Close never publishes a file result");
                        Check(IsWindowEnabled(owner) != FALSE, "owned caller reenabled after Show");
                    }
                    if (state.cookies[0]) Check(SUCCEEDED(dialog->Unadvise(state.cookies[0])), "first sink released");
                    if (state.cookies[1] && state.unadvise != S_OK) Check(SUCCEEDED(dialog->Unadvise(state.cookies[1])), "remaining second sink released");
                }
                dialog.Reset();
                if (owner) Check(DestroyWindow(owner) != FALSE, "owned caller destroyed");
            }
        }
        if (created != INVALID_HANDLE_VALUE) Check(DeleteFileW(file.c_str()) != FALSE, "exact fixture file removed");
        Check(RemoveDirectoryW(root.c_str()) != FALSE, "owned empty fixture directory removed");
    } catch (...) { Check(false, "setup exception; exact fixture retained if needed"); }
    if (SUCCEEDED(com)) OleUninitialize();
    SetEvent(complete.value); watchdog.join();
    std::printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
