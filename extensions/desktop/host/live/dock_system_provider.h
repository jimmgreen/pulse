// dock_system_provider.h - Live Windows system status & tray bridge for Liquid Glass Dock .dk-sys.
// Provides real Core Audio volume/devices, dynamic wlanapi Wi-Fi, Bluetooth + battery,
// GetSystemPowerStatus + PowrProf power plans, live IME/keyboard layouts, and Windows tray icons,
// polled asynchronously on a background worker so the desktop UI thread never blocks.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "host/live/widget_live_extras.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::desktop::live {

struct DockTrayApp {
    std::wstring key;          // lowercase stable key (exe path or normalized title)
    std::wstring title;        // tooltip / app display name
    std::wstring exe_path;     // process exe path for icon extraction
    HWND hwnd = nullptr;       // callback window or main window
    UINT callback_msg = 0;     // NOTIFYICONDATA callback message when from toolbar
    UINT icon_uid = 0;         // tray icon uID
    uint32_t pid = 0;
    int badge_count = 0;       // unread count (0 = none)
    bool pinned = false;       // pinned directly inside Dock .dk-sys
    uint32_t accent_argb = 0;  // fallback squircle color
    bool from_toolbar = false; // true when read from Shell_TrayWnd / NotifyIconOverflowWindow
    std::wstring uia_id;       // Windows 11 tray button AutomationId (NotifyItemIcon...), for Invoke
    bool operator==(const DockTrayApp&) const noexcept = default;
};

struct DockImeLayout {
    uint64_t hkl_raw = 0;      // uintptr_t(HKL)
    uint32_t lang_id = 0;
    std::wstring short_code;   // L"中", L"英", L"日", etc.
    std::wstring name;         // e.g. L"中文 (简体，中国)"
    std::wstring sub;          // e.g. L"微软拼音" / L"美式键盘"
    bool active = false;
    bool operator==(const DockImeLayout&) const noexcept = default;
};

struct DockWifiNetwork {
    std::wstring ssid;
    int signal_pct = 0;        // 0..100
    int bars = 0;              // 0..3
    bool secured = false;
    bool connected = false;
    bool has_profile = false;
    std::wstring profile_name;
    bool operator==(const DockWifiNetwork&) const noexcept = default;
};

struct DockAudioDevice {
    std::wstring id;           // Core Audio endpoint ID
    std::wstring name;         // Friendly device name
    bool is_default = false;
    bool operator==(const DockAudioDevice&) const noexcept = default;
};

struct DockBluetoothDevice {
    std::wstring name;
    int battery_pct = -1;      // -1 unknown, 0..100
    uint8_t kind = 0;          // 0=generic, 1=audio/headset, 2=mouse, 3=keyboard, 4=controller
    bool connected = false;
    uint64_t address = 0;
    bool operator==(const DockBluetoothDevice&) const noexcept = default;
};

enum class DockNetworkKind : uint8_t {
    Disconnected = 0,
    Wifi = 1,
    Ethernet = 2,
};

struct DockSystemSnapshot {
    bool valid = false;

    // 1. Tray Icons
    std::vector<DockTrayApp> tray_apps;

    // 2. IME / Input Language
    bool ime_chinese = false;
    std::wstring ime_badge = L"英";
    std::vector<DockImeLayout> ime_layouts;

    // 3. Network (Adaptive Ethernet / Wi-Fi / Disconnected)
    DockNetworkKind network_kind = DockNetworkKind::Disconnected;
    bool ethernet_connected = false;
    std::wstring ethernet_name;     // e.g. L"以太网 2"
    std::wstring ethernet_desc;     // e.g. L"Realtek Gaming 2.5GbE Family Controller"
    std::wstring ethernet_speed;    // e.g. L"1 Gbps" / L"2.5 Gbps"
    std::wstring ethernet_ipv4;     // e.g. L"192.168.1.102"

    bool wifi_available = false;
    bool wifi_enabled = false;
    bool wifi_connected = false;
    std::wstring wifi_ssid;
    int wifi_bars = 0;         // 0..3
    int wifi_signal_pct = 0;
    std::vector<DockWifiNetwork> wifi_networks;

    // 4. Master Volume & Audio Endpoints
    bool audio_available = false;
    int volume_pct = 50;       // 0..100
    bool muted = false;
    std::wstring default_audio_name;
    std::vector<DockAudioDevice> audio_devices;

    // 5. Bluetooth
    bool bt_available = false;
    bool bt_enabled = false;
    std::vector<DockBluetoothDevice> bt_devices;

    // 6. Battery & Power Plan (hidden on desktop PCs when !has_battery)
    bool has_battery = false;
    int battery_pct = 100;
    bool battery_charging = false;
    bool ac_online = true;
    int battery_remaining_min = -1;
    uint8_t power_mode = 1;    // 0=节能, 1=平衡, 2=性能
    std::wstring power_plan_name = L"平衡";

    // 7. Clock & Notification summary
    std::wstring time_text;    // HH:MM or h:MM
    std::wstring date_text;    // M/D
    int notification_count = 0;

    bool operator==(const DockSystemSnapshot&) const noexcept = default;
};

// Direct synchronous helpers (safe to invoke from worker or lightweight user actions).
std::wstring SanitizeTrayTitle(std::wstring_view raw);
std::wstring ExpandNotifyIconExecutablePath(std::wstring_view raw);

bool SetMasterAudioVolume(int pct) noexcept;
bool StepMasterAudioVolume(int delta_pct, int& out_pct, bool& out_muted) noexcept;
bool ToggleMasterAudioMute(int& out_pct, bool& out_muted) noexcept;
bool SetDefaultAudioOutputDevice(const std::wstring& device_id) noexcept;

bool SetSystemPowerMode(uint8_t mode) noexcept; // 0=节能, 1=平衡, 2=性能
bool ToggleImeChineseEnglish(HWND target_hwnd) noexcept;
bool SwitchKeyboardLayout(HWND target_hwnd, uint64_t hkl_raw) noexcept;

bool ConnectWifiNetwork(const std::wstring& ssid) noexcept;
bool DisconnectWifiNetwork() noexcept;

bool ActivateDockTrayApp(const DockTrayApp& app, bool right_click, POINT screen_pt) noexcept;
// The window a click on a tray app should bring up: of `pid` or any process running `exe_path`.
// out_score: 3 = on screen, 2 = minimized, 1 = hidden main window (closed to the tray), 0 = none.
// IME / tooltip / message-only helpers, tool windows and owned popups never qualify.
HWND FindDockTrayAppWindow(DWORD pid, const std::wstring& exe_path, int* out_score) noexcept;
void OpenWindowsSettingsUri(const wchar_t* uri) noexcept;
void OpenWindowsNotificationCenter() noexcept;

// Background monitor that samples all Dock .dk-sys metrics off the UI thread.
class DockSystemMonitor {
public:
    DockSystemMonitor();
    ~DockSystemMonitor();
    DockSystemMonitor(const DockSystemMonitor&) = delete;
    DockSystemMonitor& operator=(const DockSystemMonitor&) = delete;

    void Start(HWND notify_hwnd, UINT notify_msg);
    void Stop() noexcept;
    void SetEnabled(bool enabled) noexcept;
    void SetClock24h(bool clock_24h) noexcept;
    void SetPinnedTrayKeys(const std::vector<std::wstring>& keys);
    void SetForegroundHint(HWND hwnd) noexcept;
    void RequestRefresh(bool include_slow = true) noexcept;
    DockSystemSnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::desktop::live
