#pragma once
#include <cstdint>
namespace pulse::app {
// Reconciliation can transfer a pending lease once, never an already rendered
// listing or a user/real-change refresh. The source deadline remains untouched.
inline bool CanTransferExplorerInitialArm(bool arm_only, bool pending, bool listed, bool transferred,
    bool same_path, bool same_generation, bool same_selection, uint64_t now, uint64_t deadline) {
    return arm_only && pending && !listed && !transferred && same_path && same_generation && same_selection && now < deadline;
}
inline bool ExplorerArmOnlyRefresh(bool arm_missed, bool dirty) { return arm_missed && !dirty; }
}
