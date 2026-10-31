#pragma once
#include <string>

namespace pulse {
struct AppState;
bool PrepareHotEntryProbe(AppState& state);

namespace detail {
class HotEntryDataDirectory {
    // Written once during validated startup, before data reads or workers.
    // No environment lookup or filesystem operation is performed by readers.
    static inline std::wstring directory_;
    friend bool ::pulse::PrepareHotEntryProbe(AppState& state);
public:
    static const std::wstring& Read() noexcept { return directory_; }
};
}
inline const std::wstring& VerifiedHotEntryDataDirectory() noexcept {
    return detail::HotEntryDataDirectory::Read();
}
}
