#pragma once

namespace pulse::app {
struct WinEChord {
    enum class Action { Pass, Submit, Swallow };
    bool down = false;
    bool accepted = false;
    Action Press(bool eligible) noexcept {
        if (down) return accepted ? Action::Swallow : Action::Pass;
        down = true;
        accepted = false;
        return eligible ? Action::Submit : Action::Pass;
    }
    bool Release() noexcept {
        const bool swallow = down && accepted;
        down = accepted = false;
        return swallow;
    }
};
}
