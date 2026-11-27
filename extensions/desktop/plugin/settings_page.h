// settings_page.h - Native PulseSettingsPagesV1 + PulseResourcesV1 settings controller
// for pulse.desktop using public C ABI headers directly (zero client.hpp dependency).
#pragma once
#include "ipc/widget_prefs.h"
#include "glass_options.h"
#include <pulse/abi.h>
#include <pulse/resources.h>
#include <pulse/settings_pages.h>
#include <pulse/ui_events.h>
#include <pulse/ui_model.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop::plugin {

struct DesktopPrefs {
    bool enabled = true;
    bool hybrid_menu = true;       // 桌面右键使用 Pulse 菜单
    bool organize = true;          // 桌面整理
    bool tiles = true;             // 磁贴
    bool live_tiles = true;        // 动态磁贴
    bool type_to_search = true;    // 在桌面上直接打字搜索
    bool sync_tray = true;         // 桌面上的暂存区
    bool double_click_hide = true; // 双击空白处隐藏图标
    bool health_check = true;      // 桌面体检
    uint8_t material = 1;          // 0=FollowHost, 1=Acrylic, 2=Mica, 3=MicaAlt, 4=Solid("无"), 5=LiquidGlass("液态玻璃")
    uint8_t icon_size = 1;         // 0=Small, 1=Medium, 2=Large
    uint8_t scene = 0;             // 0=Work, 1=Focus, 2=Present
    uint8_t label_style = 0;       // 0=Shadow, 1=FrostedPill, 2=HiddenUntilHover, 3=Normal
    uint8_t opacity_percent = 60;  // 0..100
    uint8_t glass_preset = 5;      // 0=ClearDroplet, 1=AppleRegular, 2=DeepPrism, 3=SatinGel, 4=Custom
    uint8_t glass_thickness = 20;  // 6..64 DIP meniscus bezel width
    uint16_t glass_ior_x100 = 140; // 105..240 refractive index * 100
    uint8_t glass_dispersion = 7;  // 0..35 chromatic dispersion gain
    uint8_t glass_glare = 90;      // 0..120 dual-lobe glare %
    int16_t glass_glare_angle = -45; // -180..180 glare azimuth deg
    uint8_t glass_frost = 4;       // 0..100 internal blur mix %
    bool glass_preview = false; // Runtime-only drag transaction marker, never user preference.
    GlassOpticalSettings glass_options{};
    bool glass_reference = true;
    uint8_t glass_blur_radius = 1;
    bool glass_blur_edge = true;
    uint8_t glass_live_rate = 1;   // 0=15fps, 1=30fps, 2=60fps
    uint8_t wallpaper_kind = 0;    // 0=系统壁纸, 1=视频壁纸
    uint8_t video_fill = 0;        // 0=铺满(Fill), 1=适应(Fit)
    bool auto_wallpaper_accent = true;
    uint32_t accent_argb = 0xFF4CC2FFu;
    bool icons_hidden = false;
    bool auto_sleep = true;
    bool restore_on_exit = true;   // 关闭或卸载时恢复 Windows 桌面
    std::wstring weather_city;     // 天气小组件的城市（Open-Meteo 地理编码）
    WidgetPrefs widgets;           // 小组件 options (ipc/widget_prefs.h)
    DockPrefs dock;                // 程序坞 Dock options (ipc/widget_prefs.h)

    bool operator==(const DesktopPrefs&) const noexcept = default;
    void ApplyGlassPreset(uint8_t preset) noexcept;
    void ApplyJson(std::string_view json);
    std::string ToJson() const;
    std::string ToPersistedJson() const;
    static DesktopPrefs FromJson(std::string_view json);
};

struct ModelArena;

class DesktopSettingsPage {
public:
    using ChangedCallback = std::function<void(const DesktopPrefs& prefs, bool enabled_toggled)>;
    using CommandCallback = std::function<void(std::string_view command_id)>;

    DesktopSettingsPage() = default;
    ~DesktopSettingsPage();
    DesktopSettingsPage(const DesktopSettingsPage&) = delete;
    DesktopSettingsPage& operator=(const DesktopSettingsPage&) = delete;

    PulseStatus Open(const PulseHostV1* host,
                     ChangedCallback on_changed,
                     CommandCallback on_command,
                     bool ai_organize_enabled = true);
    void Close() noexcept;

    const DesktopPrefs& Prefs() const noexcept { return prefs_; }
    void UpdateFromHostProcess(const DesktopPrefs& updated);
    void SetStatusText(std::string_view utf8_status);
    void SetVideoWallpaperStatus(std::string_view file_utf8, std::string_view status_utf8);
    bool SetAiOrganizerEnabled(bool enabled);

private:
    void LoadPersisted();
    void SavePersisted();
    void PumpPersisted();
    void PushStatesToUi();

    static void PULSE_CALL OnResourcesCompleted(void* user, const PulseResourcesReply* reply) noexcept;
    static void PULSE_CALL OnUiStateChanged(void* user, uint64_t panel, const PulseUiStateBatch* batch) noexcept;
    static void PULSE_CALL OnUiCommandClicked(void* user, uint64_t panel, PulseStringView command) noexcept;

    void HandleResourcesReply(const PulseResourcesReply& reply);
    void HandleStateBatch(const PulseUiStateBatch& batch);
    // 小组件 swatches and world-clock chips are command controls whose look follows the prefs.
    bool HandleWidgetCommand(std::string_view id);

    PulseSettingsPagesV1 pages_{};
    PulseUiModelV1 ui_model_{};
    PulseUiEventsV1 ui_events_{};
    PulseResourcesV1 resources_{};
    bool has_pages_ = false;
    bool has_resources_ = false;
    bool ai_organize_enabled_ = true;

    uint64_t page_id_ = 0;
    void BuildTree(ModelArena& a, PulseUiModelTree& tree);
    void RebuildTree();
    int widget_tab_ = 0;           // 小组件 › which widget's options show (not persisted)
    uint64_t state_sub_ = 0;
    std::vector<uint64_t> cmd_subs_;

    uint64_t settings_version_ = 0;
    PulseResourcesRequestId pending_req_ = 0;
    bool loading_ = false;
    bool saving_ = false;
    bool dirty_while_busy_ = false;
    bool pumping_persistence_ = false;
    bool reload_requested_ = false;
    bool save_requested_ = false;

    DesktopPrefs prefs_{};
    std::string video_file_name_;
    std::string video_status_text_ = "未选择视频 · 支持 MP4、MOV、MKV";
    uint64_t last_glass_preview_ms_ = 0;
    DesktopPrefs glass_edit_start_{};
    bool has_glass_edit_start_ = false;
    ChangedCallback on_changed_;
    CommandCallback on_command_;
};

} // namespace pulse::desktop::plugin
