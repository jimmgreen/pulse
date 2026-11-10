// Only the deferred-history wrapper test links this storage implementation.
// Native COM, wrapper routing and provider configuration remain real; registry
// persistence is covered separately by file_dialog_client_state_test.
#include "../shell_bridge/file_dialog_client_state.h"
#include <map>
#include <mutex>

namespace pulse::shell_bridge {
namespace {
std::mutex mutex;
std::map<std::wstring, std::wstring> directories;
}
FileDialogClientState::FileDialogClientState() : FileDialogClientState(L"default", L"memory") {}
FileDialogClientState::FileDialogClientState(std::wstring app_identity, std::wstring registry_root)
    : app_key_(std::move(registry_root) + L"/" + std::move(app_identity)) {}
std::wstring FileDialogClientState::Key(REFGUID guid) const {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
    std::wstring key = app_key_ + L"/";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (size_t i = 0; i < sizeof(guid); ++i) {
        key += hex[bytes[i] >> 4]; key += hex[bytes[i] & 15];
    }
    return key;
}
std::optional<std::wstring> FileDialogClientState::Read(REFGUID guid) const noexcept try {
    std::lock_guard lock(mutex);
    const auto found = directories.find(Key(guid));
    return found == directories.end() ? std::nullopt : std::optional<std::wstring>(found->second);
} catch (...) { return std::nullopt; }
bool FileDialogClientState::Write(REFGUID guid, const std::wstring& directory) const noexcept try {
    std::lock_guard lock(mutex);
    directories[Key(guid)] = directory;
    return true;
} catch (...) { return false; }
bool FileDialogClientState::Clear(REFGUID guid) const noexcept try {
    std::lock_guard lock(mutex);
    directories.erase(Key(guid));
    return true;
} catch (...) { return false; }
}
