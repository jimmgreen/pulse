#include "host/live/widget_data.h"
#include "host/render/widget_painters.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <winhttp.h>
#include <dxgi1_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <memory>
#include <string_view>
#include <utility>

namespace pulse::desktop::live {
namespace {

constexpr size_t kMaxHistorySamples = 48;

void PushHistory(std::deque<float>& q, float v) {
    q.push_back(v);
    while (q.size() > kMaxHistorySamples) q.pop_front();
}

std::wstring ShortenGpuName(const wchar_t* desc) {
    if (!desc || !desc[0]) return L"GPU";
    std::wstring s(desc);
    for (const wchar_t* prefix : { L"NVIDIA GeForce ", L"NVIDIA ", L"AMD Radeon(TM) ", L"AMD Radeon ", L"Intel(R) ", L"Microsoft " }) {
        const size_t len = std::wcslen(prefix);
        if (s.compare(0, len, prefix) == 0) {
            s.erase(0, len);
            break;
        }
    }
    if (s.size() > 18) s.resize(18);
    return s;
}

bool ParseIsoMinuteOfDay(const std::string& iso, int& out_min) noexcept {
    // Expects "YYYY-MM-DDTHH:MM"
    const size_t t_pos = iso.find('T');
    if (t_pos == std::string::npos || t_pos + 5 >= iso.size()) return false;
    const int hh = std::atoi(iso.c_str() + t_pos + 1);
    const int mm = std::atoi(iso.c_str() + t_pos + 4);
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    out_min = hh * 60 + mm;
    return true;
}

// ---------------------------------------------------------------- HTTP (WinHTTP, HTTPS GET)
struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET v) : h(v) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool HttpsGet(const wchar_t* host, const std::wstring& path, std::string& body) {
    Handle session(WinHttpOpen(L"PulseDesktop/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 5000, 5000, 8000, 8000);
    Handle conn(WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) return false;
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return false;
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return false;
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return false;
    body.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        if (body.size() + avail > (1u << 20)) return false;  // a forecast is a few kilobytes
        const size_t at = body.size();
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, body.data() + at, avail, &read)) return false;
        body.resize(at + read);
    }
    return !body.empty();
}

std::wstring UrlEncode(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), u.data(), n, nullptr, nullptr);
    std::wstring out;
    for (unsigned char c : u) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<wchar_t>(c));
        else { wchar_t b[4]; std::swprintf(b, 4, L"%%%02X", c); out += b; }
    }
    return out;
}

// ---------------------------------------------------------------- a small JSON reader (objects, arrays, numbers, strings)
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* Get(std::string_view k) const {
        for (const auto& [key, v] : obj) if (key == k) return &v;
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}
    bool Parse(Json& out) { return Value(out, 0) && (Ws(), i_ == s_.size()); }

private:
    std::string_view s_;
    size_t i_ = 0;
    void Ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_; }
    bool Str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c == '\\' && i_ < s_.size()) {
                const char e = s_[i_++];
                if (e == 'u' && i_ + 4 <= s_.size()) {
                    const unsigned cp = static_cast<unsigned>(std::strtoul(std::string(s_.substr(i_, 4)).c_str(), nullptr, 16));
                    i_ += 4;
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    else { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                    continue;
                }
                c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            out.push_back(c);
        }
        if (i_ >= s_.size()) return false;
        ++i_;
        return true;
    }
    bool Value(Json& out, int depth) {
        if (depth > 32) return false;
        Ws();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') {
            ++i_; out.kind = Json::Kind::Object; Ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                Ws(); std::string k; if (!Str(k)) return false;
                Ws(); if (i_ >= s_.size() || s_[i_] != ':') return false; ++i_;
                Json v; if (!Value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(k), std::move(v));
                Ws(); if (i_ >= s_.size()) return false;
                if (s_[i_] == '}') { ++i_; return true; }
                if (s_[i_] != ',') return false; ++i_;
            }
        }
        if (c == '[') {
            ++i_; out.kind = Json::Kind::Array; Ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                Json v; if (!Value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                Ws(); if (i_ >= s_.size()) return false;
                if (s_[i_] == ']') { ++i_; return true; }
                if (s_[i_] != ',') return false; ++i_;
            }
        }
        if (c == '"') { out.kind = Json::Kind::String; return Str(out.str); }
        if (s_.substr(i_, 4) == "true") { i_ += 4; out.kind = Json::Kind::Bool; out.num = 1; return true; }
        if (s_.substr(i_, 5) == "false") { i_ += 5; out.kind = Json::Kind::Bool; return true; }
        if (s_.substr(i_, 4) == "null") { i_ += 4; out.kind = Json::Kind::Null; return true; }
        const size_t start = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' ||
                                  s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) ++i_;
        if (i_ == start) return false;
        out.kind = Json::Kind::Number;
        out.num = std::strtod(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
        return true;
    }
};

std::wstring Wide(const std::string& u) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), w.data(), n);
    return w;
}
int Round(const Json* v) { return v && v->kind == Json::Kind::Number ? static_cast<int>(std::lround(v->num)) : 0; }

} // namespace

WeatherResult FetchWeather(const std::wstring& city) {
    WeatherResult r;
    if (city.empty()) { r.error = L"在设置里选一个城市"; return r; }
    std::string body;
    if (!HttpsGet(L"geocoding-api.open-meteo.com", L"/v1/search?count=1&language=zh&format=json&name=" + UrlEncode(city), body)) {
        r.error = L"连不上天气服务";
        return r;
    }
    Json geo;
    if (!Reader(body).Parse(geo)) { r.error = L"天气服务返回了无法识别的数据"; return r; }
    const Json* results = geo.Get("results");
    if (!results || results->arr.empty()) { r.error = L"找不到「" + city + L"」"; return r; }
    const Json& place = results->arr.front();
    const Json* lat = place.Get("latitude");
    const Json* lon = place.Get("longitude");
    if (!lat || !lon) { r.error = L"找不到「" + city + L"」"; return r; }
    if (const Json* nm = place.Get("name")) r.city = Wide(nm->str);
    wchar_t q[512]{};
    std::swprintf(q, 512,
                  L"/v1/forecast?latitude=%.4f&longitude=%.4f"
                  L"&current=temperature_2m,weather_code,apparent_temperature,relative_humidity_2m,wind_speed_10m,is_day"
                  L"&hourly=temperature_2m,weather_code,is_day"
                  L"&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset"
                  L"&timezone=auto&forecast_days=7",
                  lat->num, lon->num);
    if (!HttpsGet(L"api.open-meteo.com", q, body)) { r.error = L"连不上天气服务"; return r; }
    Json fc;
    if (!Reader(body).Parse(fc)) { r.error = L"天气服务返回了无法识别的数据"; return r; }
    const Json* cur = fc.Get("current");
    const Json* daily = fc.Get("daily");
    const Json* hourly = fc.Get("hourly");
    if (!cur || !daily || !hourly) { r.error = L"天气服务返回了无法识别的数据"; return r; }
    r.temp_c = Round(cur->Get("temperature_2m"));
    r.feels_c = Round(cur->Get("apparent_temperature"));
    if (const Json* off = fc.Get("utc_offset_seconds")) { r.utc_offset_s = Round(off); r.has_utc_offset = true; }
    r.humidity_pct = Round(cur->Get("relative_humidity_2m"));
    r.wind_kmh = Round(cur->Get("wind_speed_10m"));
    r.code = Round(cur->Get("weather_code"));
    if (const Json* day_v = cur->Get("is_day"); day_v && day_v->kind == Json::Kind::Number) {
        r.is_day = (day_v->num > 0.5);
    }

    const Json* dmax = daily->Get("temperature_2m_max");
    const Json* dmin = daily->Get("temperature_2m_min");
    const Json* dcode = daily->Get("weather_code");
    const Json* dtime = daily->Get("time");
    const Json* dsunrise = daily->Get("sunrise");
    const Json* dsunset = daily->Get("sunset");
    if (dmax && dmin && !dmax->arr.empty() && !dmin->arr.empty()) { r.hi_c = Round(&dmax->arr[0]); r.lo_c = Round(&dmin->arr[0]); }
    int sr_min = 0, ss_min = 0;
    if (dsunrise && dsunset && !dsunrise->arr.empty() && !dsunset->arr.empty() &&
        ParseIsoMinuteOfDay(dsunrise->arr[0].str, sr_min) &&
        ParseIsoMinuteOfDay(dsunset->arr[0].str, ss_min)) {
        r.sunrise_min = sr_min;
        r.sunset_min = ss_min;
        r.has_sun = true;
    } else {
        render::ComputeAstronomicalSunMinutes(lat->num, lon->num, r.utc_offset_s, r.sunrise_min, r.sunset_min);
        r.has_sun = true;
    }

    // The next twelve hours after the current one (local time strings "2026-11-14T15:00").
    const Json* htime = hourly->Get("time");
    const Json* htemp = hourly->Get("temperature_2m");
    const Json* hcode = hourly->Get("weather_code");
    const Json* hday = hourly->Get("is_day");
    const Json* ctime = cur->Get("time");
    if (htime && htemp && hcode && ctime && ctime->str.size() >= 13) {
        const std::string now_hour = ctime->str.substr(0, 13);
        size_t i = 0;
        while (i < htime->arr.size() && htime->arr[i].str.substr(0, 13) <= now_hour) ++i;
        for (size_t n = 0; n < 12 && i + n < htime->arr.size() && i + n < htemp->arr.size() && i + n < hcode->arr.size(); ++n) {
            const std::string& t = htime->arr[i + n].str;
            const int h = t.size() >= 13 ? std::atoi(t.c_str() + 11) : 0;
            bool is_day = (h >= 6 && h < 18);
            if (hday && i + n < hday->arr.size() && hday->arr[i + n].kind == Json::Kind::Number) {
                is_day = (hday->arr[i + n].num > 0.5);
            }
            r.hours.push_back({std::to_wstring(h) + L"时", Round(&hcode->arr[i + n]), Round(&htemp->arr[i + n]), is_day});
        }
    }
    static constexpr const wchar_t* kWd[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    if (dmax && dmin && dcode && dtime) {
        for (size_t d = 1; d < 7 && d < dmax->arr.size() && d < dmin->arr.size() && d < dcode->arr.size() && d < dtime->arr.size(); ++d) {
            std::wstring label = L"明天";
            if (d > 1) {
                std::tm tm{};
                const std::string& ds = dtime->arr[d].str;
                if (ds.size() >= 10) {
                    tm.tm_year = std::atoi(ds.c_str()) - 1900; tm.tm_mon = std::atoi(ds.c_str() + 5) - 1; tm.tm_mday = std::atoi(ds.c_str() + 8);
                    tm.tm_hour = 12;
                    std::mktime(&tm);
                    label = kWd[tm.tm_wday % 7];
                }
            }
            r.days.push_back({label, Round(&dcode->arr[d]), Round(&dmin->arr[d]), Round(&dmax->arr[d])});
        }
    }
    r.ok = true;
    return r;
}

SystemSampler::~SystemSampler() {
    if (pdh_query_) PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_query_));
    if (pdh_vram_query_) PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_vram_query_));
    if (dxgi_adapter3_) static_cast<IDXGIAdapter3*>(dxgi_adapter3_)->Release();
}

uint32_t QueryPhysicalCoreCount() noexcept {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<BYTE> buf(len);
    auto* ptr = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, ptr, &len)) return 0;
    uint32_t count = 0;
    DWORD offset = 0;
    while (offset < len) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + offset);
        if (info->Relationship == RelationProcessorCore) ++count;
        if (info->Size == 0) break;
        offset += info->Size;
    }
    return count;
}

std::wstring QuerySystemDriveRoot() {
    wchar_t win_dir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(win_dir, ARRAYSIZE(win_dir));
    if (n >= 3 && std::iswalpha(win_dir[0]) && win_dir[1] == L':') {
        std::wstring root(3, L'\0');
        root[0] = static_cast<wchar_t>(std::towupper(win_dir[0]));
        root[1] = L':';
        root[2] = L'\\';
        return root;
    }
    return L"C:\\";
}

void SystemSampler::ProbeGpuOnce() {
    if (gpu_probed_) return;
    gpu_probed_ = true;
    logical_cores_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    physical_cores_ = QueryPhysicalCoreCount();
    if (physical_cores_ == 0) physical_cores_ = logical_cores_;

    // Pick hardware GPU adapter with largest dedicated VRAM (tracks discrete GPU on dual-GPU rigs,
    // ignoring software renderers and 0-VRAM virtual display adapters).
    IDXGIFactory4* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&fac))) && fac) {
        IDXGIAdapter3* best_adapter = nullptr;
        SIZE_T best_dedicated = 0;
        LUID best_luid{};
        std::wstring best_name;

        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* ad1 = nullptr;
            if (fac->EnumAdapters1(i, &ad1) == DXGI_ERROR_NOT_FOUND) break;
            if (!ad1) continue;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(ad1->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory > (100ull << 20)) {
                IDXGIAdapter3* ad3 = nullptr;
                if (SUCCEEDED(ad1->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&ad3))) && ad3) {
                    if (!best_adapter || desc.DedicatedVideoMemory > best_dedicated) {
                        if (best_adapter) best_adapter->Release();
                        best_adapter = ad3;
                        best_dedicated = desc.DedicatedVideoMemory;
                        best_luid = desc.AdapterLuid;
                        best_name = ShortenGpuName(desc.Description);
                    } else {
                        ad3->Release();
                    }
                }
            }
            ad1->Release();
        }
        fac->Release();

        if (best_adapter) {
            dxgi_adapter3_ = best_adapter;
            adapter_luid_low_ = static_cast<uint32_t>(best_luid.LowPart);
            adapter_luid_high_ = static_cast<int32_t>(best_luid.HighPart);
            dedicated_vram_bytes_ = static_cast<uint64_t>(best_dedicated);
            gpu_name_ = std::move(best_name);
            has_vram_adapter_ = (dedicated_vram_bytes_ > (100ull << 20));
        }
    }

    // System-wide dedicated VRAM counter matching Task Manager.
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS && q) {
        PDH_HCOUNTER c = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &c) == ERROR_SUCCESS && c) {
            pdh_vram_query_ = q;
            pdh_vram_counter_ = c;
        } else {
            PdhCloseQuery(q);
        }
    }
}

SystemSample SystemSampler::Sample() {
    ProbeGpuOnce();
    SystemSample s;
    s.physical_cores = physical_cores_;
    s.logical_cores = logical_cores_;
    s.gpu_name = gpu_name_;

    double pdh_cpu_f = -1.0;
    if (!pdh_tried_) {
        pdh_tried_ = true;
        PDH_HQUERY q = nullptr;
        PDH_HCOUNTER c = nullptr;
        // The English name works whatever the display language (localized counter names differ).
        if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(q, L"\\Processor Information(_Total)\\% Processor Utility", 0, &c) == ERROR_SUCCESS) {
                pdh_query_ = q;
                pdh_cpu_ = c;
            } else {
                PdhCloseQuery(q);
            }
        }
    }
    if (pdh_query_ && PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_query_)) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE v{};
        // NOCAP100 then clamp ourselves: Task Manager also shows at most 100%.
        if (pdh_primed_ && PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(pdh_cpu_), PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                                       nullptr, &v) == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS) {
            pdh_cpu_f = std::clamp(v.doubleValue, 0.0, 100.0);
        }
        pdh_primed_ = true;
    }
    double sys_cpu_f = -1.0;
    FILETIME idle{}, kern{}, user{};
    if (GetSystemTimes(&idle, &kern, &user)) {
        auto u64 = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const uint64_t i = u64(idle), k = u64(kern), u = u64(user);
        if (primed_) {
            const uint64_t di = (i >= idle_) ? (i - idle_) : 0;
            const uint64_t dt = ((k >= kernel_) ? (k - kernel_) : 0) + ((u >= user_) ? (u - user_) : 0);
            if (dt > 0) {
                const double busy = (dt > di) ? (static_cast<double>(dt - di) * 100.0 / static_cast<double>(dt)) : 0.0;
                sys_cpu_f = std::clamp(busy, 0.0, 100.0);
            }
        }
        idle_ = i; kernel_ = k; user_ = u;
    }
    const bool has_cpu_sample = (pdh_cpu_f >= 0.0) || (sys_cpu_f >= 0.0);
    const double cpu_pct_f = (pdh_cpu_f >= 0.0) ? pdh_cpu_f : ((sys_cpu_f >= 0.0) ? sys_cpu_f : 0.0);
    s.cpu_pct_exact = static_cast<float>(cpu_pct_f);
    s.cpu_pct = static_cast<int>(std::clamp(std::lround(cpu_pct_f), 0L, 100L));

    MEMORYSTATUSEX ms{sizeof(ms)};
    float mem_pct_f = 0.0f;
    if (GlobalMemoryStatusEx(&ms)) {
        s.mem_total = ms.ullTotalPhys;
        s.mem_used = (ms.ullTotalPhys >= ms.ullAvailPhys) ? (ms.ullTotalPhys - ms.ullAvailPhys) : 0;
        if (s.mem_total > 0) {
            mem_pct_f = std::clamp(static_cast<float>(static_cast<double>(s.mem_used) * 100.0 / static_cast<double>(s.mem_total)), 0.0f, 100.0f);
        }
    }
    s.mem_pct_exact = mem_pct_f;

    // System-wide dedicated GPU VRAM usage (strictly matched by target adapter LUID via PDH;
    // never falls back to a different GPU's counter or DXGI per-process CurrentUsage).
    if (has_vram_adapter_ && dedicated_vram_bytes_ > (100ull << 20)) {
        constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
        const double total_bytes = static_cast<double>(dedicated_vram_bytes_);
        double used_bytes = -1.0;

        if (pdh_vram_query_ && pdh_vram_counter_) {
            if (PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_vram_query_)) == ERROR_SUCCESS) {
                DWORD buf_size = 0;
                DWORD item_count = 0;
                const PDH_STATUS st = PdhGetFormattedCounterArrayW(
                    static_cast<PDH_HCOUNTER>(pdh_vram_counter_), PDH_FMT_LARGE, &buf_size, &item_count, nullptr);
                if (st == PDH_MORE_DATA && buf_size > 0) {
                    std::vector<BYTE> buf(buf_size);
                    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
                    if (PdhGetFormattedCounterArrayW(
                            static_cast<PDH_HCOUNTER>(pdh_vram_counter_), PDH_FMT_LARGE, &buf_size, &item_count, items) == ERROR_SUCCESS) {
                        wchar_t target_prefix[64]{};
                        std::swprintf(target_prefix, ARRAYSIZE(target_prefix), L"luid_0x%08x_0x%08x",
                                      static_cast<unsigned int>(adapter_luid_high_),
                                      static_cast<unsigned int>(adapter_luid_low_));
                        double matched_sum = 0.0;
                        bool matched_any = false;
                        for (DWORD i = 0; i < item_count; ++i) {
                            if (items[i].FmtValue.CStatus != ERROR_SUCCESS && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                            if (!items[i].szName) continue;
                            std::wstring name(items[i].szName);
                            for (auto& ch : name) ch = static_cast<wchar_t>(std::towlower(ch));
                            if (name.find(target_prefix) != std::wstring::npos) {
                                matched_sum += static_cast<double>(std::max<LONGLONG>(0, items[i].FmtValue.largeValue));
                                matched_any = true;
                            }
                        }
                        if (matched_any) {
                            used_bytes = std::min(matched_sum, total_bytes);
                        }
                    }
                }
            }
        }
        if (used_bytes >= 0.0) {
            s.vram_used_gb = static_cast<float>(used_bytes / kGb);
            s.vram_total_gb = static_cast<float>(total_bytes / kGb);
            s.vram_pct = std::clamp(static_cast<float>(used_bytes * 100.0 / total_bytes), 0.0f, 100.0f);
            s.has_vram = (s.vram_total_gb > 0.1f);
        }
    }

    static const std::wstring sys_drive = QuerySystemDriveRoot();
    ULARGE_INTEGER avail{}, total{}, free_total{};
    if (GetDiskFreeSpaceExW(sys_drive.c_str(), &avail, &total, &free_total) && total.QuadPart) {
        s.disk_free = avail.QuadPart;
        const uint64_t used = (total.QuadPart > avail.QuadPart) ? (total.QuadPart - avail.QuadPart) : 0;
        s.disk_used_pct = static_cast<int>(std::clamp<uint64_t>((used * 100 + total.QuadPart / 2) / total.QuadPart, 0, 100));
    }

    // Per-LUID hardware NIC deltas: excludes virtual VPN/TUN adapters (e.g. FlClash/Tailscale)
    // to prevent double-counting, and ignores newly connected adapters on their first tick so
    // hot-plug or wake-from-sleep never injects bogus GB/s spikes.
    bool has_net_sample = false;
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2Ex(MibIfTableNormal, &table) == NO_ERROR && table) {
        std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> cur_by_luid;
        for (ULONG n = 0; n < table->NumEntries; ++n) {
            const auto& row = table->Table[n];
            if (row.OperStatus != IfOperStatusUp) continue;
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) continue;
            if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;
            if (!row.InterfaceAndOperStatusFlags.ConnectorPresent) continue;
            cur_by_luid[row.InterfaceLuid.Value] = {row.InOctets, row.OutOctets};
        }
        FreeMibTable(table);
        const uint64_t now = GetTickCount64();
        if (net_primed_ && now > net_tick_) {
            const uint64_t dt_ms = now - net_tick_;
            if (dt_ms >= 50 && dt_ms <= 10000) {
                uint64_t delta_in = 0;
                uint64_t delta_out = 0;
                for (const auto& [luid, cur] : cur_by_luid) {
                    if (auto prev = net_prev_by_luid_.find(luid); prev != net_prev_by_luid_.end()) {
                        if (cur.first >= prev->second.first) delta_in += (cur.first - prev->second.first);
                        if (cur.second >= prev->second.second) delta_out += (cur.second - prev->second.second);
                    }
                }
                s.net_down_bps = delta_in * 1000 / dt_ms;
                s.net_up_bps = delta_out * 1000 / dt_ms;
                has_net_sample = true;
            }
        }
        net_prev_by_luid_ = std::move(cur_by_luid);
        net_tick_ = now;
        net_primed_ = true;
    }

    if (has_cpu_sample) PushHistory(cpu_history_, s.cpu_pct_exact);
    PushHistory(mem_history_, mem_pct_f);
    if (has_net_sample) {
        PushHistory(net_history_, static_cast<float>(static_cast<double>(s.net_down_bps + s.net_up_bps) / 1024.0));
    }
    s.cpu_history.assign(cpu_history_.begin(), cpu_history_.end());
    s.mem_history.assign(mem_history_.begin(), mem_history_.end());
    s.net_history.assign(net_history_.begin(), net_history_.end());

    primed_ = true;
    s.valid = true;
    return s;
}

} // namespace pulse::desktop::live
