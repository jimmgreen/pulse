// widget_live_extras.cpp - Live WinRT GSMTC media monitor, battery/Bluetooth power probe,
// and exact countdown / calendar time-progress calculator.
#include "host/live/widget_live_extras.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <setupapi.h>
#include <devpkey.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace pulse::desktop::live {
namespace {

uint64_t Fnv1a64(const uint8_t* data, size_t len) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

std::wstring ToLowerCopy(std::wstring_view s) {
    std::wstring out(s);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

std::wstring FriendlyMediaSourceName(std::wstring_view raw_aumid) {
    if (raw_aumid.empty()) return L"媒体";
    const std::wstring lo = ToLowerCopy(raw_aumid);
    if (lo.find(L"cloudmusic") != std::wstring::npos) return L"网易云音乐";
    if (lo.find(L"qqmusic") != std::wstring::npos) return L"QQ 音乐";
    if (lo.find(L"kugou") != std::wstring::npos) return L"酷狗音乐";
    if (lo.find(L"spotify") != std::wstring::npos) return L"Spotify";
    if (lo.find(L"applemusic") != std::wstring::npos || lo.find(L"itunes") != std::wstring::npos) return L"Apple Music";
    if (lo.find(L"msedge") != std::wstring::npos) return L"Edge";
    if (lo.find(L"chrome") != std::wstring::npos) return L"Chrome";
    if (lo.find(L"firefox") != std::wstring::npos) return L"Firefox";
    if (lo.find(L"bilibili") != std::wstring::npos) return L"哔哩哔哩";
    if (lo.find(L"potplayer") != std::wstring::npos) return L"PotPlayer";
    if (lo.find(L"foobar") != std::wstring::npos) return L"foobar2000";
    if (lo.find(L"vlc") != std::wstring::npos) return L"VLC";
    if (lo.find(L"zune") != std::wstring::npos || lo.find(L"mediaplayer") != std::wstring::npos) return L"媒体播放器";

    std::wstring clean(raw_aumid);
    if (const size_t slash = clean.find_last_of(L"\\/"); slash != std::wstring::npos) {
        clean = clean.substr(slash + 1);
    }
    if (const size_t bang = clean.find(L'!'); bang != std::wstring::npos) {
        clean = clean.substr(0, bang);
    }
    if (const size_t dot = clean.rfind(L".exe"); dot != std::wstring::npos && dot + 4 == clean.size()) {
        clean.resize(dot);
    }
    if (clean.size() > 18) clean.resize(18);
    return clean.empty() ? L"媒体" : clean;
}

bool MediaSnapshotsEqual(const MediaSessionSnapshot& a, const MediaSessionSnapshot& b) noexcept {
    return a.active == b.active &&
           a.playing == b.playing &&
           a.title == b.title &&
           a.artist == b.artist &&
           a.album == b.album &&
           a.source_app == b.source_app &&
           a.cover_hash == b.cover_hash;
}

uint8_t ClassifyBluetoothDevice(std::wstring_view name) noexcept {
    const std::wstring lo = ToLowerCopy(name);
    if (lo.find(L"airpod") != std::wstring::npos || lo.find(L"buds") != std::wstring::npos ||
        lo.find(L"headphone") != std::wstring::npos || lo.find(L"headset") != std::wstring::npos ||
        lo.find(L"wh-") != std::wstring::npos || lo.find(L"wf-") != std::wstring::npos ||
        lo.find(L"bose") != std::wstring::npos || lo.find(L"beats") != std::wstring::npos ||
        lo.find(L"ear") != std::wstring::npos || lo.find(L"耳机") != std::wstring::npos ||
        lo.find(L"音箱") != std::wstring::npos || lo.find(L"speaker") != std::wstring::npos) {
        return 1; // Audio
    }
    if (lo.find(L"mouse") != std::wstring::npos || lo.find(L"鼠标") != std::wstring::npos ||
        lo.find(L"mx master") != std::wstring::npos || lo.find(L"anywhere") != std::wstring::npos ||
        lo.find(L"viper") != std::wstring::npos || lo.find(L"g pro") != std::wstring::npos) {
        return 2; // Mouse
    }
    if (lo.find(L"keyboard") != std::wstring::npos || lo.find(L"键盘") != std::wstring::npos ||
        lo.find(L"keychron") != std::wstring::npos || lo.find(L"hhkb") != std::wstring::npos ||
        lo.find(L"mx keys") != std::wstring::npos) {
        return 3; // Keyboard
    }
    if (lo.find(L"xbox") != std::wstring::npos || lo.find(L"controller") != std::wstring::npos ||
        lo.find(L"dualsense") != std::wstring::npos || lo.find(L"dualshock") != std::wstring::npos ||
        lo.find(L"手柄") != std::wstring::npos) {
        return 4; // Gamepad
    }
    return 0;
}

std::wstring QueryActivePowerPlanName() {
    using PowerGetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
    static HMODULE mod = LoadLibraryW(L"powrprof.dll");
    static auto fn = mod ? reinterpret_cast<PowerGetActiveSchemeFn>(GetProcAddress(mod, "PowerGetActiveScheme")) : nullptr;
    if (!fn) return L"平衡";

    GUID* scheme = nullptr;
    if (fn(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return L"平衡";

    // Standard Windows Power Scheme GUIDs
    static constexpr GUID kPowerSaver = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
    static constexpr GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
    static constexpr GUID kHighPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
    static constexpr GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

    std::wstring name = L"自定义";
    if (IsEqualGUID(*scheme, kBalanced)) name = L"平衡";
    else if (IsEqualGUID(*scheme, kHighPerf)) name = L"高性能";
    else if (IsEqualGUID(*scheme, kPowerSaver)) name = L"节能";
    else if (IsEqualGUID(*scheme, kUltimate)) name = L"卓越性能";

    LocalFree(scheme);
    return name;
}

std::vector<BatteryDeviceInfo> QueryBluetoothBatteries() {
    std::vector<BatteryDeviceInfo> out;
    using SetupDiGetClassDevsWFn = HDEVINFO(WINAPI*)(const GUID*, PCWSTR, HWND, DWORD);
    using SetupDiEnumDeviceInfoFn = BOOL(WINAPI*)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
    using SetupDiGetDevicePropertyWFn = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY*, DEVPROPTYPE*, PBYTE, DWORD, PDWORD, DWORD);
    using SetupDiDestroyDeviceInfoListFn = BOOL(WINAPI*)(HDEVINFO);

    static HMODULE mod = LoadLibraryW(L"setupapi.dll");
    static auto fn_get_devs = mod ? reinterpret_cast<SetupDiGetClassDevsWFn>(GetProcAddress(mod, "SetupDiGetClassDevsW")) : nullptr;
    static auto fn_enum = mod ? reinterpret_cast<SetupDiEnumDeviceInfoFn>(GetProcAddress(mod, "SetupDiEnumDeviceInfo")) : nullptr;
    static auto fn_prop = mod ? reinterpret_cast<SetupDiGetDevicePropertyWFn>(GetProcAddress(mod, "SetupDiGetDevicePropertyW")) : nullptr;
    static auto fn_destroy = mod ? reinterpret_cast<SetupDiDestroyDeviceInfoListFn>(GetProcAddress(mod, "SetupDiDestroyDeviceInfoList")) : nullptr;
    if (!fn_get_devs || !fn_enum || !fn_prop || !fn_destroy) return out;

    // DEVPKEY_Bluetooth_Battery = {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, pid 2 (BYTE 0..100)
    static constexpr DEVPROPKEY kBtBatteryKey = {
        {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
    // DEVPKEY_Device_IsConnected = {83DA6326-97A6-4088-9453-A1923F573B29}, pid 15 (DEVPROP_BOOLEAN)
    static constexpr DEVPROPKEY kDevConnectedKey = {
        {0x83da6326, 0x97a6, 0x4088, {0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29}}, 15};
    // DEVPKEY_NAME = {B725F130-47EF-101A-A5F1-02608C9EEBAC}, pid 10 (DEVPROP_TYPE_STRING)
    static constexpr DEVPROPKEY kDevNameKey = {
        {0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}}, 10};
    // GUID_DEVCLASS_BLUETOOTH = {e0cbf06c-cd8b-4647-bb8a-263b43f0f974}
    static constexpr GUID kBtClassGuid = {0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};

    HDEVINFO hdev = fn_get_devs(&kBtClassGuid, nullptr, nullptr, DIGCF_PRESENT);
    if (hdev == INVALID_HANDLE_VALUE) return out;

    std::unordered_set<std::wstring> seen_names;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD idx = 0; fn_enum(hdev, idx, &dev) && out.size() < 4; ++idx) {
        DEVPROPTYPE pt = 0;
        BYTE pct = 0;
        if (!fn_prop(hdev, &dev, &kBtBatteryKey, &pt, &pct, sizeof(pct), nullptr, 0)) continue;
        if (pt != DEVPROP_TYPE_BYTE || pct > 100u) continue;

        DEVPROP_BOOLEAN conn = DEVPROP_FALSE;
        DEVPROPTYPE cpt = 0;
        if (fn_prop(hdev, &dev, &kDevConnectedKey, &cpt, reinterpret_cast<PBYTE>(&conn), sizeof(conn), nullptr, 0) &&
            cpt == DEVPROP_TYPE_BOOLEAN && conn == DEVPROP_FALSE) {
            continue;
        }

        wchar_t name_buf[128]{};
        DEVPROPTYPE npt = 0;
        if (!fn_prop(hdev, &dev, &kDevNameKey, &npt, reinterpret_cast<PBYTE>(name_buf), sizeof(name_buf), nullptr, 0) ||
            name_buf[0] == L'\0') {
            continue;
        }
        std::wstring clean_name(name_buf);
        // Strip common Windows Hands-Free / Battery Service suffixes for clean macOS-style names.
        for (const wchar_t* suf : {L" Hands-Free AG Audio", L" Hands-Free", L" Avrcp Transport", L" Battery Service"}) {
            if (const size_t p = clean_name.find(suf); p != std::wstring::npos && p >= 2) {
                clean_name.resize(p);
            }
        }
        const std::wstring key = ToLowerCopy(clean_name);
        if (!seen_names.insert(key).second) continue;

        BatteryDeviceInfo info;
        info.name = std::move(clean_name);
        info.percent = static_cast<int>(pct);
        info.kind = ClassifyBluetoothDevice(info.name);
        out.push_back(std::move(info));
    }

    fn_destroy(hdev);
    return out;
}

int64_t CivilDaysFromYmd(int y, int m, int d) noexcept {
    // Howard Hinnant's exact civil-from-days formula (valid across Gregorian leap years).
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (static_cast<unsigned>(m) + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u +
                         static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool IsLeapYear(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int DaysInMonth(int y, int m) noexcept {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    return (m == 2 && IsLeapYear(y)) ? 29 : kDays[m - 1];
}

} // namespace

// ---------------------------------------------------------------------------
// MediaSessionMonitor (WinRT GSMTC background worker)
// ---------------------------------------------------------------------------
struct MediaSessionMonitor::Impl {
    HWND notify_hwnd = nullptr;
    UINT notify_msg = 0;

    mutable std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
    bool enabled = false;
    bool refresh_requested = false;
    std::deque<MediaCommand> cmd_queue;
    MediaSessionSnapshot latest{};
    std::thread worker;

    void RunLoop() noexcept {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        namespace wmc = winrt::Windows::Media::Control;
        namespace wss = winrt::Windows::Storage::Streams;

        wmc::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        bool mgr_tried = false;

        auto ensure_mgr = [&]() -> bool {
            if (mgr) return true;
            if (mgr_tried) return false;
            mgr_tried = true;
            try {
                mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                return static_cast<bool>(mgr);
            } catch (...) {
                return false;
            }
        };

        auto pick_session = [&]() -> wmc::GlobalSystemMediaTransportControlsSession {
            if (!ensure_mgr()) return nullptr;
            try {
                auto cur = mgr.GetCurrentSession();
                if (cur) return cur;
                auto list = mgr.GetSessions();
                if (list && list.Size() > 0) return list.GetAt(0);
            } catch (...) {}
            return nullptr;
        };

        auto exec_command = [&](MediaCommand cmd) {
            bool dispatched = false;
            if (auto sess = pick_session()) {
                try {
                    switch (cmd) {
                    case MediaCommand::PlayPause:
                        dispatched = sess.TryTogglePlayPauseAsync().get();
                        break;
                    case MediaCommand::Next:
                        dispatched = sess.TrySkipNextAsync().get();
                        break;
                    case MediaCommand::Previous:
                        dispatched = sess.TrySkipPreviousAsync().get();
                        break;
                    }
                } catch (...) {}
            }
            if (!dispatched) {
                const WORD vk = (cmd == MediaCommand::Next) ? VK_MEDIA_NEXT_TRACK
                              : (cmd == MediaCommand::Previous) ? VK_MEDIA_PREV_TRACK
                                                                : VK_MEDIA_PLAY_PAUSE;
                INPUT in[2]{};
                in[0].type = INPUT_KEYBOARD;
                in[0].ki.wVk = vk;
                in[1].type = INPUT_KEYBOARD;
                in[1].ki.wVk = vk;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
        };

        std::wstring last_cover_key;
        std::vector<uint8_t> cached_cover;
        uint64_t cached_cover_hash = 0;

        auto poll_once = [&]() {
            MediaSessionSnapshot snap{};
            if (auto sess = pick_session()) {
                try {
                    const auto pb = sess.GetPlaybackInfo();
                    const auto status = pb.PlaybackStatus();
                    snap.active = (status != wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed);
                    snap.playing = (status == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    snap.source_app = FriendlyMediaSourceName(sess.SourceAppUserModelId().c_str());

                    auto props = sess.TryGetMediaPropertiesAsync().get();
                    if (props) {
                        snap.title = props.Title().c_str();
                        snap.artist = props.Artist().c_str();
                        snap.album = props.AlbumTitle().c_str();

                        const std::wstring cover_key = snap.title + L"|" + snap.artist + L"|" + snap.album + L"|" + snap.source_app;
                        if (cover_key == last_cover_key && !cached_cover.empty()) {
                            snap.cover_bytes = cached_cover;
                            snap.cover_hash = cached_cover_hash;
                        } else {
                            last_cover_key = cover_key;
                            cached_cover.clear();
                            cached_cover_hash = 0;
                            if (auto thumb_ref = props.Thumbnail()) {
                                auto stream = thumb_ref.OpenReadAsync().get();
                                if (stream) {
                                    const uint64_t sz = stream.Size();
                                    if (sz > 16 && sz <= (4ull << 20)) {
                                        wss::Buffer buf(static_cast<uint32_t>(sz));
                                        auto read_buf = stream.ReadAsync(buf, static_cast<uint32_t>(sz), wss::InputStreamOptions::None).get();
                                        const uint32_t got = read_buf ? read_buf.Length() : 0u;
                                        if (got > 16) {
                                            wss::DataReader dr = wss::DataReader::FromBuffer(read_buf);
                                            cached_cover.resize(got);
                                            dr.ReadBytes(winrt::array_view<uint8_t>(cached_cover));
                                            cached_cover_hash = Fnv1a64(cached_cover.data(), cached_cover.size());
                                            snap.cover_bytes = cached_cover;
                                            snap.cover_hash = cached_cover_hash;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (snap.title.empty() && snap.artist.empty() && !snap.playing) {
                        snap.active = false;
                    }
                } catch (...) {}
            }

            bool notify = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!MediaSnapshotsEqual(latest, snap)) {
                    latest = std::move(snap);
                    notify = true;
                }
            }
            if (notify && notify_hwnd && notify_msg && IsWindow(notify_hwnd)) {
                PostMessageW(notify_hwnd, notify_msg, 0, 0);
            }
        };

        while (true) {
            std::deque<MediaCommand> cmds;
            bool do_poll = false;
            {
                std::unique_lock<std::mutex> lk(mtx);
                if (!enabled && cmd_queue.empty()) {
                    cv.wait(lk, [this] { return stop || enabled || !cmd_queue.empty(); });
                } else {
                    cv.wait_for(lk, std::chrono::milliseconds(1500),
                                [this] { return stop || refresh_requested || !cmd_queue.empty(); });
                }
                if (stop) break;
                cmds.swap(cmd_queue);
                do_poll = enabled || refresh_requested || !cmds.empty();
                refresh_requested = false;
            }
            for (const auto c : cmds) {
                exec_command(c);
            }
            if (!cmds.empty()) {
                // Give the player ~160ms to update playback state / next track before reading back.
                std::this_thread::sleep_for(std::chrono::milliseconds(160));
            }
            if (do_poll) {
                poll_once();
            }
        }
        winrt::uninit_apartment();
    }
};

MediaSessionMonitor::MediaSessionMonitor() : impl_(std::make_unique<Impl>()) {}
MediaSessionMonitor::~MediaSessionMonitor() { Stop(); }

void MediaSessionMonitor::Start(HWND notify_hwnd, UINT notify_msg) {
    if (!impl_) return;
    Stop();
    impl_->notify_hwnd = notify_hwnd;
    impl_->notify_msg = notify_msg;
    impl_->stop = false;
    impl_->worker = std::thread([this]() { impl_->RunLoop(); });
}

void MediaSessionMonitor::Stop() noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void MediaSessionMonitor::SetEnabled(bool enabled) noexcept {
    if (!impl_) return;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->enabled != enabled) {
            impl_->enabled = enabled;
            if (enabled) impl_->refresh_requested = true;
            wake = true;
        }
    }
    if (wake) impl_->cv.notify_all();
}

void MediaSessionMonitor::RequestRefresh() noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->refresh_requested = true;
    }
    impl_->cv.notify_all();
}

void MediaSessionMonitor::SendCommand(MediaCommand cmd) noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->cmd_queue.size() < 4) impl_->cmd_queue.push_back(cmd);
    }
    impl_->cv.notify_all();
}

MediaSessionSnapshot MediaSessionMonitor::Snapshot() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->latest;
}

// ---------------------------------------------------------------------------
// Battery & Power Sampler
// ---------------------------------------------------------------------------
BatteryPowerSnapshot SampleBatteryPower(bool include_bluetooth) {
    BatteryPowerSnapshot s{};
    s.valid = true;
    s.uptime_sec = GetTickCount64() / 1000ull;
    s.power_plan = QueryActivePowerPlanName();

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        const bool no_batt = (sps.BatteryFlag == 128) || (sps.BatteryFlag == 255) || (sps.BatteryLifePercent == 255);
        s.has_battery = !no_batt;
        s.ac_online = (sps.ACLineStatus == 1);
        s.charging = ((sps.BatteryFlag & 8) != 0) || (s.has_battery && s.ac_online && sps.BatteryLifePercent < 100);
        s.power_saver = (sps.SystemStatusFlag != 0);
        s.battery_pct = s.has_battery ? std::clamp<int>(static_cast<int>(sps.BatteryLifePercent), 0, 100) : 100;
        if (s.has_battery && !s.ac_online && sps.BatteryLifeTime != static_cast<DWORD>(-1) && sps.BatteryLifeTime > 0) {
            s.remaining_min = static_cast<int>(sps.BatteryLifeTime / 60u);
        }
    }

    if (include_bluetooth) {
        s.bt_devices = QueryBluetoothBatteries();
    }
    return s;
}

// ---------------------------------------------------------------------------
// Countdown & Calendar Progress Math
// ---------------------------------------------------------------------------
bool ParseIsoDate(const std::wstring& iso, int& year, int& month, int& day) noexcept {
    int y = 0, m = 0, d = 0;
    if (swscanf_s(iso.c_str(), L"%d-%d-%d", &y, &m, &d) != 3 &&
        swscanf_s(iso.c_str(), L"%d/%d/%d", &y, &m, &d) != 3 &&
        swscanf_s(iso.c_str(), L"%d.%d.%d", &y, &m, &d) != 3) {
        return false;
    }
    if (y < 1970 || y > 2200 || m < 1 || m > 12) return false;
    const int dim = DaysInMonth(y, m);
    if (d < 1 || d > dim) return false;
    year = y;
    month = m;
    day = d;
    return true;
}

CountdownComputed ComputeCountdown(const std::wstring& title,
                                   const std::wstring& target_iso,
                                   const SYSTEMTIME& now) {
    CountdownComputed out{};
    const int cur_y = std::clamp<int>(now.wYear, 1970, 2200);
    const int cur_m = std::clamp<int>(now.wMonth, 1, 12);
    const int cur_d = std::clamp<int>(now.wDay, 1, DaysInMonth(cur_y, cur_m));

    int ty = cur_y + 1, tm = 1, td = 1;
    const bool parsed = ParseIsoDate(target_iso, ty, tm, td);
    if (!parsed) {
        out.title = title.empty() || title == L"倒数日"
                        ? (std::to_wstring(ty) + L" 元旦")
                        : title;
    } else {
        out.title = title.empty() ? L"目标日" : title;
    }

    wchar_t iso_buf[24]{};
    std::swprintf(iso_buf, ARRAYSIZE(iso_buf), L"%04d-%02d-%02d", ty, tm, td);
    out.target_iso = iso_buf;

    const int64_t today_serial = CivilDaysFromYmd(cur_y, cur_m, cur_d);
    const int64_t target_serial = CivilDaysFromYmd(ty, tm, td);
    out.days_delta = static_cast<int>(target_serial - today_serial);
    out.is_today = (out.days_delta == 0);
    out.is_past = (out.days_delta < 0);

    // Real-time progress bars (Today / Week Mon-Sun / Month / Year)
    const float sec_today = static_cast<float>(now.wHour * 3600 + now.wMinute * 60 + now.wSecond);
    out.day_pct = std::clamp(sec_today * 100.0f / 86400.0f, 0.0f, 100.0f);

    const int mon_idx = (now.wDayOfWeek == 0) ? 6 : (static_cast<int>(now.wDayOfWeek) - 1);
    out.week_pct = std::clamp((static_cast<float>(mon_idx) + out.day_pct * 0.01f) * 100.0f / 7.0f, 0.0f, 100.0f);

    const int dim = DaysInMonth(cur_y, cur_m);
    out.month_pct = std::clamp((static_cast<float>(cur_d - 1) + out.day_pct * 0.01f) * 100.0f / static_cast<float>(dim),
                               0.0f, 100.0f);

    const int64_t jan1 = CivilDaysFromYmd(cur_y, 1, 1);
    const int doy = static_cast<int>(today_serial - jan1);
    const int diy = IsLeapYear(cur_y) ? 366 : 365;
    out.year_pct = std::clamp((static_cast<float>(doy) + out.day_pct * 0.01f) * 100.0f / static_cast<float>(diy),
                              0.0f, 100.0f);
    return out;
}

} // namespace pulse::desktop::live
