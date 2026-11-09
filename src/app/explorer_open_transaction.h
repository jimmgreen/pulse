#pragma once

#include "explorer_handoff.h"

namespace pulse::app {

// Kept on the newly allocated layout, independently of navigation leases:
// a failed listing or navigation reset must not lose the rollback owner.
struct ExplorerOpenTransaction {
    std::shared_ptr<ExplorerHandoff> handoff;
    std::wstring path;
    bool user_interacted = false;
    const void* previous_layout = nullptr;

    ~ExplorerOpenTransaction() { if (handoff) handoff->Cancel(); }
};

} // namespace pulse::app
