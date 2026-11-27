#include "../ui/tag_manager_panel.h"
#include "../ui/ui_compositor.h"
#include <algorithm>
#include <cstdio>

namespace {
using namespace pulse::ui;
struct Fixture {
    HWND owner = nullptr;
    HWND other_owner = nullptr;
    Compositor compositor;
    int clicks = 0, peak = 0, returns = 0, cancelled = 0;
    bool complete = false;
    bool independent = false, coexisted = false, other_returned = false;
    int phase = 0;
    DWORD started = 0;
};
Fixture* fixture = nullptr;
int CountPanels(bool close = false, HWND owner = nullptr) {
    struct Context { int count = 0; bool close = false; HWND owner = nullptr; } context{0, close, owner ? owner : fixture->owner};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
        auto& c = *reinterpret_cast<Context*>(data);
        wchar_t name[64]{};
        GetClassNameW(window, name, 64);
        if (wcscmp(name, L"PulseTagManager") == 0 && GetWindow(window, GW_OWNER) == c.owner) {
            ++c.count;
            if (c.close) PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    fixture->peak = (std::max)(fixture->peak, context.count);
    return context.count;
}
TagManagerAction OpenPanel(HWND owner = nullptr) {
    std::vector<TagManagerRow> rows{{L"fixture", L"Fixture tag", {}, {1, 0, 0, 1}, 1, 0, true, false}};
    TagManagerView view;
    TagManagerText text;
    text.title = L"Isolated tag manager reentry test";
    text.filter_all = L"All"; text.done = L"Done";
    return TagManagerPopup::Edit(owner ? owner : fixture->owner, &fixture->compositor, 1.0f,
        {180, 180}, false, {0, 0.4f, 0.8f, 1}, text, rows, view, 8);
}
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_APP + 1) {
        OpenPanel(fixture->other_owner);
        fixture->other_returned = true;
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        CountPanels();
        const auto result = OpenPanel();
        ++fixture->returns;
        if (result == TagManagerAction::Cancelled) ++fixture->cancelled;
        return 0;
    }
    if (message == WM_TIMER) {
        const int count = CountPanels();
        if (GetTickCount() - fixture->started > 12000) {
            std::puts("FAIL watchdog");
            ExitProcess(3);
        }
        if (fixture->independent) {
            const int other = CountPanels(false, fixture->other_owner);
            if (fixture->phase == 0) {
                fixture->phase = 1;
                PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
            } else if (fixture->phase == 1 && count == 1) {
                fixture->phase = 2;
                PostMessageW(window, WM_APP + 1, 0, 0);
            } else if (fixture->phase == 2 && other == 1) {
                fixture->coexisted = count == 1;
                fixture->phase = 3;
                CountPanels(true, fixture->other_owner);
            } else if (fixture->phase == 3 && fixture->other_returned && !other) {
                fixture->coexisted = fixture->coexisted && count == 1;
                fixture->phase = 4;
                CountPanels(true);
            } else if (fixture->phase == 4 && !count) {
                fixture->complete = true;
            }
            return 0;
        }
        if (fixture->clicks < 12) {
            ++fixture->clicks;
            PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
        } else if (count) {
            CountPanels(true);
        } else {
            fixture->complete = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    int failed = 0;
    {
        Fixture state; fixture = &state;
        WNDCLASSW wc{}; wc.lpfnWndProc = OwnerProc;
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseTagReentryFixture";
        if (!RegisterClassW(&wc)) return 2;
        state.owner = CreateWindowExW(0, wc.lpszClassName, L"Probe-owned owner",
            WS_OVERLAPPEDWINDOW, 80, 80, 500, 500, nullptr, nullptr, wc.hInstance, nullptr);
        if (!state.owner || !state.compositor.Init(state.owner)) return 2;
        for (int round = 0; round < 2; ++round) {
            state.clicks = state.peak = state.returns = state.cancelled = 0;
            state.complete = false; state.started = GetTickCount();
            SetTimer(state.owner, 1, 80, nullptr);
            MSG message{};
            while (!state.complete && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            KillTimer(state.owner, 1);
            const bool ok = state.clicks == 12 && state.returns == 12 && state.peak == 1 && CountPanels() == 0;
            std::printf("%s round=%d clicks=%d returns=%d peak_owned_panels=%d remaining=%d\n",
                ok ? "PASS" : "FAIL", round, state.clicks, state.returns, state.peak, CountPanels());
            if (!ok) ++failed;
        }
        state.other_owner = CreateWindowExW(0, L"STATIC", L"Second isolated owner",
            WS_OVERLAPPEDWINDOW, 100, 100, 500, 500, nullptr, nullptr, wc.hInstance, nullptr);
        state.independent = true; state.complete = false; state.started = GetTickCount();
        SetTimer(state.owner, 1, 80, nullptr);
        MSG message{};
        while (!state.complete && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        KillTimer(state.owner, 1);
        const bool independent = state.coexisted && state.other_returned &&
            CountPanels() == 0 && CountPanels(false, state.other_owner) == 0;
        std::printf("%s independent_owners=%d both_closed=%d\n", independent ? "PASS" : "FAIL",
            state.coexisted, CountPanels() == 0 && CountPanels(false, state.other_owner) == 0);
        if (!independent) ++failed;
        DestroyWindow(state.other_owner);
        DestroyWindow(state.owner);
    }
    CoUninitialize();
    std::printf("SUMMARY passed=%d failed=%d\n", 3 - failed, failed);
    return failed ? 1 : 0;
}
