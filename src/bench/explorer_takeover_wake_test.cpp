#include "explorer_handoff.h"
#include "shell_window_plan.h"

#include <cstdio>
#include <functional>
#include <thread>
#include <type_traits>

using namespace pulse::app;

namespace {
bool Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    return value;
}

template<class Handoff>
bool TestWake() {
    if constexpr (!std::is_constructible_v<Handoff, uint64_t, std::function<void()>>) {
        return Check(false, "Ready transition notifies worker without waiting for poll timer");
    } else {
        unsigned wakes = 0;
        auto lifetime = std::make_shared<unsigned>(0);
        std::weak_ptr<unsigned> weak = lifetime;
        auto wake = [weak, &wakes] { if (auto active = weak.lock()) { ++*active; ++wakes; } };
        Handoff ready(1000, wake);
        bool ok = Check(ready.Ready(10) && wakes == 1 && ready.state == HandoffState::Ready,
                        "Ready transition notifies worker without waiting for poll timer");
        ok &= Check(!ready.Ready(11) && wakes == 1, "duplicate Ready does not notify twice");
        ok &= Check(!ready.ConfirmClose(true) && ready.state == HandoffState::Ready,
                    "notification never bypasses close commitment");
        ok &= Check(ready.ClaimClose(12) && !ready.ConfirmClose(false) &&
                    ready.state == HandoffState::Closing && ready.ConfirmClose(true),
                    "close still requires observed source destruction");
        Handoff cancelled(1000, wake);
        cancelled.Cancel();
        Handoff expired(1000, wake);
        ok &= Check(!cancelled.Ready(20) && !expired.Ready(1000) && wakes == 1,
                    "cancelled and expired requests never wake close processing");
        Handoff stopped(1000, wake);
        lifetime.reset();
        ok &= Check(stopped.Ready(20) && wakes == 1,
                    "expired worker lifetime makes pending notification inert");
        std::atomic<unsigned> raced_wakes{0};
        Handoff raced(1000, [&] { ++raced_wakes; });
        std::thread first([&] { raced.Ready(10); });
        std::thread second([&] { raced.Ready(10); });
        first.join(); second.join();
        ok &= Check(raced_wakes == 1, "concurrent Ready transitions notify exactly once");
        return ok;
    }
}
}

int main() {
    bool ok = TestWake<ExplorerHandoff>();
    ExplorerSelectionStability stability;
    ExplorerWindowProbe probe;
    probe.view_ready = probe.supported = probe.selection_read = true;
    probe.age_ms = 20;
    probe.stable_age_ms = stability.Observe(20, true, L"C:\\fixture", {});
    ok &= Check(DecideExplorerTakeover(probe) == ExplorerTakeoverStep::Wait,
                "immediate first observation does not skip selection grace");
    probe.age_ms = 119;
    probe.stable_age_ms = stability.Observe(119, true, L"C:\\fixture", {});
    ok &= Check(DecideExplorerTakeover(probe) == ExplorerTakeoverStep::Wait,
                "early wake preserves full 100ms stable observation");
    probe.age_ms = 120;
    probe.stable_age_ms = stability.Observe(120, true, L"C:\\fixture", {});
    ok &= Check(DecideExplorerTakeover(probe) == ExplorerTakeoverStep::Take,
                "stable observation permits submission at unchanged deadline");
    SourceCloseProbe close{true, false, false, true, 1000};
    ok &= Check(DecideSourceClose(close) == SourceCloseStep::Abort,
                "multiple source tabs still prevent close");
    close.single_tab = true; close.selection_changed = true;
    ok &= Check(DecideSourceClose(close) == SourceCloseStep::SelectAndKeep,
                "late selection still requires a fresh acknowledgement");
    return ok ? 0 : 1;
}
