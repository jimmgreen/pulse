#pragma once
#include <cstdint>
namespace pulse::app {
constexpr uint64_t OpenUiWaitMs = 30000;
enum class OpenWaitAction { Execute, Defer, Cancel };
inline OpenWaitAction AdvanceOpenWait(uint64_t& deadline, uint64_t now, bool busy) {
    if (deadline && now >= deadline) return OpenWaitAction::Cancel;
    if (!busy) return OpenWaitAction::Execute;
    if (!deadline) deadline = now + OpenUiWaitMs;
    return OpenWaitAction::Defer;
}
}
