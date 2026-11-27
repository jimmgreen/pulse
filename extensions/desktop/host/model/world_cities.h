// world_cities.h - Cities the 时钟 widget's world clock can show, with their Windows time zone keys.
// Shared by the host (times) and the plugin's settings page (the pick list).
#pragma once
#include <array>
#include <string_view>

namespace pulse::desktop::model {

struct WorldCity {
    std::wstring_view label;   // what the widget and settings show
    std::wstring_view tz_key;  // HKLM\...\Time Zones key (TIME_ZONE_INFORMATION source)
};

inline constexpr std::array<WorldCity, 18> kWorldCities = {{
    {L"北京", L"China Standard Time"},
    {L"上海", L"China Standard Time"},
    {L"香港", L"China Standard Time"},
    {L"台北", L"Taipei Standard Time"},
    {L"东京", L"Tokyo Standard Time"},
    {L"首尔", L"Korea Standard Time"},
    {L"新加坡", L"Singapore Standard Time"},
    {L"迪拜", L"Arabian Standard Time"},
    {L"莫斯科", L"Russian Standard Time"},
    {L"伦敦", L"GMT Standard Time"},
    {L"巴黎", L"Romance Standard Time"},
    {L"柏林", L"W. Europe Standard Time"},
    {L"纽约", L"Eastern Standard Time"},
    {L"芝加哥", L"Central Standard Time"},
    {L"洛杉矶", L"Pacific Standard Time"},
    {L"温哥华", L"Pacific Standard Time"},
    {L"悉尼", L"AUS Eastern Standard Time"},
    {L"奥克兰", L"New Zealand Standard Time"},
}};

// The world clock's cities when the user has not picked any.
inline constexpr std::array<std::wstring_view, 4> kDefaultWorldCities = {L"北京", L"东京", L"伦敦", L"纽约"};

inline const WorldCity* FindWorldCity(std::wstring_view label) noexcept {
    for (const auto& c : kWorldCities)
        if (c.label == label) return &c;
    return nullptr;
}

} // namespace pulse::desktop::model
