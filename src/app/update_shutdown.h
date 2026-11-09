#pragma once
#include "../ops/ops_manager.h"

namespace pulse::app {

inline UINT UpdateShutdownMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.PrepareUpdateShutdown.v1");
    return message;
}

// Registered-message results: zero also covers older versions without the protocol.
inline constexpr LRESULT kUpdateShutdownAccepted = 1;
inline constexpr LRESULT kUpdateShutdownBusy = 2;
inline constexpr LRESULT kUpdateShutdownSaveFailed = 3;
inline constexpr LRESULT kUpdateShutdownCloseFailed = 3;

// The installer must repeat the shutdown handshake before replacing files.
// This launch gate only prevents starting it while the initiating window is busy.
inline bool RequestUpdateLaunch(ops::OpsManager& operations, bool migration_pending,
                                const std::function<void()>& launch) {
    if (migration_pending || !operations.TryPrepareForUpdate()) return false;
    launch();
    operations.CancelUpdatePreparation();
    return true;
}

inline LRESULT RequestUpdateShutdown(ops::OpsManager& operations, bool migration_pending,
                                    const std::function<bool()>& close_window) {
    // All windows share the operations manager. Keep admission closed while
    // the installer repeats this handshake for each remaining (even hidden) window.
    if (migration_pending || !operations.TryPrepareForUpdate(true)) return kUpdateShutdownBusy;
    if (close_window()) return kUpdateShutdownAccepted;
    operations.CancelUpdatePreparation();
    return kUpdateShutdownCloseFailed;
}

}
