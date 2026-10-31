#pragma once
#include <windows.h>
#include <optional>
#include <string>

namespace pulse::shell_bridge {
// Pulse-owned directory memory; Windows private dialog history is untouched.
class FileDialogClientState {
public:
    FileDialogClientState();
    FileDialogClientState(std::wstring app_identity, std::wstring registry_root);
    std::optional<std::wstring> Read(REFGUID guid) const noexcept;
    bool Write(REFGUID guid, const std::wstring& directory) const noexcept;
    bool Clear(REFGUID guid) const noexcept;
private:
    std::wstring Key(REFGUID guid) const;
    std::wstring app_key_;
};
}

