#include "../ui/batch_rename_dialog.h"
#include "../common/localization.h"
#include <commctrl.h>
#include <cstdio>
#include <string>

namespace {
ULONGLONG ready_at = 0;
unsigned paints = 0, polls = 0;
bool watchdog = false, visible_at_ready = false;
LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) ++paints;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Observe, id);
    return DefSubclassProc(hwnd, message, wp, lp);
}
void CALLBACK Timeout(HWND owner, UINT, UINT_PTR id, DWORD) {
    KillTimer(owner, id); watchdog = true;
    if (HWND dialog = FindWindowW(L"PulseBatchRenameWindow", nullptr)) DestroyWindow(dialog);
}
unsigned long long Cpu() {
    FILETIME c{}, e{}, k{}, u{}; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (static_cast<unsigned long long>(k.dwHighDateTime) << 32) + k.dwLowDateTime +
           (static_cast<unsigned long long>(u.dwHighDateTime) << 32) + u.dwLowDateTime;
}
}
int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::printf("native_probe_start\n");
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"PulseAnimationBatch_" + std::to_wstring(GetCurrentProcessId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) return 3;
    // Consume the hidden console launch's first-ShowWindow override on a fixture, not the tested popup.
    HWND bootstrap = CreateWindowExW(0, L"STATIC", L"Startup fixture", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(bootstrap, SW_SHOW);
    DestroyWindow(bootstrap);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Isolated review owner", WS_POPUP | WS_VISIBLE,
        60, 60, 900, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0;
    for (int round = 0; round < 4; ++round) {
        ready_at = 0; paints = 0; polls = 0; watchdog = false;
        pulse::ui::BatchRenameDialogOptions options;
        options.ai_enabled = false;
        std::vector<std::wstring> paths, names;
        for (int i = 0; i < 100; ++i) {
            const auto stem = L"fixture-" + std::to_wstring(i);
            const auto path = L"C:\\PulseAnimationFixture\\" + stem + L".txt";
            paths.push_back(path); names.push_back(stem + L"-renamed.txt");
            pulse::app::rename::Source source; source.path = path; source.name = stem + L".txt";
            source.folder = L"PulseAnimationFixture"; options.proposed_sources.push_back(std::move(source));
        }
        options.proposed_names = names;
        options.proposed_plan = pulse::app::rename::RunNames(options.proposed_sources, names, {},
            [](const auto&, const auto&) { return false; });
        unsigned long long cpu = 0;
        options.on_review_ready = [&](HWND dialog, auto) {
            ready_at = GetTickCount64(); cpu = Cpu(); visible_at_ready = IsWindowVisible(dialog) && !IsIconic(dialog);
            SetWindowSubclass(dialog, Observe, 1, 0);
        };
        options.review_valid = [&] { ++polls; return !ready_at || GetTickCount64() - ready_at < 750; };
        SetTimer(owner, 0xB91, 5000, Timeout);
        const auto result = pulse::ui::ShowBatchRenameDialog(owner, paths, false, D2D1::ColorF(0.2f, 0.4f, 0.8f), options);
        KillTimer(owner, 0xB91);
        const auto elapsed = ready_at ? GetTickCount64() - ready_at : 0;
        const bool ok = !watchdog && visible_at_ready && ready_at && polls >= 2 && !result.accepted && IsWindowEnabled(owner);
        std::printf("[%s] native_review round=%d ready=%d paints=%u validity_polls=%u steady_ms=%llu cpu_ms=%.3f owner_enabled=%d watchdog=%d visible_at_ready=%d\n",
            ok ? "PASS" : "FAIL", round, ready_at != 0, paints, polls, elapsed, cpu ? (Cpu() - cpu) / 10000.0 : -1.0,
            IsWindowEnabled(owner), watchdog, visible_at_ready);
        if (!ok) ++failures;
    }
    DestroyWindow(owner); CoUninitialize(); SetThreadDesktop(original); CloseDesktop(isolated);
    return failures ? 1 : 0;
}
