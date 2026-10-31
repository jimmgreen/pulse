#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace pulse::shell_bridge::runtime {
inline constexpr wchar_t kParentSessionFile[] = L"parent-session.bin";
enum class ChildSessionStage { None, Parent, Security, Directory, Environment, Identity, Binding, Complete };
struct ChildSessionDiagnostic {
    ChildSessionStage stage = ChildSessionStage::None;
    DWORD error = ERROR_SUCCESS;
    std::wstring retained_control; // Owned diagnostics are retained, never deleted here.
};

// No process creation, injection, resume, termination or deletion. Valid caller
// environment blocks are copied; only the three exact Pulse variables change.
// Explicit blocks preserve their encoding. NULL inherits Unicode without an ACP
// roundtrip; EnvironmentFlags then adds only CREATE_UNICODE_ENVIRONMENT.
class ChildSessionPreparation {
public:
    ChildSessionPreparation() noexcept;
    ~ChildSessionPreparation();
    ChildSessionPreparation(ChildSessionPreparation&&) noexcept;
    ChildSessionPreparation& operator=(ChildSessionPreparation&&) noexcept;
    ChildSessionPreparation(const ChildSessionPreparation&) = delete;
    ChildSessionPreparation& operator=(const ChildSessionPreparation&) = delete;
    LPVOID Environment() noexcept;
    DWORD EnvironmentFlags() const noexcept;
    const std::wstring& ControlPath() const noexcept;
    DWORD Bind(HANDLE newly_created_process, DWORD pid, ChildSessionDiagnostic& diagnostic) noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    friend DWORD PrepareChildSession(const std::wstring&, LPCVOID, DWORD,
                                    ChildSessionPreparation&, ChildSessionDiagnostic&) noexcept;
};
DWORD PrepareChildSession(const std::wstring& parent_control, LPCVOID original_environment,
                          DWORD creation_flags, ChildSessionPreparation& output,
                          ChildSessionDiagnostic& diagnostic) noexcept;
// Checks the current control and every journaled ancestor, including disable,
// exact live PID/time/image, protected directory and exact parent Session bytes.
// Depth is bounded to 16; malformed/missing child journals fail closed. A parent
// exit revokes descendants. Root directories have no parent journal.
// Early startup authorization additionally binds all environment selectors to
// this exact current process, not merely a live ancestor.
DWORD ValidateBoundChildRouting(const std::wstring& control, const std::wstring& host,
                                const std::wstring& nonce) noexcept;
// UI authorization requires positive initialization receipts from every
// ancestor. Creation may precede these receipts and uses ValidateParentChain.
DWORD ValidateReadyParentChain(const std::wstring& control) noexcept;
DWORD ValidateParentChain(const std::wstring& control) noexcept;
}
