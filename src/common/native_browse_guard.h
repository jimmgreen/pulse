#pragma once
#include <windows.h>
#include <cwchar>

namespace pulse::native_browse {
inline thread_local unsigned depth = 0;
inline void Name(wchar_t (&name)[96]) noexcept {
    swprintf_s(name, L"Local\\Pulse.NativeBrowse.%lu.%lu", GetCurrentProcessId(), GetCurrentThreadId());
}
inline bool Active() noexcept {
    if (depth) return true;
    const DWORD error = GetLastError();
    wchar_t name[96]{}; Name(name);
    HANDLE guard = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (guard) CloseHandle(guard);
    SetLastError(error);
    return guard != nullptr;
}
// Core, AUTO and helper can independently link this header. The named object
// shares one thread's fallback scope across DLLs without affecting other calls.
class Scope {
public:
    Scope() noexcept {
        const DWORD error = GetLastError();
        if (!depth) {
            wchar_t name[96]{}; Name(name);
            guard_ = CreateEventW(nullptr, TRUE, FALSE, name);
        }
        ++depth;
        SetLastError(error);
    }
    ~Scope() {
        const DWORD error = GetLastError();
        --depth;
        if (guard_) CloseHandle(guard_);
        SetLastError(error);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    HANDLE guard_ = nullptr;
};
}
