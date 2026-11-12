#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace pulse::shell_bridge::application_identity {
inline uint64_t ImageHash(std::wstring_view value) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (auto c : value) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        hash ^= static_cast<uint64_t>(c); hash *= 1099511628211ull;
    }
    return value.empty() ? 0 : hash;
}
inline std::wstring ExecutableBasename(std::wstring_view image) {
    const auto slash = image.find_last_of(L"\\/");
    auto base = image.substr(slash == image.npos ? 0 : slash + 1);
    if (base.empty() || base.size() > 255 || base.find_first_of(L":\"<>|?*") != base.npos) return {};
    for (auto c : base) if (c < 32 || (c >= 127 && c <= 159) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) return {};
    return std::wstring(base);
}
inline uint64_t Filetime(const FILETIME& time) noexcept {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
struct ApplicationIdentity {
    std::wstring basename;
    std::string json_basename;
    uint64_t creation_time = 0, image_id = 0, instance_id = 0;
};
inline const ApplicationIdentity& CurrentApplication() {
    // Query only this already-running process, once per module. No directory scans.
    static const ApplicationIdentity identity = [] {
        ApplicationIdentity value;
        wchar_t image[32768]{};
        const DWORD size = GetModuleFileNameW(nullptr, image, 32768);
        if (size && size < 32768) {
            value.basename = ExecutableBasename(std::wstring_view(image, size));
            if (!value.basename.empty()) {
                value.image_id = ImageHash(value.basename);
                // Path fingerprint is opaque, never exported as the source path.
                value.instance_id = ImageHash(std::wstring_view(image, size));
                for (auto c : value.basename) {
                    char escaped[7]{};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    value.json_basename += escaped;
                }
            }
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) value.creation_time = Filetime(created);
        return value;
    }();
    return identity;
}
}
