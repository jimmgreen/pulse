#include "../common/current_user_security.h"
#include "../picker_inprocess/picker_ui_api.h"
#include <objbase.h>
#include <filesystem>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
}
struct BenchHandle {
    HANDLE value = nullptr;
    BenchHandle() = default;
    explicit BenchHandle(HANDLE h) : value(h) {}
    ~BenchHandle() { if (value) CloseHandle(value); }
    void Reset(HANDLE h) { if (value) CloseHandle(value); value = h; }
    HANDLE Get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
constexpr UINT kExternalCommand = WM_APP + 93;
struct ExternalShared { volatile UINT_PTR window = 0, replacement = 0; };
ExternalShared* external_shared = nullptr;
LRESULT CALLBACK ExternalProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kExternalCommand) {
        if (wp == 1) EnableWindow(window, TRUE);
        if (wp == 2) {
            DestroyWindow(window);
            HWND replacement_window = CreateWindowW(L"PulsePickerExternalOwner", L"disabled replacement", WS_OVERLAPPED | WS_DISABLED,
                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (external_shared) external_shared->replacement = reinterpret_cast<UINT_PTR>(replacement_window);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
int ExternalOwnerChild(const wchar_t* mapping_name, const wchar_t* ready_name) {
    BenchHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name));
    BenchHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    if (!mapping || !ready) return 11;
    auto* data = static_cast<ExternalShared*>(MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExternalShared)));
    if (!data) return 12;
    external_shared = data;
    WNDCLASSW wc{}; wc.lpfnWndProc = ExternalProc; wc.lpszClassName = L"PulsePickerExternalOwner"; wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"owned child disabled window", WS_OVERLAPPED | WS_DISABLED,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    data->window = reinterpret_cast<UINT_PTR>(window);
    SetEvent(ready.Get());
    if (window) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (data->replacement && IsWindow(reinterpret_cast<HWND>(data->replacement))) DestroyWindow(reinterpret_cast<HWND>(data->replacement));
    external_shared = nullptr; UnmapViewOfFile(data);
    return window ? 0 : 13;
}
struct ExternalFixture {
    BenchHandle mapping, ready, process, job;
    ExternalShared* data = nullptr;
    DWORD thread = 0, pid = 0;
    bool started = false;
    explicit ExternalFixture(const wchar_t* executable) {
        static LONG serial = 0;
        const auto prefix = L"Local\\PulseInprocExternal." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(InterlockedIncrement(&serial));
        pulse::CurrentUserSecurityAttributes security;
        if (!security) return;
        mapping.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, sizeof(ExternalShared), prefix.c_str()));
        const auto ready_name = prefix + L".ready";
        ready.Reset(CreateEventW(security.get(), TRUE, FALSE, ready_name.c_str()));
        if (!mapping || !ready) return;
        data = static_cast<ExternalShared*>(MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExternalShared)));
        if (!data) return;
        *data = {};
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --owner-fixture " + prefix + L" " + ready_name;
        wchar_t desktop_name[1024]{}; DWORD needed = 0;
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop_name, sizeof(desktop_name), &needed)) return;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop_name;
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) return;
        process.Reset(info.hProcess); BenchHandle initial_thread(info.hThread); thread = info.dwThreadId; pid = info.dwProcessId;
        if (!AssignProcessToJobObject(job.Get(), process.Get()) || ResumeThread(initial_thread.Get()) == DWORD(-1)) {
            TerminateProcess(process.Get(), 92); WaitForSingleObject(process.Get(), 5000); return;
        }
        started = WaitForSingleObject(ready.Get(), 5000) == WAIT_OBJECT_0 && data->window;
    }
    HWND Window() const { return data ? reinterpret_cast<HWND>(data->window) : nullptr; }
    ~ExternalFixture() {
        if (process) {
            PostThreadMessageW(thread, WM_QUIT, 0, 0);
            const DWORD waited = WaitForSingleObject(process.Get(), 2000);
            if (waited != WAIT_OBJECT_0) { TerminateProcess(process.Get(), 91); WaitForSingleObject(process.Get(), 5000); }
            DWORD exit = 0; GetExitCodeProcess(process.Get(), &exit);
            Check(waited == WAIT_OBJECT_0 && exit == 0, "owned external window process cleaned up without termination");
        }
        if (data) UnmapViewOfFile(data);
    }
};
void OwnerCases(PulsePickerUiApi& api, PulsePickerUiConfig config, PulsePickerUiResult& result, const std::wstring& path, int selected) {
    struct State {
        PulsePickerUiApi* api; HWND owner; int action; bool ready = false, posted = false, identity = true;
        DWORD thread = GetCurrentThreadId();
    };
    const auto event = [](void* opaque, const PulsePickerUiEvent* event) -> HRESULT {
        auto& state = *static_cast<State*>(opaque);
        HWND actual = nullptr;
        state.identity = state.identity && GetCurrentThreadId() == state.thread &&
            state.api->GetWindow(&actual) == S_OK && actual == event->window;
        std::cout << "[OWNER-EVENT] action=" << state.action << " kind=" << event->kind << " tick=" << GetTickCount64() << '\n';
        if (event->kind == PULSE_PICKER_UI_READY) {
            state.ready = true;
            if (state.action == 0 || state.action == 4) {
                const HWND edit = GetDlgItem(event->window, 102);
                state.posted = edit && PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            } else if (state.action == 1) return state.api->Close();
            else state.posted = PostMessageW(state.owner, kExternalCommand, state.action == 2 ? 2 : 1, 0) != FALSE;
        }
        if (event->kind == PULSE_PICKER_UI_FILE_OK && state.action == 4) {
            DWORD_PTR returned = 0;
            state.posted = SendMessageTimeoutW(state.owner, kExternalCommand, 1, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &returned) != 0 && IsWindowEnabled(state.owner);
        }
        return S_OK;
    };
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    for (int action = 0; action != 5; ++action) {
        if (selected >= 0 && selected != action) continue;
        std::cout << "[OWNER-CASE] begin action=" << action << " tick=" << GetTickCount64() << '\n';
        ExternalFixture remote(executable);
        Check(remote.started, "inproc separate-process disabled owner fixture ready");
        if (!remote.started) continue;
        config.owner = remote.Window();
        State state{&api, config.owner, action};
        PulsePickerUiCallbacks callbacks{sizeof(callbacks), 0, event, &state};
        const HRESULT hr = api.Show(&config, &callbacks, &result);
        std::cout << "[OWNER-CASE] show-return action=" << action << " hr=" << static_cast<long>(hr) << " tick=" << GetTickCount64() << '\n';
        Check(state.ready && state.identity, "external owner Show uses actual calling-STA picker callbacks");
        if (action == 0) {
            Check(state.posted && hr == S_OK && result.status == PULSE_PICKER_UI_ACCEPTED &&
                std::wstring(result.path, result.path_units) == path, "external disabled owner accepts exact fixture");
            Check(!IsWindowEnabled(remote.Window()), "accept leaves external owner disabled");
        } else {
            Check(hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.status == PULSE_PICKER_UI_CANCELLED &&
                result.path_units == 0 && result.path[0] == 0, "external Close or identity loss cancels without result");
            if (action == 1) Check(!IsWindowEnabled(remote.Window()), "cancel leaves external owner disabled");
            if (action == 2) Check(state.posted && !IsWindow(remote.Window()) && remote.data->replacement &&
                !IsWindowEnabled(reinterpret_cast<HWND>(remote.data->replacement)), "destroyed owner replacement is never enabled");
            if (action == 3 || action == 4) {
                Check(state.posted && IsWindowEnabled(remote.Window()), "caller reenable cancels without undoing caller transition");
                const HRESULT rejected = api.Show(&config, &callbacks, &result);
                Check(rejected == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && result.path_units == 0,
                    "enabled external owner is rejected before new Show");
            }
        }
    }
    if (selected >= 0 && selected != 5) return;
    std::cout << "[OWNER-CASE] begin original-disabled tick=" << GetTickCount64() << '\n';
    HWND local = CreateWindowW(L"STATIC", L"initially disabled inproc owner", WS_OVERLAPPED | WS_DISABLED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(local != nullptr, "same-thread initially disabled owner created");
    if (local) {
        config.owner = local; State state{&api, local, 1};
        PulsePickerUiCallbacks callbacks{sizeof(callbacks), 0, event, &state};
        Check(api.Show(&config, &callbacks, &result) == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
            state.ready && !IsWindowEnabled(local), "initially disabled same-thread owner is not enabled by Close");
        DestroyWindow(local);
    }
}
constexpr ULONG_PTR input_marker = 0x50495046;
struct DpiApi {
    using Thread = DPI_AWARENESS_CONTEXT (WINAPI*)();
    using Window = DPI_AWARENESS_CONTEXT (WINAPI*)(HWND);
    using Equal = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    using Dpi = UINT (WINAPI*)(HWND);
    Thread thread = nullptr; Window window = nullptr; Equal equal = nullptr; Dpi dpi = nullptr;
    DpiApi() {
        HMODULE user = GetModuleHandleW(L"user32.dll");
        thread = reinterpret_cast<Thread>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
        window = reinterpret_cast<Window>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
        equal = reinterpret_cast<Equal>(GetProcAddress(user, "AreDpiAwarenessContextsEqual"));
        dpi = reinterpret_cast<Dpi>(GetProcAddress(user, "GetDpiForWindow"));
    }
    bool Available() const { return thread && window && equal && dpi; }
};

struct Context {
    PulsePickerUiApi* api = nullptr;
    DWORD thread = 0;
    std::wstring path;
    HANDLE ready = nullptr, veto = nullptr;
    std::atomic<HWND> window{nullptr};
    std::atomic<unsigned> keyups{0}, accepts{0}, keyups_before_accept{0};
    std::atomic<bool> identity{true}, candidate{true}, live_veto{false}, input_ok{false};
    bool cancel = false;
    bool render_failure = false;
    bool failure_armed = false;
    DpiApi dpi;
    HWND owner = nullptr;
    DPI_AWARENESS_CONTEXT prior_thread = nullptr, prior_owner = nullptr;
    bool dpi_seen = false, dpi_valid = true;
};
thread_local Context* hook_context = nullptr;
LRESULT CALLBACK ObserveMessage(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == PM_REMOVE && hook_context) {
        const auto* message = reinterpret_cast<const MSG*>(lparam);
        if (message->message == WM_KEYUP && message->wParam == VK_RETURN &&
            static_cast<ULONG_PTR>(GetMessageExtraInfo()) == input_marker)
            ++hook_context->keyups;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
HRESULT WINAPI Event(void* opaque, const PulsePickerUiEvent* event) {
    auto& context = *static_cast<Context*>(opaque);
    HWND window = nullptr; DWORD pid = 0;
    if (GetCurrentThreadId() != context.thread || context.api->GetWindow(&window) != S_OK ||
        window != event->window || GetWindowThreadProcessId(window, &pid) != context.thread || pid != GetCurrentProcessId())
        context.identity = false;
    if (event->kind == PULSE_PICKER_UI_READY) {
        context.dpi_seen = true;
        if (context.dpi.Available()) {
            const auto thread = context.dpi.thread();
            const auto picker = context.dpi.window(event->window);
            const auto owner = context.dpi.window(context.owner);
            std::cout << "[DPI-INSIDE] thread=" << static_cast<void*>(thread) << " picker=" << static_cast<void*>(picker)
                << " owner=" << static_cast<void*>(owner) << " picker_dpi=" << context.dpi.dpi(event->window)
                << " owner_dpi=" << context.dpi.dpi(context.owner) << '\n';
            context.dpi_valid = context.dpi_valid && context.dpi.equal(thread, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
                context.dpi.equal(picker, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && context.dpi.equal(owner, context.prior_owner);
        } else context.dpi_valid = false;
        unsigned children = 0;
        EnumChildWindows(event->window, [](HWND child, LPARAM opaque) -> BOOL {
            ++*reinterpret_cast<unsigned*>(opaque);
            wchar_t name[128]{}; const int count = GetClassNameW(child, name, 128);
            std::cout << "[READY-CHILD] hwnd=" << static_cast<void*>(child) << " id=" << GetDlgCtrlID(child)
                << " parent=" << static_cast<void*>(GetParent(child)) << " visible=" << IsWindowVisible(child)
                << " enabled=" << IsWindowEnabled(child) << " class=";
            for (int i = 0; i < count; ++i) std::cout << (name[i] < 128 ? static_cast<char>(name[i]) : '?');
            std::cout << '\n'; return TRUE;
        }, reinterpret_cast<LPARAM>(&children));
        std::cout << "[READY] children=" << children << " filename_id102="
            << static_cast<void*>(GetDlgItem(event->window, 102)) << " thread=" << GetCurrentThreadId() << '\n';
        if (!children && !context.cancel) {
            // Diagnostic only, on the exact Show STA/current activation context.
            // Reproduce EditHost's creation stages with a temporary hidden child,
            // then destroy it; it never substitutes for the actual filename EDIT.
            SetLastError(ERROR_SUCCESS);
            HWND probe = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"",
                WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 0, 0, event->window, nullptr, GetModuleHandleW(nullptr), nullptr);
            const DWORD create_error = probe ? ERROR_SUCCESS : GetLastError();
            BOOL layered = FALSE; DWORD layered_error = ERROR_SUCCESS;
            if (probe) {
                SetLastError(ERROR_SUCCESS); layered = SetLayeredWindowAttributes(probe, 0, 255, LWA_ALPHA);
                if (!layered) layered_error = GetLastError();
            }
            std::cout << "[READY-PROBE] create=" << (probe != nullptr) << " create_error=" << create_error
                << " layered=" << layered << " layered_error=" << layered_error << '\n';
            if (!probe || !layered) {
                SetLastError(ERROR_SUCCESS);
                HWND plain = CreateWindowExW(0, L"EDIT", L"",
                    WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL,
                    0, 0, 0, 0, event->window, nullptr, GetModuleHandleW(nullptr), nullptr);
                const DWORD plain_error = plain ? ERROR_SUCCESS : GetLastError();
                std::cout << "[READY-PLAIN] create=" << (plain != nullptr) << " create_error=" << plain_error << '\n';
                if (plain) {
                    SetLastError(ERROR_SUCCESS); const BOOL destroyed = DestroyWindow(plain);
                    const DWORD destroy_error = destroyed ? ERROR_SUCCESS : GetLastError();
                    std::cout << "[READY-PLAIN] destroyed=" << destroyed << " error=" << destroy_error << '\n';
                }
            }
            if (probe) {
                SetLastError(ERROR_SUCCESS); const BOOL destroyed = DestroyWindow(probe);
                const DWORD destroy_error = destroyed ? ERROR_SUCCESS : GetLastError();
                std::cout << "[READY-PROBE] destroyed=" << destroyed << " error=" << destroy_error << '\n';
            }
        }
        context.window = event->window; SetEvent(context.ready);
        if (context.cancel) return context.api->Close();
        return S_OK;
    }
    if (event->kind != PULSE_PICKER_UI_FILE_OK || !event->path ||
        std::wstring(event->path, event->path_units) != context.path) context.candidate = false;
    if (++context.accepts == 1) { SetEvent(context.veto); return S_FALSE; }
    context.keyups_before_accept = context.keyups.load();
    if (context.render_failure) {
        context.failure_armed = SetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", L"1") != FALSE;
        std::cout << "[RENDER-FAILURE] second_file_ok_armed=" << context.failure_armed << '\n';
    }
    return S_OK;
}
bool Owned(HWND window, DWORD thread) {
    DWORD pid = 0;
    return IsWindow(window) && GetWindowThreadProcessId(window, &pid) == thread && pid == GetCurrentProcessId();
}
bool Enter(Context& context, unsigned attempt, ULONGLONG deadline) {
    HWND window = context.window.load();
    const ULONGLONG wait_started = GetTickCount64();
    const ULONGLONG foreground_deadline = (std::min)(deadline, wait_started + 4000);
    while (Owned(window, context.thread) && GetForegroundWindow() != window && GetTickCount64() < foreground_deadline) Sleep(20);
    const bool owned = Owned(window, context.thread);
    const HWND foreground = GetForegroundWindow();
    std::cout << "[DRIVE] enter=" << attempt << " owned=" << owned << " window=" << static_cast<void*>(window)
        << " foreground=" << static_cast<void*>(foreground) << " foreground_match=" << (foreground == window)
        << " waited_ms=" << (GetTickCount64() - wait_started) << '\n';
    if (!owned || foreground != window || GetTickCount64() >= deadline) {
        std::cout << "[DRIVE] send_called=0 attempt=" << attempt << '\n'; return false;
    }
    INPUT input[2]{};
    for (auto& item : input) { item.type = INPUT_KEYBOARD; item.ki.wVk = VK_RETURN; item.ki.dwExtraInfo = input_marker; }
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SetLastError(ERROR_SUCCESS);
    const UINT sent = SendInput(2, input, sizeof(INPUT));
    const DWORD error = sent == 2 ? ERROR_SUCCESS : GetLastError();
    std::cout << "[DRIVE] send_called=1 attempt=" << attempt << " sent=" << sent << " error=" << error << '\n';
    if (sent == 1) {
        SetLastError(ERROR_SUCCESS);
        const UINT released = SendInput(1, &input[1], sizeof(INPUT));
        const DWORD release_error = released ? ERROR_SUCCESS : GetLastError();
        std::cout << "[DRIVE] partial-send release_count=" << released << " error=" << release_error << '\n';
    }
    return sent == 2;
}
void Drive(Context& context) {
    const ULONGLONG deadline = GetTickCount64() + 20000;
    const DWORD ready_wait = WaitForSingleObject(context.ready, 8000);
    const DWORD ready_error = ready_wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    std::cout << "[DRIVE] ready_wait=" << ready_wait << " error=" << ready_error << '\n';
    if (ready_wait != WAIT_OBJECT_0) return;
    // Readiness is a real EDIT value, not merely a newly created parent HWND.
    bool ready = false;
    unsigned edit_count = 0, read_count = 0; DWORD read_error = 0;
    std::wstring last_text;
    const ULONGLONG edit_deadline = GetTickCount64() + 2000;
    for (unsigned attempt = 0; attempt != 100 && !ready && GetTickCount64() < edit_deadline; ++attempt) {
        struct Search { bool matched = false; unsigned edits = 0, reads = 0; DWORD error = 0; std::wstring text; } search;
        EnumChildWindows(context.window.load(), [](HWND child, LPARAM opaque) -> BOOL {
            wchar_t name[32]{}; GetClassNameW(child, name, 32);
            if (_wcsicmp(name, L"EDIT")) return TRUE;
            wchar_t text[256]{}; DWORD_PTR copied = 0;
            auto& observed = *reinterpret_cast<Search*>(opaque); ++observed.edits;
            SetLastError(ERROR_SUCCESS);
            if (SendMessageTimeoutW(child, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(text),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied)) {
                ++observed.reads; observed.text = text;
                if (observed.text == L"selection.txt") observed.matched = true;
            } else observed.error = GetLastError();
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        ready = search.matched; edit_count = search.edits; read_count = search.reads; read_error = search.error; last_text = std::move(search.text);
        if (!ready) Sleep(20);
    }
    std::cout << "[DRIVE] edit_matched=" << ready << " edit_count=" << edit_count << " reads=" << read_count
        << " last_error=" << read_error << " last_text_units=" << last_text.size() << " last_text_hex=";
    for (const wchar_t unit : last_text) std::cout << std::hex << static_cast<unsigned>(unit) << ':';
    std::cout << std::dec << '\n';
    if (ready && Enter(context, 1, deadline)) {
        const auto now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>((std::min)(deadline - now, 8000ull)) : 0;
        const DWORD veto_wait = WaitForSingleObject(context.veto, remaining);
        const DWORD veto_error = veto_wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        std::cout << "[DRIVE] veto_wait=" << veto_wait << " error=" << veto_error << " budget_ms=" << remaining << '\n';
        if (veto_wait == WAIT_OBJECT_0) {
            Sleep(150);
            context.live_veto = Owned(context.window.load(), context.thread) && IsWindowVisible(context.window.load());
            context.input_ok = context.live_veto && Enter(context, 2, deadline);
        }
    } else std::cout << "[DRIVE] veto_wait_called=0\n";
    if (!context.input_ok && Owned(context.window.load(), context.thread)) PostMessageW(context.window.load(), WM_CLOSE, 0, 0);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--owner-fixture") return ExternalOwnerChild(argv[2], argv[3]);
    std::cout << std::unitbuf;
    const bool owners_only = (argc == 3 || argc == 4) && std::wstring(argv[2]) == L"--owners";
    int owner_case = -1;
    if (owners_only && argc == 4) {
        const std::wstring_view names[]{L"accept", L"cancel", L"destroy", L"reenable", L"fileok-reenable", L"original-disabled"};
        for (int i = 0; i != 6; ++i) if (std::wstring_view(argv[3]) == names[i]) owner_case = i;
        if (owner_case < 0) { std::cerr << "unknown owner case\n"; return 2; }
    }
    const bool dpi_only = argc == 3 && std::wstring(argv[2]) == L"--dpi";
    const bool inspect = argc == 3 && std::wstring(argv[2]) == L"--inspect";
    const bool render_failure = argc == 3 && std::wstring(argv[2]) == L"--render-failure";
    if ((argc != 2 && !dpi_only && !inspect && !render_failure && !owners_only) || !std::filesystem::path(argv[1]).is_absolute()) return 2;
    std::cout << "mode=" << (owners_only ? "owner-lifecycle (not keyboard acceptance)" : inspect ? "visual-inspection (not automated input acceptance)" : dpi_only ? "dpi-only (not keyboard acceptance)" : render_failure ? "render-failure (post-FileOk submission rejection)" : "full-input") << '\n';
    struct FailureEnvironment {
        bool active;
        ~FailureEnvironment() { if (active) SetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", nullptr); }
    } failure_environment{render_failure};
    if (render_failure) Check(SetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", nullptr) != FALSE,
        "render failure starts disarmed in this test process");
    HANDLE complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([&] {
        if (WaitForSingleObject(complete, inspect ? 600000 : 30000) != WAIT_OBJECT_0) {
            std::cerr << "[FAIL] in-process proof watchdog: only this test process is terminated\n";
            TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    const HRESULT ole = OleInitialize(nullptr);
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"inprocess_picker_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    const auto path = directory / L"selection.txt";
    std::error_code ec; const bool created = std::filesystem::create_directories(directory, ec);
    std::wcout << L"fixture=" << directory.native() << L'\n';
    HANDLE file = created ? CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    Check(file != INVALID_HANDLE_VALUE, "exclusive owned fixture created");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SetLastError(ERROR_SUCCESS);
    HMODULE dll = LoadLibraryW(argv[1]);
    const DWORD load_error = dll ? ERROR_SUCCESS : GetLastError();
    FARPROC symbol = nullptr;
    DWORD symbol_error = ERROR_SUCCESS;
    if (dll) {
        SetLastError(ERROR_SUCCESS);
        symbol = GetProcAddress(dll, "PulsePickerUiGetApi");
        if (!symbol) symbol_error = GetLastError();
    }
    auto get_api = reinterpret_cast<decltype(&PulsePickerUiGetApi)>(symbol);
    PulsePickerUiApi api{};
    const HRESULT api_hr = get_api ? get_api(PULSE_PICKER_UI_ABI, sizeof(api), &api) : E_UNEXPECTED;
    std::cout << "[FACT] dll=" << static_cast<const void*>(dll) << " load_error=" << load_error
        << " export_found=" << (symbol != nullptr) << " export_error=" << symbol_error
        << " get_api_called=" << (get_api != nullptr) << " get_api_hr=" << static_cast<long>(api_hr)
        << " requested_size=" << sizeof(api) << " returned_size=" << api.size
        << " requested_version=" << PULSE_PICKER_UI_ABI << " returned_version=" << api.version
        << " ole_hr=" << static_cast<long>(ole) << '\n';
    Check(get_api && api_hr == S_OK, "load explicit proof ABI");
    if (get_api && api_hr == S_OK && api.Initialize && SUCCEEDED(ole) && file != INVALID_HANDLE_VALUE) {
        Check(api.Initialize(1) == E_INVALIDARG, "initialization reserved flags rejected");
        const HRESULT initialized = api.Initialize(0);
        Check(initialized == S_OK, "explicit calling STA initialization");
        if (initialized == S_OK) {
            HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse owned in-process proof", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                150, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(owner != nullptr, "same-thread visible owned owner");
            std::wstring folder = directory.native(), leaf = L"selection.txt";
            PulsePickerUiConfig config{sizeof(config), 1, 0, 0, owner, folder.c_str(), static_cast<uint32_t>(folder.size()), leaf.c_str(), static_cast<uint32_t>(leaf.size())};
            std::vector<wchar_t> buffer(PULSE_PICKER_UI_MAX_UNITS + 1);
            PulsePickerUiResult result{sizeof(result), 0, 0, 0, buffer.data(), static_cast<uint32_t>(buffer.size())};
            Context context; context.api = &api; context.thread = GetCurrentThreadId(); context.path = path.native();
            context.render_failure = render_failure;
            context.owner = owner;
            Check(context.dpi.Available(), "DPI inspection APIs available");
            if (context.dpi.Available()) {
                context.prior_thread = context.dpi.thread(); context.prior_owner = context.dpi.window(owner);
                std::cout << "[DPI-BEFORE] thread=" << static_cast<void*>(context.prior_thread)
                    << " owner=" << static_cast<void*>(context.prior_owner) << " owner_dpi=" << context.dpi.dpi(owner) << '\n';
            }
            const auto check_dpi_restored = [&] {
                const auto thread = context.dpi.Available() ? context.dpi.thread() : nullptr;
                const auto owner_context = context.dpi.Available() ? context.dpi.window(owner) : nullptr;
                std::cout << "[DPI-AFTER] thread=" << static_cast<void*>(thread) << " owner=" << static_cast<void*>(owner_context) << '\n';
                Check(context.dpi.Available() && context.dpi.equal(thread, context.prior_thread), "Show restores original calling thread DPI context");
                Check(context.dpi.Available() && context.dpi.equal(owner_context, context.prior_owner), "Show preserves existing owner DPI context");
                Check(context.dpi_seen && context.dpi_valid, "actual picker and callback thread use PMv2 without owner context mutation");
            };
            context.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr); context.veto = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            PulsePickerUiCallbacks callbacks{sizeof(callbacks), 0, Event, &context};
            if (inspect) {
                PulsePickerUiCallbacks visual_callbacks{sizeof(visual_callbacks), 0,
                    [](void*, const PulsePickerUiEvent* event) -> HRESULT {
                        std::cout << "[VISUAL] event=" << event->kind << " hwnd=" << static_cast<void*>(event->window) << '\n';
                        return S_OK;
                    }, nullptr};
                const HRESULT visual_hr = api.Show(&config, &visual_callbacks, &result);
                std::cout << "[VISUAL] show_hr=" << static_cast<long>(visual_hr) << " status=" << result.status << '\n';
                Check(visual_hr == S_OK || visual_hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), "inspection ends by acceptance or cancellation");
                Check(IsWindowEnabled(owner), "inspection restores owner");
            }
            if (!dpi_only && !inspect && !owners_only) {
            hook_context = &context;
            HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, ObserveMessage, nullptr, context.thread);
            Check(hook != nullptr, "observe actual Show-thread keyboard message retrieval");
            std::thread driver([&] { Drive(context); });
            const HRESULT hr = api.Show(&config, &callbacks, &result);
            driver.join();
            const auto keyups_at_return = context.keyups.load();
            // The caller resumes its ordinary message loop after the modal Show.
            // Keep the hook alive for real queued releases; never synthesize them.
            const auto release_deadline = GetTickCount64() + 250;
            while (context.keyups < 2 && GetTickCount64() < release_deadline) {
                MSG message{};
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                    TranslateMessage(&message); DispatchMessageW(&message);
                } else {
                    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
            std::cout << "[CALLER-QUEUE] keyups_at_show_return=" << keyups_at_return
                << " after_caller_pump=" << context.keyups.load()
                << " before_accept=" << context.keyups_before_accept.load() << '\n';
            if (hook) UnhookWindowsHookEx(hook); hook_context = nullptr;
            std::cout << "show_hr=" << static_cast<long>(hr) << " status=" << result.status << " error=" << result.win32_error
                << " callbacks=" << context.accepts << " keyups=" << context.keyups << '\n';
            Check(context.input_ok && context.keyups >= 2, "both real SendInput Return releases retrieved on calling STA");
            Check(context.keyups_before_accept >= 1, "first real Return release retrieved before second FileOk acceptance");
            Check(context.identity && context.candidate, "callbacks expose actual HWND and candidate on calling STA");
            Check(context.live_veto && context.accepts == 2, "first FileOk veto retains same actual window before second acceptance");
            if (render_failure) {
                Check(context.failure_armed && context.accepts == 2, "second real FileOk returns S_OK after arming Present failure");
                Check(hr == HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED) && result.status == PULSE_PICKER_UI_FAILED &&
                    result.win32_error == ERROR_FUNCTION_FAILED && result.path_units == 0 && buffer[0] == L'\0',
                    "post-FileOk Present failure returns Failed Win32 error and no committed path");
                Check(IsWindowEnabled(owner), "owner restored after post-FileOk rendering failure");
                const BOOL cleared = SetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", nullptr);
                wchar_t value[2]{}; SetLastError(ERROR_SUCCESS);
                const DWORD remaining = GetEnvironmentVariableW(L"PULSE_PICKER_UI_PROOF_FAIL_PRESENT", value, 2);
                Check(cleared && !remaining && GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                    "render fault cleared before the next Show");
                context.render_failure = false;
            } else {
            Check(hr == S_OK && result.status == PULSE_PICKER_UI_ACCEPTED && std::wstring(buffer.data()) == context.path, "second FileOk commits exact fixture path");
            Check(IsWindowEnabled(owner), "owner restored after acceptance");
            }
            check_dpi_restored();
            }
            if (owners_only) OwnerCases(api, config, result, path.native(), owner_case);
            HWND outside = reinterpret_cast<HWND>(1);
            Check(FAILED(api.GetWindow(&outside)) && !outside, "no stale picker HWND after Show");
            if (owner_case < 0) {
            context.cancel = true; context.window = nullptr;
            context.dpi_seen = false; context.dpi_valid = true;
            const HRESULT cancelled = api.Show(&config, &callbacks, &result);
            check_dpi_restored();
            Check(cancelled == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.status == PULSE_PICKER_UI_CANCELLED && !result.path_units && !buffer[0], "callback Close cancels actual picker with no result");
            Check(context.identity && IsWindowEnabled(owner), "cancel callback identity and owner restoration");
            }
            CloseHandle(context.ready); CloseHandle(context.veto);
            if (owner) DestroyWindow(owner);
            Check(api.Shutdown() == S_OK && api.Shutdown() == S_FALSE, "STA resource shutdown is balanced and repeatable");
        }
    }
    if (dll) FreeLibrary(dll);
    if (file != INVALID_HANDLE_VALUE) Check(DeleteFileW(path.c_str()) != FALSE, "owned fixture has no lingering handle");
    if (created) Check(RemoveDirectoryW(directory.c_str()) != FALSE, "owned fixture directory removed");
    if (SUCCEEDED(ole)) OleUninitialize();
    SetEvent(complete); watchdog.join(); CloseHandle(complete);
    std::cout << checks << " checks " << failures << " failures\n";
    return failures ? 1 : 0;
}
