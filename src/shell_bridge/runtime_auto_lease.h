#pragma once
#include <windows.h>
#include <string>

namespace pulse::shell_bridge::runtime {
inline constexpr wchar_t kAutoBindingFile[] = L"auto-binding.bin";
// Immutable protected session binding. Holding the process object prevents PID
// reuse from reviving a lease after its helper exits. Missing files are allowed
// only for the older explicit bootstrap; the automatic publisher must add one.
class AutoLease {
public:
    AutoLease() = default;
    AutoLease(const AutoLease&) = delete;
    AutoLease& operator=(const AutoLease&) = delete;
    ~AutoLease();
    DWORD Open(const std::wstring& control, const std::wstring& nonce, bool required = false) noexcept;
    bool Alive() const noexcept;
    bool Bound() const noexcept { return helper_ != nullptr; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE helper_ = nullptr;
};
}
