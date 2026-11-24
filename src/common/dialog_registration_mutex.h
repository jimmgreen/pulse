#pragma once
#include <windows.h>

namespace pulse::dialog_auto {
inline HANDLE CreateRegistrationMutex(
    const wchar_t* name = L"Global\\Pulse.DialogAuto.Registration.v1") noexcept {
    // Waiting and releasing only require SYNCHRONIZE. CreateMutexW requests
    // ALL_ACCESS on existing objects, which can reject otherwise valid callers.
    return CreateMutexExW(nullptr, name, 0, SYNCHRONIZE);
}
}
