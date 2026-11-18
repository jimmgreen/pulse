#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/file_dialog_ui_provider.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
constexpr HRESULT close_result = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x62A);
constexpr ULONG_PTR input_marker = 0x50534C56;
enum class Mode { New, Accept, Refuse, Veto, Close, OverwriteClose, ReplaceFileOk };
bool ExistingMode(Mode mode) { return mode == Mode::Accept || mode == Mode::Refuse || mode == Mode::OverwriteClose || mode == Mode::ReplaceFileOk; }
unsigned checks = 0, failures = 0;
void Check(bool value, const char* text) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text); }
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Absent(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool Exact(IShellItem* item, const std::wstring& expected) {
    PWSTR path = nullptr;
    const HRESULT hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &path) : E_POINTER;
    const bool exact = SUCCEEDED(hr) && path && _wcsicmp(path, expected.c_str()) == 0;
    CoTaskMemFree(path); return exact;
}
bool ReadExisting(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info, const char* expected = "owned-existing") {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    char buffer[32]{}; DWORD read = 0;
    return file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.value, &info) &&
        ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr) && read == 14 && std::memcmp(buffer, expected, 14) == 0;
}
struct State {
    IUnknown* identity = nullptr;
    DWORD thread = GetCurrentThreadId();
    HWND owner = nullptr;
    bool owned_messages = false;
    Mode mode{};
    std::wstring expected, backup;
    bool replaced = false;
    BY_HANDLE_FILE_INFORMATION replacement_identity{};
    unsigned overwrite_sequence[2]{}, fileok_sequence[2]{};
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> input_ok{false}, cleanup_cancel{false};
    std::atomic<ULONG_PTR> expected_key{0}, acknowledged_key{0};
    std::atomic<WORD> expected_virtual_key{0};
    unsigned input_sequence = 0;
    HANDLE ready = nullptr, finished = nullptr, decision = nullptr;
    unsigned file_ok = 0, overwrite = 0, sequence = 0;
    bool identity_ok = true, candidate_ok = true;
    HRESULT closed = E_PENDING;
};
thread_local State* observing_input = nullptr;
LRESULT CALLBACK ObserveInput(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE && observing_input) {
        const auto* message = reinterpret_cast<const MSG*>(lp);
        const auto marker = static_cast<ULONG_PTR>(GetMessageExtraInfo());
        if ((message->message == WM_KEYDOWN || message->message == WM_KEYUP ||
            message->message == WM_SYSKEYDOWN || message->message == WM_SYSKEYUP) &&
            (message->wParam == VK_RETURN || message->wParam == VK_ESCAPE)) {
            std::printf("OWN_QUEUE msg=%04X key=%llu hwnd=%p marker=%llu expected=%llu focus=%p foreground=%p tick=%llu\n",
                message->message, static_cast<unsigned long long>(message->wParam), static_cast<void*>(message->hwnd),
                static_cast<unsigned long long>(marker), static_cast<unsigned long long>(observing_input->expected_key.load()),
                static_cast<void*>(GetFocus()), static_cast<void*>(GetForegroundWindow()), GetTickCount64());
        }
        if ((message->message == WM_KEYUP || message->message == WM_SYSKEYUP) &&
            GetCurrentThreadId() == observing_input->thread && marker > input_marker &&
            marker == observing_input->expected_key.load() &&
            message->wParam == observing_input->expected_virtual_key.load()) {
            observing_input->acknowledged_key.store(marker);
            std::printf("INPUT_ACK key=%llu marker=%llu\n",
                static_cast<unsigned long long>(message->wParam), static_cast<unsigned long long>(marker));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
bool Owned(HWND window, DWORD thread) {
    DWORD pid = 0;
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &pid) == thread && pid == GetCurrentProcessId();
}

class Sink final : public IFileDialogEvents {
    std::atomic<ULONG> refs_{1}; State& state_;
    bool Identity(IUnknown* from) {
        ComPtr<IUnknown> identity;
        return from && SUCCEEDED(from->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()))) &&
            identity.Get() == state_.identity && GetCurrentThreadId() == state_.thread;
    }
public:
    explicit Sink(State& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override {
        state_.identity_ok &= Identity(dialog);
        ComPtr<IOleWindow> ole; HWND window = nullptr;
        const HRESULT qi = dialog->QueryInterface(IID_PPV_ARGS(ole.GetAddressOf()));
        const HRESULT got = SUCCEEDED(qi) ? ole->GetWindow(&window) : qi;
        std::printf("TYPE hr=%08lX window=%p\n", static_cast<unsigned long>(got), static_cast<void*>(window));
        if (got == S_OK && Owned(window, state_.thread)) { state_.window = window; SetEvent(state_.ready); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        state_.identity_ok &= Identity(dialog); ++state_.file_ok;
        ComPtr<IShellItem> item; const HRESULT got = dialog->GetResult(item.GetAddressOf());
        state_.candidate_ok &= got == S_OK && Exact(item.Get(), state_.expected);
        if (!ExistingMode(state_.mode))
            Check(Absent(state_.expected), "new target absent inside real FileOk");
        const HRESULT answer = state_.mode == Mode::Veto && state_.file_ok == 1 ? S_FALSE : S_OK;
        std::printf("EVENT seq=%u FileOk count=%u candidate=%08lX return=%08lX\n", ++state_.sequence,
            state_.file_ok, static_cast<unsigned long>(got), static_cast<unsigned long>(answer));
        if (state_.file_ok <= 2) state_.fileok_sequence[state_.file_ok - 1] = state_.sequence;
        if (state_.mode == Mode::ReplaceFileOk && state_.file_ok == 1) {
            // Only the uniquely owned fixture is renamed; never replace in place
            // so the second metadata identity must differ from the approved one.
            const BOOL renamed = MoveFileExW(state_.expected.c_str(), state_.backup.c_str(), 0);
            bool written_ok = false;
            if (renamed) {
                Handle file(CreateFileW(state_.expected.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                DWORD written = 0;
                written_ok = file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, "owned-replaced", 14, &written, nullptr) &&
                    written == 14 && FlushFileBuffers(file.value);
            }
            state_.replaced = renamed && written_ok && ReadExisting(state_.expected, state_.replacement_identity, "owned-replaced");
            Check(state_.replaced, "first FileOk replaces only owned target with a newly created identity");
            if (!state_.replaced) { state_.closed = dialog->Close(E_FAIL); return E_FAIL; }
        }
        if (answer == S_FALSE) SetEvent(state_.decision);
        if (state_.mode == Mode::Close) state_.closed = dialog->Close(close_result);
        return answer;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog* dialog, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER;
        state_.identity_ok &= Identity(dialog); ++state_.overwrite;
        state_.candidate_ok &= Exact(item, state_.expected);
        *response = state_.mode == Mode::Refuse ? FDEOR_REFUSE : FDEOR_ACCEPT;
        std::printf("EVENT seq=%u Overwrite response=%u\n", ++state_.sequence, static_cast<unsigned>(*response));
        if (state_.overwrite <= 2) state_.overwrite_sequence[state_.overwrite - 1] = state_.sequence;
        if (state_.mode == Mode::OverwriteClose) state_.closed = dialog->Close(close_result);
        if (state_.mode == Mode::Refuse) SetEvent(state_.decision);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER; *response = FDESVR_DEFAULT; return S_OK;
    }
};
bool Key(State& state, WORD key, ULONGLONG deadline) {
    const HWND window = state.window.load();
    if (!Owned(window, state.thread) || GetForegroundWindow() != window ||
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) return false;
    const ULONG_PTR marker = input_marker + ++state.input_sequence;
    state.expected_virtual_key.store(key);
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

bool FocusFilename(State& state, ULONGLONG deadline) {
    bool activated = false, retried_ready = false;
    while (GetTickCount64() < deadline && WaitForSingleObject(state.finished, 0) == WAIT_TIMEOUT) {
        HWND window = state.window.load();
        HWND edit = Owned(window, state.thread) ? GetDlgItem(window, 102) : nullptr;
        if (edit && Owned(edit, state.thread) && IsWindowVisible(window) &&
            (!activated || (!retried_ready && IsWindowEnabled(edit) && IsWindowVisible(edit) && GetForegroundWindow() != window))) {
            const bool retry = activated; activated = true; if (retry) retried_ready = true;
            const BOOL activation = SetForegroundWindow(window);
            GUITHREADINFO observed{sizeof(observed)}; const BOOL queried = GetGUIThreadInfo(state.thread, &observed);
            DWORD foreground_pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
            std::printf("ACTIVATION retry=%d result=%d own=%p foreground=%p foreground_pid=%lu edit=%p visible=%d enabled=%d queried=%d focus=%p\n",
                retry, activation, static_cast<void*>(window), static_cast<void*>(GetForegroundWindow()), foreground_pid,
                static_cast<void*>(edit), IsWindowVisible(edit), IsWindowEnabled(edit), queried, static_cast<void*>(observed.hwndFocus));
        }
        GUITHREADINFO info{sizeof(info)};
        if (edit && Owned(edit, state.thread) && IsWindowEnabled(edit) && IsWindowVisible(edit) &&
            GetForegroundWindow() == window && GetGUIThreadInfo(state.thread, &info)) {
            if (info.hwndFocus == edit) return true;
            if (!Key(state, VK_TAB, deadline)) return false;
        } else Sleep(20);
    }
    const HWND window = state.window.load(); GUITHREADINFO final_focus{sizeof(final_focus)};
    const BOOL queried = GetGUIThreadInfo(state.thread, &final_focus);
    const HWND edit = Owned(window, state.thread) ? GetDlgItem(window, 102) : nullptr;
    std::printf("FOCUS_FAILED owned=%d visible=%d enabled=%d foreground=%p target=%p edit=%p queried=%d focus=%p\n",
        Owned(window, state.thread), edit && IsWindowVisible(edit), edit && IsWindowEnabled(edit),
        static_cast<void*>(GetForegroundWindow()), static_cast<void*>(window), static_cast<void*>(edit), queried, static_cast<void*>(final_focus.hwndFocus));
    return false;
}
bool WaitOwnedFilename(State& state, ULONGLONG deadline, HWND& edit) {
    while (GetTickCount64() < deadline && WaitForSingleObject(state.finished, 0) == WAIT_TIMEOUT) {
        const HWND window = state.window.load();
        edit = Owned(window, state.thread) ? GetDlgItem(window, 102) : nullptr;
        if (edit && Owned(edit, state.thread) && GetParent(edit) == window && IsChild(window, edit) &&
            GetWindow(window, GW_OWNER) == state.owner && IsWindowVisible(window) && IsWindowEnabled(window) &&
            IsWindowVisible(edit) && IsWindowEnabled(edit)) return true;
        Sleep(10);
    }
    return false;
}
bool PostOwnedKey(State& state, WORD key, ULONGLONG deadline) {
    HWND edit = nullptr;
    if (!WaitOwnedFilename(state, deadline, edit)) return false;
    const HWND window = state.window.load();
    // This is the test instance obtained through its exact wrapper's IOleWindow.
    // No foreground activation, SendInput, marker or physical-input claim here.
    if (!Owned(window, state.thread) || !Owned(edit, state.thread) || GetDlgItem(window, 102) != edit ||
        GetParent(edit) != window || GetWindow(window, GW_OWNER) != state.owner) return false;
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    const LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16);
    const LPARAM up = down | static_cast<LPARAM>(static_cast<ULONG_PTR>(3) << 30);
    const BOOL posted_down = PostMessageW(edit, WM_KEYDOWN, key, down);
    const BOOL posted_up = posted_down && PostMessageW(edit, WM_KEYUP, key, up);
    std::printf("OWNED_MESSAGES key=%u window=%p edit=%p down=%d up=%d (not physical input)\n",
        static_cast<unsigned>(key), static_cast<void*>(window), static_cast<void*>(edit), posted_down, posted_up);
    return posted_down && posted_up;
}
void Drive(State& state) {
    const ULONGLONG deadline = GetTickCount64() + 20000;
    HANDLE waits[]{state.ready, state.finished};
    bool sent = WaitForMultipleObjects(2, waits, FALSE, 8000) == WAIT_OBJECT_0;
    if (sent) sent = state.owned_messages ? PostOwnedKey(state, VK_RETURN, deadline) :
        (FocusFilename(state, deadline) && Key(state, VK_RETURN, deadline));
    if (sent && (state.mode == Mode::Veto || state.mode == Mode::Refuse)) {
        HANDLE decisions[]{state.decision, state.finished};
        sent = WaitForMultipleObjects(2, decisions, FALSE, 6000) == WAIT_OBJECT_0;
        if (!state.owned_messages) {
            GUITHREADINFO before_release{sizeof(before_release)};
            const BOOL got_focus = GetGUIThreadInfo(state.thread, &before_release);
            const HWND observed = state.window.load();
            std::printf("DECISION observed=%d ack=%llu expected=%llu return_async=%04X got_focus=%d focus=%p edit_enabled=%d foreground=%p\n",
                sent, static_cast<unsigned long long>(state.acknowledged_key.load()), static_cast<unsigned long long>(state.expected_key.load()),
                static_cast<unsigned>(static_cast<unsigned short>(GetAsyncKeyState(VK_RETURN))), got_focus,
                static_cast<void*>(before_release.hwndFocus), Owned(observed, state.thread) && IsWindowEnabled(GetDlgItem(observed, 102)),
                static_cast<void*>(GetForegroundWindow()));
            // The first real Return release must be consumed before a retry/cancel;
            // a callback may run before the queued keyup reaches the Show loop.
            while (sent && state.acknowledged_key.load() != state.expected_key.load() && GetTickCount64() < deadline &&
                WaitForSingleObject(state.finished, 0) == WAIT_TIMEOUT) Sleep(1);
            sent = sent && state.acknowledged_key.load() == state.expected_key.load();
            std::printf("RELEASE_WAIT success=%d ack=%llu expected=%llu remaining=%llu\n", sent,
                static_cast<unsigned long long>(state.acknowledged_key.load()), static_cast<unsigned long long>(state.expected_key.load()),
                GetTickCount64() < deadline ? deadline - GetTickCount64() : 0);
            // Wait for validation to release the edit, then drive the same actual UI.
            if (sent) sent = FocusFilename(state, deadline);
            if (sent) sent = Key(state, state.mode == Mode::Veto ? VK_RETURN : VK_ESCAPE, deadline);
        } else if (sent) sent = PostOwnedKey(state, state.mode == Mode::Veto ? VK_RETURN : VK_ESCAPE, deadline);
    }
    state.input_ok = sent;
    const ULONGLONG now = GetTickCount64();
    const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    if (WaitForSingleObject(state.finished, remaining) != WAIT_OBJECT_0) {
        state.cleanup_cancel = true;
        const HWND window = state.window.load();
        if (Owned(window, state.thread)) PostMessageW(window, WM_CLOSE, 0, 0);
        std::printf("DRIVER deadline cleanup; not semantic success\n");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    const bool owned_messages = argc == 4 && std::wstring(argv[3]) == L"--owned-messages";
    if ((argc != 3 && !owned_messages) || !std::filesystem::path(argv[1]).is_absolute()) return 2;
    const std::wstring option = argv[2]; Mode mode;
    if (option == L"--new") mode = Mode::New;
    else if (option == L"--overwrite-accept") mode = Mode::Accept;
    else if (option == L"--overwrite-refuse") mode = Mode::Refuse;
    else if (option == L"--veto") mode = Mode::Veto;
    else if (option == L"--close") mode = Mode::Close;
    else if (option == L"--overwrite-close") mode = Mode::OverwriteClose;
    else if (option == L"--replace-fileok") mode = Mode::ReplaceFileOk;
    else return 2;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("DRIVER_MODE %s\n", owned_messages ? "owned-window messages; NOT physical keyboard UX" : "owned-foreground physical SendInput");
    Handle complete(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!complete.value) return 2;
    std::thread watchdog([&] { if (WaitForSingleObject(complete.value, 30000) != WAIT_OBJECT_0) {
        std::printf("[FAIL] own-process 30s watchdog; fixture retained\n"); TerminateProcess(GetCurrentProcess(), 99);
    }});
    const HRESULT com = OleInitialize(nullptr);
    try {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"save-live-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root.parent_path());
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw GetLastError();
        const bool existing = ExistingMode(mode);
        const auto path = (root / (existing ? L"existing.txt" : L"new-target.txt")).wstring();
        BY_HANDLE_FILE_INFORMATION before{}, after{}, replacement_before{};
        bool replaced = false;
        const auto backup = (root / L"existing-backup.txt").wstring();
        bool fixture_ok = Absent(path);
        if (existing) {
            Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            DWORD written = 0;
            fixture_ok = file.value != INVALID_HANDLE_VALUE && WriteFile(file.value, "owned-existing", 14, &written, nullptr) && written == 14;
        }
        if (existing && fixture_ok) fixture_ok = ReadExisting(path, before);
        Check(fixture_ok, "unique isolated fixture ready"); Check(SUCCEEDED(com), "calling STA initialized");
        if (fixture_ok && SUCCEEDED(com)) {
            std::shared_ptr<const FileDialogInprocessProvider> provider;
            const HRESULT loaded = LoadFileDialogUiProvider(argv[1], provider, true);
            Check(loaded == S_OK && provider, "actual proof provider loaded with explicit test authorization");
            if (loaded == S_OK && provider) {
                Check(provider->allow_unverified_proof && provider->api.capabilities == 0, "cap-zero proof remains explicitly test-only");
                HWND owner = CreateWindowW(L"STATIC", L"Pulse Save live verification", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(owner != nullptr, "owned caller window");
                State state; state.mode = mode; state.expected = path; state.backup = backup;
                state.owner = owner; state.owned_messages = owned_messages;
                Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)), finished(CreateEventW(nullptr, TRUE, FALSE, nullptr)), decision(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                state.ready = ready.value; state.finished = finished.value; state.decision = decision.value;
                unsigned gate_calls = 0, native_calls = 0;
                FileDialogBridgeServices services; services.executable = argv[1]; services.inprocess_provider = provider;
                services.save_access_policy = AccessPolicy::LocalSaveV1;
                // Explicit test authorization; this does not certify general native Save parity.
                services.validation_verified = [&](const ContractDecision& contract) {
                    ++gate_calls;
                    return contract.disposition == Disposition::Prepared && contract.validation_category == ValidationCategory::LocalSaveAccessV1 &&
                        contract.supported_access_policy == AccessPolicy::LocalSaveV1 &&
                        contract.required_runtime_checks == (SupportedAccessPolicy | ShellResultContract);
                };
                services.show_native = [&](IFileDialog*, HWND) { ++native_calls; return E_UNEXPECTED; };
                ComPtr<IFileDialog> dialog;
                HRESULT hr = CreatePulseFileDialog(DialogFactory::Save, services, IID_PPV_ARGS(dialog.GetAddressOf()));
                Check(hr == S_OK && dialog, "actual Save COM wrapper created");
                if (hr == S_OK && dialog && owner && ready.value && finished.value && decision.value) {
                    ComPtr<IUnknown> identity; hr = dialog.As(&identity); state.identity = identity.Get();
                    ComPtr<IShellItem> folder;
                    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(root.c_str(), nullptr, IID_PPV_ARGS(folder.GetAddressOf()));
                    if (SUCCEEDED(hr)) hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                        FOS_DONTADDTORECENT | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN);
                    if (SUCCEEDED(hr)) hr = dialog->SetFolder(folder.Get());
                    const COMDLG_FILTERSPEC filter{L"Text", L"*.txt"};
                    if (SUCCEEDED(hr)) hr = dialog->SetFileTypes(1, &filter);
                    if (SUCCEEDED(hr)) hr = dialog->SetFileTypeIndex(1);
                    if (SUCCEEDED(hr)) hr = dialog->SetDefaultExtension(L"txt");
                    if (SUCCEEDED(hr)) hr = dialog->SetFileName(existing ? L"existing.txt" : L"new-target");
                    Check(hr == S_OK, "native-mirrored Save settings and default extension");
                    ComPtr<IFileDialogEvents> sink; sink.Attach(new Sink(state)); DWORD cookie = 0;
                    if (SUCCEEDED(hr)) hr = dialog->Advise(sink.Get(), &cookie);
                    Check(hr == S_OK, "Save event sink advised");
                    if (hr == S_OK) {
                        observing_input = owned_messages ? nullptr : &state;
                        HHOOK hook = owned_messages ? nullptr : SetWindowsHookExW(WH_GETMESSAGE, ObserveInput, nullptr, GetCurrentThreadId());
                        if (!owned_messages) Check(hook != nullptr, "observe own STA input consumption");
                        std::thread driver([&] { Drive(state); });
                        const HRESULT shown = dialog->Show(owner);
                        SetEvent(finished.value); driver.join();
                        if (hook) Check(UnhookWindowsHookEx(hook) != FALSE, "own input hook removed");
                        observing_input = nullptr;
                        std::printf("SHOW hr=%08lX gate=%u native=%u FileOk=%u Overwrite=%u\n", static_cast<unsigned long>(shown), gate_calls, native_calls, state.file_ok, state.overwrite);
                        Check(state.input_ok && !state.cleanup_cancel, owned_messages ?
                            "explicit owned-window message sequence completed without deadline cleanup (not keyboard UX)" :
                            "owned foreground real input completed without deadline cleanup");
                        Check(gate_calls == 1 && native_calls == 0, "actual inprocess UI without native replay");
                        Check(state.identity_ok && state.candidate_ok, "callbacks have wrapper identity and exact target");
                        ComPtr<IShellItem> result; const HRESULT got = dialog->GetResult(result.GetAddressOf());
                        if (mode == Mode::Refuse || mode == Mode::Close || mode == Mode::OverwriteClose) {
                            Check(shown == ((mode == Mode::Close || mode == Mode::OverwriteClose) ? close_result : HRESULT_FROM_WIN32(ERROR_CANCELLED)) && FAILED(got) && !result,
                                "cancel or callback Close yields exact status and no result");
                            if (mode == Mode::Close) Check(state.closed == S_OK && state.file_ok == 1, "FileOk Close actually called once");
                            else if (mode == Mode::OverwriteClose) Check(state.closed == S_OK && state.overwrite == 1 && state.file_ok == 0,
                                "Overwrite Close actually called once and prevents FileOk");
                            else Check(state.overwrite == 1 && state.file_ok == 0, "REFUSE preserves native-observed no-FileOk behavior");
                        } else {
                            Check(shown == S_OK && got == S_OK && Exact(result.Get(), path), "accepted real Shell result has exact extended target");
                            UINT index = 0; Check(dialog->GetFileTypeIndex(&index) == S_OK && index == 1, "actual COM result filter index stays one-based");
                            Check(state.file_ok == ((mode == Mode::Veto || mode == Mode::ReplaceFileOk) ? 2u : 1u), "actual FileOk count proves veto retry or acceptance");
                        }
                        Check(state.overwrite == (mode == Mode::ReplaceFileOk ? 2u : existing ? 1u : 0u), "actual overwrite callback only for existing target");
                        if (mode == Mode::ReplaceFileOk) {
                            replaced = state.replaced; replacement_before = state.replacement_identity;
                            Check(replaced && state.overwrite_sequence[0] < state.fileok_sequence[0] &&
                                state.fileok_sequence[0] < state.overwrite_sequence[1] &&
                                state.overwrite_sequence[1] < state.fileok_sequence[1],
                                "replacement forces second Overwrite then second FileOk without additional input");
                        }
                        Check(IsWindowEnabled(owner) != FALSE, "owned caller restored");
                        Check(dialog->Unadvise(cookie) == S_OK, "sink unadvised");
                    }
                }
                dialog.Reset();
                if (owner) Check(DestroyWindow(owner) != FALSE, "owned caller destroyed");
            }
        }
        if (existing) {
            const auto& expected_identity = mode == Mode::ReplaceFileOk ? replacement_before : before;
            Check(ReadExisting(path, after, mode == Mode::ReplaceFileOk ? "owned-replaced" : "owned-existing") &&
                expected_identity.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
                expected_identity.nFileIndexHigh == after.nFileIndexHigh && expected_identity.nFileIndexLow == after.nFileIndexLow &&
                CompareFileTime(&expected_identity.ftCreationTime, &after.ftCreationTime) == 0 &&
                CompareFileTime(&expected_identity.ftLastWriteTime, &after.ftLastWriteTime) == 0,
                "test-created target identity content and write time not modified by picker");
            if (mode == Mode::ReplaceFileOk) {
                BY_HANDLE_FILE_INFORMATION old{};
                Check(replaced && (before.nFileIndexHigh != replacement_before.nFileIndexHigh || before.nFileIndexLow != replacement_before.nFileIndexLow) &&
                    ReadExisting(backup, old) && before.dwVolumeSerialNumber == old.dwVolumeSerialNumber &&
                    before.nFileIndexHigh == old.nFileIndexHigh && before.nFileIndexLow == old.nFileIndexLow,
                    "replacement has different file identity and owned backup preserves original");
                if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES)
                    Check(DeleteFileW(backup.c_str()) != FALSE, "exact owned backup removed");
            }
            Check(DeleteFileW(path.c_str()) != FALSE, "exact existing fixture removed");
        } else {
            Check(Absent(path), "new target absent after access probe cleanup");
            Check(Absent((root / L"new-target").wstring()), "unextended target absent after Show");
        }
        Check(RemoveDirectoryW(root.c_str()) != FALSE, "exact empty fixture removed");
    } catch (...) { Check(false, "setup exception; fixture retained"); }
    if (SUCCEEDED(com)) OleUninitialize();
    SetEvent(complete.value); watchdog.join();
    std::printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
