#pragma once
#include "drive_kind.h"
#include <iterator>

namespace pulse::fs {

namespace detail {
inline bool SchedulingPrefix(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && CompareStringOrdinal(value.data(),
        static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}
}

// Admission only: unknown devices use the bounded remote lane. Never query a
// volume here; navigation submits work from the UI thread. The injected query
// reads DOS device mappings, not filesystem metadata or drive readiness.
template<class Query>
bool NeedsBoundedEnumeration(std::wstring_view path, Query query) {
    const bool extended = detail::SchedulingPrefix(path, L"\\\\?\\") || detail::SchedulingPrefix(path, L"\\\\.\\");
    if (extended) path.remove_prefix(4);
    else if (detail::SchedulingPrefix(path, L"\\\\")) return true;
    if (detail::SchedulingPrefix(path, L"UNC\\")) return true;
    wchar_t letter = DriveLetterOf(path);
    if (!letter) return extended;
    unsigned visited = 0;
    for (unsigned depth = 0; depth < 8; ++depth) {
        const unsigned bit = 1u << (letter - L'A');
        if (visited & bit) return true;
        visited |= bit;
        const wchar_t device[] = {letter, L':', L'\0'};
        wchar_t buffer[1024]{};
        const DWORD length = query(device, buffer, static_cast<DWORD>(std::size(buffer)));
        if (!length || length >= std::size(buffer)) return true;
        const std::wstring_view target(buffer);
        if (detail::SchedulingPrefix(target, L"\\Device\\Mup") ||
            detail::SchedulingPrefix(target, L"\\Device\\LanmanRedirector") ||
            detail::SchedulingPrefix(target, L"\\Device\\WebDavRedirector") ||
            IsUserModeVolumeDevice(target)) return true;
        constexpr std::wstring_view local = L"\\Device\\HarddiskVolume";
        if (detail::SchedulingPrefix(target, local)) {
            const auto number = target.substr(local.size());
            if (number.empty()) return true;
            for (wchar_t c : number) if (c < L'0' || c > L'9') return true;
            return false;
        }
        if (!detail::SchedulingPrefix(target, L"\\??\\")) return true;
        const auto substituted = target.substr(4);
        letter = DriveLetterOf(substituted);
        if (!letter) return true;
    }
    return true;
}

inline bool NeedsBoundedEnumeration(std::wstring_view path) {
    return NeedsBoundedEnumeration(path, [](const wchar_t* device, wchar_t* target, DWORD capacity) {
        if (detail::TestMarksRemoteLike(device[0])) {
            constexpr wchar_t remote[] = L"\\Device\\Mup";
            if (capacity < std::size(remote)) return DWORD{0};
            for (size_t i = 0; i < std::size(remote); ++i) target[i] = remote[i];
            return static_cast<DWORD>(std::size(remote));
        }
        return QueryDosDeviceW(device, target, capacity);
    });
}

} // namespace pulse::fs
