// window_registry_test.cpp - Multi-window M3a: the per-process window registry
// keeps most-recently-activated order, survives unknown and repeated calls,
// and empties when the last window leaves. M3b: a shared result reaches every
// window of the core, or only the caller when it is not registered. M3f:
// closing windows stops the process services exactly once, with the last one.
#include "../app/window_registry.h"

#include <cstdio>
#include <vector>

namespace {
struct FakeWindow { int id = 0; };
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
struct FakeCore;
struct FakeAppWindow {
    HWND hwnd = nullptr;
    FakeCore& core;
    int id = 0;
};
struct FakeCore { pulse::app::BasicWindowRegistry<FakeAppWindow> windows; };
std::vector<int> Visited(FakeAppWindow& from) {
    std::vector<int> ids;
    pulse::app::ForEachWindowSharingCore(from, [&](FakeAppWindow& w) { ids.push_back(w.id); });
    return ids;
}
HWND H(int n) { return reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1000 + n)); }
std::vector<int> Order(const pulse::app::BasicWindowRegistry<FakeWindow>& r) {
    std::vector<int> ids;
    r.ForEach([&](HWND, FakeWindow& w) { ids.push_back(w.id); });
    return ids;
}
}  // namespace

int main() {
    pulse::app::BasicWindowRegistry<FakeWindow> registry;
    FakeWindow a{1}, b{2}, c{3};
    Check(registry.Empty() && !registry.MostRecent() && !registry.MostRecentHwnd(), "a new registry is empty");

    registry.Add(H(1), &a);
    Check(registry.Count() == 1 && registry.MostRecent() == &a && registry.MostRecentHwnd() == H(1),
          "the first window is the most recent one");
    registry.Add(H(2), &b);
    registry.Add(H(3), &c);
    Check((Order(registry) == std::vector<int>{3, 2, 1}), "new windows are most recent first");

    registry.Touch(H(1));
    Check((Order(registry) == std::vector<int>{1, 3, 2}), "activation moves a window to the front");
    registry.Touch(H(1));
    Check((Order(registry) == std::vector<int>{1, 3, 2}), "activating the front window changes nothing");
    registry.Touch(H(99));
    Check((Order(registry) == std::vector<int>{1, 3, 2}) && registry.Count() == 3,
          "activating an unregistered window is ignored");

    registry.Add(H(2), &b);
    Check((Order(registry) == std::vector<int>{2, 1, 3}) && registry.Count() == 3,
          "registering again does not duplicate and makes it most recent");

    Check(registry.Lookup(H(3)) == &c && registry.Lookup(H(42)) == nullptr, "lookup by window handle");
    Check(!registry.Remove(H(42)), "removing an unknown window reports false");

    Check(registry.Remove(H(2)) && registry.MostRecent() == &a,
          "closing the most recent window hands routing to the next one");
    registry.Add(nullptr, &b);
    registry.Add(H(5), nullptr);
    Check(registry.Count() == 2, "null handles or states are not registered");

    Check(registry.Remove(H(1)) && registry.Remove(H(3)) && registry.Empty() && !registry.MostRecent(),
          "the registry is empty after the last window leaves");

    FakeCore core;
    FakeAppWindow w1{H(11), core, 1}, w2{H(12), core, 2}, loose{nullptr, core, 9};
    Check((Visited(w1) == std::vector<int>{1}), "an unregistered window gets the result alone");
    core.windows.Add(w1.hwnd, &w1);
    core.windows.Add(w2.hwnd, &w2);
    Check((Visited(w1) == std::vector<int>{2, 1}) && (Visited(w2) == std::vector<int>{2, 1}),
          "a registered window shares the result with every window, most recent first");
    Check((Visited(loose) == std::vector<int>{9}), "a window without a handle (tests, probes) stays alone");
    FakeAppWindow impostor{H(12), core, 7};
    Check((Visited(impostor) == std::vector<int>{7}), "a handle registered to another state does not fan out");
    core.windows.Remove(w2.hwnd);
    Check((Visited(w1) == std::vector<int>{1}), "a closed window no longer receives results");

    // M3f window lifecycle. Mirrors WM_DESTROY: IsLastWindow is read before
    // the window leaves the registry; only the last one tears the core down.
    pulse::app::BasicWindowRegistry<FakeWindow> life;
    Check(life.IsLastWindow(H(1)) && life.IsLastWindow(nullptr), "an unregistered window is the last one");
    life.Add(H(1), &a);
    Check(life.IsLastWindow(H(1)), "the only window is the last one");
    Check(!life.IsLastWindow(H(2)), "a window outside a non-empty registry is not alone");
    life.Add(H(2), &b);
    life.Add(H(3), &c);
    Check(!life.IsLastWindow(H(1)) && !life.IsLastWindow(H(3)), "with other windows open no window is the last");
    Check(life.ExitOrder().empty() == false && life.ExitOrder().back() == H(3) &&
              (life.ExitOrder() == std::vector<HWND>{H(1), H(2), H(3)}),
          "exit closes least recent first and the most recent window last");
    life.Touch(H(1));
    Check((life.ExitOrder() == std::vector<HWND>{H(2), H(3), H(1)}), "exit order follows activation");
    // Exit: destroy in ExitOrder; count core teardowns.
    int teardowns = 0;
    HWND torn_down_by = nullptr;
    for (HWND h : life.ExitOrder()) {
        if (life.IsLastWindow(h)) { ++teardowns; torn_down_by = h; }
        life.Remove(h);
    }
    Check(teardowns == 1 && torn_down_by == H(1) && life.Empty(),
          "a full exit stops the services once, in the most recent window");
    // Closing windows one by one: services survive until the last one closes.
    life.Add(H(1), &a);
    life.Add(H(2), &b);
    const bool first_last = life.IsLastWindow(H(2));
    life.Remove(H(2));
    Check(!first_last && life.MostRecent() == &a, "closing one of two windows keeps the services running");
    Check(life.IsLastWindow(H(1)), "the remaining window becomes the last one");
    Check(life.ExitOrder() == std::vector<HWND>{H(1)} && pulse::app::BasicWindowRegistry<FakeWindow>{}.ExitOrder().empty(),
          "exit order of one window, and of none");

    std::printf("window_registry_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
