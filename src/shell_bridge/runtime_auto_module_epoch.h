#pragma once
#include <windows.h>
#include <intrin.h>
namespace pulse::shell_bridge::runtime {
// The loader callback only changes resident atomic data. Enumeration, allocation,
// module retention and IAT writes happen later on the initialization worker.
class AutoModuleEpoch {
    using Notify = void (NTAPI*)(ULONG, const void*, void*);
    using Register = LONG (NTAPI*)(ULONG, Notify, void*, void**);
    using Unregister = LONG (NTAPI*)(void*);
    static inline volatile LONG epoch_ = 1;
    LONG observed_ = 0;
    void* cookie_ = nullptr;
    Unregister unregister_ = nullptr;
    static void NTAPI Notification(ULONG, const void*, void*) noexcept {
        _InterlockedIncrement(&epoch_);
    }
public:
    ~AutoModuleEpoch() { Stop(); }
    bool Start() noexcept {
        const auto module = GetModuleHandleW(L"ntdll.dll");
        const auto start = reinterpret_cast<Register>(GetProcAddress(module, "LdrRegisterDllNotification"));
        unregister_ = reinterpret_cast<Unregister>(GetProcAddress(module, "LdrUnregisterDllNotification"));
        return start && unregister_ && start(0, Notification, nullptr, &cookie_) == 0;
    }
    bool Changed() noexcept {
        const LONG current = _InterlockedCompareExchange(&epoch_, 0, 0);
        if (current == observed_) return false;
        observed_ = current; return true;
    }
    void Stop() noexcept { if (cookie_) { unregister_(cookie_); cookie_ = nullptr; } }
};
}
