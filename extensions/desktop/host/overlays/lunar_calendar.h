#pragma once

#include <string>

namespace pulse::desktop::overlays {

// The Chinese lunar date of a Gregorian day, from Windows' own ICU (icu.dll, the Chinese calendar).
// `valid` is false where ICU is unavailable (before Windows 10 1903); callers then simply show no lunar line.
struct LunarDay {
    bool valid = false;
    int month = 0;   // 1..12
    int day = 0;     // 1..30
    bool leap = false;
};
LunarDay LunarDate(int year, int month, int day);

// What a calendar cell shows under the date: a festival (元旦, 春节, 中秋…) when there is one, the lunar month
// on its first day (十月, 闰六月), otherwise the lunar day (初九, 廿三). `festival` says it is a named day.
std::wstring LunarCellLabel(int year, int month, int day, bool* festival);
// "农历十月初九" for the selected-day line; empty without ICU.
std::wstring LunarFullText(int year, int month, int day);
// A named day on this date, or empty.
std::wstring FestivalName(int year, int month, int day);

} // namespace pulse::desktop::overlays
