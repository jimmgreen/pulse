#include "../ui/preview_handler_host.h"
#include "../common/runtime_log.h"
#include "preview_resource_test_metrics.h"
#include <filesystem>
#include <fstream>
#include <shobjidl.h>
#include <windows.h>
#include <dwmapi.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>

namespace pulse::ui {
extern IUnknown* (*g_preview_handler_factory_for_test)();
extern std::atomic<uint32_t> g_preview_handler_provider_for_test;
extern HRESULT g_preview_handler_activation_hr_for_test;
void ResetSlowPreviewProvidersForTest();
bool PreviewProviderCoolingDownForTest(const std::wstring& path);
}
namespace {
HANDLE entered = nullptr, release_preview = nullptr, finished = nullptr;
std::atomic<HWND> preview_parent{nullptr};
std::atomic<HRESULT> window_result{S_OK}, preview_result{S_OK}, initialize_result{S_OK};
std::atomic<unsigned> live_handlers{0};
bool require_visible_surface = false;
bool HasCloak(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                            sizeof(cloaked))) && cloaked != 0;
}

class SlowHandler final : public IPreviewHandler, public IInitializeWithFile {
public:
    SlowHandler() { ++live_handlers; }
    ~SlowHandler() { --live_handlers; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IPreviewHandler) *out = static_cast<IPreviewHandler*>(this);
        else if (iid == IID_IInitializeWithFile) *out = static_cast<IInitializeWithFile*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR, DWORD) override { return initialize_result; }
    HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT*) override { preview_parent = hwnd; return window_result; }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DoPreview() override {
        // Model a successful provider that initializes its surface only if
        // the parent is visible. Returning S_OK alone does not create it.
        const HWND parent = preview_parent.load();
        if (require_visible_surface && IsWindowVisible(parent)) {
            content_ = CreateWindowExW(0, L"STATIC", L"Provider content",
                WS_CHILD | WS_VISIBLE, 0, 0, 100, 30, parent, nullptr, nullptr, nullptr);
        }
        SetEvent(entered);
        WaitForSingleObject(release_preview, 15000);
        return preview_result;
    }
    HRESULT STDMETHODCALLTYPE Unload() override {
        if (content_) DestroyWindow(content_);
        content_ = nullptr;
        SetEvent(finished);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND* hwnd) override { *hwnd = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }
private:
    std::atomic<ULONG> refs_{1};
    HWND content_ = nullptr;
};
IUnknown* CreateSlowHandler() { return static_cast<IPreviewHandler*>(new SlowHandler); }
bool PumpUntil(HANDLE event) {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(event, 0) == WAIT_OBJECT_0) return true;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(5);
    }
    return false;
}
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && std::string_view(argv[1]) == "--visible-initialization") {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
        require_visible_surface = true;
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_preview = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Preview owner",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, -30000, -30000, 300, 300,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!owner || !entered || !release_preview || !finished) return 2;
        pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
        bool ok = true;
        const auto check = [&](bool pass, const char* label) {
            std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
            ok &= pass;
        };
        {
            using Host = pulse::ui::PreviewHandlerHost;
            using Content = pulse::ui::PreviewHandlerContent;
            Host host;
            const auto content = [&] {
                return pulse::ui::ResolvePreviewHandlerContent(host.state(), host.Presented(), true);
            };
            check(content() == Content::Loading, "first Office frame waits for the system layout");
            const auto sync = [&](uint64_t generation) {
                host.Sync(owner, {0, 0, 200, 200}, L"visible-init.docx",
                    FILE_ATTRIBUTE_NORMAL, generation, 1, 1, false,
                    D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            };
            const auto wait_for = [](auto predicate) {
                const auto end = GetTickCount64() + 5000;
                while (!predicate() && GetTickCount64() < end) Sleep(5);
                return predicate();
            };
            sync(1);
            check(PumpUntil(entered), "provider enters DoPreview");
            const HWND surface = preview_parent.load();
            check(IsWindowVisible(surface) && GetWindow(surface, GW_CHILD),
                "DoPreview creates content on a Win32-visible surface");
            check((!IsWindowVisible(surface) || HasCloak(surface)) && !host.Presented(),
                "unfinished preview remains off the desktop");
            check(content() == Content::Loading, "slow DoPreview does not flash the native Office layout");
            SetEvent(release_preview);
            check(wait_for([&] { return host.state() == Host::State::Shown; }) &&
                    host.Presented() && !HasCloak(surface) &&
                    GetWindow(surface, GW_CHILD),
                "foreground preview presents content without switching apps");
            check(content() == Content::Overlay, "loading hands off directly to the system layout");
            host.Hide();
            check(PumpUntil(finished), "provider unloads");
            ResetEvent(finished);
            ResetEvent(entered);
            ResetEvent(release_preview);
            sync(2);
            check(PumpUntil(entered), "subsequent provider enters DoPreview");
            const HWND cancelled_surface = preview_parent.load();
            host.Hide();
            SetEvent(release_preview);
            check(PumpUntil(finished), "cancelled provider unloads");
            check((!IsWindowVisible(cancelled_surface) || HasCloak(cancelled_surface)) &&
                    !host.Presented(),
                "late completion does not present cancelled contents");
            ResetEvent(finished);
            ResetEvent(entered);
            ResetEvent(release_preview);
            sync(3);
            check(PumpUntil(entered), "third provider enters DoPreview");
            host.SetCovered(true);
            SetEvent(release_preview);
            check(wait_for([&] { return host.state() == Host::State::Shown; }) &&
                    !host.Presented() && (!IsWindowVisible(preview_parent.load()) ||
                                          HasCloak(preview_parent.load())),
                "coverage requested during DoPreview prevents presentation");
            check(content() == Content::Concealed, "a covered system overlay does not switch to the native layout");
            host.SetCovered(false);
            check(wait_for([&] { return host.Presented(); }) &&
                    GetWindow(preview_parent.load(), GW_CHILD),
                "removing coverage shows existing content without reopening");

            ResetEvent(entered);
            ResetEvent(release_preview);
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", L"80");
            sync(4);
            check(PumpUntil(entered), "system preview enters the watchdog fixture");
            ResetEvent(finished);
            Sleep(120);
            sync(4);
            check(pulse::ui::PreviewProviderCoolingDownForTest(L"visible-init.docx") &&
                    !Host::CanHost(L"visible-init.docx") && !host.Presented(),
                "timed-out system preview runs the native route on the next frame");
            SetEvent(release_preview);
            check(PumpUntil(finished), "timed-out provider unloads without presenting");
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", nullptr);
        }
        DestroyWindow(owner);
        CloseHandle(finished);
        CloseHandle(release_preview);
        CloseHandle(entered);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--file-failure-isolation") {
        const auto dir = std::filesystem::absolute(argv[2]);
        const auto bad = (dir / L"first.pulse-preview-recovery-test").wstring();
        const auto next = (dir / L"second.pulse-preview-recovery-test").wstring();
        const auto other = (dir / L"third.pulse-preview-other-test").wstring();
        if (!std::filesystem::is_regular_file(bad) || !std::filesystem::is_regular_file(next) ||
            !std::filesystem::is_regular_file(other) || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
            return 2;
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_preview = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Preview failure isolation fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, -30000, -30000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!entered || !release_preview || !finished || !owner) return 2;
        pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
        using Host = pulse::ui::PreviewHandlerHost;
        bool ok = true;
        const auto check = [&](bool pass, const char* label) {
            std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
            ok &= pass;
        };
        const auto wait_for = [&](Host& host, Host::State wanted) {
            const auto deadline = GetTickCount64() + 3000;
            while (host.state() != wanted && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(5);
            }
            return host.state() == wanted;
        };
        for (unsigned phase = 0; phase < 3; ++phase) {
            pulse::ui::ResetSlowPreviewProvidersForTest();
            Host host;
            initialize_result = phase == 0 ? E_ACCESSDENIED : S_OK;
            window_result = phase == 1 ? E_FAIL : S_OK;
            preview_result = phase == 2 ? E_FAIL : S_OK;
            const auto sync = [&](const std::wstring& path, uint64_t identity, uint64_t size) {
                host.Sync(owner, {0, 0, 200, 200}, path, FILE_ATTRIBUTE_NORMAL, identity, 1, size,
                    false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black),
                    Host::CanHost(path), true);
            };
            std::printf("phase=%u (initialize=0, set-window=1, render=2)\n", phase);
            sync(bad, 1, 512ull * 1024 * 1024);
            check(wait_for(host, Host::State::Failed), "first document fails without showing stale content");
            using Content = pulse::ui::PreviewHandlerContent;
            check(pulse::ui::ResolvePreviewHandlerContent(host.state(), host.Presented(), true) ==
                    Content::NativeFallback,
                "confirmed provider failure allows Office content fallback");
            check(pulse::ui::ResolvePreviewHandlerContent(host.state(), host.Presented(), false) ==
                    Content::Unavailable,
                "confirmed provider failure without a native reader shows unavailable");
            initialize_result = window_result = preview_result = S_OK;
            check(Host::CanHost(next), "a file-specific failure does not disable the extension");
            sync(next, 2, 1);
            check(wait_for(host, Host::State::Shown) && IsWindowVisible(host.overlay_window_for_test()),
                "healthy document of the same format becomes visible in the same pane");
            sync(other, 3, 1);
            check(wait_for(host, Host::State::Shown) && IsWindowVisible(host.overlay_window_for_test()),
                "unrelated format still renders in the same pane");
        }
        pulse::ui::g_preview_handler_factory_for_test = nullptr;
        DestroyWindow(owner);
        CloseHandle(entered);
        CloseHandle(release_preview);
        CloseHandle(finished);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (argc == 2 && (std::string(argv[1]) == "--resource-bounds" || std::string(argv[1]) == "--resource-concurrent")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_preview = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"resource bound fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, -30000, -30000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
        };
        auto pump = [](unsigned duration) {
            const auto end = GetTickCount64() + duration;
            do {
                MSG msg{};
                while (GetTickCount64() < end && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                Sleep(5);
            } while (GetTickCount64() < end);
        };
        const auto before = PreviewResourceMetrics::Capture("before");
        for (int phase = 0; phase < 2; ++phase) {
            ResetEvent(release_preview);
            std::vector<std::unique_ptr<pulse::ui::PreviewHandlerHost>> hosts;
            for (int i = 0; i < 16; ++i) {
                pulse::ui::g_preview_handler_provider_for_test = phase == 0 ? 1 : i + 20;
                hosts.push_back(std::make_unique<pulse::ui::PreviewHandlerHost>());
                auto& host = *hosts.back();
                host.Sync(owner, {0, 0, 200, 200}, L"blocked.xls", FILE_ATTRIBUTE_NORMAL, i + 1, 1, 1,
                    false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
                pump(50);
                if (std::string(argv[1]) == "--resource-bounds") {
                    const auto retirement_started = GetTickCount64();
                    hosts.clear();
                    check(GetTickCount64() - retirement_started < 500, "blocked provider retirement does not wait on UI");
                }
            }
            const auto held = PreviewResourceMetrics::Capture(phase == 0 ? "same-provider-held" : "distinct-providers-held");
            const unsigned limit = phase == 0 ? 2 : 8;
            check(live_handlers > 0 && live_handlers <= limit,
                phase == 0 ? "same provider live sessions remain bounded" : "all live apartments remain globally bounded");
            check(held.threads <= before.threads + 8, "thread count stays within admission bound plus COM overhead");
            SetEvent(release_preview);
            pump(200);
            hosts.clear();
            const auto deadline = GetTickCount64() + 5000;
            while (live_handlers && GetTickCount64() < deadline) pump(20);
            pump(100);
            check(live_handlers == 0, "blocked providers release after completion");
            PreviewResourceMetrics::Capture("released");
        }
        if (std::string(argv[1]) == "--resource-bounds") {
            ResetEvent(entered); ResetEvent(release_preview);
            pulse::ui::PreviewHandlerHost host;
            host.Sync(owner, {0, 0, 200, 200}, L"owner-close.xls", FILE_ATTRIBUTE_NORMAL, 98, 1, 1,
                false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            check(PumpUntil(entered), "owner-close case enters blocked provider");
            const auto close_started = GetTickCount64();
            DestroyWindow(owner);
            check(GetTickCount64() - close_started < 500, "owner destruction does not wait for blocked provider");
            SetEvent(release_preview); pump(100);
            owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"resource reopen fixture",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE, -30000, -30000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        pulse::ui::g_preview_handler_provider_for_test = 1;
        ResetEvent(entered);
        {
            pulse::ui::PreviewHandlerHost host;
            host.Sync(owner, {0, 0, 200, 200}, L"resumed.xls", FILE_ATTRIBUTE_NORMAL, 99, 1, 1,
                false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            check(PumpUntil(entered), "provider can reopen after released capacity");
            pump(50);
        }
        pump(100);
        const auto after = PreviewResourceMetrics::Capture("final");
        check(after.handles <= before.handles + 8, "provider handles return near baseline after release");
        DestroyWindow(owner);
        CloseHandle(entered); CloseHandle(release_preview); CloseHandle(finished);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (argc == 2 && std::string(argv[1]) == "--embedded-window") {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        release_preview = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
        HWND root = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"embedded preview regression",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 500, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND pane = CreateWindowExW(0, L"STATIC", L"pane", WS_CHILD | WS_VISIBLE,
            200, 20, 220, 220, root, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
        };
        auto pump = [] {
            const auto deadline = GetTickCount64() + 250;
            while (GetTickCount64() < deadline) {
                MSG msg{};
                while (GetTickCount64() < deadline && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                Sleep(5);
            }
        };
        {
            pulse::ui::PreviewHandlerHost host;
            host.Sync(pane, {10, 10, 210, 210}, L"embedded.xls", FILE_ATTRIBUTE_NORMAL, 1, 1, 1,
                false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            check(PumpUntil(entered), "native handler receives embedded host");
            pump();
            HWND overlay = preview_parent.load();
            check(host.state() == pulse::ui::PreviewHandlerHost::State::Shown, "embedded native preview is shown");
            host.Sync(pane, {10, 10, 180, 180}, L"embedded.xls", FILE_ATTRIBUTE_NORMAL, 1, 1, 1,
                false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            pump();
            check(!(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST), "embedded preview is not globally topmost");
            SetWindowPos(root, nullptr, 280, 240, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            pump();
            RECT actual{}; GetWindowRect(overlay, &actual);
            POINT expected{10, 10}; ClientToScreen(pane, &expected);
            check(actual.left == expected.x && actual.top == expected.y, "native preview follows ancestor move without repaint or Sync");
            ShowWindow(root, SW_HIDE); pump();
            check(!IsWindowVisible(overlay), "ancestor hide hides native preview");
            ShowWindow(root, SW_SHOWNOACTIVATE); pump();
            check(IsWindowVisible(overlay), "ancestor show restores native preview");
            SetWindowPos(pane, nullptr, 200, 20, 100, 100, SWP_NOZORDER | SWP_NOACTIVATE); pump();
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            RECT clipped{};
            const int kind = GetWindowRgn(overlay, region);
            GetRgnBox(region, &clipped); DeleteObject(region);
            check(kind != ERROR && clipped.right <= 90 && clipped.bottom <= 90,
                "native preview is clipped to embedded parent client area");
            ShowWindow(root, SW_MINIMIZE); pump();
            check(!IsWindowVisible(overlay), "ancestor minimize hides native preview");
            ShowWindow(root, SW_SHOWNOACTIVATE); pump();
            host.Hide(); pump();
            check(!IsWindowVisible(overlay), "reset hides native preview");
        }
        {
            pulse::ui::PreviewHandlerHost host;
            ResetEvent(entered);
            host.Sync(root, {10, 10, 180, 180}, L"top-level.docx", FILE_ATTRIBUTE_NORMAL, 2, 1, 1,
                false, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
            check(PumpUntil(entered), "top-level system preview starts");
            pump();
            const HWND overlay = host.overlay_window_for_test();
            const auto in_top_band = [&] { return (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; };
            check(overlay && IsWindowVisible(overlay) && in_top_band(), "top-level system preview initially placed above its window");
            for (int cycle = 0; cycle < 3; ++cycle) {
                host.SetCovered(true); pump();
                check(!IsWindowVisible(overlay) && !in_top_band(), "covered system preview yields z-order");
                host.SetCovered(false); pump();
                check(IsWindowVisible(overlay) && in_top_band(), "uncover restores the preview above its window without a resize");
            }
        }
        DestroyWindow(root);
        CloseHandle(entered); CloseHandle(release_preview); CloseHandle(finished);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    const auto log_root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"handler-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    pulse::diagnostics::runtime::Initialize(log_root.wstring(), "handler_test");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_preview = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"preview regression", WS_OVERLAPPED | WS_VISIBLE,
        -32000, -32000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = true;
    auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
    };
    // Text associations must stay on the native text path, which answers
    // instantly instead of starting a handler process for the same plain text:
    // .gitignore (a text/plain content type, whatever handler claimed it), and
    // the extensions whose only handler is the built-in TXT previewer.
    // Extensions with any other handler keep the handler path. Real association
    // lookups run here, so the fake factory below must not be installed yet.
    pulse::ui::g_preview_handler_factory_for_test = nullptr;
    {
        struct AssocFixture {
            const wchar_t* extension;
            const wchar_t* content_type;  // "Content Type" value; null: none
            const wchar_t* progid;
            const wchar_t* handler;
        };
        const AssocFixture fixtures[] = {
            {L".pulse-handler-plain-test", L"text/plain", L"PulseHandlerPlainTest",
             L"{1a2b3c4d-1111-2222-3333-445566778899}"},
            {L".pulse-handler-txtprev-test", nullptr, L"PulseHandlerTxtprevTest",
             L"{1531d583-8375-4d3f-b5fb-d23bbd169f22}"},
            {L".pulse-handler-other-test", nullptr, L"PulseHandlerOtherTest",
             L"{1a2b3c4d-1111-2222-3333-445566778899}"},
            {L".pulse-handler-dangling-test", nullptr, L"PulseHandlerDanglingTest",
             L"{2b3c4d5e-1111-2222-3333-445566778899}"},
            {L".pulse-handler-inproconly-test", nullptr, L"PulseHandlerInprocOnlyTest",
             L"{3c4d5e6f-1111-2222-3333-445566778899}"},
            {L".pulse-handler-surrogate-test", nullptr, L"PulseHandlerSurrogateTest",
             L"{4d5e6f70-1111-2222-3333-445566778899}"},
            {L".pulse-handler-missingexe-test", nullptr, L"PulseHandlerMissingExeTest",
             L"{5e6f7081-1111-2222-3333-445566778899}"},
            {L".pulse-handler-missingdll-test", nullptr, L"PulseHandlerMissingDllTest",
             L"{6f708192-1111-2222-3333-445566778899}"},
            {L".pulse-handler-nosurrogate-test", nullptr, L"PulseHandlerNoSurrogateTest",
             L"{708192a3-1111-2222-3333-445566778899}"},
            {L".pulse-handler-fallback-test", nullptr, L"PulseHandlerFallbackTest",
             L"{1a2b3c4d-1111-2222-3333-445566778899}"},
        };
        const wchar_t* test_clsids[] = {
            L"{1a2b3c4d-1111-2222-3333-445566778899}",
            L"{2b3c4d5e-1111-2222-3333-445566778899}",
            L"{3c4d5e6f-1111-2222-3333-445566778899}",
            L"{4d5e6f70-1111-2222-3333-445566778899}",
            L"{5e6f7081-1111-2222-3333-445566778899}",
            L"{6f708192-1111-2222-3333-445566778899}",
            L"{708192a3-1111-2222-3333-445566778899}",
        };
        auto set_string = [](const std::wstring& key_path, const wchar_t* name,
                             const wchar_t* value) {
            HKEY key{};
            if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                return false;
            const bool set = RegSetValueExW(key, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value),
                (lstrlenW(value) + 1) * sizeof(wchar_t)) == ERROR_SUCCESS;
            RegCloseKey(key);
            return set;
        };
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{2b3c4d5e-1111-2222-3333-445566778899}");
        bool registered = true;
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{1a2b3c4d-1111-2222-3333-445566778899}\\LocalServer32",
            nullptr, L"prevhost.exe");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{3c4d5e6f-1111-2222-3333-445566778899}\\InprocServer32",
            nullptr, L"fake.dll");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{4d5e6f70-1111-2222-3333-445566778899}",
            L"AppID", L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{4d5e6f70-1111-2222-3333-445566778899}\\InprocServer32",
            nullptr, L"%SystemRoot%\\System32\\shell32.dll");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{5e6f7081-1111-2222-3333-445566778899}\\LocalServer32",
            nullptr, L"C:\\NonExistentPulseOffice\\WINWORD.EXE");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{6f708192-1111-2222-3333-445566778899}",
            L"AppID", L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{6f708192-1111-2222-3333-445566778899}\\InprocServer32",
            nullptr, L"C:\\NonExistentPulseWps\\wpspreview.dll");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{708192a3-1111-2222-3333-445566778899}",
            L"AppID", L"{99999999-8888-7777-6666-555555555555}");
        registered &= set_string(
            L"Software\\Classes\\CLSID\\{708192a3-1111-2222-3333-445566778899}\\InprocServer32",
            nullptr, L"%SystemRoot%\\System32\\shell32.dll");
        for (const AssocFixture& fixture : fixtures) {
            const std::wstring ext_key = std::wstring(L"Software\\Classes\\") + fixture.extension;
            registered &= set_string(ext_key, nullptr, fixture.progid);
            if (fixture.content_type)
                registered &= set_string(ext_key, L"Content Type", fixture.content_type);
            registered &= set_string(std::wstring(L"Software\\Classes\\") + fixture.progid +
                                         L"\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}",
                                     nullptr, fixture.handler);
        }
        registered &= set_string(
            L"Software\\Classes\\.pulse-handler-fallback-test\\shellex\\{8895b1c6-b41f-4c1c-a562-0d564250836f}",
            nullptr, L"{5e6f7081-1111-2222-3333-445566778899}");
        if (registered) {
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"a.pulse-handler-plain-test"),
                  "text/plain content type stays on the native text path");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"b.pulse-handler-txtprev-test"),
                  "built-in TXT previewer stays on the native text path");
            check(pulse::ui::PreviewHandlerHost::CanHost(L"c.pulse-handler-other-test"),
                  "other preview handlers keep the handler path");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"d.pulse-handler-dangling-test"),
                  "unregistered CLSID handler is rejected before hosting");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"e.pulse-handler-inproconly-test"),
                  "inproc-only CLSID without AppID or LocalServer32 is rejected before hosting");
            check(pulse::ui::PreviewHandlerHost::CanHost(L"f.pulse-handler-surrogate-test"),
                  "surrogate InprocServer32 with AppID keeps the handler path");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"g.pulse-handler-missingexe-test"),
                  "LocalServer32 with missing exe on disk is rejected before hosting");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"h.pulse-handler-missingdll-test"),
                  "surrogate InprocServer32 with missing dll on disk is rejected before hosting");
            check(!pulse::ui::PreviewHandlerHost::CanHost(L"i.pulse-handler-nosurrogate-test"),
                  "AppID without DllSurrogate or LocalService is rejected before hosting");
            check(pulse::ui::PreviewHandlerHost::CanHost(L"j.pulse-handler-fallback-test"),
                  "dangling direct extension ShellEx falls back to activatable ProgId handler");
        } else {
            std::printf("[SKIP] handler association fixtures need HKCU write access\n");
        }
        for (const AssocFixture& fixture : fixtures) {
            RegDeleteTreeW(HKEY_CURRENT_USER,
                (std::wstring(L"Software\\Classes\\") + fixture.extension).c_str());
            RegDeleteTreeW(HKEY_CURRENT_USER,
                (std::wstring(L"Software\\Classes\\") + fixture.progid).c_str());
        }
        for (const wchar_t* clsid : test_clsids) {
            RegDeleteTreeW(HKEY_CURRENT_USER,
                (std::wstring(L"Software\\Classes\\CLSID\\") + clsid).c_str());
        }
    }
    pulse::ui::g_preview_handler_factory_for_test = CreateSlowHandler;
    {
        pulse::ui::PreviewHandlerHost host;
        D2D1_RECT_F bounds{0, 0, 200, 200};
        host.Sync(owner, bounds, L"isolated.docx", FILE_ATTRIBUTE_NORMAL, 1, 1, 1,
            true, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
        check(PumpUntil(entered), "slow COM handler entered DoPreview");
        HWND overlay = preview_parent.load();
        check(overlay && (!IsWindowVisible(overlay) || HasCloak(overlay)) && !host.Presented(),
              "provider initializes on a surface that is not presented");
        host.Hide();
        preview_result = E_ABORT;
        SetEvent(release_preview);
        check(PumpUntil(finished), "cancelled provider is unloaded after returning");
        check(!(GetWindowLongPtrW(overlay, GWL_STYLE) & WS_VISIBLE),
              "late COM completion cannot show cancelled preview");
        preview_result = S_OK;
        ResetEvent(entered);
        ResetEvent(release_preview);
        host.Sync(owner, bounds, L"next.docx", FILE_ATTRIBUTE_NORMAL, 2, 1, 1,
            true, D2D1::ColorF(D2D1::ColorF::White), D2D1::ColorF(D2D1::ColorF::Black), true, true);
        check(PumpUntil(entered), "next preview initializes after cancellation");
        SetEvent(release_preview);
        const auto deadline = GetTickCount64() + 5000;
        while (host.state() != pulse::ui::PreviewHandlerHost::State::Shown && GetTickCount64() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        check(host.state() == pulse::ui::PreviewHandlerHost::State::Shown &&
            (GetWindowLongPtrW(preview_parent.load(), GWL_STYLE) & WS_VISIBLE),
            "current successful preview becomes visible");
    }
    for (int phase = 0; phase < 3; ++phase) {
        window_result = phase == 0 ? E_ACCESSDENIED : S_OK;
        preview_result = phase == 1 ? E_FAIL : S_OK;
        initialize_result = phase == 2 ? E_ACCESSDENIED : S_OK;
        pulse::ui::PreviewHandlerHost host;
        host.Sync(owner, D2D1_RECT_F{0, 0, 200, 200}, L"failure-private.docx", FILE_ATTRIBUTE_NORMAL,
            3 + phase, 1, 1, true, D2D1::ColorF(D2D1::ColorF::White),
            D2D1::ColorF(D2D1::ColorF::Black), true, true);
        const auto deadline = GetTickCount64() + 5000;
        while (host.state() != pulse::ui::PreviewHandlerHost::State::Failed && GetTickCount64() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        check(host.state() == pulse::ui::PreviewHandlerHost::State::Failed,
            "injected SetWindow, DoPreview or initialization failure reaches failed state");
    }
    {
        pulse::ui::g_preview_handler_activation_hr_for_test = REGDB_E_CLASSNOTREG;
        pulse::ui::g_preview_handler_factory_for_test = []() -> IUnknown* { return nullptr; };
        pulse::ui::PreviewHandlerHost host;
        const std::wstring dangling = L"dangling-runtime.pulse-handler-runtime-test";
        host.Sync(owner, D2D1_RECT_F{0, 0, 200, 200}, dangling, FILE_ATTRIBUTE_NORMAL,
            7, 1, 1, true, D2D1::ColorF(D2D1::ColorF::White),
            D2D1::ColorF(D2D1::ColorF::Black), true, true);
        const auto deadline = GetTickCount64() + 5000;
        while (host.state() != pulse::ui::PreviewHandlerHost::State::Failed && GetTickCount64() < deadline) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(5);
        }
        check(host.state() == pulse::ui::PreviewHandlerHost::State::Failed,
            "unregistered stage-2 activation reaches failed state");
        check(!pulse::ui::PreviewHandlerHost::CanHost(dangling),
            "unregistered stage-2 activation marks extension unavailable for fallback");
        pulse::ui::ResetSlowPreviewProvidersForTest();
        pulse::ui::g_preview_handler_activation_hr_for_test = E_FAIL;
        {
            pulse::ui::PreviewHandlerHost efail_host;
            const std::wstring broken = L"broken-runtime.pulse-handler-efail-test";
            efail_host.Sync(owner, D2D1_RECT_F{0, 0, 200, 200}, broken, FILE_ATTRIBUTE_NORMAL,
                8, 1, 1, true, D2D1::ColorF(D2D1::ColorF::White),
                D2D1::ColorF(D2D1::ColorF::Black), true, true);
            const auto efail_deadline = GetTickCount64() + 5000;
            while (efail_host.state() != pulse::ui::PreviewHandlerHost::State::Failed &&
                   GetTickCount64() < efail_deadline) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg); DispatchMessageW(&msg);
                }
                Sleep(5);
            }
            check(efail_host.state() == pulse::ui::PreviewHandlerHost::State::Failed,
                "E_FAIL stage-2 activation reaches failed state");
            check(!pulse::ui::PreviewHandlerHost::CanHost(broken),
                "E_FAIL stage-2 activation marks extension unavailable for fallback");
        }
        pulse::ui::ResetSlowPreviewProvidersForTest();
        pulse::ui::g_preview_handler_factory_for_test = nullptr;
    }
    pulse::diagnostics::runtime::Shutdown();
    const auto log_path = log_root / L"Diagnostics" / L"Runtime" /
        (L"handler_test-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    const auto critical_path = log_root / L"Diagnostics" / L"Runtime" /
        (L"handler_test-" + std::to_wstring(GetCurrentProcessId()) + L".critical.jsonl");
    std::ifstream log_file(log_path, std::ios::binary);
    std::ifstream critical_file(critical_path, std::ios::binary);
    const std::string log{std::istreambuf_iterator<char>(log_file), std::istreambuf_iterator<char>()};
    const std::string critical_log{std::istreambuf_iterator<char>(critical_file), std::istreambuf_iterator<char>()};
    const auto has_failure = [&](const char* needle) {
        return log.find(needle) != std::string::npos || critical_log.find(needle) != std::string::npos;
    };
    check(has_failure("\"stage\":7,\"hresult\":2147942405") &&
        has_failure("\"stage\":8,\"hresult\":2147500037") &&
        has_failure("\"stage\":3,\"hresult\":2147942405") &&
        has_failure("\"stage\":2,\"hresult\":2147746132"),
        "failure records retain raw HRESULT and distinguish initialization, SetWindow and DoPreview");
    check(log.find("\"event\":\"preview_handler_cancelled\",\"severity\":0") != std::string::npos,
        "obsolete provider failure is classified as normal cancellation");
    check(log.find("isolated.docx") == std::string::npos &&
        log.find("failure-private.docx") == std::string::npos &&
        log.find("dangling-runtime") == std::string::npos &&
        critical_log.find("isolated.docx") == std::string::npos &&
        critical_log.find("failure-private.docx") == std::string::npos &&
        critical_log.find("dangling-runtime") == std::string::npos,
        "handler diagnostics omit document paths");
    log_file.close();
    critical_file.close();
    if (ok) std::filesystem::remove_all(log_root);
    else std::wprintf(L"[DIAGNOSTICS] %ls\n", log_root.c_str());
    DestroyWindow(owner);
    CloseHandle(entered); CloseHandle(release_preview); CloseHandle(finished);
    CoUninitialize();
    return ok ? 0 : 1;
}
