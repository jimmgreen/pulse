// widget_data.h - data behind the 天气 and 系统 widgets.
// Weather comes from Open-Meteo (free API, non-commercial terms, no key) for the city picked in settings;
// FetchWeather blocks and runs on a worker thread. System metrics are sampled on the UI thread every few seconds.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::desktop::live {

struct WeatherResult {
    bool ok = false;
    std::wstring city;          // the geocoder's name for the place
    std::wstring error;         // shown on the tile when !ok
    int temp_c = 0, code = 0, hi_c = 0, lo_c = 0;
    int feels_c = 0, humidity_pct = 0, wind_kmh = 0;  // the detail card's extras
    int utc_offset_s = 0;       // the city's offset from UTC (its clock in the 时钟 card)
    bool has_utc_offset = false;
    struct Hour { std::wstring label; int code = 0; int temp_c = 0; };
    struct Day { std::wstring label; int code = 0; int lo_c = 0; int hi_c = 0; };
    std::vector<Hour> hours;    // the next twelve hours (tiles show the first few)
    std::vector<Day> days;      // tomorrow and the five days after (tiles show the first three)
};

// Geocodes `city` (Open-Meteo geocoding API, first match) and fetches the forecast. Blocking.
WeatherResult FetchWeather(const std::wstring& city);

struct SystemSample {
    bool valid = false;
    int cpu_pct = 0;
    uint64_t mem_used = 0, mem_total = 0;
    int disk_used_pct = 0;
    uint64_t disk_free = 0;
    uint64_t net_down_bps = 0;
};

// CPU and network rates are deltas between calls; the first call primes them.
class SystemSampler {
public:
    SystemSampler() = default;
    ~SystemSampler();
    SystemSampler(const SystemSampler&) = delete;
    SystemSampler& operator=(const SystemSampler&) = delete;
    SystemSample Sample();

private:
    // PDH "\Processor Information(_Total)\% Processor Utility": what Task Manager's Processes and
    // Performance tabs show. It scales busy time by delivered/base clock, so it reads higher than
    // GetSystemTimes on a boosting CPU. Opened on first use; null when the counter is unavailable.
    void* pdh_query_ = nullptr;
    void* pdh_cpu_ = nullptr;
    bool pdh_tried_ = false, pdh_primed_ = false;
    uint64_t idle_ = 0, kernel_ = 0, user_ = 0;
    uint64_t net_bytes_ = 0, net_tick_ = 0;
    bool primed_ = false;
};

} // namespace pulse::desktop::live
