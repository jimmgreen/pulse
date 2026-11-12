#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace pulse::extensions {
// Empty policy allows unsigned development packages. A supplied signature always
// requires an explicitly pinned publisher; signature identity grants no capability.
bool VerifyPackageSignature(std::string_view manifest, std::string_view signature,
    std::string_view policy, std::string& error);

// Verifies an explicit publisher key-rotation statement signed by old_public_key over
// "Pulse.Extension.Publisher.Rotation\0v1\0" + id + "\n" + old_publisher_id + "\n" + new_publisher_id.
// Both old and new keys must be valid 2048-4096 bit CNG RSA public keys matching their SHA-256 pins.
bool VerifyPublisherRotation(std::string_view id,
    std::string_view old_publisher_id, std::string_view old_public_key,
    std::string_view new_publisher_id, std::string_view new_public_key,
    std::string_view rotation, std::string& error);
bool VerifyPublisherRotationPolicy(std::string_view id,
    std::string_view old_publisher_id, std::string_view new_publisher_id,
    std::string_view rotation, std::string_view policy, std::string& error);

namespace publish_trust_detail {
inline bool ValidPackageId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
inline bool ValidPublisherPin(std::string_view pin) noexcept {
    return pin.size() == 64 && std::all_of(pin.begin(), pin.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline std::wstring AsciiWide(std::string_view ascii) {
    std::wstring out;
    out.reserve(ascii.size());
    for (unsigned char c : ascii) out.push_back(static_cast<wchar_t>(c));
    return out;
}
inline bool ValidWidePublisherPin(std::wstring_view pin) noexcept {
    return pin.size() == 64 && std::all_of(pin.begin(), pin.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
} // namespace publish_trust_detail

// Formats persistent "always allow publish" key bound to (publisher, id):
// L"<publisher_sha256>:<package_id>" for signed packages, L"unsigned:<package_id>" for unsigned dev packages.
inline std::wstring FormatPublishTrustKey(std::string_view publisher_id, std::string_view package_id) {
    if (!publish_trust_detail::ValidPackageId(package_id)) return {};
    if (publisher_id.empty()) return L"unsigned:" + publish_trust_detail::AsciiWide(package_id);
    if (!publish_trust_detail::ValidPublisherPin(publisher_id)) return {};
    return publish_trust_detail::AsciiWide(publisher_id) + L":" + publish_trust_detail::AsciiWide(package_id);
}
// Legacy bare "<package_id>" entries never match signed or unsigned packages.
inline bool IsPublishTrusted(const std::vector<std::wstring>& trusted,
    std::string_view publisher_id, std::string_view package_id) {
    const auto key = FormatPublishTrustKey(publisher_id, package_id);
    return !key.empty() && std::find(trusted.begin(), trusted.end(), key) != trusted.end();
}
inline size_t RevokePublishTrustForOrigin(std::vector<std::wstring>& trusted,
    std::string_view publisher_id, std::string_view package_id) {
    if (!publish_trust_detail::ValidPackageId(package_id)) return 0;
    const auto key = FormatPublishTrustKey(publisher_id, package_id);
    if (!publisher_id.empty() && key.empty()) return 0;
    const auto legacy = publish_trust_detail::AsciiWide(package_id);
    return std::erase_if(trusted, [&](const std::wstring& item) {
        return item == key || item == legacy;
    });
}
inline size_t RevokePublishTrustForPackage(std::vector<std::wstring>& trusted, std::string_view package_id) {
    if (!publish_trust_detail::ValidPackageId(package_id)) return 0;
    const auto wide_id = publish_trust_detail::AsciiWide(package_id);
    const auto unsigned_key = L"unsigned:" + wide_id;
    return std::erase_if(trusted, [&](const std::wstring& item) {
        if (item == wide_id || item == unsigned_key) return true;
        if (item.size() == 65 + wide_id.size() && item[64] == L':' &&
            std::wstring_view(item).substr(65) == wide_id &&
            publish_trust_detail::ValidWidePublisherPin(std::wstring_view(item).substr(0, 64))) {
            return true;
        }
        return false;
    });
}
inline bool GrantPublishTrust(std::vector<std::wstring>& trusted,
    std::string_view publisher_id, std::string_view package_id) {
    const auto key = FormatPublishTrustKey(publisher_id, package_id);
    if (key.empty()) return false;
    const auto legacy = publish_trust_detail::AsciiWide(package_id);
    bool changed = std::erase(trusted, legacy) > 0;
    if (std::find(trusted.begin(), trusted.end(), key) == trusted.end()) {
        if (trusted.size() >= 64) trusted.erase(trusted.begin());
        trusted.push_back(key);
        changed = true;
    }
    return changed;
}
}
