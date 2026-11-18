#pragma once
#include <windows.h>
#include <memory>
#include "single_instance_coordinator.h"

namespace pulse::app {
// Only a fixed Win+E intent crosses this privilege boundary. The client never
// sends filenames or executable commands to the elevated process.
class WinEBrokerClient {
public:
    explicit WinEBrokerClient(HWND owner);
#if defined(PULSE_WIN_E_BROKER_TEST)
    WinEBrokerClient(HANDLE child, HANDLE mapping);
#endif
    ~WinEBrokerClient();
    void Poll(SingleInstanceCoordinator&, HWND host, UINT wake, bool can_activate);
    bool Active() const noexcept;
    unsigned HeldFlags() const noexcept;
private:
    struct Session;
    std::shared_ptr<Session> session_;
    std::shared_ptr<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>> delivery_;
    uint64_t request_high_ = 0, request_low_ = 0, sequence_ = 0;
};
}
