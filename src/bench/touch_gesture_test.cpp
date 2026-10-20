#include "../app/touch_gesture.h"
#include <cstdio>
#include <cassert>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void TestTap() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    const auto act_down = reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    Check(act_down.kind == TouchActionKind::None, "Tap: Down produces no immediate action");
    Check(reducer.State() == TouchGestureState::Pending, "Tap: State is Pending after Down");

    const auto act_up = reducer.OnUp(1, 102.0f, 101.0f, 1080, 1.0f);
    Check(act_up.kind == TouchActionKind::Tap, "Tap: Clean release produces Tap");
    Check(reducer.State() == TouchGestureState::Ended, "Tap: State is Ended after Up");
}

void TestDoubleTap() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    // First tap
    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    const auto act1 = reducer.OnUp(1, 101.0f, 101.0f, 1060, 1.0f);
    Check(act1.kind == TouchActionKind::Tap, "DoubleTap: First release is Tap");

    // Second tap within 200ms and close distance
    reducer.OnDown(1, 104.0f, 102.0f, 1200, 1.0f);
    const auto act2 = reducer.OnUp(1, 103.0f, 102.0f, 1260, 1.0f);
    Check(act2.kind == TouchActionKind::DoubleTap, "DoubleTap: Second release within interval is DoubleTap");

    // Third tap after timeout (500ms later)
    reducer.OnDown(1, 100.0f, 100.0f, 2000, 1.0f);
    const auto act3 = reducer.OnUp(1, 100.0f, 100.0f, 2060, 1.0f);
    Check(act3.kind == TouchActionKind::Tap, "DoubleTap: Tap after interval reset is single Tap");
}

void TestHold() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);

    // Before timeout
    const auto act_early = reducer.CheckHold(1300);
    Check(act_early.kind == TouchActionKind::None, "Hold: Early check produces no action");

    // At/after 600ms timeout
    const auto act_hold = reducer.CheckHold(1650);
    Check(act_hold.kind == TouchActionKind::Hold, "Hold: 650ms check fires Hold action");
    Check(reducer.State() == TouchGestureState::HoldConsumed, "Hold: State is HoldConsumed");

    // Release after hold consumed
    const auto act_up = reducer.OnUp(1, 100.0f, 100.0f, 1800, 1.0f);
    Check(act_up.kind == TouchActionKind::None, "Hold: Release after HoldConsumed does NOT fire Tap");
}

void TestPanningAndAxisLock() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);

    // Small move within 8 DIP deadzone
    const auto act_deadzone = reducer.OnUpdate(1, 103.0f, 102.0f, 1020, 1.0f);
    Check(act_deadzone.kind == TouchActionKind::None, "Pan: Sub-deadzone movement produces no pan");

    // Crossing deadzone horizontally (dx=20, dy=2)
    const auto act_start = reducer.OnUpdate(1, 120.0f, 102.0f, 1050, 1.0f);
    Check(act_start.kind == TouchActionKind::PanStart, "Pan: Crossing deadzone fires PanStart");
    Check(act_start.primary_axis == 1, "Pan: Horizontal motion locks horizontal axis");
    Check(act_start.delta_x > 0.0f, "Pan: delta_x is positive");
    Check(act_start.delta_y == 0.0f, "Pan: delta_y is locked to zero on horizontal axis");

    // PanUpdate
    const auto act_update = reducer.OnUpdate(1, 140.0f, 105.0f, 1080, 1.0f);
    Check(act_update.kind == TouchActionKind::PanUpdate, "Pan: Continuing movement fires PanUpdate");

    // PanEnd with release velocity
    const auto act_end = reducer.OnUp(1, 160.0f, 105.0f, 1100, 1.0f);
    Check(act_end.kind == TouchActionKind::PanEnd, "Pan: Release fires PanEnd");
    Check(act_end.velocity_x > 0.0f, "Pan: Release calculates positive horizontal velocity");
    Check(act_end.velocity_y == 0.0f, "Pan: Vertical velocity is zero due to horizontal axis lock");
}

void TestMultiTouchCancellation() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    Check(reducer.State() == TouchGestureState::Pending, "MultiTouch: Finger 1 starts Pending");

    // Finger 2 arrives while finger 1 is pending
    const auto act_multi = reducer.OnDown(2, 250.0f, 250.0f, 1020, 1.0f);
    Check(act_multi.kind == TouchActionKind::Cancel, "MultiTouch: Finger 2 fires Cancel action");
    Check(reducer.State() == TouchGestureState::MultiTouchCancelled,
          "MultiTouch: State transitions to MultiTouchCancelled");

    // Updates and releases do not fire Tap or Pan
    const auto act_up1 = reducer.OnUp(1, 100.0f, 100.0f, 1080, 1.0f);
    Check(act_up1.kind == TouchActionKind::None, "MultiTouch: Finger 1 Up does not fire Tap");
}

void TestInertiaSimulation() {
    using namespace pulse::app;
    TouchInertiaTracker tracker(4.2f, 20.0f);

    tracker.Start(800.0f, 0.0f);
    Check(tracker.IsActive(), "Inertia: Tracker is active after Start");

    float total_dx = 0.0f;
    float total_dy = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    int steps = 0;
    while (tracker.Step(0.016f, dx, dy)) {
        total_dx += dx;
        total_dy += dy;
        ++steps;
        if (steps > 200) break; // Safeguard against infinite loop
    }

    Check(!tracker.IsActive(), "Inertia: Tracker naturally halts after friction decay");
    Check(total_dx > 100.0f, "Inertia: Total displacement is significant and positive");
    Check(total_dy == 0.0f, "Inertia: Zero initial vy produces zero vertical displacement");
    Check(steps > 10 && steps < 150, "Inertia: Step count is bounded within reasonable range");
}

} // namespace

int main() {
    TestTap();
    TestDoubleTap();
    TestHold();
    TestPanningAndAxisLock();
    TestMultiTouchCancellation();
    TestInertiaSimulation();

    std::printf("touch_gesture_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
