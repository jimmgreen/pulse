#pragma once

#include "../ipc/picker_legacy_model.h"
#include <functional>

namespace pulse::shell_bridge {
struct LegacyAnsiDialogServices {
    // These three original entry points must remain non-null.
    decltype(&GetOpenFileNameA) open_native = &GetOpenFileNameA;
    decltype(&GetSaveFileNameA) save_native = &GetSaveFileNameA;
    decltype(&CommDlgExtendedError) native_error = &CommDlgExtendedError;
    // Absent by default. Return the ABI obligations actually verified for this
    // exact request; Prepared::Ready is not proof of native compatibility.
    std::function<uint32_t(const ipc::picker::legacy::Prepared&)> verified_abi_checks;
    std::wstring executable;
    std::function<ipc::picker::Message(const ipc::picker::Message&, HWND)> run_bridge;
};

// Explicit ACP adapter only: no interception or registration. Unsupported or
// lossy input bypasses before dispatch; unrepresentable results fail atomically.
// Caller pointers must refer to valid readable/writable memory with the stated
// capacity. Bounded copies do not validate pointers or concurrent mutation.
class LegacyAnsiFileDialogAdapter {
public:
    explicit LegacyAnsiFileDialogAdapter(LegacyAnsiDialogServices services = {});
    BOOL Open(OPENFILENAMEA* input) const;
    BOOL Save(OPENFILENAMEA* input) const;
    DWORD ExtendedError() const;
    // Must be called before a common-dialog call outside this adapter (including
    // font/color dialogs). TLS cannot observe those calls automatically. Error
    // state follows the last A/W adapter operation on this thread, across instances.
    static void UseNativeError() noexcept;
private:
    BOOL Invoke(OPENFILENAMEA* input, bool save) const;
    LegacyAnsiDialogServices services_;
};
}
