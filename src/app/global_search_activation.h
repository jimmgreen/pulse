#pragma once
#include <cstdint>
#include "global_search_trigger.h"

namespace pulse::app {

// UI-thread state. Posted reconciliation carries a generation, not a time-based
// grace period: old deactivation cannot dismiss a newly summoned window.
class SearchActivationState {
public:
    SearchVisibility Visibility() const { return visibility_; }
    SearchTrigger Trigger() const { return trigger_; }
    uintptr_t Generation() const { return generation_; }
    bool Open() const { return visibility_ != SearchVisibility::Hidden; }
    uintptr_t Begin(SearchTrigger trigger) {
        ++generation_;
        trigger_ = trigger;
        visibility_ = SearchVisibility::Opening;
        opening_complete_ = false;
        focused_during_open_ = false;
        deactivated_ = false;
        return generation_;
    }
    void Hide() {
        ++generation_;
        visibility_ = SearchVisibility::Hidden;
        deactivated_ = false;
    }
    void Activated() {
        if (visibility_ == SearchVisibility::Opening) focused_during_open_ = true;
        else if (Open()) visibility_ = SearchVisibility::VisibleFocused;
    }
    void Deactivated() { if (Open()) deactivated_ = true; }
    void CompleteOpening(uintptr_t generation) {
        if (generation == generation_ && visibility_ == SearchVisibility::Opening) opening_complete_ = true;
    }
    // Returns true only when the owner must dismiss. A rejected first activation
    // stays clickable; actual switching after focus is never hidden by a timer.
    bool Reconcile(uintptr_t generation, bool focused, bool owned_or_menu_active, bool foreground_changed) {
        if (generation != generation_ || !Open()) return false;
        if (visibility_ == SearchVisibility::Opening && !opening_complete_) return false;
        if (focused || owned_or_menu_active) {
            visibility_ = SearchVisibility::VisibleFocused;
            deactivated_ = false;
            return false;
        }
        if (visibility_ == SearchVisibility::Opening) {
            const bool dismiss = focused_during_open_ || (deactivated_ && foreground_changed);
            visibility_ = SearchVisibility::VisibleUnfocused;
            deactivated_ = false;
            return dismiss;
        }
        return deactivated_;
    }
private:
    SearchVisibility visibility_ = SearchVisibility::Hidden;
    SearchTrigger trigger_ = SearchTrigger::Hotkey;
    uintptr_t generation_ = 0;
    bool opening_complete_ = false;
    bool focused_during_open_ = false;
    bool deactivated_ = false;
};
}
