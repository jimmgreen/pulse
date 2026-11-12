#pragma once
#include <windows.h>
#include <propidl.h>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>

namespace pulse::shell_bridge::property_text {
inline bool ExactScalar(VARTYPE type) { return type == VT_R4 || type == VT_R8 || type == VT_FILETIME; }
template<class T> HRESULT FormatNumber(T value, std::wstring& result) {
    if (!std::isfinite(value)) return E_NOTIMPL;
    char buffer[128];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, std::numeric_limits<T>::max_digits10);
    if (converted.ec != std::errc{}) return E_INVALIDARG;
    result.assign(buffer, converted.ptr);
    return S_OK;
}
inline HRESULT Format(const PROPVARIANT& value, std::wstring& result) {
    if (value.vt == VT_R4) return FormatNumber(value.fltVal, result);
    if (value.vt == VT_R8) return FormatNumber(value.dblVal, result);
    if (value.vt != VT_FILETIME) return E_INVALIDARG;
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&value.filetime, &time) || time.wYear > 9999) return E_NOTIMPL;
    const uint64_t ticks = (static_cast<uint64_t>(value.filetime.dwHighDateTime) << 32) | value.filetime.dwLowDateTime;
    wchar_t buffer[32]{};
    const int count = swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u.%07uZ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, static_cast<unsigned>(ticks % 10000000));
    if (count != 28) return E_INVALIDARG;
    result.assign(buffer, static_cast<size_t>(count));
    return S_OK;
}
template<class T> HRESULT ParseNumber(std::wstring_view text, T& result) {
    if (text.empty() || text.size() > 128) return E_INVALIDARG;
    char buffer[128];
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] > 127) return E_INVALIDARG;
        buffer[i] = static_cast<char>(text[i]);
    }
    const char* first = buffer;
    const char* end = buffer + text.size();
    if (*first == '+') ++first;
    if (first == end) return E_INVALIDARG;
    T candidate{};
    const auto converted = std::from_chars(first, end, candidate, std::chars_format::general);
    if (converted.ec != std::errc{} || converted.ptr != end || !std::isfinite(candidate)) return E_INVALIDARG;
    result = candidate;
    return S_OK;
}
inline HRESULT Parse(VARTYPE type, std::wstring_view text, PROPVARIANT& result) {
    PROPVARIANT candidate{};
    HRESULT hr = E_INVALIDARG;
    if (type == VT_R4) hr = ParseNumber(text, candidate.fltVal);
    else if (type == VT_R8) hr = ParseNumber(text, candidate.dblVal);
    else if (type == VT_FILETIME) {
        if (text.size() != 28 || text[4] != L'-' || text[7] != L'-' || text[10] != L'T' ||
            text[13] != L':' || text[16] != L':' || text[19] != L'.' || text[27] != L'Z') return E_INVALIDARG;
        auto digits = [&](size_t start, size_t count, unsigned& value) {
            value = 0;
            for (size_t i = start; i < start + count; ++i) {
                if (text[i] < L'0' || text[i] > L'9') return false;
                value = value * 10 + static_cast<unsigned>(text[i] - L'0');
            }
            return true;
        };
        unsigned year, month, day, hour, minute, second, fraction;
        if (!digits(0, 4, year) || !digits(5, 2, month) || !digits(8, 2, day) || !digits(11, 2, hour) ||
            !digits(14, 2, minute) || !digits(17, 2, second) || !digits(20, 7, fraction)) return E_INVALIDARG;
        SYSTEMTIME time{};
        time.wYear = static_cast<WORD>(year); time.wMonth = static_cast<WORD>(month); time.wDay = static_cast<WORD>(day);
        time.wHour = static_cast<WORD>(hour); time.wMinute = static_cast<WORD>(minute); time.wSecond = static_cast<WORD>(second);
        FILETIME file{};
        SYSTEMTIME checked{};
        if (!SystemTimeToFileTime(&time, &file) || !FileTimeToSystemTime(&file, &checked) ||
            checked.wYear != year || checked.wMonth != month || checked.wDay != day || checked.wHour != hour ||
            checked.wMinute != minute || checked.wSecond != second) return E_INVALIDARG;
        const uint64_t ticks = ((static_cast<uint64_t>(file.dwHighDateTime) << 32) | file.dwLowDateTime) + fraction;
        candidate.filetime = {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        hr = S_OK;
    }
    if (FAILED(hr)) return hr;
    candidate.vt = type;
    result = candidate;
    return S_OK;
}
inline bool Same(const PROPVARIANT& a, const PROPVARIANT& b) {
    if (a.vt != b.vt) return false;
    if (a.vt == VT_R4) return std::memcmp(&a.fltVal, &b.fltVal, sizeof(float)) == 0;
    if (a.vt == VT_R8) return std::memcmp(&a.dblVal, &b.dblVal, sizeof(double)) == 0;
    if (a.vt == VT_FILETIME) return a.filetime.dwLowDateTime == b.filetime.dwLowDateTime && a.filetime.dwHighDateTime == b.filetime.dwHighDateTime;
    return false;
}
}
