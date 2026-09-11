#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::app {
inline uint64_t ChangeNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) /
        10000000ull) - 11644473600ull;
}
inline uint64_t ChangeLocalMidnight(uint64_t now) {
    const uint64_t ticks = (now + 11644473600ull) * 10000000ull;
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{}, local{}, midnight{};
    if (!FileTimeToSystemTime(&ft, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return now;
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &midnight) ||
        !SystemTimeToFileTime(&midnight, &ft)) return now;
    return (((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) /
        10000000ull) - 11644473600ull;
}
inline uint64_t ChangeSince(int days, uint64_t now) {
    if (days == 1) return ChangeLocalMidnight(now);
    const uint64_t duration = (days == 3 ? 3ull : 7ull) * 86400;
    return now > duration ? now - duration : 0;
}
inline FILETIME ChangeFileTime(uint64_t seconds) {
    const uint64_t ticks = (seconds + 11644473600ull) * 10000000ull;
    return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}
}
