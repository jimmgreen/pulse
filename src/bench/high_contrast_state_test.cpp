#include "../ui/high_contrast_state.h"
#include <cstdio>
#include <thread>

namespace {
std::atomic<unsigned> calls{0};
std::atomic<bool> value{false}, fail{false}, slow{false}, entered{false};
bool Query(bool& out) noexcept {
    ++calls;
    if (slow.load()) { entered = true; Sleep(120); }
    if (fail.load()) return false;
    out = value.load();
    return true;
}
}
int main() {
    int failures = 0;
    const auto check = [&](bool pass, const char* label) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
        if (!pass) ++failures;
    };
    pulse::ui::HighContrastState state(Query);
    check(!state.Read() && calls == 1, "initial system snapshot");
    slow = true;
    const auto start = GetTickCount64();
    for (int frame = 0; frame < 72; ++frame)
        for (int row = 0; row < 100; ++row)
            for (int column = 0; column < 8; ++column) (void)state.Read();
    check(calls == 1 && GetTickCount64() - start < 120, "57600 draw reads never call the delayed system API");
    value = true;
    std::thread refresh([&] { state.Refresh(); });
    while (!entered.load()) SwitchToThread();
    const auto read_start = GetTickCount64();
    check(!state.Read() && GetTickCount64() - read_start < 60, "drawing reads old snapshot while refresh is blocked");
    refresh.join();
    check(state.Read() && calls == 2, "settings refresh enables high contrast");
    slow = false;
    fail = true;
    value = false;
    check(!state.Refresh() && state.Read(), "failed refresh preserves last valid accessibility state");
    fail = false;
    check(state.Refresh() && !state.Read(), "settings refresh disables high contrast");
    fail = true;
    pulse::ui::HighContrastState unavailable(Query);
    check(!unavailable.Read(), "initial failure uses existing false fallback");
    fail = false;
    value = true;
    check(unavailable.Refresh() && unavailable.Read(), "initial failure recovers on next notification");
    return failures ? 1 : 0;
}
