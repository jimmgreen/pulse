#pragma once
#include <windows.h>
#include <atomic>
#include <vector>
#include "single_instance_coordinator.h"
#include "win_e_input.h"

namespace pulse::app {
class WinETrace;
// One instance belongs to AppCore; its hook runs on a dedicated message thread.
class WinEHotkey {
public:
    WinEHotkey(SingleInstanceCoordinator& coordinator, HWND host, UINT wake);
    ~WinEHotkey();
    WinEHotkey(const WinEHotkey&) = delete;
    WinEHotkey& operator=(const WinEHotkey&) = delete;
    void Stop();
    bool Active() const noexcept { return active_.load(); }
private:
    static DWORD WINAPI Thread(void* value);
    static LRESULT CALLBACK Keyboard(int code, WPARAM message, LPARAM data);
    void Prepare() noexcept;
    void FinishChord() noexcept;
    void RefreshHook(HWND foreground, unsigned reason) noexcept;
    static void CALLBACK Foreground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    SingleInstanceCoordinator& coordinator_;
    HWND host_ = nullptr;
    UINT wake_ = 0;
    HANDLE thread_ = nullptr;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> stopping_{false}, active_{false};
    HHOOK hook_ = nullptr;
    SingleInstanceCoordinator::OpenRequest request_;
    bool prepared_ = false;
    std::shared_ptr<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>> prepared_delivery_;
    struct Delivery {
        std::array<unsigned char, 16> id;
        std::shared_ptr<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>> state;
    };
    std::vector<Delivery> deliveries_;
    WinEInputState input_;
    WinETrace* trace_ = nullptr;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    HWND last_foreground_ = nullptr;
    uint64_t refresh_tick_ = 0, cycle_ = 0;
    unsigned hook_generation_ = 0;
    bool trace_chord_ = false, chord_pending_ = false, pending_accepted_ = false;
    inline static thread_local WinEHotkey* current_ = nullptr;
};
}
