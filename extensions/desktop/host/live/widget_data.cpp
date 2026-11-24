#include "host/live/widget_data.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string_view>
#include <utility>

namespace pulse::desktop::live {
namespace {

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
    std::swprintf(q, 512, L"/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,apparent_temperature,relative_humidity_2m,wind_speed_10m"
                          L"&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min"
                          L"&timezone=auto&forecast_days=7", lat->num, lon->num);
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
    const Json* dmax = daily->Get("temperature_2m_max");
    const Json* dmin = daily->Get("temperature_2m_min");
    const Json* dcode = daily->Get("weather_code");
    const Json* dtime = daily->Get("time");
    if (dmax && dmin && !dmax->arr.empty() && !dmin->arr.empty()) { r.hi_c = Round(&dmax->arr[0]); r.lo_c = Round(&dmin->arr[0]); }
    // The next five hours after the current one (local time strings "2026-11-14T15:00").
    const Json* htime = hourly->Get("time");
    const Json* htemp = hourly->Get("temperature_2m");
    const Json* hcode = hourly->Get("weather_code");
    const Json* ctime = cur->Get("time");
    if (htime && htemp && hcode && ctime && ctime->str.size() >= 13) {
        const std::string now_hour = ctime->str.substr(0, 13);
        size_t i = 0;
        while (i < htime->arr.size() && htime->arr[i].str.substr(0, 13) <= now_hour) ++i;
        for (size_t n = 0; n < 12 && i + n < htime->arr.size() && i + n < htemp->arr.size() && i + n < hcode->arr.size(); ++n) {
            const std::string& t = htime->arr[i + n].str;
            const int h = t.size() >= 13 ? std::atoi(t.c_str() + 11) : 0;
            r.hours.push_back({std::to_wstring(h) + L"时", Round(&hcode->arr[i + n]), Round(&htemp->arr[i + n])});
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
}

SystemSample SystemSampler::Sample() {
    SystemSample s;
    int utility = -1;
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
                                                       nullptr, &v) == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS)
            utility = static_cast<int>(std::clamp(std::lround(v.doubleValue), 0L, 100L));
        pdh_primed_ = true;
    }
    FILETIME idle{}, kern{}, user{};
    if (GetSystemTimes(&idle, &kern, &user)) {
        auto u64 = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const uint64_t i = u64(idle), k = u64(kern), u = u64(user);
        if (primed_) {
            const uint64_t di = i - idle_, dt = (k - kernel_) + (u - user_);  // kernel time includes idle
            if (dt > 0) s.cpu_pct = static_cast<int>(std::clamp<int64_t>(100 - static_cast<int64_t>(di * 100 / dt), 0, 100));
        }
        idle_ = i; kernel_ = k; user_ = u;
    }
    if (utility >= 0) s.cpu_pct = utility;
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) { s.mem_total = ms.ullTotalPhys; s.mem_used = ms.ullTotalPhys - ms.ullAvailPhys; }
    ULARGE_INTEGER avail{}, total{}, free_total{};
    if (GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &free_total) && total.QuadPart) {
        s.disk_free = avail.QuadPart;
        s.disk_used_pct = static_cast<int>((total.QuadPart - free_total.QuadPart) * 100 / total.QuadPart);
    }
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table) {
        uint64_t bytes = 0;
        for (ULONG n = 0; n < table->NumEntries; ++n) {
            const auto& row = table->Table[n];
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp || !row.InterfaceAndOperStatusFlags.HardwareInterface) continue;
            bytes += row.InOctets;
        }
        FreeMibTable(table);
        const uint64_t now = GetTickCount64();
        if (primed_ && now > net_tick_ && bytes >= net_bytes_) s.net_down_bps = (bytes - net_bytes_) * 1000 / (now - net_tick_);
        net_bytes_ = bytes; net_tick_ = now;
    }
    primed_ = true;
    s.valid = true;
    return s;
}

} // namespace pulse::desktop::live
