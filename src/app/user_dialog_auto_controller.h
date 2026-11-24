#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>

namespace pulse::app {

// Owns the background non-elevated adaptive dialog attach worker inside pulse.exe.
// Starts only when both dialog takeover and adaptive legacy compatibility are enabled.
class UserDialogAutoController {
public:
    UserDialogAutoController() = default;
    ~UserDialogAutoController();
    UserDialogAutoController(const UserDialogAutoController&) = delete;
    UserDialogAutoController& operator=(const UserDialogAutoController&) = delete;

    // Also starts, once per process, the background rebind of a per-user dialog
    // registration left on an older installed version (dialog_auto_repair.h).
    void Update(bool enabled, const std::wstring& data_dir);
    void Stop();
    bool Running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void StartRepair(const std::wstring& install_root);

    HANDLE stop_event_ = nullptr;
    std::thread worker_;
    // Signaled when the rebind finished; the adaptive worker waits for it so its
    // registration check sees the current bundle.
    HANDLE repair_done_ = nullptr;
    std::thread repair_;
    std::atomic<bool> running_{false};
    std::wstring current_root_;
    std::wstring current_data_dir_;
};

} // namespace pulse::app
