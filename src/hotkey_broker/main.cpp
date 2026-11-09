#include "../ipc/win_e_broker_protocol.h"
#include "../ipc/elevated_transfer_transport.h"
#include "../app/win_e_input.h"
#include "../common/runtime_log.h"
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <wrl/client.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <utility>
#ifdef PULSE_HOTKEY_BROKER_TEST
#include "../../bench_data/win-e-native/production-broker/fixture.h"
#endif

namespace {
namespace protocol = pulse::win_e_broker;
namespace log = pulse::diagnostics::runtime;
using pulse::elevated::Handle;
using Microsoft::WRL::ComPtr;
using State = protocol::SlotState;
template<size_t N, size_t... I>
void Event(const char* name, const log::Field (&fields)[N], std::index_sequence<I...>) {
    static_assert(N <= 12); log::Event(name, {fields[I]...});
}
template<size_t N> void Event(const char* name, const log::Field (&fields)[N]) {
    Event(name, fields, std::make_index_sequence<N>{});
}
unsigned Modifiers() {
    unsigned value = 0;
    for (const DWORD key : {VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT})
        if (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) value |= pulse::app::WinEInputState::Modifier(key);
    return value;
}
bool Released() { return !Modifiers() && !(GetAsyncKeyState('E') & 0x8000); }
struct Audit { unsigned kind = 0, phase = 0, action = 0, mods = 0; uint64_t sequence = 0; };
struct Broker {
    protocol::Shared* shared = nullptr;
    HANDLE parent = nullptr;
    bool armed = false, stopping = false;
    pulse::app::WinEInputState input;
    std::array<Audit, 128> audit{};
    size_t audit_count = 0;
    uint64_t dropped = 0;
    void Note(Audit record) { if (audit_count < audit.size()) audit[audit_count++] = record; else ++dropped; }
    bool Active() {
        const auto beat = static_cast<uint64_t>(InterlockedCompareExchange64(&shared->parent_heartbeat, 0, 0));
        const auto now = GetTickCount64();
        return !stopping && protocol::LoadOwner(*shared) == protocol::Owner::Broker &&
            InterlockedCompareExchange(&shared->ready_pid, 0, 0) == static_cast<LONG>(GetCurrentProcessId()) &&
            now >= beat && now - beat < 2000 && WaitForSingleObject(parent, 0) == WAIT_TIMEOUT;
    }
    void Drain() {
        for (size_t i = 0; i < audit_count; ++i) {
            const auto& a = audit[i];
            Event(a.kind ? "broker_request" : "broker_input", {{"sequence", a.sequence},
                {"phase", a.phase}, {"action", a.action}, {"modifiers", a.mods}});
        }
        audit_count = 0;
        if (dropped) { Event("broker_health", {{"dropped", dropped}}); dropped = 0; }
    }
};
Broker* current = nullptr;
LRESULT CALLBACK Hook(int code, WPARAM message, LPARAM data) {
    if (code < 0 || !current) return CallNextHookEx(nullptr, code, message, data);
    auto& self = *current;
    const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    if (key.flags & LLKHF_INJECTED) {
#ifdef PULSE_HOTKEY_BROKER_TEST
        if (!pulse::win_e_broker_test::AllowsInjected(key))
#endif
            return CallNextHookEx(nullptr, code, message, data);
    }
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return CallNextHookEx(nullptr, code, message, data);
    if (!self.Active()) self.armed = false;
    else if (!self.armed && Released()) self.armed = true;
    self.input.ObserveModifier(key.vkCode, down);
    if (key.vkCode != 'E') return CallNextHookEx(nullptr, code, message, data);
    const unsigned mods = Modifiers();
    if (up) {
        const bool swallow = self.input.chord.Release();
        InterlockedExchange(&self.shared->chord_flags, 0);
        self.Note({0, 2, swallow ? 2u : 0u, mods, protocol::LoadSequence(*self.shared)});
        return swallow ? 1 : CallNextHookEx(nullptr, code, message, data);
    }
    const auto action = self.input.chord.Press(self.armed && self.Active() && self.input.Eligible(mods));
    if (action == pulse::app::WinEChord::Action::Submit) {
        if (protocol::Transition(*self.shared, State::Empty, State::Writing)) {
            const auto sequence = static_cast<uint64_t>(InterlockedIncrement64(&self.shared->sequence));
            if (self.Active()) {
                self.input.chord.accepted = true;
                InterlockedExchange(&self.shared->chord_flags, 3);
            }
            if (self.input.chord.accepted && protocol::Transition(*self.shared, State::Writing, State::Offered)) {
                self.Note({1, 0, 1, mods, sequence});
                // Suppress the Start menu after consuming Win+E. The callback
                // ignores these synthetic unassigned-key transitions; do not alter Control.
                INPUT mask[2]{};
                for (auto& item : mask) { item.type = INPUT_KEYBOARD; item.ki.wVk = 0xE8; }
                mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, mask, sizeof(INPUT));
            } else {
                self.input.chord.accepted = false;
                protocol::Transition(*self.shared, State::Writing, State::Empty);
            }
        }
    }
    InterlockedExchange(&self.shared->chord_flags, 1 | (self.input.chord.accepted ? 2 : 0));
    self.Note({0, 1, self.input.chord.accepted ? 2u : 0u, mods, protocol::LoadSequence(*self.shared)});
    return self.input.chord.accepted ? 1 : CallNextHookEx(nullptr, code, message, data);
}
HRESULT NativeFromDesktop() {
#ifdef PULSE_HOTKEY_BROKER_TEST
    return pulse::win_e_broker_test::Fallback();
#else
    wchar_t directory[MAX_PATH]{};
    const UINT count = GetWindowsDirectoryW(directory, ARRAYSIZE(directory));
    if (!count || count >= ARRAYSIZE(directory)) return E_FAIL;
    const std::wstring executable = std::wstring(directory) + L"\\explorer.exe";

    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return hr;
    VARIANT location{}; location.vt = VT_I4; location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    long window = 0;
    ComPtr<IDispatch> dispatch;
    hr = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window, SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(hr) || !dispatch) return FAILED(hr) ? hr : E_NOINTERFACE;
    DWORD pid = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(window)), &pid);
    Handle shell(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    DWORD session = 0, own_session = 0;
    if (!shell || _wcsicmp(pulse::elevated::ProcessImage(shell.Get()).c_str(), executable.c_str()) != 0 ||
        !pulse::elevated::SameUser(shell.Get(), GetCurrentProcess()) ||
        !ProcessIdToSessionId(pid, &session) || !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        session != own_session) return E_ACCESSDENIED;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(shell.Get(), TOKEN_QUERY, &raw_token)) return HRESULT_FROM_WIN32(GetLastError());
    Handle token(raw_token); TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes))
        return HRESULT_FROM_WIN32(GetLastError());
    if (elevation.TokenIsElevated) return E_ACCESSDENIED;
    ComPtr<IServiceProvider> provider;
    if (FAILED(hr = dispatch.As(&provider))) return hr;
    ComPtr<IShellBrowser> browser;
    if (FAILED(hr = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) return hr;
    ComPtr<IShellView> view;
    if (FAILED(hr = browser->QueryActiveShellView(&view))) return hr;
    ComPtr<IDispatch> background;
    if (FAILED(hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background)))) return hr;
    ComPtr<IShellFolderViewDual> folder;
    if (FAILED(hr = background.As(&folder))) return hr;
    ComPtr<IDispatch> application;
    if (FAILED(hr = folder->get_Application(&application))) return hr;
    ComPtr<IShellDispatch2> shell_dispatch;
    if (FAILED(hr = application.As(&shell_dispatch))) return hr;
    BSTR file = SysAllocString(executable.c_str());
    VARIANT arguments{}, operation{}, working{}, show{};
    arguments.vt = VT_BSTR; arguments.bstrVal = SysAllocString(L"/n,/e,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
    operation.vt = VT_BSTR; operation.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4; show.lVal = SW_SHOWNORMAL;
    if (!file || !arguments.bstrVal || !operation.bstrVal) hr = E_OUTOFMEMORY;
    else hr = shell_dispatch->ShellExecute(file, arguments, working, operation, show);
    SysFreeString(file); VariantClear(&arguments); VariantClear(&operation);
    return hr;
#endif
}
struct Fallback {
    std::thread worker;
    std::atomic<bool> done{false};
    std::atomic<HRESULT> result{E_PENDING};
    uint64_t sequence = 0, started = 0;
    void Start(uint64_t id) {
        sequence = id; started = GetTickCount64();
        worker = std::thread([this] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            HRESULT hr = com;
            if (SUCCEEDED(com)) {
                try { hr = NativeFromDesktop(); } catch (...) { hr = E_FAIL; }
                CoUninitialize();
            }
            result = hr; done = true;
        });
    }
};
int Run(DWORD parent_id, const protocol::Nonce& nonce) {
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_id));
    if (!parent || WaitForSingleObject(parent.Get(), 0) != WAIT_TIMEOUT ||
        !pulse::elevated::SameUser(parent.Get(), GetCurrentProcess())) return 3;
    DWORD parent_session = 0, session = 0;
    if (!ProcessIdToSessionId(parent_id, &parent_session) || !ProcessIdToSessionId(GetCurrentProcessId(), &session) ||
        session != parent_session) return 3;
    const auto image = pulse::elevated::ProcessImage(GetCurrentProcess());
    if (!pulse::elevated::IsElevated(GetCurrentProcess())) return 3;
#ifdef PULSE_HOTKEY_BROKER_TEST
    const auto expected = std::filesystem::path(image).parent_path() / L"pulse_win_e_broker_live_test.exe";
#else
    const auto expected = std::filesystem::path(image).parent_path() / L"pulse.exe";
#endif
    if (_wcsicmp(pulse::elevated::ProcessImage(parent.Get()).c_str(), expected.c_str()) != 0) return 3;
    Handle mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, protocol::MappingName(parent_id, nonce).c_str()));
    Handle stop(OpenEventW(SYNCHRONIZE, FALSE, protocol::StopEventName(parent_id, nonce).c_str()));
    if (!mapping || !stop) return 4;
    auto* shared = static_cast<protocol::Shared*>(MapViewOfFile(mapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(protocol::Shared)));
    if (!shared) return 4;
    struct Unmap { void* value; ~Unmap() { UnmapViewOfFile(value); } } unmap{shared};
    if (!protocol::ValidHeader(*shared, parent_id, session, protocol::CreationTime(parent.Get()))) return 3;
#ifdef PULSE_HOTKEY_BROKER_TEST
    log::Initialize((std::filesystem::path(image).parent_path() /
        (L"native-" + std::to_wstring(parent_id))).wstring(), "hotkey_broker");
    if (!pulse::win_e_broker_test::Prepare(parent_id)) { log::Shutdown(); return 6; }
#else
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        log::Initialize(std::wstring(local) + L"\\Pulse", "hotkey_broker"); CoTaskMemFree(local);
    }
#endif
    Broker broker; broker.shared = shared; broker.parent = parent.Get(); current = &broker;
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(nullptr), 0);
    Event("broker_start", {{"pid", GetCurrentProcessId()}, {"parent_pid", parent_id}, {"session", session},
        {"hook_installed", hook != nullptr}, {"error", hook ? 0u : GetLastError()}});
    if (!hook) {
        current = nullptr;
#ifdef PULSE_HOTKEY_BROKER_TEST
        pulse::win_e_broker_test::Shutdown();
#endif
        log::Shutdown(); return 5;
    }
    Fallback fallback;
    bool parent_dead = false;
    for (;;) {
        if (WaitForSingleObject(parent.Get(), 0) == WAIT_OBJECT_0) parent_dead = true;
        if (parent_dead || WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) {
            broker.stopping = true;
            InterlockedExchange(&shared->owner, static_cast<LONG>(protocol::Owner::Medium));
            InterlockedExchange(&shared->ready_pid, 0);
        }
        if (!broker.stopping && Released()) {
            InterlockedExchange(&shared->ready_pid, static_cast<LONG>(GetCurrentProcessId()));
            if (broker.Active()) broker.armed = true;
        }
        auto state = protocol::LoadState(*shared);
        if (broker.stopping && state == State::Offered) {
            protocol::Transition(*shared, State::Offered, State::Rejected);
            state = protocol::LoadState(*shared);
        }
        if (parent_dead && (state == State::Offered || state == State::Claimed || state == State::Admitted)) {
            protocol::Transition(*shared, state, State::Rejected); state = protocol::LoadState(*shared);
        }
        if (state == State::Dispatched && protocol::Transition(*shared, state, State::Empty))
            Event("broker_delivery", {{"sequence", protocol::LoadSequence(*shared)}, {"dispatched", 1}});
        if (state == State::Rejected && !fallback.started) {
            Event("broker_delivery", {{"sequence", protocol::LoadSequence(*shared)}, {"dispatched", 0}});
            try { fallback.Start(protocol::LoadSequence(*shared)); }
            catch (...) { Event("broker_fallback", {{"sequence", protocol::LoadSequence(*shared)}, {"error", ERROR_NOT_ENOUGH_MEMORY}, {"ambiguous", 0}}); break; }
        }
        if (fallback.started && fallback.done) {
            fallback.worker.join();
            Event("broker_fallback", {{"sequence", fallback.sequence}, {"hresult", static_cast<uint32_t>(fallback.result.load())},
                {"accepted", SUCCEEDED(fallback.result.load())}, {"ambiguous", 0}});
            protocol::Transition(*shared, State::Rejected, State::Empty);
            fallback.started = 0; fallback.done = false;
        } else if (fallback.started && GetTickCount64() - fallback.started >= 5000) {
            InterlockedExchange(&shared->owner, static_cast<LONG>(protocol::Owner::Medium));
            InterlockedExchange(&shared->ready_pid, 0);
            UnhookWindowsHookEx(hook); current = nullptr;
            Event("broker_fallback", {{"sequence", fallback.sequence}, {"error", WAIT_TIMEOUT}, {"ambiguous", 1}});
#ifdef PULSE_HOTKEY_BROKER_TEST
            pulse::win_e_broker_test::Shutdown();
#endif
            log::Flush(200); ExitProcess(WAIT_TIMEOUT); // COM may have submitted: never retry it.
        }
        broker.Drain();
        if (broker.stopping && protocol::LoadState(*shared) == State::Empty && !fallback.started &&
            !broker.input.chord.accepted) break;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) broker.stopping = true;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
#ifdef PULSE_HOTKEY_BROKER_TEST
        pulse::win_e_broker_test::Poll(*shared);
#endif
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
    }
    InterlockedExchange(&shared->owner, static_cast<LONG>(protocol::Owner::Medium));
    InterlockedExchange(&shared->ready_pid, 0);
    UnhookWindowsHookEx(hook); current = nullptr;
#ifdef PULSE_HOTKEY_BROKER_TEST
    pulse::win_e_broker_test::Shutdown();
#endif
    broker.Drain(); log::Shutdown(); return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 2;
    DWORD parent = 0; protocol::Nonce nonce{}; bool valid = false;
    if (count == 5 && std::wstring_view(args[1]) == L"--parent" && std::wstring_view(args[3]) == L"--nonce") {
        wchar_t* end = nullptr; const auto value = wcstoull(args[2], &end, 10);
        valid = args[2][0] >= L'0' && args[2][0] <= L'9' && end && !*end && value && value <= MAXDWORD && protocol::ParseNonce(args[4], nonce);
        parent = static_cast<DWORD>(value);
    }
    LocalFree(args);
    if (!valid) return 2;
    try { return Run(parent, nonce); } catch (...) { return 5; }
}
