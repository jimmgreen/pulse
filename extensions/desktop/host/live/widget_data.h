// widget_data.h - data behind the 天气 and 系统 widgets.
// Weather comes from Open-Meteo (free API, non-commercial terms, no key) for the city picked in settings;
// FetchWeather blocks and runs on a worker thread. System metrics are sampled on the UI thread every second
// when a System widget (or its detail card) is visible.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
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
    bool is_day = true;
    bool has_sun = false;
    int sunrise_min = 6 * 60 + 15;   // local minute of day (0..1439)
    int sunset_min = 18 * 60 + 25;   // local minute of day (0..1439)
    struct Hour { std::wstring label; int code = 0; int temp_c = 0; bool is_day = true; };
    struct Day { std::wstring label; int code = 0; int lo_c = 0; int hi_c = 0; };
    std::vector<Hour> hours;    // the next twelve hours (tiles show the first few)
    std::vector<Day> days;      // tomorrow and the five days after (tiles show the first three)
};

// Geocodes `city` (Open-Meteo geocoding API, first match) and fetches the forecast. Blocking.
WeatherResult FetchWeather(const std::wstring& city);

struct SystemSample {
    bool valid = false;
    int cpu_pct = 0;
    float cpu_pct_exact = 0.0f;
    uint64_t mem_used = 0, mem_total = 0;
    float mem_pct_exact = 0.0f;
    int disk_used_pct = 0;
    uint64_t disk_free = 0;
    uint64_t net_down_bps = 0;
    uint64_t net_up_bps = 0;
    uint32_t physical_cores = 0;
    uint32_t logical_cores = 0;
    bool has_vram = false;
    float vram_pct = 0.0f;
    float vram_used_gb = 0.0f;
    float vram_total_gb = 0.0f;
    std::wstring gpu_name;
    std::vector<float> cpu_history;   // up to 48 samples (0..100%)
    std::vector<float> mem_history;   // up to 48 samples (0..100%)
    std::vector<float> net_history;   // up to 48 samples (KB/s, down + up)
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
    void ProbeGpuOnce();

    // PDH "\Processor Information(_Total)\% Processor Utility": what Task Manager's Processes and
    // Performance tabs show. It scales busy time by delivered/base clock, so it reads higher than
    // GetSystemTimes on a boosting CPU. Opened on first use; null when the counter is unavailable.
    void* pdh_query_ = nullptr;
    void* pdh_cpu_ = nullptr;
    bool pdh_tried_ = false, pdh_primed_ = false;

    // GPU dedicated VRAM telemetry: DXGI IDXGIAdapter3 (largest non-software adapter) + PDH
    // "\GPU Adapter Memory(*)\Dedicated Usage" for system-wide usage matching Task Manager.
    void* dxgi_adapter3_ = nullptr;
    void* pdh_vram_query_ = nullptr;
    void* pdh_vram_counter_ = nullptr;
    uint32_t adapter_luid_low_ = 0;
    int32_t adapter_luid_high_ = 0;
    uint64_t dedicated_vram_bytes_ = 0;
    std::wstring gpu_name_;
    bool gpu_probed_ = false;
    bool has_vram_adapter_ = false;

    uint32_t physical_cores_ = 0;
    uint32_t logical_cores_ = 0;
    uint64_t idle_ = 0, kernel_ = 0, user_ = 0;
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> net_prev_by_luid_;
    uint64_t net_tick_ = 0;
    bool net_primed_ = false;
    bool primed_ = false;

    std::deque<float> cpu_history_;
    std::deque<float> mem_history_;
    std::deque<float> net_history_;
};

} // namespace pulse::desktop::live
