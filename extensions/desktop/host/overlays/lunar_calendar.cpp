#include "host/overlays/lunar_calendar.h"

#include <windows.h>

#include <cstdint>

namespace pulse::desktop::overlays {
namespace {

// The few ICU C entry points this needs, from the system's icu.dll (unversioned exports on Windows).
using UCalendar = void;
using UErrorCode = int;
using OpenFn = UCalendar* (__cdecl*)(const wchar_t* zone, int32_t len, const char* locale, int type, UErrorCode* status);
using SetMillisFn = void(__cdecl*)(UCalendar* cal, double millis, UErrorCode* status);
using GetFn = int32_t(__cdecl*)(const UCalendar* cal, int field, UErrorCode* status);
constexpr int kMonth = 2, kDate = 5, kIsLeapMonth = 22;

struct Icu {
    SetMillisFn set_millis = nullptr;
    GetFn get = nullptr;
    UCalendar* chinese = nullptr;
};

Icu& Calendar() {
    static Icu icu = [] {
        Icu out;
        const HMODULE dll = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return out;
        const auto open = reinterpret_cast<OpenFn>(GetProcAddress(dll, "ucal_open"));
        out.set_millis = reinterpret_cast<SetMillisFn>(GetProcAddress(dll, "ucal_setMillis"));
        out.get = reinterpret_cast<GetFn>(GetProcAddress(dll, "ucal_get"));
        if (!open || !out.set_millis || !out.get) return Icu{};
        UErrorCode status = 0;
        out.chinese = open(L"Asia/Shanghai", -1, "zh_CN@calendar=chinese", 0, &status);
        if (status > 0 || !out.chinese) return Icu{};
        return out;
    }();
    return icu;
}

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

constexpr const wchar_t* kMonthName[12] = {L"正", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九", L"十", L"冬", L"腊"};

std::wstring DayName(int day) {
    static constexpr const wchar_t* kDigit[10] = {L"", L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九"};
    if (day == 10) return L"初十";
    if (day == 20) return L"二十";
    if (day == 30) return L"三十";
    if (day < 10) return std::wstring(L"初") + kDigit[day];
    if (day < 20) return std::wstring(L"十") + kDigit[day - 10];
    return std::wstring(L"廿") + kDigit[day - 20];
}

std::wstring MonthName(const LunarDay& l) {
    return std::wstring(l.leap ? L"闰" : L"") + kMonthName[(l.month - 1) % 12] + L"月";
}

} // namespace

LunarDay LunarDate(int year, int month, int day) {
    Icu& icu = Calendar();
    LunarDay out;
    if (!icu.chinese) return out;
    UErrorCode status = 0;
    // Noon in China, well clear of any day boundary.
    const double millis = static_cast<double>(DaysFromCivil(year, month, day)) * 86400000.0 + 4.0 * 3600000.0;
    icu.set_millis(icu.chinese, millis, &status);
    const int m = icu.get(icu.chinese, kMonth, &status);
    const int d = icu.get(icu.chinese, kDate, &status);
    const int leap = icu.get(icu.chinese, kIsLeapMonth, &status);
    if (status > 0) return out;
    out.valid = true;
    out.month = m + 1;
    out.day = d;
    out.leap = leap != 0;
    return out;
}

std::wstring FestivalName(int year, int month, int day) {
    struct Fixed { int m, d; const wchar_t* name; };
    static constexpr Fixed kSolar[] = {{1, 1, L"元旦"}, {2, 14, L"情人节"}, {3, 8, L"妇女节"}, {3, 12, L"植树节"},
                                       {5, 1, L"劳动节"}, {5, 4, L"青年节"}, {6, 1, L"儿童节"}, {7, 1, L"建党节"},
                                       {8, 1, L"建军节"}, {9, 10, L"教师节"}, {10, 1, L"国庆节"}, {12, 24, L"平安夜"},
                                       {12, 25, L"圣诞节"}};
    const LunarDay l = LunarDate(year, month, day);
    if (l.valid && !l.leap) {
        static constexpr Fixed kLunar[] = {{1, 1, L"春节"}, {1, 15, L"元宵"}, {5, 5, L"端午"}, {7, 7, L"七夕"},
                                           {8, 15, L"中秋"}, {9, 9, L"重阳"}, {12, 8, L"腊八"}};
        for (const auto& f : kLunar)
            if (f.m == l.month && f.d == l.day) return f.name;
        if (l.month == 12 && l.day >= 29) {
            // 除夕: the last day of the year, whether the twelfth month has 29 days or 30.
            int ny = year, nm = month, nd = day + 1;
            static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            const bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
            const int dim = month == 2 && leap_year ? 29 : kDays[month - 1];
            if (nd > dim) { nd = 1; if (++nm > 12) { nm = 1; ++ny; } }
            const LunarDay next = LunarDate(ny, nm, nd);
            if (next.valid && next.month == 1 && next.day == 1 && !next.leap) return L"除夕";
        }
    }
    for (const auto& f : kSolar)
        if (f.m == month && f.d == day) return f.name;
    return {};
}

std::wstring LunarCellLabel(int year, int month, int day, bool* festival) {
    if (festival) *festival = false;
    const std::wstring named = FestivalName(year, month, day);
    if (!named.empty()) {
        if (festival) *festival = true;
        return named;
    }
    const LunarDay l = LunarDate(year, month, day);
    if (!l.valid) return {};
    return l.day == 1 ? MonthName(l) : DayName(l.day);
}

std::wstring LunarFullText(int year, int month, int day) {
    const LunarDay l = LunarDate(year, month, day);
    if (!l.valid) return {};
    return L"农历" + MonthName(l) + DayName(l.day);
}

} // namespace pulse::desktop::overlays
