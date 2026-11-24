#pragma once

#include <cstdint>
#include <string_view>

namespace pulse::path {

// UniqueTemporaryPath in ops_manager.cpp appends -PID-task-index[-suffix].
// Only data being written is scratch; replacement/recovery backups are not.
inline uint32_t TransferScratchOwner(std::wstring_view path) noexcept {
    const auto separator = path.find_last_of(L"\\/");
    const auto name = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
    constexpr std::wstring_view copy = L".pulse-copy-";
    constexpr std::wstring_view verify = L".pulse-verify-";
    auto marker = name.rfind(copy);
    auto marker_size = copy.size();
    if (marker == std::wstring_view::npos) {
        marker = name.rfind(verify);
        marker_size = verify.size();
    }
    if (!marker || marker == std::wstring_view::npos) return 0;
    const auto suffix = name.substr(marker + marker_size);
    uint64_t owner = 0;
    unsigned fields = 1;
    unsigned digits = 0;
    for (const wchar_t ch : suffix) {
        if (ch == L'-') {
            if (!digits || ++fields > 4) return 0;
            digits = 0;
        } else {
            if (ch < L'0' || ch > L'9' || ++digits > 20) return 0;
            if (fields == 1) {
                owner = owner * 10 + static_cast<unsigned>(ch - L'0');
                if (owner > UINT32_MAX) return 0;
            }
        }
    }
    return digits && fields >= 3 ? static_cast<uint32_t>(owner) : 0;
}

} // namespace pulse::path
