#pragma once

#include <windows.h>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace pulse::format {

enum class ByteSizeStyle { Spaced, Compact };

inline std::wstring ByteSize(uint64_t bytes, bool empty_zero = false,
                             ByteSizeStyle style = ByteSizeStyle::Spaced) {
    if (empty_zero && bytes == 0) return L"";
    static constexpr const wchar_t* spaced_units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    static constexpr const wchar_t* compact_units[] = { L"B", L"K", L"M", L"G", L"TB" };
    const auto* units = style == ByteSizeStyle::Compact ? compact_units : spaced_units;
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    constexpr size_t unit_count = 5;
    while (value >= 1024.0 && unit + 1 < unit_count) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t text[64];
    if (style == ByteSizeStyle::Compact) {
        if (unit == 0) swprintf_s(text, L"%lluB", bytes);
        else if (value >= 10.0) swprintf_s(text, L"%.0f%s", value, units[unit]);
        else swprintf_s(text, L"%.1f%s", value, units[unit]);
    } else if (unit == 0) swprintf_s(text, L"%llu %s", bytes, units[unit]);
    else swprintf_s(text, L"%.1f %s", value, units[unit]);
    return text;
}

inline std::wstring LocalFileTime(const FILETIME& time,
                                  std::wstring_view fallback = {}) {
    FILETIME local{};
    SYSTEMTIME system{};
    if (!FileTimeToLocalFileTime(&time, &local) ||
        !FileTimeToSystemTime(&local, &system)) {
        return std::wstring(fallback);
    }
    wchar_t text[48];
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", system.wYear, system.wMonth,
               system.wDay, system.wHour, system.wMinute);
    return text;
}

// 2895851315 -> "2,895,851,315"
inline std::wstring GroupedInt(uint64_t value) {
    wchar_t digits[24];
    swprintf_s(digits, L"%llu", value);
    std::wstring out;
    const size_t len = wcslen(digits);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

} // namespace pulse::format
