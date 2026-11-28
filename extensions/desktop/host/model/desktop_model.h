// desktop_model.h - Pure C++20 data structures and layout/tile/rule/health/undo engines
// for Pulse Desktop Enhancement Pack (pulse.desktop). Zero Direct2D/COM/UI dependencies
// so 100% of geometry, collision, displacement, rule matching, and undo logic can be
// verified deterministically in pulse_desktop_model_test.
#pragma once
#include "ipc/widget_prefs.h"
#include "glass_options.h"
#include "host/video/video_wallpaper_types.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::model {

// ---------------------------------------------------------------------------
// Visual & Grid Constants (1:1 from desktop_pack_mockup.html)
// ---------------------------------------------------------------------------
inline constexpr int kSnapGridPx = 8;
inline constexpr int kGroupHeaderHeight = 38;
inline constexpr int kGroupFooterHeight = 28;
inline constexpr int kGroupPaddingX = 10;
inline constexpr int kGroupPaddingTop = 8;
inline constexpr int kGroupPaddingBottom = 10;
inline constexpr int kGroupCellW = 86;
inline constexpr int kGroupCellH = 92;
inline constexpr int kGroupMinCols = 2;
inline constexpr int kGroupMaxCols = 7;
inline constexpr int kGroupMinRows = 1;
inline constexpr int kGroupMaxRows = 8;

inline constexpr int kLooseMarginX = 24;
inline constexpr int kLooseMarginY = 20;
inline constexpr int kLooseCellW = 92;
inline constexpr int kLooseCellH = 98;

inline constexpr int kTileCols = 5;
inline constexpr int kTileRows = 7;
inline constexpr int kTileUnitPx = 100;
inline constexpr int kTileGapPx = 8;
inline constexpr int kTileBoardRightMargin = 28;
inline constexpr int kTileBoardTopMargin = 44;
inline constexpr int kTileMinW = 92;
inline constexpr int kTileMinH = 72;
inline constexpr int kTileMaxCols = 12;
inline constexpr int kTileMaxRows = 12;

struct RectI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    constexpr int right() const noexcept { return x + w; }
    constexpr int bottom() const noexcept { return y + h; }
    constexpr bool empty() const noexcept { return w <= 0 || h <= 0; }
    constexpr bool contains(int px, int py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    constexpr bool intersects(const RectI& o, int pad = 0) const noexcept {
        return !(right() + pad <= o.x || o.right() + pad <= x ||
                 bottom() + pad <= o.y || o.bottom() + pad <= y);
    }
    constexpr bool operator==(const RectI&) const noexcept = default;
};

struct PointI {
    int x = 0;
    int y = 0;
    constexpr bool operator==(const PointI&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Appearance, Scene & Preferences
// ---------------------------------------------------------------------------
enum class CardMaterial : uint8_t {
    FollowHost = 0,
    Acrylic = 1,
    Mica = 2,
    MicaAlt = 3,
    Solid = 4,
    LiquidGlass = 5,
};

enum class GlassPreset : uint8_t {
    ClearDroplet = 0, // Clear crystal lens + crisp Snell bezel refraction (Studio default)
    AppleRegular = 1, // Balanced Apple WWDC25 Regular Liquid Glass
    DeepPrism = 2,    // Thick bezel + high IOR + vivid spectral dispersion
    SatinGel = 3,     // Soft frosted gel interior + smooth meniscus rim
    Custom = 4,       // User-tuned optical slider values
};

enum class IconLabelStyle : uint8_t {
    Shadow = 0,
    FrostedPill = 1,
    HiddenUntilHover = 2,
    Normal = 3,
};

enum class DesktopScene : uint8_t {
    Work = 0,     // Full groups + loose icons + 5x7 live tiles
    Focus = 1,    // Only focus_visible group unfolded; other groups & non-keep tiles dimmed
    Present = 2,  // Desktop icons & groups hidden; personal tiles veiled with privacy frost
};

enum class DesktopIconSize : uint8_t {
    Small = 0,    // 74 x 74
    Medium = 1,   // 86 x 92
    Large = 2,    // 104 x 122
};

enum class WallpaperPreset : uint8_t {
    System = 0,   // Live Windows desktop wallpaper
    Bloom = 1,    // Deep sapphire/indigo aurora ("蓝")
    Dusk = 2,     // Warm sunset violet/amber ("暮色")
    Forest = 3,   // Emerald mist ("森林")
    Photo = 4,    // Alpine dusk lake ("照片")
};

enum class GroupSortBy : uint8_t {
    Name = 0,
    ModifiedTime = 1,
    Size = 2,
    Type = 3,
    Manual = 4,
};

constexpr PointI IconCellMetrics(DesktopIconSize sz) noexcept {
    switch (sz) {
        case DesktopIconSize::Small:  return {74, 74};
        case DesktopIconSize::Large:  return {104, 122};
        case DesktopIconSize::Medium:
        default:                      return {kGroupCellW, kGroupCellH};
    }
}

struct BeautifySettings {
    bool dark_mode = true;
    CardMaterial default_material = CardMaterial::Acrylic;
    uint8_t card_opacity_percent = 60; // 0..100; below 30 the card material fades out
    bool auto_wallpaper_accent = true;
    uint32_t accent_argb = 0xFF4CC2FFu;
    WallpaperPreset wallpaper_preset = WallpaperPreset::System;
    IconLabelStyle label_style = IconLabelStyle::Shadow;
    DesktopIconSize icon_size = DesktopIconSize::Medium;
    DesktopScene active_scene = DesktopScene::Work;
    bool organize_enabled = true;
    bool tiles_enabled = true;
    bool live_tiles_enabled = true;
    bool align_to_grid = true;
    GroupSortBy loose_sort_by = GroupSortBy::ModifiedTime;
    bool loose_sort_ascending = false;
    bool double_click_hide_icons = true;
    bool icons_hidden = false;
    bool type_to_search = true;
    bool sync_staging_tray = true;
    bool hybrid_context_menu = true;
    bool health_check_enabled = true;
    bool restore_on_exit = true;
    bool pause_on_fullscreen_or_battery = true;
    uint8_t glass_preset = 5;         // GlassPreset: 0=ClearDroplet, 1=AppleRegular, 2=DeepPrism, 3=SatinGel, 4=Custom
    uint8_t glass_thickness = 20;     // Refraction meniscus bezel thickness in DIP (6..64)
    uint16_t glass_ior_x100 = 140;     // Index of refraction * 100 (105..240 -> 1.05..2.40)
    uint8_t glass_dispersion = 7;     // Chromatic dispersion gain (0..35)
    uint8_t glass_glare = 90;         // Dual-lobe specular glare intensity percent (0..120)
    int16_t glass_glare_angle = -45;  // Primary glare azimuth in degrees (-180..180)
    uint8_t glass_frost = 4;          // Interior frosting mix percent (0..100)
    GlassOpticalSettings glass_options{};
    bool glass_reference = true;
    uint8_t glass_blur_radius = 1;
    bool glass_blur_edge = true;
    uint8_t glass_live_rate = 1;      // 0=Static/15fps (PowerSave), 1=30fps (Standard), 2=60fps (Smooth)
    // macOS 26 Tahoe Liquid Glass Dock (replaces legacy Apps tile on desktop).
    bool dock_enabled = false;        // enabled by default on live host; false in bare unit-test fixtures
    uint8_t dock_icon_size = 48;      // 42 (Small), 48 (Medium), 56 (Large)
    bool dock_magnification = true;
    uint16_t dock_max_scale_x100 = 152; // 132 (Subtle), 152 (Classic), 178 (Bold)
    uint8_t dock_hide_mode = 0;       // 0=AlwaysVisible, 1=SmartHide (window overlap), 2=AutoHide (edge reveal)
    uint8_t dock_minimize_effect = 1; // 0=Off, 1=Genie (curved funnel), 2=Scale (spring scale & slide)
    uint8_t dock_position = 0;        // 0=Bottom, 1=Left, 2=Right
    bool dock_system_area = true;     // Show unified .dk-sys tray & system status capsule on Dock
    // Dock stacks (叠放): how a Dock folder opens when clicked, and its order.
    uint8_t dock_stack_view = 0;      // 0=Automatic, 1=Fan, 2=Grid, 3=List (overlays::DockStackView)
    uint8_t dock_stack_sort = 0;      // 0=Date created, 1=Name, 2=Date modified, 3=Kind
    bool dock_apps_stack = true;      // 应用程序 stack (Start menu apps in a grid) beside 下载
    std::vector<std::wstring> dock_pinned_paths;     // empty => auto-discover default Windows apps
    std::vector<std::wstring> dock_tray_pinned_keys; // tray icons pinned directly inside Dock .dk-sys
    // 天气: the city picked in settings (no location API); coordinates come from Open-Meteo's geocoder.
    std::wstring weather_city;
    WidgetPrefs widgets{};             // 小组件 (plugin settings › 小组件), ipc/widget_prefs.h
    bool operator==(const BeautifySettings&) const noexcept = default;
};

inline DockPrefs ExtractDockPrefs(const BeautifySettings& s) noexcept {
    DockPrefs d{};
    d.enabled = s.dock_enabled;
    d.position = std::min<uint8_t>(s.dock_position, 2);
    d.hide_mode = std::min<uint8_t>(s.dock_hide_mode, 2);
    d.minimize_effect = std::min<uint8_t>(s.dock_minimize_effect, 2);
    d.icon_size = std::clamp<uint8_t>(s.dock_icon_size, 36, 68);
    d.magnification = s.dock_magnification;
    d.max_scale_x100 = std::clamp<uint16_t>(s.dock_max_scale_x100, 100, 195);
    d.system_area = s.dock_system_area;
    return d;
}

inline void ApplyDockPrefs(const DockPrefs& d, BeautifySettings& s) noexcept {
    s.dock_enabled = d.enabled;
    s.dock_position = std::min<uint8_t>(d.position, 2);
    s.dock_hide_mode = std::min<uint8_t>(d.hide_mode, 2);
    s.dock_minimize_effect = std::min<uint8_t>(d.minimize_effect, 2);
    s.dock_icon_size = std::clamp<uint8_t>(d.icon_size, 36, 68);
    s.dock_magnification = d.magnification;
    s.dock_max_scale_x100 = std::clamp<uint16_t>(d.max_scale_x100, 100, 195);
    s.dock_system_area = d.system_area;
}

inline uint8_t GlassPresetToModeIndex(uint8_t preset) noexcept {
    // Maps persisted glass_preset IDs onto the 4 curated Liquid Glass UI modes:
    // 0=清透水滴 (5/0), 1=标准液态 (1), 2=柔雾凝脂 (3), 3=流光棱镜 (2).
    if (preset == 1) return 1;
    if (preset == 3) return 2;
    if (preset == 2) return 3;
    return 0;
}

inline uint8_t GlassModeIndexToPreset(uint8_t mode_idx) noexcept {
    static constexpr uint8_t kMap[4] = {5, 1, 3, 2};
    return kMap[mode_idx < 4 ? mode_idx : 0];
}

inline void ApplyGlassPreset(uint8_t preset, BeautifySettings& s) noexcept {
    if (preset < 4) s.glass_reference = false;
    switch (preset) {
    case 0: // 清透水滴 (Clear Droplet)
        s.glass_preset = 0;
        s.glass_thickness = 28;
        s.glass_ior_x100 = 152;
        s.glass_dispersion = 9;
        s.glass_glare = 92;
        s.glass_glare_angle = -45;
        s.glass_frost = 4;
        break;
    case 1: // Apple 标准 (Apple Regular)
        s.glass_preset = 1;
        s.glass_thickness = 24;
        s.glass_ior_x100 = 146;
        s.glass_dispersion = 7;
        s.glass_glare = 84;
        s.glass_glare_angle = -45;
        s.glass_frost = 24;
        break;
    case 2: // 超厚棱镜 (Deep Prism)
        s.glass_preset = 2;
        s.glass_thickness = 44;
        s.glass_ior_x100 = 188;
        s.glass_dispersion = 22;
        s.glass_glare = 108;
        s.glass_glare_angle = -38;
        s.glass_frost = 0;
        break;
    case 3: // 柔雾凝脂 (Satin Gel)
        s.glass_preset = 3;
        s.glass_thickness = 20;
        s.glass_ior_x100 = 136;
        s.glass_dispersion = 4;
        s.glass_glare = 72;
        s.glass_glare_angle = -45;
        s.glass_frost = 58;
        break;
    case 5: // Liquid Glass Studio default, old preset IDs retain their meanings.
        s.glass_preset = 5; s.glass_reference = true; s.glass_options = {};
        s.glass_thickness = 20; s.glass_ior_x100 = 140; s.glass_dispersion = 7;
        s.glass_glare = 90; s.glass_glare_angle = -45;
        s.glass_frost = 4;
        s.glass_blur_radius = 1; s.glass_blur_edge = true;
        break;
    default: // 自定义 (Custom)
        s.glass_preset = 4;
        break;
    }
    if (preset < 4) s.glass_options = LegacyGlassOptions(s.glass_thickness,s.glass_ior_x100,s.glass_dispersion,s.glass_glare,s.glass_glare_angle);
}

inline uint8_t MapLegacyGlassDispersionToGain(uint64_t legacy_level) noexcept {
    if (legacy_level == 0) return 0u;
    if (legacy_level == 1) return 9u;
    if (legacy_level == 2) return 20u;
    return static_cast<uint8_t>(std::min<uint64_t>(legacy_level, 35u));
}

inline uint8_t MapGlassGainToLegacyTier(uint8_t gain) noexcept {
    if (gain == 0) return 0u;
    if (gain >= 16) return 2u;
    return 1u;
}

// ---------------------------------------------------------------------------
// Desktop File / Icon Items & Rules
// ---------------------------------------------------------------------------
struct DesktopItem {
    std::wstring path;
    std::wstring name;
    std::wstring extension;        // lowercase dotted, e.g. L".png"
    std::wstring link_target;      // resolved target when extension == L".lnk"
    std::wstring installed_app_hint; // optional matched app version hint
    bool is_folder = false;
    bool folder_is_empty = false;
    bool is_shortcut = false;
    bool link_target_missing = false;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t tag_argb = 0;         // 0 = untagged
    std::wstring tag_id;
    PointI native_pos{0, 0};       // original Explorer coordinate
    PointI loose_pos{0, 0};        // settled loose grid coordinate
    std::string pinned_group_id;   // manual override group id ("" = rule/loose; kAppsTileGroupId = 应用 tile)
    bool force_loose = false;      // explicitly dragged out to bare desktop
    bool in_apps_tile = false;     // transient (UpdateAppsTileMembership): shown in the 应用 tile, not on the desktop
    std::string mapped_owner_group_id; // transient: enumerated directly from group.mapped_folder_path
    bool hidden_by_mapped_group = false; // transient: top-level folder currently displayed as a mapped group card
};
// pinned_group_id of a shortcut dragged into the 应用 tile.
inline constexpr char kAppsTileGroupId[] = "@apps";

struct GroupRule {
    std::vector<std::wstring> extensions;      // lowercase dotted, e.g. {L".exe", L".msi"}
    std::vector<std::wstring> name_prefixes;   // case-insensitive prefixes, e.g. {L"screenshot", L"屏幕截图"}
    std::vector<std::wstring> name_contains;   // case-insensitive substrings
    std::wstring tag_id;                       // match specific Pulse color tag
    int32_t modified_within_days = 0;          // >0 matches items modified within N days
    bool match_folders = false;
    bool match_files = true;
    bool fallback_catch_all = false;           // "未归类" fallback group
};

enum class GroupViewMode : uint8_t { Icons, LargeIcons, Tiles, Details };

struct DesktopGroup {
    std::string id;
    std::wstring title;
    wchar_t icon_glyph = L'\xE8B7';            // Segoe Fluent Icons glyph
    std::wstring rule_summary;                 // e.g. L"最近 7 天改过的文档"
    std::wstring mapped_folder_path;           // optional synced directory ("对应到真实文件夹")
    uint32_t accent_argb = 0;                  // 0 = inherit desktop accent
    CardMaterial material = CardMaterial::FollowHost;
    GroupSortBy sort_by = GroupSortBy::ModifiedTime;
    bool sort_ascending = false;
    bool collapsed = false;
    GroupViewMode view_mode = GroupViewMode::Icons;
    std::string tab_stack_id;
    int tab_order = 0;
    bool tab_active = true;
    bool focus_visible = false;                // kept expanded & full-opacity in Focus scene
    int x = 24;
    int y = 24;
    int cols = 4;                              // 2..7
    int visible_rows = 2;                      // 1..8, the most rows shown before "还有 N 个"
    int content_count = -1;                    // transient: members this layout pass (-1 = unknown)
    std::optional<PointI> rest_origin;         // non-destructive push-away memory
    GroupRule rule;
    std::vector<std::wstring> manual_order;    // paths when sort_by == Manual
};

struct GroupComputedView {
    std::string group_id;
    RectI frame;                               // actual pixel bounds (collapsed = 38px tall)
    std::vector<size_t> visible_item_indices;  // indices into DesktopModelState::items for active page
    std::vector<size_t> member_item_indices;   // every member, across all pages
    size_t total_item_count = 0;
    size_t overflow_count = 0;                 // >0 when total_item_count > page_capacity (multi-page card)
    int page_index = 0;                        // 0-based active page inside this group card
    int page_count = 1;                        // total pages (>= 1)
    size_t page_capacity = 0;                  // cols * visible_rows (0 when collapsed)
    std::wstring single_subfolder_path;        // non-empty when an unmapped custom group holds exactly 1 folder
    std::wstring single_subfolder_name;
    GroupViewMode view_mode = GroupViewMode::Icons;
    std::vector<std::string> tab_group_ids;
    bool tab_active = true;
    int active_tab_index = 0;
    bool expanded = true;
    bool temporarily_expanded = false;
    RectI content_rect;
    int item_columns = 1;
    int cell_width = 1;
    int cell_height = 1;
    int details_header_height = 0;
};

// ---------------------------------------------------------------------------
// 5x7 Live Tile Board
// ---------------------------------------------------------------------------
enum class TileKind : uint8_t {
    Photos = 0,
    Downloads = 1,
    Recent = 2,
    PinnedFolder = 3,
    RecycleBin = 4,
    ThisPC = 5,
    DiskRing = 6,
    PulseLauncher = 7,
    Clock = 8,
    StickyNote = 9,
    StagingTray = 10,
    PinnedItem = 11,
    Apps = 12,       // the desktop's app shortcuts, collected while organised
    Weather = 13,
    Calendar = 14,   // date, weekday and month (no events yet)
    Todo = 15,       // local list kept with the desktop state
    System = 16,     // CPU, memory, C: and network
    Pomodoro = 17,   // interactive 25/5 focus timer
    MediaPlayer = 18,// Windows GSMTC Now Playing + Vinyl cover & playback controls
    Countdown = 19,  // event countdown + Today/Week/Month/Year progress bars
    Battery = 20,    // PC power / battery + connected Bluetooth device batteries
};
inline constexpr TileKind kLastTileKind = TileKind::Battery;

// A tile's own look (自定义磁贴). `material` on the tile is its Windows effect.
enum class TileBackground : uint8_t {
    Material = 0,    // the effect only
    Color = 1,       // a colour over the effect (opaque when the effect is Solid)
    Image = 2,       // a picture; title strip and content get a frosted veil
};
struct TileLook {
    TileBackground background = TileBackground::Material;
    uint32_t color_argb = 0xFF0078D4u;
    std::wstring image_path;
    std::wstring title_override;   // empty: the tile's own title
    bool show_title = true;
    bool operator==(const TileLook&) const noexcept = default;
};
// One widget inside a stack (叠放). The tile keeps the stack's place and size.
struct TileStackEntry {
    TileKind kind = TileKind::PinnedFolder;
    std::wstring title;
    std::wstring target_path;
    std::string origin_id;         // the tile id it had before it was stacked
    bool analog_clock = true;
    bool operator==(const TileStackEntry&) const noexcept = default;
};

struct TileSize {
    int cols = 2;
    int rows = 2;
    constexpr bool operator==(const TileSize&) const noexcept = default;
};

bool IsValidTileSize(TileSize sz) noexcept;
std::span<const TileSize> AllowedTileSizes() noexcept;

struct DesktopTile {
    std::string id;
    TileKind kind = TileKind::PinnedFolder;
    std::wstring title;
    std::wstring target_path;                  // folder/file path for PinnedFolder, PinnedItem, Photos
    std::wstring note_body;                    // UTF-16 text for StickyNote
    std::wstring countdown_date;               // YYYY-MM-DD target date for Countdown (empty = auto next New Year)
    bool analog_clock = true;                  // 1x1 analog sweep clock vs 2x1 digital clock
    bool live_content = true;                  // per-tile dynamic content switch
    CardMaterial material = CardMaterial::FollowHost;
    uint32_t accent_argb = 0;
    int col = 0;                               // 0..kTileCols-1
    int row = 0;                               // 0..kTileRows-1
    TileSize size{2, 2};
    TileLook look;
    std::vector<TileStackEntry> stack;         // empty: a single widget; else every widget, this one included
    int stack_index = 0;
    bool smart_rotate = true;                  // a stack turns to its next widget while nobody looks at it
    int photo_interval_s = 10;                 // 照片: seconds per picture; 0 = only when asked (换一张)
    bool photo_shuffle = false;                // 照片: random order instead of newest first
    int note_color = 0;                        // 便笺: 0 黄 1 绿 2 蓝 3 粉 4 紫 5 灰
    bool free_placed = false;                  // true: use free desktop pixel coordinates (x, y)
    int x = 0;                                 // free desktop pixel X (8px aligned)
    int y = 0;                                 // free desktop pixel Y (8px aligned)
    int w = 0;                                 // custom pixel width (0 = derive from size.cols)
    int h = 0;                                 // custom pixel height (0 = derive from size.rows)
    std::optional<PointI> rest_origin;         // non-destructive push-away memory
};
struct TodoItem {
    std::wstring text;
    bool done = false;
    bool operator==(const TodoItem&) const noexcept = default;
};

struct TilePlacementResult {
    bool valid = false;
    std::vector<DesktopTile> updated_tiles;    // committed or previewed displaced positions
};

// ---------------------------------------------------------------------------
// Desktop Health Check
// ---------------------------------------------------------------------------
enum class HealthIssueKind : uint8_t {
    InstalledInstaller = 0,   // Setup .exe/.msi matching an already-installed application
    StaleScreenshot = 1,      // Screenshot (>14d) or untouched desktop file (>30d)
    BrokenShortcut = 2,       // .lnk whose target no longer exists
    DuplicateOrTemp = 3,      // (1)/copy/.tmp/.crdownload/0-byte file
    EmptyFolder = 4,          // Empty folder on desktop
    OneTimeUse = 5,           // Archive/log likely one-time use (unchecked by default)
};

struct InstalledAppRecord {
    std::wstring display_name;
    std::wstring display_version;
    std::wstring publisher;
};

struct HealthIssue {
    HealthIssueKind kind = HealthIssueKind::StaleScreenshot;
    std::wstring path;
    std::wstring name;
    uint64_t reclaimable_bytes = 0;
    std::wstring reason;                       // e.g. L"已安装 7-Zip 26.00"
    bool selected_by_default = true;
};

struct HealthReport {
    int score = 100;                           // 0..100 desktop health score
    uint64_t total_reclaimable_bytes = 0;
    std::vector<HealthIssue> issues;
};

// ---------------------------------------------------------------------------
// Complete Model State & Engines
// ---------------------------------------------------------------------------
struct MonitorWorkArea {
    std::wstring device_name;
    RectI bounds{0, 0, 1920, 1080};
    RectI work_area{0, 0, 1920, 1032};
    uint32_t dpi = 96;
    bool primary = true;
};

struct DesktopModelState {
    BeautifySettings settings;
    std::vector<MonitorWorkArea> monitors;
    std::vector<DesktopItem> items;
    std::vector<DesktopGroup> groups;
    std::vector<DesktopTile> tiles;
    std::vector<TodoItem> todos;
    bool widget_defaults_applied = false;      // the one-time 最近使用/日历/天气 stack on boards saved before widgets
    video::VideoWallpaperPersistedState video_wallpaper;
};

// Default factory matching desktop_pack_mockup.html (4 default groups + 10-tile 5x7 board).
std::vector<DesktopGroup> MakeDefaultGroups(const RectI& primary_work_area);
std::vector<DesktopTile> MakeDefaultTiles();

// Group Layout & Non-Destructive Collision Engine (layout_groups.cpp)
RectI ComputeTileBoardRect(const RectI& primary_work_area, bool tiles_enabled) noexcept;
int SnapCoord(int value, int step = kSnapGridPx) noexcept;
RectI ComputeGroupFrame(const DesktopGroup& group,
                        DesktopIconSize icon_size = DesktopIconSize::Medium) noexcept;
// Counts each group's members so frames fit their rows; call before ResolveGroupCollisions.
void UpdateGroupContentCounts(DesktopModelState& state, int64_t now_unix_ms);
void ResolveGroupCollisions(std::vector<DesktopGroup>& groups, std::optional<size_t> active_index,
                            const RectI& work_area, const RectI& tile_board_rect,
                            DesktopIconSize icon_size = DesktopIconSize::Medium,
                            std::span<const RectI> extra_obstacles = {});
void MoveGroupInteractive(std::vector<DesktopGroup>& groups, size_t group_index,
                          int proposed_x, int proposed_y,
                          const RectI& work_area, const RectI& tile_board_rect,
                          DesktopIconSize icon_size = DesktopIconSize::Medium,
                          std::span<const RectI> extra_obstacles = {});
// Finds the nearest snapped non-overlapping spot for `group_index` keeping all other groups stationary.
std::optional<PointI> FindNearestFreeGroupSpot(const std::vector<DesktopGroup>& groups,
                                               size_t group_index,
                                               int proposed_x, int proposed_y,
                                               const RectI& work_area, const RectI& tile_board_rect,
                                               DesktopIconSize icon_size = DesktopIconSize::Medium,
                                               std::span<const RectI> extra_obstacles = {});
void CommitGroupRestOrigins(std::vector<DesktopGroup>& groups) noexcept;
void ResizeGroupColumns(std::vector<DesktopGroup>& groups, size_t group_index,
                        int proposed_cols, int proposed_rows,
                        const RectI& work_area, const RectI& tile_board_rect,
                        DesktopIconSize icon_size = DesktopIconSize::Medium,
                        std::span<const RectI> extra_obstacles = {});
std::vector<GroupComputedView> ComputeGroupViews(
    const DesktopModelState& state,
    int64_t now_unix_ms,
    const std::unordered_map<std::string, int>* group_pages = nullptr,
    std::string_view hover_expanded_group_id = {});
RectI GroupItemRect(const GroupComputedView& view, size_t visible_index) noexcept;

// Files dragged onto a group card (group_drop.cpp). The slot is where they land: an index into the card's
// member order across pages, and the caret drawn between icons (empty for an empty card). Outside the
// content area of an expanded card there is no slot: the card takes the files at its end.
struct GroupDropSlot {
    size_t insert_index = 0;
    RectI caret;
};
std::optional<GroupDropSlot> ComputeGroupDropSlot(const GroupComputedView& view, int x, int y) noexcept;
// `display_order` with `dragged` (those already in it move, the rest join) placed at `insert_index`.
std::vector<std::wstring> ComposeManualOrder(std::span<const std::wstring> display_order,
                                             std::span<const std::wstring> dragged, size_t insert_index);
// Gives the card that order (手动排列) and drops the paths from other cards' orders. True when the card
// switched to 手动排列 just now.
bool ApplyGroupManualOrder(std::vector<DesktopGroup>& groups, std::string_view group_id,
                           std::vector<std::wstring> order);
// "新建分组", then "新建分组 2", "新建分组 3"... the first title no group has.
std::wstring NextNewGroupTitle(const std::vector<DesktopGroup>& groups, std::wstring_view base);

// Loose Icon Settlement Engine (loose_icons.cpp)
void SettleLooseIcons(std::vector<DesktopItem>& items,
                      std::span<const GroupComputedView> group_views,
                      const RectI& work_area,
                      const RectI& tile_board_rect,
                      DesktopIconSize icon_size = DesktopIconSize::Medium,
                      bool organize_enabled = true,
                      bool align_to_grid = true,
                      GroupSortBy loose_sort_by = GroupSortBy::ModifiedTime,
                      bool loose_sort_ascending = false,
                      std::span<const RectI> extra_obstacles = {});

// Live Tile Board & Free Desktop Tile Engine (tile_board.cpp)
int DefaultTileSpanPx(int units) noexcept;
TileSize EffectiveTileUnits(int w, int h) noexcept;
RectI ComputeTilePixelRect(const RectI& board_rect, const DesktopTile& tile) noexcept;
RectI ResolveTileRect(const DesktopTile& tile, const RectI& default_board_rect) noexcept;
std::vector<RectI> CollectTileObstacleRects(std::span<const DesktopTile> tiles,
                                            const RectI& default_board_rect,
                                            bool tiles_visible) noexcept;
void MaterializeFreeTiles(std::vector<DesktopTile>& tiles, const RectI& default_board_rect) noexcept;
void MoveTileFreeInteractive(std::vector<DesktopTile>& tiles, size_t tile_index,
                             int proposed_x, int proposed_y,
                             const RectI& work_area, const RectI& default_board_rect,
                             std::span<const RectI> group_frames = {});
void ResizeTileFreeInteractive(std::vector<DesktopTile>& tiles, size_t tile_index,
                               int proposed_w, int proposed_h,
                               const RectI& work_area, const RectI& default_board_rect,
                               std::span<const RectI> group_frames = {});
void CommitTileRestOrigins(std::vector<DesktopTile>& tiles) noexcept;
PointI FindFreeDesktopSpotForTile(std::span<const DesktopTile> tiles,
                                  std::span<const RectI> group_frames,
                                  const RectI& work_area,
                                  const RectI& default_board_rect,
                                  int w, int h) noexcept;
PointI ComputeTileDropCell(const RectI& board_rect, TileSize size, PointI top_left) noexcept;
TilePlacementResult PreviewTilePlacement(std::span<const DesktopTile> tiles,
                                         std::string_view moving_id,
                                         int target_col, int target_row, TileSize target_size);
TilePlacementResult InsertNewTile(std::span<const DesktopTile> tiles, const DesktopTile& new_tile);
// Sizes a kind offers (a subset of AllowedTileSizes()).
std::span<const TileSize> AllowedTileSizesFor(TileKind kind) noexcept;
// The widget a tile shows now: itself, or the active member of its stack (place, size and look stay the tile's).
DesktopTile ActiveTileView(const DesktopTile& tile);
// Stacks `moving_id` onto `target_id` (same size); the target shows the newest member. False when not allowed.
// Whether dropping tile `moving_id` on `target_id` stacks it: every incoming widget must support the target's
// size (the stack keeps it), at most six members, and the paged 应用 tile never joins a stack.
bool CanStackTiles(const std::vector<DesktopTile>& tiles, std::string_view moving_id, std::string_view target_id);
bool StackTiles(std::vector<DesktopTile>& tiles, std::string_view moving_id, std::string_view target_id);
// Takes the active member out of a stack into a tile of its own at the first free cell (its size, else 1x1).
// False when the board has no room; a stack left with one member becomes a plain tile again.
bool UnstackActiveMember(std::vector<DesktopTile>& tiles, std::string_view id);
// The first cell (row by row) where a tile of this size fits without moving anything.
std::optional<PointI> FindFreeTileCell(std::span<const DesktopTile> tiles, TileSize size);
// True when some tile (or stack member) is of this kind.
bool BoardHasKind(std::span<const DesktopTile> tiles, TileKind kind) noexcept;
// Marks shortcuts that the 应用 tile collects (organised desktop, tiles shown, an 应用 tile on the board).
void UpdateAppsTileMembership(DesktopModelState& state);
// Saved boards that still carry the old default 源代码 tile get the 应用 tile in its place.
void MigrateDefaultTiles(std::vector<DesktopTile>& tiles);
// Once per saved board: an untouched 2x2 最近使用 tile becomes the 最近使用 / 日历 / 天气 stack (when the board has
// neither widget). Marks the state so removing them later sticks.
void ApplyWidgetDefaults(DesktopModelState& state);
// A shortcut that starts a program (the 应用 tile collects these, not links to documents or folders).
bool IsAppShortcut(const DesktopItem& item) noexcept;
// The four local to-dos a new desktop starts with.
std::vector<TodoItem> MakeDefaultTodos();

// Rule Classification, Semantic Search & Desktop Health Check (rules_and_health.cpp)
std::wstring FormatRuleSummary(const GroupRule& rule);
bool MatchesDesktopSearchQuery(const DesktopItem& item, std::wstring_view query);
bool ItemMatchesRule(const DesktopItem& item, const GroupRule& rule, int64_t now_unix_ms);
std::optional<size_t> ClassifyItemToGroup(const DesktopItem& item,
                                          std::span<const DesktopGroup> groups,
                                          int64_t now_unix_ms);
int CompareVersionStrings(std::wstring_view a, std::wstring_view b) noexcept;
HealthReport AnalyzeDesktopHealth(std::span<const DesktopItem> items,
                                  std::span<const InstalledAppRecord> installed_apps,
                                  int64_t now_unix_ms,
                                  int stale_screenshot_days = 14);
std::vector<InstalledAppRecord> QueryInstalledAppsFromRegistry();

// Bounded Undo/Redo Stack & JSON Persistence (undo_history.cpp)
class UndoHistory {
public:
    explicit UndoHistory(size_t max_depth = 32) noexcept : max_depth_(max_depth) {}
    void Push(const DesktopModelState& snapshot, std::wstring action_label);
    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }
    std::wstring NextUndoLabel() const;
    bool Undo(DesktopModelState& current);
    bool Redo(DesktopModelState& current);
    void Clear() noexcept { undo_.clear(); redo_.clear(); }
    size_t UndoDepth() const noexcept { return undo_.size(); }

private:
    struct Entry {
        DesktopModelState state;
        std::wstring label;
    };
    size_t max_depth_ = 32;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

std::string SerializeModelStateJson(const DesktopModelState& state);
bool DeserializeModelStateJson(std::string_view utf8, DesktopModelState& out_state);

} // namespace pulse::desktop::model
