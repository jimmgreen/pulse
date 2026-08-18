#pragma once

#include <windows.h>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace pulse::format {

inline std::wstring ByteSize(uint64_t bytes, bool empty_zero = false) {
    if (empty_zero && bytes == 0) return L"";
    static constexpr const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t text[64];
    if (unit == 0) swprintf_s(text, L"%llu %s", bytes, units[unit]);
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
