#include "../app/global_search_activation.h"
#include "../app/global_search_foreground_policy.h"
#include <iostream>
using pulse::app::SearchActivationState;
using pulse::app::SearchTrigger;
using pulse::app::SearchVisibility;
int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failures;
    };
    SearchActivationState state;
    check(!state.Open(), "starts Hidden");
    auto generation = state.Begin(SearchTrigger::DoubleCtrl);
    state.Deactivated();
    check(!state.Reconcile(generation, false, false, true) && state.Visibility() == SearchVisibility::Opening,
        "opening deactivation cannot hide inside activation stack");
    state.CompleteOpening(generation);
    check(!state.Reconcile(generation, false, false, false) && state.Visibility() == SearchVisibility::VisibleUnfocused,
        "denied activation retains clickable dismissible window");
    state.Activated();
    check(state.Visibility() == SearchVisibility::VisibleFocused, "later click promotes to focused");
    state.Deactivated();
    check(state.Reconcile(generation, false, false, true), "real foreground switch dismisses without time grace");
    state.Hide();
    check(!state.Open() && !state.Reconcile(generation, true, false, false), "hidden invalidates posted activation");
    const auto old_generation = generation;
    generation = state.Begin(SearchTrigger::Hotkey);
    state.CompleteOpening(generation);
    check(!state.Reconcile(old_generation, false, false, true) && state.Visibility() == SearchVisibility::Opening,
        "old deactivation cannot cancel fresh summon");
    check(!state.Reconcile(generation, true, false, false) && state.Visibility() == SearchVisibility::VisibleFocused,
        "hotkey direct focus completes opening");
    state.Deactivated();
    check(!state.Reconcile(generation, false, true, true), "same-process result menu retains search");
    state.Deactivated();
    check(state.Reconcile(generation, false, false, true), "external app switch from menu dismisses");
    generation = state.Begin(SearchTrigger::External);
    state.Activated(); state.Deactivated(); state.CompleteOpening(generation);
    check(state.Reconcile(generation, false, false, false), "gained then lost focus during opening dismisses");
    generation = state.Begin(SearchTrigger::DoubleCtrl);
    state.Deactivated(); state.CompleteOpening(generation);
    check(state.Reconcile(generation, false, false, true), "genuine switch during opening dismisses");
    generation = state.Begin(SearchTrigger::External);
    state.Hide(); state.CompleteOpening(generation);
    check(!state.Reconcile(generation, true, false, false) && !state.Open(), "explicit close during opening stays hidden");
    generation = state.Begin(SearchTrigger::Hotkey);
    state.CompleteOpening(generation);
    state.Reconcile(generation, true, false, false);
    state.Deactivated();
    check(!state.Reconcile(generation, false, true, true) && state.Open(),
        "owned popup focus retains active interaction");
    state.Deactivated();
    check(state.Reconcile(generation, false, false, true),
        "unrelated same-process window dismisses without app deactivation");
    check(pulse::app::ShouldPulseSearchForeground(true, false, true, false, true, true),
        "released double-Ctrl refusal permits one mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(false, false, true, false, true, true),
        "non-double-Ctrl trigger never injects a mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(true, true, true, false, true, true),
        "direct acceptance never injects a mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(true, false, false, false, true, true),
        "stale summon never injects a mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(true, false, true, true, true, true),
        "actual foreground already acquired never injects a mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(true, false, true, false, false, true),
        "foreground switch prevents mouse pulse");
    check(!pulse::app::ShouldPulseSearchForeground(true, false, true, false, true, false),
        "held input prevents mouse pulse");
    std::cout << "RESULT " << (failures ? "FAIL" : "PASS") << " activation-state failures=" << failures << '\n';
    return failures ? 1 : 0;
}
