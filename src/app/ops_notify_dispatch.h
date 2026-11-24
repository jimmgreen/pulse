#pragma once
#include <windows.h>
#include <memory>
#include "app_messages.h"
#include "ops_notify_gate.h"

namespace pulse::app {
inline constexpr UINT_PTR kOpsNotifyTimer = 0x4F504E46; // OPNF

// All accesses to due are on the process UI thread. A posted frame/UI tick
// can service the same deadline when lower-priority WM_TIMER is starved.
inline void ResumeOpsNotify(OpsNotifyGate& gate, HWND service, ULONGLONG& due) {
    if (due == 0 || GetTickCount64() < due) return;
    KillTimer(service, kOpsNotifyTimer);
    due = 0;
    gate.Resume([service] { return PostMessageW(service, WM_OPS_NOTIFY, 0, 0) != FALSE; });
}

// The process host owns the cooldown, not the active window: changing or
// closing that window cannot strand a shared operation's final notification.
class OpsNotifyDispatch {
public:
    OpsNotifyDispatch(std::shared_ptr<OpsNotifyGate> gate, HWND service, ULONGLONG& due)
        : gate_(std::move(gate)), service_(service), due_(due) {}
    ~OpsNotifyDispatch() {
        gate_->EndDispatch([this] {
            due_ = GetTickCount64() + 33;
            if (SetTimer(service_, kOpsNotifyTimer, 33, nullptr) != 0) return true;
            due_ = 0;
            return false;
        });
    }
    OpsNotifyDispatch(const OpsNotifyDispatch&) = delete;
    OpsNotifyDispatch& operator=(const OpsNotifyDispatch&) = delete;
private:
    std::shared_ptr<OpsNotifyGate> gate_;
    HWND service_;
    ULONGLONG& due_;
};
}
