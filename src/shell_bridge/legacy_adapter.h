#pragma once

#include "../ipc/picker_legacy_model.h"
#include <functional>

namespace pulse::shell_bridge {
struct LegacyDialogServices {
    // These three original entry points must remain non-null.
    decltype(&GetOpenFileNameW) open_native = &GetOpenFileNameW;
    decltype(&GetSaveFileNameW) save_native = &GetSaveFileNameW;
    decltype(&CommDlgExtendedError) native_error = &CommDlgExtendedError;
    // Absent by default. Return the ABI obligations actually verified for this
    // exact request; Prepared::Ready is not proof of native compatibility.
    std::function<uint32_t(const ipc::picker::legacy::Prepared&)> verified_abi_checks;
    std::wstring executable;
    std::function<ipc::picker::Message(const ipc::picker::Message&, HWND)> run_bridge;
};

// Explicit Unicode adapter only: no interception, registration or ANSI support.
// Caller pointers must refer to valid readable/writable memory with the stated
// capacity. Bounded copies do not validate pointers or concurrent mutation.
class LegacyFileDialogAdapter {
public:
    explicit LegacyFileDialogAdapter(LegacyDialogServices services = {});
    BOOL Open(OPENFILENAMEW* input) const;
    BOOL Save(OPENFILENAMEW* input) const;
    DWORD ExtendedError() const;
    // Must be called before a common-dialog call outside this adapter (including
    // font/color dialogs). TLS cannot observe those calls automatically. Error
    // state follows the last A/W adapter operation on this thread, across instances.
    static void UseNativeError() noexcept;
private:
    BOOL Invoke(OPENFILENAMEW* input, bool save) const;
    LegacyDialogServices services_;
};
}
