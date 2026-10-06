// ui_renderer_internal.h — Draw + HitTest shared geometry (not a public API).
#pragma once
#include "ui_renderer.h"
#include "empty_state_layout.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"
#include "typography.h"
#include "name_highlight.h"
#include "../app/places.h"
#include "../app/search_query.h"
#include "../common/text_format.h"
#include "../common/display_path.h"
#include "preview_format_catalog.h"
#include "link_type_text.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::ui {

namespace {

struct TextWidthKey {
    IDWriteTextFormat* format = nullptr;
    std::wstring text;
    std::uint64_t generation = 0;

    bool operator==(const TextWidthKey&) const = default;
};

struct TextWidthLookup {
    IDWriteTextFormat* format = nullptr;
    std::wstring_view text;
    std::uint64_t generation = 0;
};

struct TextWidthKeyHash {
    using is_transparent = void;

    static size_t Hash(IDWriteTextFormat* format, std::wstring_view value,
                       std::uint64_t generation) noexcept {
        const size_t pointer = std::hash<void*>{}(format);
        const size_t text = std::hash<std::wstring_view>{}(value);
        const size_t version = std::hash<std::uint64_t>{}(generation);
        return pointer ^ (text + 0x9e3779b9u + (pointer << 6) + (pointer >> 2)) ^
               (version + (text << 6) + (text >> 2));
    }

    size_t operator()(const TextWidthKey& key) const noexcept {
        return Hash(key.format, key.text, key.generation);
    }

    size_t operator()(const TextWidthLookup& key) const noexcept {
        return Hash(key.format, key.text, key.generation);
    }
};

struct TextWidthKeyEqual {
    using is_transparent = void;

    bool operator()(const TextWidthKey& a, const TextWidthKey& b) const noexcept {
        return a == b;
    }

    bool operator()(const TextWidthKey& a, const TextWidthLookup& b) const noexcept {
        return a.format == b.format && a.text == b.text && a.generation == b.generation;
    }

    bool operator()(const TextWidthLookup& a, const TextWidthKey& b) const noexcept {
        return a.format == b.format && a.text == b.text && a.generation == b.generation;
    }
};

thread_local std::unordered_map<TextWidthKey, float, TextWidthKeyHash, TextWidthKeyEqual>
    g_text_width_cache;
constexpr size_t kTextWidthCacheLimit = 4096;

void ClearTextWidthCache() {
    g_text_width_cache.clear();
}

    // Transparent fill: button resting state (hover fill drawn on interaction).
    constexpr D2D1_COLOR_F kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    // Split panes reserve room before the title for the focused-pane dot, in
    // every pane so titles do not shift when focus moves.

    // Opacity of the three window layers: title strip, the sheet shared by the
    // active tab/toolbar/sidebar/status bar, and the pane/details cards. The
    // active tab and the sheet must use the same value so they read as one.
    struct LayerAlphas { float title = 1.0f, sheet = 1.0f, card = 1.0f; };
    // Piecewise-linear lookup at t = 0, .25, .5, .75, 1.
    inline float LerpStops(float t, const float (&v)[5]) noexcept {
        t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
        const int i = (std::min)(3, static_cast<int>(t));
        return v[i] + (v[i + 1] - v[i]) * (t - static_cast<float>(i));
    }
    // transparency: 0..90 slider; 25/50/75 reproduce the former
    // subtle/balanced/vivid tables exactly.
    inline LayerAlphas ComputeLayerAlphas(bool image_mode, bool backdrop_drawn,
                                          bool backdrop_enabled, bool dark,
                                          int transparency, int wallpaper_blur) noexcept {
        LayerAlphas a;
        const float t = static_cast<float>(std::clamp(transparency, 0, 90)) / 100.0f;
        if (image_mode) {
            // Decode failure stays opaque rather than exposing the desktop.
            if (!backdrop_drawn) return a;
            // Dark wallpaper uses the legacy single scrim. Balanced restores
            // its 55% cover; blur affects image detail, not the tint strength.
            if (dark) {
                static constexpr float cover[5] = {0.85f, 0.70f, 0.55f, 0.40f, 0.25f};
                return {LerpStops(t, cover), 0.0f, 0.0f};
            }
            static constexpr float kTitle[5] = {0.70f, 0.60f, 0.50f, 0.40f, 0.30f};
            static constexpr float kSheet[5] = {0.80f, 0.68f, 0.55f, 0.45f, 0.35f};
            static constexpr float kCard[5] = {0.97f, 0.94f, 0.90f, 0.85f, 0.78f};
            a.title = LerpStops(t, kTitle);
            a.sheet = LerpStops(t, kSheet);
            a.card = LerpStops(t, kCard);
            // Sidebar text sits directly on the sheet; unblurred detail needs more cover.
            if (wallpaper_blur <= 0) {
                a.title = (std::min)(1.0f, a.title + 0.10f);
                a.sheet = (std::min)(1.0f, a.sheet + 0.12f);
            }
            return a;
        }
        if (backdrop_drawn) a.title = 0.0f;
        else if (backdrop_enabled) a.title = dark ? 0.72f : 0.78f;
        if (backdrop_enabled) {
            // Acrylic / Mica: the slider scales the default (t = .5) opacities.
            static constexpr float kScale[5] = {1.40f, 1.20f, 1.0f, 0.80f, 0.60f};
            const float k = LerpStops(t, kScale);
            a.sheet = (std::min)(1.0f, (dark ? 0.55f : 0.60f) * k);
            a.card = (std::min)(1.0f, (dark ? 0.72f : 0.78f) * k);
        }
        return a;
    }
    inline float WallpaperBlurDip(int wallpaper_blur) noexcept {
        return static_cast<float>(std::clamp(wallpaper_blur, 0, 40));
    }
    constexpr int kPanelTransparencyMax = 90;
    constexpr int kWallpaperBlurMax = 40;
    // Settings slider track inside the three segment cells the layout reserves;
    // the right 50 DIPs hold the value label.
    struct SettingsSliderGeom { float left = 0.0f, right = 0.0f, cy = 0.0f; };
    inline SettingsSliderGeom SettingsSlider(const D2D1_RECT_F* cells, float scale) noexcept {
        return {cells[0].left + 9.0f * scale, cells[2].right - 50.0f * scale,
                (cells[0].top + cells[0].bottom) * 0.5f - 5.0f * scale};
    }
    inline int SettingsSliderValue(const SettingsSliderGeom& g, float x, int max_value) noexcept {
        const float t = std::clamp((x - g.left) / (std::max)(1.0f, g.right - g.left), 0.0f, 1.0f);
        return static_cast<int>(t * static_cast<float>(max_value) + 0.5f);
    }
    constexpr float kTabMinW = 72.0f;
    constexpr float kTabMaxW = 240.0f;
    constexpr float kTabPinnedW = 36.0f; // Chrome pinned tab: icon-only square
    constexpr float kTabPinnedNamedW = 112.0f;
    constexpr float kTabCloseAlwaysW = 96.0f;
    constexpr float kTabClosePadDip = 10.0f;
    constexpr float kTabCloseSizeDip = 16.0f;
    constexpr float kCommandIconButtonDip = 32.0f;
    constexpr float kCommandIconStepDip = 34.0f;
    constexpr float kRecentControlsDip = 40.0f;
    constexpr float kSearchFiltersDip = 40.0f;
    // d2d1.lib does not export the effect CLSIDs; define the shadow one locally.
    // (CLSID_D2D1Shadow from d2d1effects.h; same idiom as fluent_menu.cpp.)
    constexpr GUID kShadowEffectClsid = { 0xC67EA361, 0x1863, 0x4e69,
        { 0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53 } };
    // Segoe Fluent Icons codepoints (fall back to text if font missing).
    constexpr const wchar_t* kIconHome = L"\xE80F";
    constexpr const wchar_t* kIconBack = L"\xE72B";
    constexpr const wchar_t* kIconForward = L"\xE72A";
    constexpr const wchar_t* kIconUp = L"\xE898";
    constexpr const wchar_t* kIconRefresh = L"\xE72C";
    constexpr const wchar_t* kIconAdd = L"\xE710";
    constexpr const wchar_t* kIconCut = L"\xE8C6";
    constexpr const wchar_t* kIconCopy = L"\xE8C8";
    constexpr const wchar_t* kIconPaste = L"\xE77F";
    constexpr const wchar_t* kIconRename = L"\xE8AC";
    constexpr const wchar_t* kIconDelete = L"\xE74D";
    constexpr const wchar_t* kIconSplit = L"\xE8A9";
    constexpr const wchar_t* kIconDetailsOpen = L"\xE8A0";
    constexpr const wchar_t* kIconDetailsClose = L"\xE89F";
    constexpr const wchar_t* kIconView = L"\xE700";
    constexpr const wchar_t* kIconSearch = L"\xE721";
    constexpr const wchar_t* kIconInfo = L"\xE946";
    constexpr const wchar_t* kIconFilter = L"\xE71C";
    constexpr const wchar_t* kIconTheme = L"\xE793";
    constexpr const wchar_t* kIconSettings = L"\xE713";
    constexpr const wchar_t* kIconMinimize = L"\xE921";
    constexpr const wchar_t* kIconMaximize = L"\xE922";
    constexpr const wchar_t* kIconRestore = L"\xE923";
    constexpr const wchar_t* kIconClose = L"\xE711";
    constexpr const wchar_t* kIconFolder = L"\xE8B7";
    constexpr const wchar_t* kIconFile = L"\xE8A5";
    constexpr const wchar_t* kIconTray = L"\xE8A1";
    constexpr const wchar_t* kIconChevronRight = L"\xE76C";
    constexpr const wchar_t* kIconChevronUp = L"\xE70E";
    constexpr const wchar_t* kIconChevronDown = L"\xE70D";
    constexpr const wchar_t* kIconCloseSmall = L"\xE711";
    constexpr const wchar_t* kIconPinFilled = L"\xE841";

    std::wstring JoinDirName(const std::wstring& dir, const std::wstring& name) {
        if (name.empty()) return dir;
        if (dir.empty()) return name;
        if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
        return dir + L'\\' + name;
    }

    std::wstring FormatListType(const std::wstring& name, bool isDir) {
        using pulse::l10n::StringId;
        if (isDir) return pulse::l10n::Get(StringId::TypeFolder);
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size())
            return pulse::l10n::Get(StringId::TypeFile);
        std::wstring ext = name.substr(dot + 1);
        for (auto& c : ext) c = std::towlower(c);
        if (ext == L"txt" || ext == L"md" || ext == L"log") return pulse::l10n::Get(StringId::TypeTextDocument);
        if (ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"gif" || ext == L"bmp" || ext == L"webp") return pulse::l10n::Get(StringId::TypeImage);
        if (ext == L"mp4" || ext == L"mkv" || ext == L"avi" || ext == L"mov") return pulse::l10n::Get(StringId::TypeVideo);
        if (ext == L"mp3" || ext == L"wav" || ext == L"flac") return pulse::l10n::Get(StringId::TypeAudio);
        if (ext == L"zip" || ext == L"rar" || ext == L"7z") return pulse::l10n::Get(StringId::TypeArchive);
        if (ext == L"exe" || ext == L"msi") return pulse::l10n::Get(StringId::TypeApplication);
        if (ext == L"dwg") return L"AutoCAD " + pulse::l10n::Get(StringId::TypeFile);
        return pulse::l10n::Get(StringId::TypeFile);
    }

    // "This PC" rows: the drive kind instead of "Folder".
    inline std::wstring DriveTypeText(uint8_t drive_type) {
        using pulse::l10n::StringId;
        switch (drive_type) {
        case DRIVE_FIXED: return pulse::l10n::Get(StringId::LocalDisk);
        case DRIVE_REMOVABLE: return pulse::l10n::Get(StringId::RemovableDisk);
        case DRIVE_REMOTE: return pulse::l10n::Get(StringId::NetworkDrive);
        case DRIVE_CDROM: return pulse::l10n::Get(StringId::CdDrive);
        case DRIVE_RAMDISK: return pulse::l10n::Get(StringId::RamDisk);
        default: return pulse::l10n::Get(StringId::Drive);
        }
    }

    const ListEntryView& MakeVisibleEntry(const PaneViewModel& vm, size_t index) {
        if (!vm.snapshot) return vm.entries[index];

        if (!vm.row_cache) vm.row_cache = std::make_shared<RowPresentationCache>();
        auto& cache = *vm.row_cache;
        if (cache.snapshot != vm.snapshot) {
            cache.snapshot = vm.snapshot;
            cache.rows.clear();
            cache.order.clear();
        }
        if (const auto found = cache.rows.find(index); found != cache.rows.end())
            return found->second;

        index::ContentResultStore::Row stored;
        if(vm.content_results && !vm.content_results->Get(index,stored)) {
            static const ListEntryView pending{}; return pending;
        }
        const fs::DirEntry& source = vm.content_results ? stored.entry : (*vm.snapshot)[index];
        const bool penetrated = !source.link_target.empty();
        ListEntryView entry;
        // Keep the on-disk name, including .lnk. Stripping hid the suffix and
        // made the icon cache treat resolved shortcuts as extensionless files.
        entry.name = source.name;
        entry.size_text = penetrated
            ? (source.link_target_is_dir ? L"" : pulse::format::ByteSize(source.link_target_size, true))
            : (source.is_dir ? L"" : pulse::format::ByteSize(source.size, true));
        // A zero FILETIME means "no time" (drives, servers, index rows without
        // it), not 1601-01-01.
        const auto list_time = [](const FILETIME& time) {
            return time.dwLowDateTime || time.dwHighDateTime ? pulse::format::LocalFileTime(time)
                                                             : std::wstring();
        };
        const auto time_value = [](const FILETIME& time) {
            return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        entry.date_text = list_time(source.mtime);
        entry.created_text = list_time(source.ctime);
        entry.accessed_text = list_time(source.atime);
        entry.created_value = time_value(source.ctime);
        entry.accessed_value = time_value(source.atime);
        entry.path = !source.full_path.empty() ? source.full_path
                     : (fs::IsVirtualPath(vm.path) ? L"" : JoinDirName(vm.path, source.name));
        entry.attrs = source.attrs;
        entry.size_value = source.size;
        entry.modified_value = (static_cast<uint64_t>(source.mtime.dwHighDateTime) << 32) |
                               source.mtime.dwLowDateTime;
        entry.is_dir = penetrated ? source.link_target_is_dir : source.is_dir;
        entry.is_reparse = source.is_reparse;
        entry.link_kind = fs::ClassifyLink(source.attrs, source.reparse_tag);
        entry.is_link = entry.link_kind != fs::LinkKind::None ||
            (!source.is_dir && IsShortcutName(source.name));
        entry.link_destination = source.link_destination;
        entry.cloud_recall = source.cloud_recall;
        if (vm.tag_catalog && source.attrs == 0 && !entry.path.empty()) {
            app::PlaceItemKind known_kind = app::PlaceItemKind::Unknown;
            if (const app::StarredItem* starred = vm.tag_catalog->FindStarred(entry.path))
                known_kind = starred->kind;
            else if (const app::RecentItem* recent = vm.tag_catalog->FindRecent(entry.path))
                known_kind = recent->kind;
            if (known_kind != app::PlaceItemKind::Unknown)
                entry.is_dir = known_kind == app::PlaceItemKind::Folder;
            entry.type_text = pulse::l10n::Get(pulse::l10n::StringId::Unavailable);
        } else {
            entry.type_text = FormatListType(
                penetrated ? fs::StripLnkSuffix(source.name) : source.name, entry.is_dir);
        }
        if (source.drive_type != 0) entry.type_text = DriveTypeText(source.drive_type);
        if (entry.link_kind != fs::LinkKind::None) entry.type_text = LinkTypeText(entry.link_kind);
        if (source.drive_type != 0 && source.drive_total > 0) {
            const uint64_t free_bytes = std::min(source.drive_free, source.drive_total);
            entry.drive_used = static_cast<float>(
                static_cast<double>(source.drive_total - free_bytes) /
                static_cast<double>(source.drive_total));
            wchar_t space[128]{};
            swprintf_s(space, pulse::l10n::Get(pulse::l10n::StringId::PickerDriveFreeFormat).c_str(),
                       pulse::format::ByteSize(free_bytes).c_str(),
                       pulse::format::ByteSize(source.drive_total).c_str());
            entry.drive_space_text = space;
        }
        entry.record_only = source.change_record_only;
        if (!source.change_type_text.empty()) entry.type_text = source.change_type_text;
        entry.starred = vm.tag_catalog && !entry.path.empty() &&
            vm.tag_catalog->IsStarred(entry.path);
        if(vm.content_results) entry.snippet = std::move(stored.snippet);
        else if (vm.search_snippets && index < vm.search_snippets->size())
            entry.snippet = (*vm.search_snippets)[index];
        if (entry.starred) {
            if (const app::StarredItem* starred = vm.tag_catalog->FindStarred(entry.path)) {
                entry.badge = starred->badge;
                entry.badge_color = HexColor(starred->badge_rgb);
            }
        }
        constexpr size_t kMaxCachedRows = 256;
        if (cache.rows.size() >= kMaxCachedRows && !cache.order.empty()) {
            cache.rows.erase(cache.order.front());
            cache.order.pop_front();
        }
        cache.order.push_back(index);
        return cache.rows.emplace(index, std::move(entry)).first->second;
    }

    struct SidebarSlot {
        enum Kind {
            Header,
            Item,
            Drive,
            Tag,
            // Rail only: a section's own icon row (always present, so the icon
            // never disappears and the section can be folded from it).
            Rail,
            TrayPanel,
            TrayRelease,
            TrayClear
        } kind = Header;
        D2D1_RECT_F rc{};
        int group = -1;
        int item = -1;
        int run = -1;
        int batch = -1;
        int sub = -1;
    };

    struct SidebarMetrics {
        float scale = 1.0f;
        float pad = 8.0f;
        float headerH = 36.0f;
        float itemH = 32.0f;
        float driveH = 48.0f;
        float tagH = 26.0f;
        float itemGap = 2.0f;
        float groupGap = 16.0f;
        float trayInner = 8.0f;
        float trayHeaderH = 22.0f;
        float trayHelperH = 28.0f;
        float trayDeckH = 102.0f;
        float badgeW = 40.0f;
        float actionW = 44.0f;
    };

    SidebarMetrics MakeSidebarMetrics(float scale) {
        SidebarMetrics m;
        m.scale = scale;
        m.pad = 8.0f * scale;
        m.headerH = 36.0f * scale;
        m.itemH = 32.0f * scale;
        m.driveH = 48.0f * scale;
        m.tagH = 26.0f * scale;
        m.itemGap = 2.0f * scale;
        m.groupGap = 16.0f * scale;
        m.trayInner = 8.0f * scale;
        m.trayHeaderH = 22.0f * scale;
        m.trayHelperH = 28.0f * scale;
        m.trayDeckH = 102.0f * scale;
        m.badgeW = 40.0f * scale;
        m.actionW = 72.0f * scale;
        return m;
    }

    // Extra space under the vertical-tabs block; its divider sits mid-gap.
    constexpr float kSidebarTabsDividerGapDip = 8.0f;

    float SidebarItemHeight(const SidebarItem& item, const SidebarMetrics& m) {
        if (item.is_drive) return m.driveH;
        if (item.is_tag) return m.tagH;
        return m.itemH;
    }

    int TrayTotalCount(const WindowViewModel& vm) { return vm.tray_deck.total_count; }

    // Staging tray card metrics (DIPs): thumbnail follows the "staging tray
    // icon" setting (32..64), the card adds 10 dip of padding above/below.
    inline float TrayThumbDip(float thumb_dip) {
        return std::clamp(thumb_dip + 2.0f, 40.0f, 66.0f);
    }
    inline float TrayCardHeightDip(float thumb_dip) { return TrayThumbDip(thumb_dip) + 20.0f; }

    // Recent drop destinations: one chip row at the very bottom of the tray
    // panel; the deck and its footer sit in the area above it.
    inline float TrayDestRowH(const TrayDeckView& deck, float scale) {
        return deck.total_count > 0 && !deck.dests.empty() ? 30.0f * scale : 0.0f;
    }
    // Stale-items notice ("⚠ N moved or deleted  [Find] [Remove]") above the chips.
    inline float TrayStaleRowH(const TrayDeckView& deck, float scale) {
        return deck.total_count > 0 && deck.stale_count > 0 ? 28.0f * scale : 0.0f;
    }
    inline float TrayBottomRowsH(const TrayDeckView& deck, float scale) {
        return TrayDestRowH(deck, scale) + TrayStaleRowH(deck, scale);
    }
    inline D2D1_RECT_F TrayDeckArea(const D2D1_RECT_F& panel, const TrayDeckView& deck,
                                    float scale) {
        D2D1_RECT_F r = panel;
        r.bottom -= TrayBottomRowsH(deck, scale);
        return r;
    }
    inline D2D1_RECT_F TrayStaleRowRect(const D2D1_RECT_F& panel, const TrayDeckView& deck,
                                        float scale) {
        const float bottom = panel.bottom - TrayDestRowH(deck, scale) - 8.0f * scale;
        return D2D1::RectF(panel.left + 10.0f * scale, bottom - 22.0f * scale,
                           panel.right - 10.0f * scale, bottom);
    }
    // i = 0 find, 1 remove (right-most).
    inline D2D1_RECT_F TrayStaleButtonRect(const D2D1_RECT_F& panel, const TrayDeckView& deck,
                                           float scale, int i) {
        const D2D1_RECT_F row = TrayStaleRowRect(panel, deck, scale);
        const float w = 52.0f * scale;
        const float right = row.right - (i == 0 ? w + 4.0f * scale : 0.0f);
        return D2D1::RectF(right - w, row.top, right, row.bottom);
    }
    inline D2D1_RECT_F TrayDestChipRect(const D2D1_RECT_F& panel, const TrayDeckView& deck,
                                        float scale, int i) {
        const int n = std::max(1, static_cast<int>(deck.dests.size()));
        const float gap = 6.0f * scale;
        const float left = panel.left + 10.0f * scale;
        const float right = panel.right - 10.0f * scale;
        const float bottom = panel.bottom - 8.0f * scale;
        const float w = std::max(1.0f, (right - left - gap * static_cast<float>(n - 1)) /
                                       static_cast<float>(n));
        const float x = left + static_cast<float>(i) * (w + gap);
        return D2D1::RectF(x, bottom - 24.0f * scale, x + w, bottom);
    }

    // Compare table (two staged files): 5 rows in the card-stack area.
    inline float TrayCompareBoxDip(float thumb_dip) {
        return std::max(TrayCardHeightDip(thumb_dip) + 22.0f, 98.0f);
    }
    struct TrayCompareGeom {
        D2D1_RECT_F box{};
        float label_w = 0.0f, row_h = 0.0f, col_w = 0.0f, scale = 1.0f;
    };
    inline TrayCompareGeom TrayCompareGeometry(const D2D1_RECT_F& deck, float scale,
                                               float thumb_dip) {
        TrayCompareGeom g;
        g.scale = scale;
        const float top = deck.top + 38.0f * scale;
        g.box = D2D1::RectF(deck.left + 10.0f * scale, top, deck.right - 10.0f * scale,
                            top + TrayCompareBoxDip(thumb_dip) * scale);
        g.row_h = std::min(20.0f * scale, (g.box.bottom - g.box.top - 8.0f * scale) / 5.0f);
        g.label_w = 58.0f * scale;
        g.col_w = std::max(0.0f, (g.box.right - 6.0f * scale - (g.box.left + g.label_w)) * 0.5f);
        return g;
    }
    // col -1 = row label, 0/1 = file columns, 2 = both file columns.
    inline D2D1_RECT_F TrayCompareCell(const TrayCompareGeom& g, int row, int col) {
        const float top = g.box.top + 4.0f * g.scale + static_cast<float>(row) * g.row_h;
        const float bottom = top + g.row_h;
        if (col < 0) return D2D1::RectF(g.box.left + 10.0f * g.scale, top, g.box.left + g.label_w, bottom);
        const float l = g.box.left + g.label_w + (col == 1 ? g.col_w : 0.0f);
        const float r = col == 2 ? g.box.right - 6.0f * g.scale : l + g.col_w - 4.0f * g.scale;
        return D2D1::RectF(l, top, r, bottom);
    }

    float ExpandedTrayHeight(const WindowViewModel& vm, const SidebarMetrics& m) {
        if (vm.tray_deck.cards.empty())
            return 112.0f * m.scale; // header + dashed empty state
        // Card stack: header (38) + top card + peeking layers when fanned
        // out (25) + gap + footer row (31) + destination chips when any.
        const float card = TrayCardHeightDip(vm.tray_deck.thumb_dip);
        const float compare_extra = vm.tray_deck.comparing
            ? std::max(0.0f, TrayCompareBoxDip(vm.tray_deck.thumb_dip) - (card + 22.0f)) : 0.0f;
        return (card + 94.0f + compare_extra) * m.scale +
               TrayBottomRowsH(vm.tray_deck, m.scale);
    }

    float SidebarContentHeight(const WindowViewModel& vm, const SidebarMetrics& m) {
        float height = m.pad;
        for (const auto& group : vm.sidebar) {
            if (group.hidden) continue;
            if (group.items.empty() && group.add_action == SidebarAddAction::None) continue;
            // A header-less section (the starred root) is its own first row: it
            // has nothing to fold, so it never carries the header height.
            const bool has_header = !group.header.empty();
            if (has_header) height += m.headerH + 4.0f * m.scale;
            if (!has_header || !group.collapsed) {
                for (const auto& item : group.items)
                    height += SidebarItemHeight(item, m) + m.itemGap;
                height += m.groupGap - m.itemGap;
            }
            // Same extra gap LayoutSidebar leaves under the vertical-tabs well.
            if (group.tabs_section) height += kSidebarTabsDividerGapDip * m.scale;
        }
        return height + m.pad;
    }

    fluent::ScrollbarSpec SidebarScrollbarSpec(const WindowViewModel& vm,
                                               const D2D1_RECT_F& sb, float scale) {
        const SidebarMetrics m = MakeSidebarMetrics(scale);
        const float tray_height = std::min(ExpandedTrayHeight(vm, m),
            std::max(120.0f * scale, (sb.bottom - sb.top) * 0.52f));
        const float bottom = std::max(sb.top, sb.bottom - tray_height - 2.0f * m.pad);
        fluent::ScrollbarSpec bar;
        bar.viewport = D2D1::RectF(sb.right - 12.0f * scale, sb.top,
                                   sb.right - 2.0f * scale, bottom);
        bar.viewport_extent = bottom - sb.top;
        bar.content_extent = SidebarContentHeight(vm, m);
        bar.offset = std::clamp(vm.sidebar_scroll, 0.0f,
            std::max(0.0f, bar.content_extent - bar.viewport_extent));
        bar.expand_progress = vm.sidebar_scrollbar_expand;
        bar.opacity = vm.sidebar_scrollbar_opacity;
        bar.enabled = !SidebarRailLayout(sb.right - sb.left, scale);
        return bar;
    }

    void LayoutSidebar(const WindowViewModel& vm, const D2D1_RECT_F& sb, float scale,
                       std::vector<SidebarSlot>& out,
                       std::vector<SidebarGroupBand>* bands = nullptr) {
        out.clear();
        const float width = sb.right - sb.left;
        const bool compact = SidebarRailLayout(width, scale);
        const SidebarMetrics m = MakeSidebarMetrics(scale);

        if (compact) {
            const float rowH = 38.0f * scale;
            const float trayH = 44.0f * scale;
            float y = sb.top;
            int run = 0;
            for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
                const auto& group = vm.sidebar[g];
                if (group.hidden) continue;
                // Empty sections stay invisible, exactly as in the wide layout.
                if (group.items.empty() && group.add_action == SidebarAddAction::None) continue;
                // Header-less sections (starred root, OneDrive accounts) are
                // their own rows and cannot fold; every other section gets a
                // permanent rail row that folds and unfolds it, so the section
                // icon never disappears.
                if (!group.header.empty()) {
                    if (y + rowH > sb.bottom - trayH) break;
                    SidebarSlot slot;
                    slot.kind = SidebarSlot::Rail;
                    slot.rc = D2D1::RectF(4.0f * scale, y, width - 4.0f * scale, y + rowH);
                    slot.group = g;
                    slot.item = -1;
                    slot.run = run++;
                    out.push_back(slot);
                    y += rowH;
                    if (group.collapsed) continue;
                }
                for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
                    if (group.items[i].starred_child) continue;
                    if (y + rowH > sb.bottom - trayH) break;
                    SidebarSlot slot;
                    slot.kind = group.items[i].is_drive ? SidebarSlot::Drive
                              : group.items[i].is_tag ? SidebarSlot::Tag : SidebarSlot::Item;
                    slot.rc = D2D1::RectF(4.0f * scale, y, width - 4.0f * scale, y + rowH);
                    slot.group = g;
                    slot.item = i;
                    slot.run = run++;
                    out.push_back(slot);
                    y += rowH;
                }
            }
            SidebarSlot tray;
            tray.kind = SidebarSlot::TrayPanel;
            tray.rc = D2D1::RectF(4.0f * scale, sb.bottom - trayH,
                                  width - 4.0f * scale, sb.bottom - 4.0f * scale);
            out.push_back(tray);
            return;
        }

        float trayH = ExpandedTrayHeight(vm, m);
        trayH = std::min(trayH, std::max(120.0f * scale, (sb.bottom - sb.top) * 0.52f));
        const float trayTop = sb.bottom - m.pad - trayH;
        const float contentBottom = trayTop - m.pad;
        const float innerL = sb.left + m.pad;
        const float innerR = sb.right - m.pad;

        float y = sb.top + m.pad - std::max(0.0f, vm.sidebar_scroll);
        int run = 0;
        for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
            const auto& group = vm.sidebar[g];
            if (group.hidden) continue;
            if (group.items.empty() && group.add_action == SidebarAddAction::None) continue;
            if (y >= contentBottom) break;
            const float band_top = y;
            // Sections without a header (the starred root) show their rows
            // directly and cannot be folded as a whole.
            const bool has_header = !group.header.empty();
            if (has_header) {
                SidebarSlot header;
                header.kind = SidebarSlot::Header;
                header.rc = D2D1::RectF(innerL, y, innerR, y + m.headerH);
                header.group = g;
                if (header.rc.bottom > sb.top && header.rc.bottom <= contentBottom)
                    out.push_back(header);
                y += m.headerH + 4.0f * scale;
            }
            if (has_header && group.collapsed) {
                if (group.tabs_section) y += kSidebarTabsDividerGapDip * scale;
                if (bands) bands->push_back(
                    { g, band_top, std::min(y + m.groupGap - m.itemGap, contentBottom) });
                continue;
            }
            for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
                const auto& item = group.items[i];
                if (g == vm.tag_drag_group && i == vm.tag_drag_item) {
                    // Keep the tentative slot open (SortableJS-style gap); the
                    // dragged card itself is drawn floating below. Packing the
                    // flow instead would teleport every sibling on grab/drop.
                    y += SidebarItemHeight(item, m) + m.itemGap;
                    continue;
                }
                const float h = SidebarItemHeight(item, m);
                if (y >= contentBottom) break;
                SidebarSlot slot;
                slot.kind = item.is_drive ? SidebarSlot::Drive
                          : item.is_tag ? SidebarSlot::Tag : SidebarSlot::Item;
                float top = y;
                if (item.is_tag && item.y_offset != 0.0f)
                    top += item.y_offset * (m.tagH + m.itemGap); // slot units -> px
                slot.rc = D2D1::RectF(innerL + item.indent * 16.0f * scale, top, innerR, top + h);
                slot.group = g;
                slot.item = i;
                slot.run = run++;
                if (slot.rc.bottom > sb.top && slot.rc.bottom <= contentBottom)
                    out.push_back(slot);
                y += h + m.itemGap;
            }
            y += m.groupGap - m.itemGap;
            if (group.tabs_section) y += kSidebarTabsDividerGapDip * scale;
            if (bands) bands->push_back({ g, band_top, std::min(y, contentBottom) });
        }

        // Dragged tag floats at the cursor, clamped to the tag flow range.
        if (vm.tag_drag_group >= 0 && vm.tag_drag_group < static_cast<int>(vm.sidebar.size())) {
            const auto& dgroup = vm.sidebar[vm.tag_drag_group];
            if (vm.tag_drag_item >= 0 && vm.tag_drag_item < static_cast<int>(dgroup.items.size())) {
                float minTop = 1e30f, maxBottom = -1e30f;
                for (const auto& slot : out) {
                    if (slot.kind == SidebarSlot::Tag && slot.group == vm.tag_drag_group) {
                        // Clamp against the *rest* geometry: slide offsets are
                        // baked into slot.rc, and a mid-flight sibling would
                        // otherwise shrink/extend the range on that side.
                        float base = slot.rc.top;
                        const auto& sib = dgroup.items[static_cast<size_t>(slot.item)];
                        if (sib.y_offset != 0.0f)
                            base -= sib.y_offset * (m.tagH + m.itemGap);
                        minTop = std::min(minTop, base);
                        maxBottom = std::max(maxBottom, base + (slot.rc.bottom - slot.rc.top));
                    }
                }
                float cy = vm.tag_drag_y;
                if (minTop <= maxBottom) {
                    // The gap slot contributes no rect, so extend the clamp by
                    // one pitch: the float must still reach the first/last
                    // position. The gesture clamps to the exact range anyway.
                    const float pitch = m.tagH + m.itemGap;
                    cy = std::clamp(cy, minTop + m.tagH * 0.5f - pitch,
                                    maxBottom - m.tagH * 0.5f + pitch);
                }
                SidebarSlot dragged;
                dragged.kind = SidebarSlot::Tag;
                dragged.rc = D2D1::RectF(innerL, cy - m.tagH * 0.5f, innerR, cy + m.tagH * 0.5f);
                dragged.group = vm.tag_drag_group;
                dragged.item = vm.tag_drag_item;
                dragged.run = run++;
                out.push_back(dragged);
            }
        }

        SidebarSlot tray;
        tray.kind = SidebarSlot::TrayPanel;
        tray.rc = D2D1::RectF(innerL, trayTop, innerR, sb.bottom - m.pad);
        out.push_back(tray);

        const float inset = 10.0f * scale;
        if (vm.tray_deck.total_count > 0) {
            const float headerTop = trayTop + 6.0f * scale;
            const float headerBottom = headerTop + m.trayHeaderH;
            SidebarSlot release;
            release.kind = SidebarSlot::TrayRelease;
            release.batch = vm.tray_deck.batch_count - 1;
            release.rc = D2D1::RectF(innerR - inset - m.actionW,
                                     headerTop,
                                     innerR - inset,
                                     headerBottom);
            out.push_back(release);

            // Footer "clear all" action, bottom-right of the panel.
            SidebarSlot clear;
            clear.kind = SidebarSlot::TrayClear;
            const float deckBottom = tray.rc.bottom - TrayBottomRowsH(vm.tray_deck, scale);
            clear.rc = D2D1::RectF(innerR - inset - 72.0f * scale,
                                   deckBottom - m.trayInner - 22.0f * scale,
                                   tray.rc.right - inset,
                                   deckBottom - m.trayInner);
            out.push_back(clear);
        }
    }

    bool RectContains(const D2D1_RECT_F& rc, float x, float y) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    }

    float ChangeBannerLayout(const PaneViewModel& pane, float width, float scale,
                             Compositor* compositor, ComPtr<IDWriteTextLayout>& layout) {
        std::wstring text = pane.banner_message;
        if (!pane.change_status_text.empty()) {
            if (!text.empty()) text += L"\n";
            text += pane.change_status_text;
        }
        if (text.empty()) return 0.0f;
        if (!compositor || !compositor->DwriteFactory()) return 36 * scale;
        if (FAILED(compositor->DwriteFactory()->CreateTextLayout(text.c_str(),
            static_cast<UINT32>(text.size()), compositor->SmallFormat(),
            std::max(1.0f, width - 32 * scale), 10000 * scale, &layout))) return 36 * scale;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return std::max(36 * scale, metrics.height + 16 * scale);
    }

    float PaneBannerHeight(const PaneViewModel& pane, float width, float scale, Compositor* compositor) {
        if (!pane.is_changes) return pane.banner_message.empty() ? 0.0f : 36 * scale;
        ComPtr<IDWriteTextLayout> layout;
        return ChangeBannerLayout(pane, width, scale, compositor, layout);
    }

    // Folder compare status colors (rows, banner chips).
    D2D1_COLOR_F CompareMarkColor(uint8_t mark, bool dark) {
        switch (static_cast<CompareMark>(mark)) {
        case CompareMark::OnlyHere: return HexColor(dark ? 0x3FB950u : 0x1A7F37u);
        case CompareMark::Newer:    return HexColor(dark ? 0x58A6FFu : 0x0969DAu);
        case CompareMark::Older:    return HexColor(dark ? 0xD29922u : 0x9A6700u);
        default:                    return HexColor(dark ? 0xBC8CFFu : 0x8250DFu);
        }
    }

    // Folder compare banner: [只看差异|显示全部] [退出对比] on the right.
    void CompareBannerButtons(const D2D1_RECT_F& bar, float scale, D2D1_RECT_F& diff,
                              D2D1_RECT_F& exit) {
        const float avail = bar.right - bar.left;
        const float ew = std::min(56.0f * scale, avail * 0.16f);
        const float dw = std::min(84.0f * scale, avail * 0.24f);
        exit = D2D1::RectF(bar.right - ew, bar.top, bar.right, bar.bottom);
        diff = D2D1::RectF(exit.left - dw, bar.top, exit.left, bar.bottom);
    }

    float PaneExtraTop(const PaneViewModel& pane, float scale, float width, Compositor* compositor) {
        return PaneBannerHeight(pane, width, scale, compositor) +
               (pane.is_recent ? kRecentControlsDip * scale : 0.0f) +
               (pane.is_query_search ? kSearchFiltersDip * scale : 0.0f) +
               (pane.is_changes ? 36.0f * scale : 0.0f);
    }

    void FillSearchFilterChipLabels(const std::wstring& query, std::wstring labels[5]) {
        const app::AdvancedSearchSpec spec = app::ParseSearchQuery(query);
        auto kind_label = [&] {
            switch (spec.kind) {
            case pulse::index::SearchKind::Folder:
                return pulse::l10n::Get(pulse::l10n::StringId::KindFolder);
            case pulse::index::SearchKind::Document:
                return pulse::l10n::Get(pulse::l10n::StringId::KindDocument);
            case pulse::index::SearchKind::Image:
                return pulse::l10n::Get(pulse::l10n::StringId::KindImage);
            case pulse::index::SearchKind::Video:
                return pulse::l10n::Get(pulse::l10n::StringId::KindVideo);
            case pulse::index::SearchKind::Audio:
                return pulse::l10n::Get(pulse::l10n::StringId::KindAudio);
            case pulse::index::SearchKind::Archive:
                return pulse::l10n::Get(pulse::l10n::StringId::KindArchive);
            case pulse::index::SearchKind::Code:
                return pulse::l10n::Get(pulse::l10n::StringId::KindCode);
            case pulse::index::SearchKind::Custom:
                return spec.custom_exts.empty()
                    ? pulse::l10n::Get(pulse::l10n::StringId::KindCustom)
                    : spec.custom_exts;
            default:
                return pulse::l10n::Get(pulse::l10n::StringId::SearchChipType);
            }
        };
        auto date_label = [&] {
            switch (spec.date) {
            case app::DatePreset::Today:
                return pulse::l10n::Get(pulse::l10n::StringId::DateToday);
            case app::DatePreset::Yesterday:
                return pulse::l10n::Get(pulse::l10n::StringId::DateYesterday);
            case app::DatePreset::ThisWeek:
                return pulse::l10n::Get(pulse::l10n::StringId::DateThisWeek);
            case app::DatePreset::ThisMonth:
                return pulse::l10n::Get(pulse::l10n::StringId::DateThisMonth);
            case app::DatePreset::ThisYear:
                return pulse::l10n::Get(pulse::l10n::StringId::DateThisYear);
            default:
                return pulse::l10n::Get(pulse::l10n::StringId::SearchChipDate);
            }
        };
        auto size_label = [&] {
            switch (spec.size) {
            case app::SizePreset::Empty:
                return pulse::l10n::Get(pulse::l10n::StringId::SizeEmpty);
            case app::SizePreset::Lt1MB:
                return pulse::l10n::Get(pulse::l10n::StringId::SizeLt1MB);
            case app::SizePreset::From1To10MB:
                return pulse::l10n::Get(pulse::l10n::StringId::Size1To10MB);
            case app::SizePreset::Gt10MB:
                return pulse::l10n::Get(pulse::l10n::StringId::SizeGt10MB);
            case app::SizePreset::Gt100MB:
                return pulse::l10n::Get(pulse::l10n::StringId::SizeGt100MB);
            case app::SizePreset::Gt1GB:
                return pulse::l10n::Get(pulse::l10n::StringId::SizeGt1GB);
            default:
                return pulse::l10n::Get(pulse::l10n::StringId::SearchChipSize);
            }
        };
        labels[0] = kind_label();
        labels[1] = date_label();
        labels[2] = size_label();
        if (spec.content.empty()) {
            labels[3] = pulse::l10n::Get(pulse::l10n::StringId::SearchChipContent);
        } else {
            // "Contents: word" so the chip says what the word filters.
            std::wstring text(spec.content.size() + 64, L'\0');
            const int n = swprintf_s(text.data(), text.size(),
                pulse::l10n::Get(pulse::l10n::StringId::SearchChipContentFormat).c_str(),
                spec.content.c_str());
            text.resize(n > 0 ? static_cast<size_t>(n) : 0);
            labels[3] = text.empty() ? spec.content : text;
        }
        labels[4] = pulse::l10n::Get(pulse::l10n::StringId::AdvancedSearch);
    }

    // The Advanced chip leads the row so it is never the one clipped by a narrow pane.
    inline constexpr int kSearchFilterOrder[5] = {4, 0, 1, 2, 3};
    inline constexpr const wchar_t* kSearchAdvancedGlyph = L"\xE9E9";

    void SearchFilterChipWidthsPx(const fluent::Painter& painter, float scale,
                                  const std::wstring labels[5], float widths[5]) {
        const float cap = 220.0f * scale;
        const float min_w = 32.0f * scale;
        for (int i = 0; i < 5; ++i) {
            float w = painter.MeasureButtonWidth(labels[i], i == 4 ? kSearchAdvancedGlyph : L"", i < 3);
            if (w < 1.0f) {
                float dip = 18.0f + (i < 3 ? 20.0f : 0.0f);
                for (wchar_t c : labels[i]) dip += (c > 0x7F) ? 13.0f : 7.4f;
                w = dip * scale;
            }
            // Hinting can overhang the measured advance by a pixel or two.
            widths[i] = std::min(cap, std::max(min_w, std::ceil(w + 4.0f * scale)));
        }
    }

    D2D1_RECT_F SearchFilterRect(const D2D1_RECT_F& pane, float header_height,
                                 float scale, int index, const float widths_px[5]) {
        float x = pane.left + 10.0f * scale;
        const float gap = 8.0f * scale;
        for (int slot = 0; slot < 5 && kSearchFilterOrder[slot] != index; ++slot)
            x += widths_px[kSearchFilterOrder[slot]] + gap;
        const float top = pane.top + header_height + 5.0f * scale;
        const float width = widths_px[std::clamp(index, 0, 4)];
        return D2D1::RectF(x, top, x + width, top + 30.0f * scale);
    }

    D2D1_RECT_F RecentFilterRect(const D2D1_RECT_F& pane, float header_height,
                                 float scale, int index) {
        const float top = pane.top + header_height + 5.0f * scale;
        const float left = pane.left + 10.0f * scale + index * 72.0f * scale;
        return D2D1::RectF(left, top, left + 72.0f * scale, top + 30.0f * scale);
    }

    D2D1_RECT_F RecentClearRect(const D2D1_RECT_F& pane, float header_height,
                                float scale) {
        const float top = pane.top + header_height + 5.0f * scale;
        return D2D1::RectF(pane.right - 42.0f * scale, top,
                           pane.right - 10.0f * scale, top + 30.0f * scale);
    }

    bool SidebarItemHasUnpin(const SidebarItem& item) {
        return item.tab_row || (item.indent == 0 && item.path.starts_with(L"pulse:workspace:"));
    }

    D2D1_RECT_F WorkspaceUnpinRect(const D2D1_RECT_F& row, float scale) {
        const float size = 18.0f * scale;
        const float pad = 8.0f * scale;
        const float right = row.right - pad;
        const float top = row.top + ((row.bottom - row.top) - size) * 0.5f;
        return D2D1::RectF(right - size, top, right, top + size);
    }

    D2D1_RECT_F SidebarExpandRect(const D2D1_RECT_F& row, float scale) {
        const float size = 22.0f * scale;
        const float right = row.right - 6.0f * scale;
        const float top = row.top + ((row.bottom - row.top) - size) * 0.5f;
        return D2D1::RectF(right - size, top, right, top + size);
    }

    bool PathIsSelfOrChild(const std::wstring& root_in, const std::wstring& path_in) {
        // Sidebar items hold normalized paths (\\?\C:\..., \\?\UNC\...), while the
        // pane path is display text without that prefix; compare both as display
        // text or file-system rows never match.
        const std::wstring root = pulse::path::FriendlyPathText(root_in);
        const std::wstring path = pulse::path::FriendlyPathText(path_in);
        if (root.empty() || path.empty()) return false;
        if (_wcsicmp(root.c_str(), path.c_str()) == 0) return true;
        std::wstring prefix = root;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
        return path.size() >= prefix.size() &&
            _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    // ---------------------------------------------------------------------
    // Staging tray card stack. Shared by DrawTrayDeck and HitTest so the two
    // can never disagree about where a card is. The top card sits at the
    // resting rect; up to two cards peek out underneath (shifted down,
    // scaled around their bottom centre, dimmed). Poses (depth, drag/throw
    // offsets, tumble) come pre-animated from the app side.
    // ---------------------------------------------------------------------
    struct TrayStackGeom {
        D2D1_RECT_F card{};     // resting rect of the top card (px)
        float thumb = 0.0f;     // thumbnail edge (px)
        float radius = 0.0f;    // card corner radius (px)
        float scale = 1.0f;
    };

    TrayStackGeom TrayStackGeometry(const D2D1_RECT_F& panel, float scale, float thumb_dip) {
        TrayStackGeom g;
        g.scale = scale;
        g.thumb = TrayThumbDip(thumb_dip) * scale;
        g.radius = 12.0f * scale;
        const float top = panel.top + 38.0f * scale;
        g.card = D2D1::RectF(panel.left + 10.0f * scale, top, panel.right - 10.0f * scale,
                             top + TrayCardHeightDip(thumb_dip) * scale);
        return g;
    }

    // Peeking layer pose for a (fractional) depth: vertical shift in DIPs,
    // scale, dim overlay strength and opacity. depth 2..3 fades out.
    struct TrayLayer { float dy = 0.0f; float scale = 1.0f; float dim = 0.0f; float opacity = 1.0f; };
    TrayLayer TrayLayerAt(float depth, float spread) {
        static constexpr float kDy[4] = {0.0f, 9.0f, 17.0f, 17.0f};
        static constexpr float kDySpread[4] = {0.0f, 4.0f, 8.0f, 8.0f};
        static constexpr float kScale[4] = {1.0f, 0.94f, 0.88f, 0.88f};
        static constexpr float kScaleSpread[4] = {0.0f, 0.01f, 0.02f, 0.02f};
        static constexpr float kDim[4] = {0.0f, 1.0f, 1.8f, 1.8f};
        const float d = std::clamp(depth, 0.0f, 3.0f);
        const int i = std::min(2, static_cast<int>(std::floor(d)));
        const float f = d - static_cast<float>(i);
        const float s = std::clamp(spread, 0.0f, 1.0f);
        auto lerp = [f](float a, float b) { return a + (b - a) * f; };
        TrayLayer l;
        l.dy = lerp(kDy[i] + kDySpread[i] * s, kDy[i + 1] + kDySpread[i + 1] * s);
        l.scale = lerp(kScale[i] + kScaleSpread[i] * s, kScale[i + 1] + kScaleSpread[i + 1] * s);
        l.dim = lerp(kDim[i], kDim[i + 1]);
        l.opacity = d <= 2.0f ? 1.0f : std::clamp(3.0f - d, 0.0f, 1.0f);
        return l;
    }

    struct TrayCardPose {
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Identity(); // card-rest space -> panel space
        float dim = 0.0f;       // 0..~1.8 layer dimming (renderer maps per theme)
        float opacity = 1.0f;
    };

    TrayCardPose TrayCardPoseOf(const TrayStackGeom& g, const TrayCardView& card, float spread) {
        const TrayLayer layer = TrayLayerAt(card.depth, spread);
        const float appear = std::clamp(card.appear, 0.0f, 1.0f);
        const float hover = std::clamp(card.hover, 0.0f, 1.0f);
        const float w = g.card.right - g.card.left;
        const float s = layer.scale * card.shrink * (1.0f + 0.05f * (1.0f - appear));
        const D2D1_POINT_2F pivot = D2D1::Point2F((g.card.left + g.card.right) * 0.5f, g.card.bottom);
        const D2D1_POINT_2F centre = D2D1::Point2F((g.card.left + g.card.right) * 0.5f,
                                                   (g.card.top + g.card.bottom) * 0.5f);
        const float tx = card.fly * w + card.dx * g.scale;
        const float ty = (layer.dy + card.dy - 2.0f * hover - 34.0f * (1.0f - appear)) * g.scale;
        TrayCardPose p;
        p.m = D2D1::Matrix3x2F::Scale(s, s, pivot) *
              D2D1::Matrix3x2F::Rotation(card.angle, centre) *
              D2D1::Matrix3x2F::Translation(tx, ty);
        p.dim = layer.dim;
        p.opacity = std::clamp(card.opacity, 0.0f, 1.0f) * layer.opacity * (0.15f + 0.85f * appear);
        return p;
    }

    // Back-to-front paint order: deeper layers first, ghosts under live cards
    // at the same depth, then anything flagged on_top (dragged / flying out).
    std::vector<int> TrayCardPaintOrder(const TrayDeckView& deck) {
        std::vector<int> order(deck.cards.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) order[static_cast<size_t>(i)] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const TrayCardView& ca = deck.cards[static_cast<size_t>(a)];
            const TrayCardView& cb = deck.cards[static_cast<size_t>(b)];
            if (ca.on_top != cb.on_top) return cb.on_top;
            if (std::abs(ca.depth - cb.depth) > 0.001f) return ca.depth > cb.depth;
            if (ca.ghost != cb.ghost) return ca.ghost;
            return a > b;
        });
        return order;
    }

    // Close badge centre in card-rest space.
    D2D1_POINT_2F TrayCloseCentre(const TrayStackGeom& g) {
        return D2D1::Point2F(g.card.right - 17.0f * g.scale, g.card.top + 17.0f * g.scale);
    }

    // Copy/move intent chip, bottom-right in card-rest space.
    D2D1_RECT_F TrayIntentRect(const TrayStackGeom& g) {
        const float w = 40.0f * g.scale, h = 18.0f * g.scale, m = 8.0f * g.scale;
        return D2D1::RectF(g.card.right - m - w, g.card.bottom - m - h,
                           g.card.right - m, g.card.bottom - m);
    }

    // Hit test a point against a posed card (inverse transform into rest space).
    bool TrayCardHit(const TrayStackGeom& g, const TrayCardView& card, float spread,
                     float x, float y, bool* close_zone, bool* intent_zone = nullptr) {
        TrayCardPose pose = TrayCardPoseOf(g, card, spread);
        D2D1::Matrix3x2F m = *D2D1::Matrix3x2F::ReinterpretBaseType(&pose.m);
        if (!m.Invert()) return false;
        const D2D1_POINT_2F local = m.TransformPoint(D2D1::Point2F(x, y));
        if (local.x < g.card.left || local.x >= g.card.right ||
            local.y < g.card.top || local.y >= g.card.bottom) return false;
        if (close_zone) {
            const D2D1_POINT_2F c = TrayCloseCentre(g);
            const float dx = local.x - c.x, dy = local.y - c.y;
            *close_zone = dx * dx + dy * dy <= (11.0f * g.scale) * (11.0f * g.scale);
        }
        if (intent_zone) {
            const D2D1_RECT_F ir = TrayIntentRect(g);
            const float slop = 3.0f * g.scale;
            *intent_zone = local.x >= ir.left - slop && local.x < ir.right + slop &&
                           local.y >= ir.top - slop && local.y < ir.bottom + slop;
        }
        return true;
    }

    // "2 / 5": position of the top card in the cyclic stack.
    std::wstring TrayIndexText(const TrayDeckView& deck) {
        if (deck.total_count <= 0) return L"";
        wchar_t buf[32];
        swprintf_s(buf, L"%d / %d", (deck.offset % deck.total_count) + 1, deck.total_count);
        return buf;
    }

    // Footer row: pager (‹ n / N ›) on the left when there is more than one
    // card, totals after it, 清空 on the right (TrayClear slot).
    struct TrayFooterGeom {
        D2D1_RECT_F row{};
        D2D1_RECT_F prev{};
        D2D1_RECT_F index{};
        D2D1_RECT_F next{};
        D2D1_RECT_F totals{};
        D2D1_RECT_F compare{};   // "⇄ Compare" pill (two staged files)
        bool pager = false;
    };
    TrayFooterGeom TrayFooterGeometry(const D2D1_RECT_F& panel, float scale, int total,
                                      float index_text_w, bool compare = false) {
        TrayFooterGeom f;
        const float h = 22.0f * scale;
        const float bottom = panel.bottom - 8.0f * scale;
        f.row = D2D1::RectF(panel.left + 10.0f * scale, bottom - h,
                            panel.right - 10.0f * scale, bottom);
        float x = f.row.left - 4.0f * scale;
        f.pager = total > 1;
        if (f.pager) {
            const float b = 22.0f * scale;
            f.prev = D2D1::RectF(x, f.row.top, x + b, f.row.bottom);
            x += b;
            f.index = D2D1::RectF(x, f.row.top, x + index_text_w + 4.0f * scale, f.row.bottom);
            x = f.index.right;
            f.next = D2D1::RectF(x, f.row.top, x + b, f.row.bottom);
            x += b + 4.0f * scale;
        } else {
            x = f.row.left;
        }
        f.totals = D2D1::RectF(x, f.row.top, f.row.right - 76.0f * scale, f.row.bottom);
        if (compare) {
            const float right = f.row.right - 78.0f * scale;
            f.compare = D2D1::RectF(right - 76.0f * scale, f.row.top, right, f.row.bottom);
            f.totals.right = std::max(f.totals.left, f.compare.left - 4.0f * scale);
        }
        return f;
    }

    // ---------------------------------------------------------------------
    // Details panel: one layout for draw + hit-test (tray-deck idiom).
    // ---------------------------------------------------------------------

    // Fixed 16:10 band driven by the panel width; no manual resize.
    float DetailsPreviewHeight(const D2D1_RECT_F& panel, float scale, float expansion = 0.0f) {
        const float w = panel.right - panel.left - 24.0f * scale - 24.0f * scale;
        const float h = w * 10.0f / 16.0f;
        const float available = std::max(1.0f, panel.bottom - panel.top - 40.0f * scale);
        const float normal = std::min(available, std::clamp(h, 160.0f * scale, 360.0f * scale));
        return normal + (available - normal) * std::clamp(expansion, 0.0f, 1.0f);
    }
    float DetailsPreviewBand(float preview_h, float scale) {
        return preview_h + 32.0f * scale;
    }

    // Display order: 基本信息 / 标签 / 属性 / 安全 / 其他. Collapsed state is
    // keyed by def.id, so reordering is safe.
    struct DetailsSectionDef { int id; pulse::l10n::StringId label; };
    constexpr DetailsSectionDef kDetailsSections[] = {
        { 0, pulse::l10n::StringId::DetailsBasic },
        { 2, pulse::l10n::StringId::DetailsTags },
        { 1, pulse::l10n::StringId::DetailsAttributes },
        { 3, pulse::l10n::StringId::DetailsSecurity },
        { 4, pulse::l10n::StringId::DetailsOther },
    };

    // Button row labels, shared between layout (width measurement) and drawing.
    constexpr pulse::l10n::StringId kDetailsButtonLabels[4] = {
        pulse::l10n::StringId::Open, pulse::l10n::StringId::OpenNewTab,
        pulse::l10n::StringId::CopyPath, pulse::l10n::StringId::More,
    };

    // Manual divider minimums and fallback widths (DIP). Automatic layouts
    // measure the displayed metadata instead of reserving a fixed type width.
    constexpr float kDetailsMinNameDip = 80.0f;
    constexpr float kDetailsMinPathDip = 110.0f;
    constexpr float kDetailsMinDateDip = 92.0f;
    constexpr float kDetailsMinTypeDip = 48.0f;
    constexpr float kDetailsMinSizeDip = 48.0f;
    constexpr float kDetailsDateDip = 130.0f;
    constexpr float kDetailsTypeDip = 128.0f;
    constexpr float kDetailsSizeDip = 90.0f;
    constexpr float kDetailsFitNameDip = 210.0f;     // narrower name: drop Type, then Date
    constexpr float kDetailsFitPathDip = 150.0f;     // search: narrower path moves under the name
    constexpr float kDetailsTwoLineMinRowDip = 42.0f;
    constexpr float kSizeUnitDip = 28.0f;            // unit sub-column ("KB", "MB") of Size
    constexpr float kTypeChipPadDip = 5.0f;
    constexpr float kTypeChipGapDip = 6.0f;

    std::wstring WeekdayName(int day) {
        const std::wstring all = pulse::l10n::Get(pulse::l10n::StringId::DateWeekdays);
        size_t start = 0;
        for (int i = 0; i < day; ++i) {
            const size_t comma = all.find(L',', start);
            if (comma == std::wstring::npos) return L"";
            start = comma + 1;
        }
        const size_t end = all.find(L',', start);
        return all.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    }

    // Recent times read faster relative to today: "今天 14:32", "昨天 09:10",
    // "周三 18:00", this year "4月13日 22:28", older "2024-04-02".
    std::wstring SmartListDate(uint64_t filetime) {
        if (!filetime) return L"";
        FILETIME ft{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
        FILETIME local{};
        SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
        SYSTEMTIME now{};
        GetLocalTime(&now);
        auto day_number = [](SYSTEMTIME day) {
            day.wHour = day.wMinute = day.wSecond = day.wMilliseconds = 0;
            FILETIME f{};
            SystemTimeToFileTime(&day, &f);
            return static_cast<long long>(((static_cast<uint64_t>(f.dwHighDateTime) << 32) |
                                           f.dwLowDateTime) / 864000000000ull);
        };
        const long long diff = day_number(now) - day_number(st);
        wchar_t hm[16];
        swprintf_s(hm, L"%02u:%02u", st.wHour, st.wMinute);
        using pulse::l10n::StringId;
        if (diff == 0) return pulse::l10n::Get(StringId::DateToday) + L" " + hm;
        if (diff == 1) return pulse::l10n::Get(StringId::DateYesterday) + L" " + hm;
        if (diff > 1 && diff < 7) return WeekdayName(st.wDayOfWeek) + L" " + hm;
        wchar_t text[64];
        if (diff > 0 && st.wYear == now.wYear) {
            swprintf_s(text, pulse::l10n::Get(StringId::DateMonthDayFormat).c_str(), st.wMonth, st.wDay);
            return std::wstring(text) + L" " + hm;
        }
        if (diff < 0) {
            swprintf_s(text, L"%04u-%02u-%02u %s", st.wYear, st.wMonth, st.wDay, hm);
            return text;
        }
        swprintf_s(text, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
        return text;
    }

    // Widest strings SmartListDate can produce (column fitting).
    std::vector<std::wstring> SmartDateSamples() {
        using pulse::l10n::StringId;
        std::vector<std::wstring> out{pulse::l10n::Get(StringId::DateToday) + L" 23:59",
                                      pulse::l10n::Get(StringId::DateYesterday) + L" 23:59",
                                      L"2026-12-30"};
        for (int day = 0; day < 7; ++day) out.push_back(WeekdayName(day) + L" 23:59");
        wchar_t text[64];
        swprintf_s(text, pulse::l10n::Get(StringId::DateMonthDayFormat).c_str(), 12u, 30u);
        out.push_back(std::wstring(text) + L" 23:59");
        return out;
    }

    // Uppercase extension shown as a colored chip before the type (<= 6 chars).
    std::wstring TypeChipLabel(const std::wstring& name, bool is_dir) {
        if (is_dir) return L"";
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
        std::wstring ext = name.substr(dot + 1);
        if (ext.size() > 6) return L"";
        for (auto& c : ext) {
            if (!std::iswalnum(c)) return L"";
            c = static_cast<wchar_t>(std::towupper(c));
        }
        return ext;
    }

    // Extension chip colors grouped by format family so neighbouring kinds stay
    // distinguishable (CAD vs PDF vs Office ...). Hues are mid-tone; the list
    // painter derives theme-specific ink/fill/edge from them.
    namespace type_chip_detail {
        inline std::wstring Lower(const std::wstring& chip) {
            std::wstring e = chip;
            for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
            return e;
        }
        inline bool In(const std::wstring& e, std::initializer_list<const wchar_t*> list) {
            for (const wchar_t* x : list) if (e == x) return true;
            return false;
        }
    }

    // Backup / lock / temp companions (e.g. AutoCAD .bak/.dwl/.dwl2): drawn neutral and quiet.
    bool TypeChipIsAux(const std::wstring& chip) {
        using namespace type_chip_detail;
        const std::wstring e = Lower(chip);
        return In(e, {L"bak", L"dwl", L"dwl2", L"tmp", L"temp", L"old", L"lck", L"lock", L"sv$", L"ac$",
                      L"bk1", L"bk2", L"bk3", L"swp", L"crdownload", L"part"});
    }

    uint32_t TypeChipRgb(const std::wstring& chip) {
        using namespace type_chip_detail;
        const std::wstring e = Lower(chip);
        if (In(e, {L"dwg", L"dxf", L"dwf", L"dwfx", L"dwt", L"dws", L"dgn"})) return 0x0891B2;      // CAD: blueprint cyan
        if (In(e, {L"rvt", L"rfa", L"rte", L"skp", L"3dm", L"ifc", L"nwd", L"nwc", L"max", L"fbx",
                   L"obj", L"stp", L"step", L"igs", L"iges", L"stl"})) return 0x6366F1;                  // BIM / 3D: indigo
        if (e == L"pdf") return 0xDC2626;                                                              // PDF: red
        if (In(e, {L"doc", L"docx", L"docm", L"rtf", L"odt", L"wps"})) return 0x2563EB;               // Word: blue
        if (In(e, {L"xls", L"xlsx", L"xlsm", L"xlsb", L"ods", L"csv", L"et"})) return 0x16A34A;       // Excel: green
        if (In(e, {L"ppt", L"pptx", L"pptm", L"odp", L"dps"})) return 0xEA580C;                       // PowerPoint: orange
        if (In(e, {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"svg", L"ico", L"heic", L"tif",
                   L"tiff", L"psd", L"ai", L"raw"})) return 0xA855F7;                                   // image: purple
        if (In(e, {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"flv", L"m4v"})) return 0xDB2777; // video: pink
        if (In(e, {L"mp3", L"wav", L"flac", L"aac", L"ogg", L"m4a", L"wma"})) return 0x0D9488;         // audio: teal
        if (In(e, {L"zip", L"rar", L"7z", L"tar", L"gz", L"xz", L"cab", L"iso", L"zst"})) return 0xCA8A04; // archive: amber
        if (In(e, {L"exe", L"msi", L"appx", L"msix", L"bat", L"cmd", L"com", L"lnk"})) return 0x65A30D; // program: lime
        if (In(e, {L"dll", L"sys", L"ocx", L"drv", L"cpl", L"mui", L"efi"})) return 0x3B82F6;         // system
        if (In(e, {L"ps1", L"py", L"js", L"ts", L"cpp", L"c", L"h", L"hpp", L"cs", L"java", L"go", L"rs",
                   L"sh", L"json", L"xml", L"yaml", L"yml", L"ini", L"toml", L"cmake", L"html", L"css",
                   L"inf", L"reg"})) return 0xD97706;                                                   // code / config
        if (In(e, {L"txt", L"md", L"log"})) return 0x64748B;                                           // text
        return 0x64748B;
    }

    // "C:\a\b\c\d\e" -> head "C:\…\d\", last "e" (last segment kept whole when possible).
    template <typename Measure>
    std::pair<std::wstring, std::wstring> MiddleEllipsisPath(const std::wstring& path, float width,
                                                             Measure measure) {
        if (path.empty() || measure(path) <= width) return {path, L""};
        const size_t cut = path.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut + 1 >= path.size()) return {path, L""};
        const std::wstring last = path.substr(cut + 1);
        std::vector<std::wstring> parts;
        size_t start = 0;
        while (start < cut) {
            const size_t next = path.find(L'\\', start);
            if (next == std::wstring::npos || next > cut) break;
            parts.push_back(path.substr(start, next - start));
            start = next + 1;
        }
        const std::wstring root = parts.empty() ? L"" : parts.front() + L"\\";
        for (int keep = std::min<int>(2, static_cast<int>(parts.size()) - 1); keep >= 0; --keep) {
            std::wstring head = root + L"\u2026\\";
            for (size_t i = parts.size() - static_cast<size_t>(keep); i < parts.size(); ++i)
                head += parts[i] + L"\\";
            if (measure(head + last) <= width) return {head, last};
        }
        return {L"\u2026\\", last};
    }

    // Longest prefix of text that fits with a trailing ellipsis.
    template <typename Measure>
    std::wstring FitEndEllipsis(const std::wstring& text, float width, Measure measure) {
        if (text.empty() || measure(text) <= width) return text;
        size_t lo = 0, hi = text.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (measure(text.substr(0, mid) + L"\u2026") <= width) lo = mid; else hi = mid - 1;
        }
        if (lo > 0 && IS_HIGH_SURROGATE(text[lo - 1])) --lo;
        return text.substr(0, lo) + L"\u2026";
    }

    float MeasureLayoutText(Compositor* compositor, IDWriteFactory2* dwrite,
                            IDWriteTextFormat* format, const std::wstring& text) {
        (void)dwrite;
        return typography::MeasureLine(compositor, format, text);
    }

    void LayoutDetailsPanel(const D2D1_RECT_F& panel, float scale,
                            const DetailsPanelView& d, IDWriteFactory2* dwrite,
                            IDWriteTextFormat* small_fmt, Compositor* compositor,
                            float preview_h, DetailsHitRects& out) {
        const float s = scale;
        const float pad = 12.0f * s;
        const float x = panel.left + pad;
        const float w = panel.right - panel.left - pad * 2.0f;
        out = DetailsHitRects{};
        if (!d.has_selection) return;
        out.preview = D2D1::RectF(x, panel.top + pad, panel.right - pad,
                                  panel.top + pad + preview_h);
        const float previewBottom = panel.top + pad + preview_h;
        // Preview off: no fold bar and no well to hit up there.
        if (d.preview_enabled) {
            out.preview_toggle = D2D1::RectF(x, previewBottom, panel.right - pad,
                std::min(panel.bottom, previewBottom + 28.0f * s));
        }
        if (d.preview_only && d.preview_enabled) {
            out.content_height_dip = (panel.bottom - panel.top) / s;
            return;
        }
        float y = previewBottom + 32.0f * s - d.scroll_y * s;
        if (d.multi_count <= 1) {
            // Name row: star then rename pencil on the right edge.
            out.rename = D2D1::RectF(panel.right - pad - 22.0f * s, y,
                                     panel.right - pad, y + 22.0f * s);
            out.star = D2D1::RectF(out.rename.left - 4.0f * s - 22.0f * s, y,
                                   out.rename.left - 4.0f * s, y + 22.0f * s);
            // Preview on/off sits left of the star, so it stays reachable after
            // the well itself collapses.
            out.preview_enable = D2D1::RectF(out.star.left - 4.0f * s - 22.0f * s, y,
                                             out.star.left - 4.0f * s, y + 22.0f * s);
        }
        y += 22.0f * s + 16.0f * s + 8.0f * s; // name + type + gap
        if (d.multi_count <= 1) {
            // Button row: 打开 / 在新标签打开 / 复制路径 / 更多 (icon over label).
            // Buttons size to their measured label width; leftover space is
            // shared equally so the row still spans the panel. Very narrow
            // panels shrink proportionally (labels then ellipsize on draw).
            const float rowH = 48.0f * s;
            const float gap = 6.0f * s;
            const float avail = w - gap * 3.0f;
            const float equal = avail / 4.0f;
            float bw[4] = { equal, equal, equal, equal };
            if (dwrite && small_fmt) {
                float needed_total = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    bw[i] = MeasureLayoutText(compositor, dwrite, small_fmt,
                                              pulse::l10n::Get(kDetailsButtonLabels[i]))
                        + 12.0f * s; // 4s text padding + slack around the label
                    needed_total += bw[i];
                }
                if (needed_total < avail) {
                    const float extra = (avail - needed_total) / 4.0f;
                    for (auto& v : bw) v += extra;
                } else if (needed_total > 0.0f) {
                    const float shrink = avail / needed_total;
                    for (auto& v : bw) v = std::max(40.0f * s, v * shrink);
                }
            }
            D2D1_RECT_F* cells[4] = { &out.open, &out.new_tab, &out.copy_path, &out.more };
            float bx = x;
            for (int i = 0; i < 4; ++i) {
                *cells[i] = D2D1::RectF(bx, y, bx + bw[i], y + rowH);
                bx += bw[i] + gap;
            }
            y += rowH + 4.0f * s;

            for (const auto& def : kDetailsSections) {
                y += 8.0f * s; // separator gap (line drawn inside DrawDetailsPanel)
                out.section_headers.push_back(D2D1::RectF(x, y, x + w, y + 28.0f * s));
                out.section_ids.push_back(def.id);
                y += 28.0f * s;
                if ((d.collapsed_mask >> def.id) & 1u) { y += 6.0f * s; continue; }
                switch (def.id) {
                case 0: { // 基本信息
                    const int rows = (d.is_dir ? 7 : 6) +
                                     static_cast<int>(d.preview_properties.size());
                    y += static_cast<float>(rows) * 18.0f * s + 8.0f * s;
                    break;
                }
                case 1: { // 属性: 只读 / 隐藏 checkboxes + 高级… on one row
                    const float advW = 64.0f * s, attrRowH = 24.0f * s, g = 8.0f * s;
                    const float cbW = (w - advW - g * 2.0f) / 2.0f;
                    out.attr_readonly = D2D1::RectF(x, y, x + cbW, y + attrRowH);
                    out.attr_hidden = D2D1::RectF(x + cbW + g, y,
                                                  x + cbW + g + cbW, y + attrRowH);
                    out.attr_advanced = D2D1::RectF(x + w - advW, y, x + w, y + attrRowH);
                    y += attrRowH + 8.0f * s;
                    break;
                }
                case 2: { // 标签: 添加标签 row + preset chip grid (wraps)
                    out.tag_add = D2D1::RectF(x, y, x + w, y + 26.0f * s);
                    y += 26.0f * s + 6.0f * s;
                    float cx = x;
                    const float chipH = 22.0f * s;
                    bool any = false;
                    for (const auto& chip : d.preset_tags) {
                        const float tw = std::min(72.0f * s,
                            MeasureLayoutText(compositor, dwrite, small_fmt, chip.name));
                        const float cw = tw + 24.0f * s;
                        if (cx + cw > panel.right - pad) {
                            cx = x;
                            y += chipH + 6.0f * s; // next row
                        }
                        out.preset_chips.push_back(D2D1::RectF(cx, y, cx + cw,
                                                               y + chipH));
                        out.preset_ids.push_back(chip.tag_index);
                        cx += cw + 8.0f * s;
                        any = true;
                    }
                    if (any) y += chipH;
                    y += 8.0f * s;
                    break;
                }
                case 3: // 安全: 所有者(+更改) / 权限
                    out.security_change = D2D1::RectF(x + w - 48.0f * s, y, x + w,
                                                      y + 18.0f * s);
                    y += 2.0f * 18.0f * s + 8.0f * s;
                    break;
                case 4: // 其他: 驱动器/文件系统/可用空间
                    y += 3.0f * 18.0f * s + 8.0f * s;
                    break;
                default: break;
                }
            }
        } else {
            // Multi-selection: preview + summary only.
            y += 20.0f * s + 2.0f * 18.0f * s + 8.0f * s;
        }
        out.content_height_dip = (y + d.scroll_y * s - panel.top) / s;
    }

D2D1_RECT_F StepLeftHeaderButton(const D2D1_RECT_F& rc, float scale) {
    const float step = kCommandIconStepDip * scale;
    const float btn = kCommandIconButtonDip * scale;
    return D2D1::RectF(rc.left - step, rc.top, rc.left - (step - btn), rc.bottom);
}
void FillRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float x, float y, float w, float h) {
    dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br);
}

void FillRoundedRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
    float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);
    dc->FillRoundedRectangle(&rr, br);
}
float MeasureTextWidth(IDWriteFactory2* factory, IDWriteTextFormat* fmt, const std::wstring& text) {
    if (!factory || !fmt || text.empty()) return 0.0f;
    const std::uint64_t generation = typography::Generation();
    if (const auto cached = g_text_width_cache.find(TextWidthLookup{fmt, text, generation});
        cached != g_text_width_cache.end())
        return cached->second;
    const float fallback = fmt->GetFontSize() * static_cast<float>(text.size());
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
        10000.0f, 100.0f, &layout)) || !layout.get()) return fallback;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return fallback;
    DWRITE_OVERHANG_METRICS om{};
    layout->GetOverhangMetrics(&om);
    const float width = m.widthIncludingTrailingWhitespace + std::max(0.0f, om.right) + 1.0f;
    if (g_text_width_cache.size() >= kTextWidthCacheLimit) g_text_width_cache.clear();
    g_text_width_cache.emplace(TextWidthKey{fmt, text, generation}, width);
    return width;
}

// Status-bar columns shared by DrawStatusBar and HitTest. Independent of the
// global 4 DIP chrome margin so stats sit clear of the resize grip.
constexpr float kStatusBarPadDip = 12.0f;

struct StatusBarMetrics {
    D2D1_RECT_F bar{};
    D2D1_RECT_F task{};
    D2D1_RECT_F cancel_search{};
    D2D1_RECT_F hint_action{};   // empty unless the hint carries a clickable action
    float pad = 0.0f;
    float right_reserved = 0.0f;
};

StatusBarMetrics MakeStatusBarMetrics(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                      float scale, float status_height,
                                      IDWriteFactory2* factory, IDWriteTextFormat* small_format) {
    const bool centered_progress = vm.status.query_active || vm.status.task_is_update;
    StatusBarMetrics m;
    m.bar = D2D1::RectF(rect.left, rect.bottom - status_height, rect.right, rect.bottom);
    m.pad = kStatusBarPadDip * scale;
    m.right_reserved = m.pad;
    const std::wstring* trailing = nullptr;
    if (!centered_progress && !vm.status.performance_text.empty()) {
        trailing = rect.right < 1100.0f * scale
            ? &vm.status.performance_compact_text : &vm.status.performance_text;
    } else if (!centered_progress && !vm.status.hint_text.empty()) {
        trailing = &vm.status.hint_text;
    }
    if (trailing && factory && small_format) {
        const float perfWidth = std::min(rect.right * 0.50f,
            MeasureTextWidth(factory, small_format, *trailing) + 16.0f * scale);
        m.right_reserved = perfWidth + m.pad;
    }
    if (trailing == &vm.status.hint_text && !vm.status.hint_action_text.empty() &&
        !vm.status.query_cancellable && factory && small_format) {
        // The action chip sits at the far right; the hint text moves left of it.
        const float width = MeasureTextWidth(factory, small_format, vm.status.hint_action_text) + 16.0f * scale;
        m.hint_action = D2D1::RectF(std::max(rect.left, rect.right - m.pad - width), m.bar.top + 5.0f * scale,
                                    rect.right - m.pad, m.bar.bottom - 5.0f * scale);
        m.right_reserved += width + 8.0f * scale;
    }
    if (vm.status.query_cancellable) {
        const float width = 22.0f * scale;
        m.cancel_search = D2D1::RectF(std::max(rect.left, rect.right - m.pad - width),
            m.bar.top + 2.0f * scale, rect.right - m.pad, m.bar.bottom - 2.0f * scale);
        m.right_reserved += width + 8.0f * scale;
    }
    const bool has_task = vm.status.query_active || !vm.status.task_text.empty() || vm.status.task_progress >= 0.0f;
    if (has_task) {
        m.task = D2D1::RectF(rect.right * (vm.status.query_cancellable ? 0.40f : 0.48f), m.bar.top,
                             rect.right - m.right_reserved, m.bar.bottom);
    }
    if (centered_progress) {
        const float cancel_width = vm.status.query_cancellable ? 30.0f * scale : 0.0f;
        const float width = std::min(std::max(0.0f, rect.right - rect.left - 2.0f * m.pad),
            MeasureTextWidth(factory, small_format, vm.status.query_active ? vm.status.query_text : vm.status.task_text) + 112.0f * scale + cancel_width);
        const float left = (rect.left + rect.right - width) * 0.5f;
        m.task = D2D1::RectF(left, m.bar.top, left + width - cancel_width, m.bar.bottom);
        if (vm.status.query_cancellable)
            m.cancel_search = D2D1::RectF(m.task.right + 8.0f * scale, m.bar.top + 2.0f * scale,
                left + width, m.bar.bottom - 2.0f * scale);
    }
    return m;
}

fluent::BadgeKind IndexVolumeBadgeKind(const std::wstring& state) {
    // `state` is the Simplified text reported by the index service (the view
    // model keeps it next to the localized label); Traditional is accepted too.
    const auto has = [&state](const wchar_t* simplified) {
        return state.find(simplified) != std::wstring::npos ||
               state.find(pulse::l10n::Cn(simplified)) != std::wstring::npos;
    };
    if (has(L"失败")) return fluent::BadgeKind::Danger;
    if (has(L"非 NTFS") || has(L"不支持")) return fluent::BadgeKind::Warning;
    if (has(L"正在") || has(L"等待")) return fluent::BadgeKind::Accent;
    if (has(L"就绪") || has(L"实时")) return fluent::BadgeKind::Success;
    return fluent::BadgeKind::Neutral;
}

// Containing folder of a full item path, for the search-results path column.
std::wstring FolderOf(const std::wstring& path) {
    if (path.empty()) return {};
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 0) return p.substr(0, 1);
    // "C:\" roots keep the backslash; UNC "\\server\share" keeps both slashes.
    if (slash == 2 && p[1] == L':') return p.substr(0, slash + 1);
    if (slash == 1 && p[0] == L'\\') return p.substr(0, 2);
    return p.substr(0, slash);
}

// Single-line text with end ellipsis (character granularity) when too wide.
void DrawTextEndEllipsis(ID2D1DeviceContext* dc, IDWriteFactory2* factory,
    IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br, const std::wstring& text,
    float x, float y, float w, float h) {
    if (!dc || !fmt || text.empty() || w <= 1.0f || h <= 0.0f) return;
    const D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    if (!factory || MeasureTextWidth(factory, fmt, text) <= w) {
        dc->DrawText(text.c_str(), (UINT32)text.size(), fmt, &rc, br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
                                         w, h, &layout)) || !layout.get()) {
        dc->DrawText(text.c_str(), (UINT32)text.size(), fmt, &rc, br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(fmt->GetTextAlignment());
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.get());
    dc->DrawTextLayout(D2D1::Point2F(x, y), layout.get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawTabTitle(ID2D1DeviceContext* dc, IDWriteFactory2* factory,
                         IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
                         const std::wstring& text, float x, float y, float w, float h,
                         float scale) {
    if (!dc || !fmt || text.empty() || w <= 1.0f || h <= 1.0f) return;
    const D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    const float fullW = MeasureTextWidth(factory, fmt, text);
    if (fullW <= w) {
        dc->DrawText(text.c_str(), (UINT32)text.size(), fmt, &rc, br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        return;
    }

    auto draw_ellipsis = [&](const std::wstring& s, float left, float width) {
        if (!factory || width <= 1.0f) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                             width, h, &layout)) || !layout.get()) {
            const D2D1_RECT_F clip = D2D1::RectF(left, y, left + width, y + h);
            dc->DrawText(s.c_str(), (UINT32)s.size(), fmt, &clip, br,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            return;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
        layout->SetTrimming(&trimming, ellipsis.get());
        dc->DrawTextLayout(D2D1::Point2F(left, y), layout.get(), br,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Keep a short extension visible so "very-long-report-name.pdf" still reads as a PDF.
    size_t dot = text.find_last_of(L'.');
    std::wstring ext;
    std::wstring stem = text;
    if (dot != std::wstring::npos && dot > 0 && dot + 1 < text.size() &&
        (text.size() - dot) <= 12) {
        ext = text.substr(dot);
        stem = text.substr(0, dot);
    }
    const float extW = MeasureTextWidth(factory, fmt, ext);
    if (!ext.empty() && extW + 20.0f * scale < w) {
        draw_ellipsis(stem, x, w - extW);
        const D2D1_RECT_F ext_rc = D2D1::RectF(x + w - extW, y, x + w, y + h);
        dc->DrawText(ext.c_str(), (UINT32)ext.size(), fmt, &ext_rc, br,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        return;
    }
    draw_ellipsis(text, x, w);
}

void MakeBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c, ComPtr<ID2D1SolidColorBrush>& br) {
    if (!br.get()) dc->CreateSolidColorBrush(c, &br);
    else br->SetColor(c);
}
struct TitleChrome {
    float chrome_left = 0.0f;
    float settings_left = 0.0f;
    float settings_w = 0.0f;
    float theme_left = 0.0f;
    float theme_w = 0.0f;
    // Settings + theme share one pill, kept apart from the window controls.
    float group_left = 0.0f;
    float group_right = 0.0f;
    float cmd_left = 0.0f;
    float cmd_w = 0.0f;
    float cmd_top = 0.0f;
    float cmd_h = 0.0f;
};

TitleChrome MakeTitleChrome(float window_w, float scale, float title_h) {
    TitleChrome c;
    const float ctrl_w = 46.0f * scale;
    c.chrome_left = window_w - ctrl_w * 3.0f;
    c.theme_w = 36.0f * scale;
    c.settings_w = 36.0f * scale;
    const float group_pad = 2.0f * scale;
    c.group_right = c.chrome_left - 14.0f * scale;
    c.theme_left = c.group_right - group_pad - c.theme_w;
    c.settings_left = c.theme_left - 1.0f * scale - c.settings_w;   // 1 DIP divider
    c.group_left = c.settings_left - group_pad;
    c.cmd_w = 0.0f;
    c.cmd_left = c.settings_left;
    const float cmd_pad = 8.0f * scale;
    c.cmd_top = cmd_pad;
    c.cmd_h = std::max(0.0f, title_h - cmd_pad * 2.0f);
    return c;
}

constexpr float kSettingsNavW = 200.0f;
constexpr int kSettingsNavCount = 6;  // 5: 预览增强包

struct SettingsLayout {
    // Cards 0-4: Explorer groups (with a master switch). Card 5: Pulse's own
    // commands, rows only (context_toggle[5] stays empty).
    static constexpr int kContextCards = 6;
    D2D1_RECT_F context_cards[kContextCards]{}, context_header[kContextCards]{}, context_toggle[kContextCards]{},
        context_empty[kContextCards]{}, context_restore{};
    std::vector<D2D1_RECT_F> context_rows;
    D2D1_RECT_F duplicate_options{};
    D2D1_RECT_F section[4]{}, group[3]{}, footer{};
    D2D1_RECT_F theme_row{}, theme_tile[3]{}, effect_choice{}, language_choice{};
    D2D1_RECT_F performance_row{}, disclosure[4]{}, filename_status{};
    D2D1_RECT_F integration_card{}, integration_status{}, integration_summary{}, integration_hint{};
    D2D1_RECT_F integration_retry{}, integration_restore{};
    D2D1_RECT_F integration_section{}, integration_badge{}, integration_bar{}, integration_list_head{};
    D2D1_RECT_F integration_chip[4]{};
    float integration_text_right = 0.0f;
    // Quick Look page: formats, source legend and system extensions.
    D2D1_RECT_F preview_section{}, preview_group{}, preview_formats{}, preview_legend{}, preview_legend_note{};
    D2D1_RECT_F preview_header_title{}, preview_header_summary{};
    D2D1_RECT_F preview_legend_chip[3]{}, preview_codec_section{}, preview_codec_group{}, preview_codec_hint{}, pack_section{};
    D2D1_RECT_F preview_codec_text[kPreviewCodecCount]{}, preview_codec_badge[kPreviewCodecCount]{};
    D2D1_RECT_F preview_codec_row[kPreviewCodecCount]{}, preview_codec_button[kPreviewCodecCount]{};
    // 预览增强包 page (5), see LayoutSettingsPacks.
    D2D1_RECT_F pack_summary{}, pack_open{}, pack_media_section{}, pack_card{}, pack_badge{}, pack_enable{},
        pack_primary{}, pack_notice{}, pack_advanced_section{}, pack_custom_row{}, pack_path_row{},
        pack_detect{}, pack_browse{}, pack_remove_row{}, pack_group{}, pack_note{}, pack_source_group{}, pack_source_status{};
    float pack_desc_h = 0.0f;
    // The image pack card (same parts as the FFmpeg one).
    D2D1_RECT_F pack_images_section{}, pack_images_card{}, pack_images_badge{}, pack_images_enable{},
        pack_images_primary{}, pack_images_notice{};
    float pack_images_desc_h = 0.0f;
    D2D1_RECT_F pack_raw_card{}, pack_raw_badge{}, pack_raw_enable{}, pack_raw_primary{}, pack_raw_notice{};
    float pack_raw_desc_h = 0.0f;
    D2D1_RECT_F pack_archive_section{}, pack_archive_card{}, pack_archive_badge{}, pack_archive_enable{}, pack_archive_primary{}, pack_archive_notice{};
    float pack_archive_desc_h = 0.0f;
    D2D1_RECT_F content_header{}, content_types{}, content_pause{}, content_options{}, content_rebuild{}, content_empty{};
    D2D1_RECT_F body{};
    D2D1_RECT_F nav{};
    D2D1_RECT_F content{};
    D2D1_RECT_F nav_row[kSettingsNavCount]{};
    D2D1_RECT_F accent_card{};
    D2D1_RECT_F accent_picker{};
    D2D1_RECT_F effect_card{};
    D2D1_RECT_F effect_row[kWindowEffectCount]{};
    D2D1_RECT_F density_card{};
    D2D1_RECT_F list_style_row[6]{};
    D2D1_RECT_F density_row[3]{};
    D2D1_RECT_F folder_sort_card{};
    D2D1_RECT_F folder_sort_row[3]{};
    D2D1_RECT_F home_folder_card{};
    D2D1_RECT_F home_folder_choose{};
    D2D1_RECT_F home_folder_reset{};
    D2D1_RECT_F startup_open_card{};
    D2D1_RECT_F startup_open_row[2]{};
    D2D1_RECT_F notify_icon_card{};
    D2D1_RECT_F notify_icon_row[3]{};
    D2D1_RECT_F new_tab_open_card{};
    D2D1_RECT_F new_tab_open_row[2]{};
    D2D1_RECT_F close_last_tab_row{};
    D2D1_RECT_F close_tab_double_click_row{};
    D2D1_RECT_F confirm_delete_row{};
    D2D1_RECT_F start_in_tray_row{};
    D2D1_RECT_F text_render_card{};
    D2D1_RECT_F text_render_row[3]{};
    D2D1_RECT_F ui_font_size_card{};
    D2D1_RECT_F ui_font_size_row[4]{};
    D2D1_RECT_F tray_icon_card{};
    D2D1_RECT_F tray_icon_row[3]{};
    D2D1_RECT_F language_card{};
    D2D1_RECT_F language_segment[3]{};
    D2D1_RECT_F wallpaper_card{};
    D2D1_RECT_F wallpaper_preview{};
    D2D1_RECT_F wallpaper_choose{};
    D2D1_RECT_F wallpaper_clear{};
    D2D1_RECT_F wallpaper_look_card{};
    D2D1_RECT_F wallpaper_look_row[3]{};
    D2D1_RECT_F wallpaper_blur_card{};
    D2D1_RECT_F wallpaper_blur_row[3]{};
    D2D1_RECT_F startup_row[3]{};
    D2D1_RECT_F hidden_files_row{};
    D2D1_RECT_F protected_files_row{};
    D2D1_RECT_F pinned_names_row{};
    D2D1_RECT_F vertical_tabs_row{};
    D2D1_RECT_F hints_row{};
    D2D1_RECT_F hints_reset_row{};
    D2D1_RECT_F hints_reset_button{};
    D2D1_RECT_F blank_click_row{};
    D2D1_RECT_F blank_click_choice[3]{};
    D2D1_RECT_F win_e_row{};
    D2D1_RECT_F this_pc_row{};
    D2D1_RECT_F explorer_windows_row{};
    D2D1_RECT_F default_manager_row{};
    D2D1_RECT_F shell_tags_row{};
    D2D1_RECT_F change_tracking_row{}, change_days_row{}, change_days[3]{};
    D2D1_RECT_F search_pinyin_row{};
    D2D1_RECT_F global_search_row{}, global_search_hotkey_row{}, global_search_hotkey_button{};
    D2D1_RECT_F content_index_row{};
    D2D1_RECT_F index_info{};
    D2D1_RECT_F index_status{};
    D2D1_RECT_F index_path{};
    D2D1_RECT_F index_action[3]{};
    std::vector<D2D1_RECT_F> index_volume_rows;
    D2D1_RECT_F index_exclude_action{};
    D2D1_RECT_F index_exclude_empty{};
    std::vector<D2D1_RECT_F> index_exclude_rows;
    std::vector<D2D1_RECT_F> index_exclude_remove;
    D2D1_RECT_F network_action[2]{};
    std::vector<D2D1_RECT_F> network_rows;
    std::vector<D2D1_RECT_F> network_remove;
    D2D1_RECT_F about_card{};
    D2D1_RECT_F apps_card{};
    D2D1_RECT_F apps_row[2]{};  // LumenPDF, LumaShot
    D2D1_RECT_F diagnostics_card{};
    D2D1_RECT_F diagnostics_perf{};
    D2D1_RECT_F diagnostics_action[3]{};
    D2D1_RECT_F update_card{};
    D2D1_RECT_F update_action[2]{};
    D2D1_RECT_F update_auto_row{};  // empty when this build has no updater
    D2D1_RECT_F about_action[2]{};
    D2D1_RECT_F release_card{};
    D2D1_RECT_F release_all{};
    std::vector<D2D1_RECT_F> release_rows;
    D2D1_RECT_F release_body{};
    D2D1_RECT_F dup_scope[3]{};
    D2D1_RECT_F dup_browse{};
    D2D1_RECT_F dup_scan{};
    D2D1_RECT_F dup_cancel{};
    D2D1_RECT_F dup_min_size[3]{};
    std::vector<D2D1_RECT_F> dup_drives;
    D2D1_RECT_F dup_progress{};
    D2D1_RECT_F dup_delete_all{};
    std::vector<D2D1_RECT_F> dup_group_cards;
    std::vector<D2D1_RECT_F> dup_keep;
    std::vector<int> dup_keep_group;
    std::vector<int> dup_keep_file;
    std::vector<D2D1_RECT_F> dup_open;
    std::vector<int> dup_open_group;
    std::vector<int> dup_open_file;
    std::vector<D2D1_RECT_F> dup_group_delete;
    float content_origin = 0.0f;
    float content_h = 0.0f;
};

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool VisibleInContent(const D2D1_RECT_F& rc, const D2D1_RECT_F& content, float pad = 0.0f) {
    return rc.bottom > content.top - pad && rc.top < content.bottom + pad;
}

// Settings > About: label/value rows fill column-major into two columns on wide cards.
constexpr float kAboutTwoColumnMinDip = 680.0f;
constexpr float kAboutRowDip = 26.0f;
bool AboutTwoColumns(float card_w, float scale) {
    return card_w >= kAboutTwoColumnMinDip * scale;
}
size_t AboutRowLines(const WindowViewModel& vm, float card_w, float scale) {
    const size_t n = vm.settings_about_rows.size();
    return AboutTwoColumns(card_w, scale) ? (n + 1) / 2 : n;
}

// Release-note body text starts 30 DIP into the row and keeps 16 DIP on the right.
constexpr float kReleaseTextInsetDip = 30.0f;
constexpr float kReleaseTextRightDip = 16.0f;
constexpr float kReleaseLineGapDip = 6.0f;
float ReleaseTextWidth(float row_w, float scale) {
    return (std::max)(40.0f * scale, row_w - (kReleaseTextInsetDip + kReleaseTextRightDip) * scale);
}
float ReleaseLineHeight(const fluent::Painter* painter, const std::wstring& line, float width,
                        float scale) {
    const float h = painter ? painter->MeasureWrappedCaptionHeight(line, width) : 0.0f;
    return h > 0.0f ? h : 20.0f * scale;
}

#include "settings_layout_sections.h"


SettingsLayout MakeSettingsLayout(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                  float scale, float title_h, float status_h,
                                  const fluent::Painter* painter) {
    SettingsLayout l;
    l.body = D2D1::RectF(rect.left, title_h, rect.right, rect.bottom - status_h);
    const float nav_width = (rect.right - rect.left < 760 * scale ? 64.0f :
        rect.right - rect.left < 1000 * scale ? 200.0f : 220.0f) * scale;
    l.nav = D2D1::RectF(l.body.left, l.body.top, l.body.left + nav_width, l.body.bottom);
    l.content = D2D1::RectF(l.nav.right, l.body.top, l.body.right, l.body.bottom);
    for (int i = 0; i < kSettingsNavCount; ++i) {
        const int position = i == 4 ? 3 : i == 5 ? 4 : i;
        const float top = i == 3 ? l.nav.bottom - 92*scale : l.nav.top + (20 + position*46)*scale;
        l.nav_row[i] = D2D1::RectF(l.nav.left + 8*scale, top, l.nav.right - 8*scale, top + 40*scale);
    }
    const float pad = 20.0f * scale;
    auto label_btn_w = [&](std::wstring_view label) {
        if (painter) return painter->MeasureButtonWidth(label);
        return 88.0f * scale;
    };
    l.content_origin = l.content.top - vm.settings_scroll;
    float y = l.content_origin + pad;
    y += 36.0f * scale;
    y += 8.0f * scale;
    if (vm.settings_page == 0) {
        y = LayoutSettingsGeneral(l, vm, scale, y, painter);
    } else if (vm.settings_page == 1) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        y += 30*scale;
        l.section[0] = D2D1::RectF(card_left, y, card_right, y + 28*scale);
        y += 30*scale;
        l.global_search_row = D2D1::RectF(card_left, y, card_right, y + 88*scale);
        y += 88*scale;
        l.global_search_hotkey_row = D2D1::RectF(card_left, y, card_right, y + 108*scale);
        l.global_search_hotkey_button = D2D1::RectF(card_left + 54*scale, y + 62*scale, card_right - 16*scale, y + 98*scale);
        y += 108*scale;
        l.search_pinyin_row = D2D1::RectF(card_left, y, card_right, y + 68*scale);
        y += 68*scale;
        l.filename_status = D2D1::RectF(card_left, y, card_right, y + 72*scale);
        y += 72*scale;
        l.disclosure[1] = D2D1::RectF(card_left, y, card_right, y + 64*scale);
        y += 64*scale;
        l.group[0] = D2D1::RectF(card_left, l.global_search_row.top, card_right, y);
        y += 10*scale;
        if (vm.settings_expanded & 2u) {
        l.index_info = D2D1::RectF(card_left, y, card_right, y + 58.0f * scale);
        y += 70.0f * scale;
        l.index_status = D2D1::RectF(card_left, y, card_right, y + 82.0f * scale);
        y += 94.0f * scale;
        const std::wstring index_actions[] = {
            pulse::l10n::Get(pulse::l10n::StringId::Rebuild),
            pulse::l10n::Get(pulse::l10n::StringId::OpenLocation),
            pulse::l10n::Get(vm.settings_index_service
                ? pulse::l10n::StringId::ChangeLocation
                : pulse::l10n::StringId::InstallService),
        };
        const bool compact_actions = card_right - card_left < 650.0f * scale;
        const float path_h = compact_actions ? 116.0f * scale : 64.0f * scale;
        l.index_path = D2D1::RectF(card_left, y, card_right, y + path_h);
        if (compact_actions) {
            const float gap = 8.0f * scale;
            const float available = card_right - card_left - 32.0f * scale - gap * 2.0f;
            const float width = available / 3.0f;
            for (int i = 0; i < 3; ++i) {
                const float left = card_left + 16.0f * scale + i * (width + gap);
                l.index_action[i] = D2D1::RectF(left, y + 68.0f * scale,
                                                left + width, y + 100.0f * scale);
            }
        } else {
            float cursor = card_right - 12.0f * scale;
            for (int i = 0; i < 3; ++i) {
                const float width = label_btn_w(index_actions[i]);
                l.index_action[i] = D2D1::RectF(cursor - width, y + 16.0f * scale,
                                                cursor, y + 48.0f * scale);
                cursor -= width + 8.0f * scale;
            }
        }
        y += path_h + 44.0f * scale;
        l.index_volume_rows.reserve(vm.settings_index_volumes.size());
        for (size_t i = 0; i < vm.settings_index_volumes.size(); ++i) {
            l.index_volume_rows.push_back(D2D1::RectF(card_left, y, card_right,
                                                       y + 58.0f * scale));
            y += 58.0f * scale;
        }
        y += 44.0f * scale;
        const float section_btn_h = 32.0f * scale;
        const float add_folder_w = label_btn_w(
            pulse::l10n::Get(pulse::l10n::StringId::AddFolder));
        l.index_exclude_action = D2D1::RectF(card_right - add_folder_w, y - 36.0f * scale,
                                             card_right, y - 36.0f * scale + section_btn_h);
        const float remove_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::Remove));
        l.index_exclude_rows.reserve(vm.settings_index_excluded_paths.size());
        l.index_exclude_remove.reserve(vm.settings_index_excluded_paths.size());
        for (size_t i = 0; i < vm.settings_index_excluded_paths.size(); ++i) {
            const D2D1_RECT_F row = D2D1::RectF(card_left, y, card_right, y + 56.0f * scale);
            l.index_exclude_rows.push_back(row);
            l.index_exclude_remove.push_back(D2D1::RectF(
                row.right - 12.0f * scale - remove_w, row.top + 12.0f * scale,
                row.right - 12.0f * scale, row.top + 44.0f * scale));
            y += 56.0f * scale;
        }
        if (vm.settings_index_excluded_paths.empty()) {
            const float empty_h = 168.0f * scale;
            l.index_exclude_empty = D2D1::RectF(card_left, y, card_right, y + empty_h);
            y += empty_h;
        }
        y += 44.0f * scale;
        const float rescan_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::Rescan));
        l.network_action[0] = D2D1::RectF(card_right - add_folder_w, y - 36.0f * scale,
                                          card_right, y - 36.0f * scale + section_btn_h);
        l.network_action[1] = D2D1::RectF(
            l.network_action[0].left - 8.0f * scale - rescan_w, y - 36.0f * scale,
            l.network_action[0].left - 8.0f * scale, y - 36.0f * scale + section_btn_h);
        l.network_rows.reserve(vm.settings_network_roots.size());
        l.network_remove.reserve(vm.settings_network_roots.size());
        for (size_t i = 0; i < vm.settings_network_roots.size(); ++i) {
            const D2D1_RECT_F row = D2D1::RectF(card_left, y, card_right, y + 64.0f * scale);
            l.network_rows.push_back(row);
            l.network_remove.push_back(D2D1::RectF(
                row.right - 12.0f * scale - remove_w, row.top + 16.0f * scale,
                row.right - 12.0f * scale, row.top + 48.0f * scale));
            y += 64.0f * scale;
        }
        if (vm.settings_network_roots.empty()) y += 44.0f * scale;
        y += 24.0f * scale;
        }
        y = LayoutSettingsContent(l, vm, scale, y, painter);
    } else if (vm.settings_page == 2) {
        y += 30*scale;
        l.context_rows.resize(vm.settings_items.size());
        for(int g=0;g<SettingsLayout::kContextCards;++g) {
            const float top=y;
            l.context_header[g]=D2D1::RectF(l.content.left+pad,y,l.content.right-pad,y+76*scale);
            if(g<5) l.context_toggle[g]=D2D1::RectF(l.content.right-pad-100*scale,y+20*scale,l.content.right-pad-56*scale,y+52*scale);
            y+=76*scale;
            if(vm.settings_expanded & (1u<<(g+8))) {
                for(size_t i=0;i<vm.settings_items.size();++i) if(vm.settings_items[i].group==g) {
                    l.context_rows[i]=D2D1::RectF(l.content.left+pad+12*scale,y,l.content.right-pad-12*scale,y+40*scale);
                    y+=40*scale;
                }
                if(y==l.context_header[g].bottom) {
                    l.context_empty[g]=D2D1::RectF(l.content.left+pad+16*scale,y,l.content.right-pad-16*scale,y+44*scale);
                    y+=44*scale;
                }
                y+=8*scale;
            }
            l.context_cards[g]=D2D1::RectF(l.content.left+pad,top,l.content.right-pad,y);
            y+=12*scale;
        }
        l.context_restore=D2D1::RectF(l.content.left+pad,y,l.content.left+pad+label_btn_w(l10n::Get(l10n::StringId::RestoreDefaults)),y+32*scale);
        y+=56*scale;
    } else if (vm.settings_page == 3) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        {
            const float lines = static_cast<float>(AboutRowLines(vm, card_right - card_left, scale));
            const float actions_y = y + 66.0f * scale + lines * kAboutRowDip * scale + 10.0f * scale;
            static constexpr pulse::l10n::StringId kAboutLabels[] = {
                pulse::l10n::StringId::AboutCopyInfo,
                pulse::l10n::StringId::AboutHomepage,
            };
            float ax = card_left + 16.0f * scale;
            for (int i = 0; i < 2; ++i) {
                const float w = label_btn_w(pulse::l10n::Get(kAboutLabels[i]));
                l.about_action[i] = D2D1::RectF(ax, actions_y, ax + w, actions_y + 32.0f * scale);
                ax += w + 8.0f * scale;
            }
            l.about_card = D2D1::RectF(card_left, y, card_right, actions_y + 48.0f * scale);
            y = l.about_card.bottom + 12.0f * scale;
        }
        {
            // Recommended apps: title + subtitle, then one full-width link row per app.
            float ry = y + 64.0f * scale;
            for (auto& row : l.apps_row) {
                row = D2D1::RectF(card_left + 8.0f * scale, ry, card_right - 8.0f * scale, ry + 52.0f * scale);
                ry += 52.0f * scale;
            }
            l.apps_card = D2D1::RectF(card_left, y, card_right, ry + 8.0f * scale);
            y = l.apps_card.bottom + 12.0f * scale;
        }

        const bool compact_diagnostics = card_right - card_left < 650.0f * scale;
        const float diagnostics_h = (compact_diagnostics ? 288.0f : 208.0f) * scale;
        l.diagnostics_card = D2D1::RectF(card_left, y, card_right, y + diagnostics_h);
        l.diagnostics_perf = D2D1::RectF(card_left + 8.0f * scale, y + 86.0f * scale,
                                         card_right - 8.0f * scale, y + 142.0f * scale);
        const float gap = 8.0f * scale;
        const float action_left = card_left + 16.0f * scale;
        const float action_right = card_right - 16.0f * scale;
        static constexpr pulse::l10n::StringId kDiagLabels[] = {
            pulse::l10n::StringId::OpenDiagnostics,
            pulse::l10n::StringId::ClearDiagnostics,
            pulse::l10n::StringId::ExportDiagnostics,
        };
        float diag_w[3]{};
        for (int i = 0; i < 3; ++i)
            diag_w[i] = label_btn_w(pulse::l10n::Get(kDiagLabels[i]));
        if (compact_diagnostics) {
            for (int i = 0; i < 3; ++i) {
                const float top = y + (152.0f + i * 40.0f) * scale;
                l.diagnostics_action[i] = D2D1::RectF(action_left, top, action_right,
                                                      top + 32.0f * scale);
            }
        } else {
            const float available = action_right - action_left;
            const float equal = (available - gap * 2.0f) / 3.0f;
            const float measured_total = diag_w[0] + diag_w[1] + diag_w[2] + gap * 2.0f;
            float left = action_left;
            for (int i = 0; i < 3; ++i) {
                const float width = measured_total > available ? equal : diag_w[i];
                const float top = y + 160.0f * scale;
                l.diagnostics_action[i] = D2D1::RectF(left, top, left + width,
                                                      top + 32.0f * scale);
                left += width + gap;
            }
        }
        y += diagnostics_h + 12.0f * scale;

        const float available_width = std::max(0.0f, card_right - card_left - 32.0f * scale);
        const float check_w = std::min(available_width, label_btn_w(
            pulse::l10n::Get(pulse::l10n::StringId::CheckForUpdates)));
        const float download_w = std::min(available_width, label_btn_w(
            pulse::l10n::Get(pulse::l10n::StringId::DownloadUpdate)));
        const bool stack_updates = vm.settings_update_available && check_w + gap + download_w > available_width;
        // The automatic-check switch sits between the status text and the buttons,
        // like the performance switch in the diagnostics card.
        const float auto_h = vm.settings_update_enabled ? 56.0f * scale : 0.0f;
        const float update_h = 174.0f * scale + auto_h +
            (stack_updates ? 40.0f * scale : 0.0f);
        l.update_card = D2D1::RectF(card_left, y, card_right, y + update_h);
        if (vm.settings_update_enabled)
            l.update_auto_row = D2D1::RectF(card_left + 8.0f * scale, y + 106.0f * scale,
                                            card_right - 8.0f * scale, y + 162.0f * scale);
        const float check_y = y + update_h - (stack_updates ? 88.0f : 48.0f) * scale;
        l.update_action[0] = D2D1::RectF(card_left + 16.0f * scale,
                                         check_y,
                                         card_left + 16.0f * scale + check_w,
                                         check_y + 32.0f * scale);
        const float download_x = stack_updates ? l.update_action[0].left : l.update_action[0].right + gap;
        const float download_y = check_y + (stack_updates ? 40.0f * scale : 0.0f);
        l.update_action[1] = D2D1::RectF(download_x, download_y,
                                         download_x + download_w, download_y + 32.0f * scale);
        y += update_h + 12.0f * scale;
        if (!vm.settings_index_error.empty()) y += 44.0f * scale; // error text under the update card

        // Release notes: a header row per embedded version; the expanded one gets a body.
        const float release_top = y;
        const float all_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::ReleaseAll));
        l.release_all = D2D1::RectF(card_right - 16.0f * scale - all_w, release_top + 14.0f * scale,
                                    card_right - 16.0f * scale, release_top + 46.0f * scale);
        float ry = release_top + 70.0f * scale;
        if (vm.settings_release_notes) {
            const auto& notes = *vm.settings_release_notes;
            const float row_left = card_left + 8.0f * scale;
            const float row_right = card_right - 8.0f * scale;
            const float text_w = ReleaseTextWidth(row_right - row_left, scale);
            for (size_t i = 0; i < notes.size(); ++i) {
                l.release_rows.push_back(D2D1::RectF(row_left, ry, row_right, ry + 36.0f * scale));
                ry += 36.0f * scale;
                if (static_cast<int>(i) != vm.settings_release_expanded) continue;
                float body = 4.0f * scale;
                for (const auto& line : notes[i].lines)
                    body += ReleaseLineHeight(painter, line, text_w, scale) + kReleaseLineGapDip * scale;
                l.release_body = D2D1::RectF(row_left, ry, row_right, ry + body);
                ry += body + 4.0f * scale;
            }
        }
        l.release_card = D2D1::RectF(card_left, release_top, card_right, ry + 10.0f * scale);
        y = l.release_card.bottom + 24.0f * scale;
    } else if (vm.settings_page == 5) {
        y = LayoutSettingsPacks(l, vm, scale, y, painter);
    } else if (vm.settings_page == 4) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        const float gap = 8.0f * scale;
        const float inner = 16.0f * scale;
        const float btn_h = 32.0f * scale;
        y += 40.0f * scale;
        const float options_top=y-12*scale;
        const float scope_w = (card_right - card_left - inner * 2 - gap * 2) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float left = card_left + inner + i * (scope_w + gap);
            l.dup_scope[i] = D2D1::RectF(left, y, left + scope_w, y + 36.0f * scale);
        }
        y += 48.0f * scale;
        if (vm.dup_scope == 0) {
            const float browse_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::DupBrowse));
            l.dup_browse = D2D1::RectF(card_right - inner - browse_w, y,
                                       card_right - inner, y + btn_h);
            y += 44.0f * scale;
        } else if (vm.dup_scope == 1) {
            float cx = card_left + inner;
            float cy = y;
            l.dup_drives.reserve(vm.dup_drives.size());
            for (const auto& drive : vm.dup_drives) {
                const float chip_w = (std::max)(48.0f * scale, label_btn_w(drive.label));
                if (cx > card_left + inner && cx + chip_w > card_right - inner) {
                    cx = card_left + inner;
                    cy += 36.0f * scale;
                }
                l.dup_drives.push_back(D2D1::RectF(cx, cy, cx + chip_w, cy + 32.0f * scale));
                cx += chip_w + gap;
            }
            y = cy + 40.0f * scale;
        }
        y += 8.0f * scale;
        y += 40.0f * scale;
        const float min_w = (card_right - card_left - inner * 2 - gap * 2) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const float left = card_left + inner + i * (min_w + gap);
            l.dup_min_size[i] = D2D1::RectF(left, y, left + min_w, y + 32.0f * scale);
        }
        y += 44.0f * scale;
        const float scan_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::DupScan));
        const float cancel_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::Cancel));
        l.dup_scan = D2D1::RectF(card_left + inner, y, card_left + inner + scan_w, y + btn_h);
        l.dup_cancel = D2D1::RectF(l.dup_scan.right + gap, y,
                                   l.dup_scan.right + gap + cancel_w, y + btn_h);
        y += 48.0f * scale;
        l.duplicate_options=D2D1::RectF(card_left,options_top,card_right,y);
        y += 36.0f * scale;
        if (vm.dup_show_progress) {
            l.dup_progress = D2D1::RectF(card_left, y, card_right, y + 72.0f * scale);
            y += 84.0f * scale;
        }
        if (!vm.dup_empty.empty()) y += 36.0f * scale;
        const float file_h = 32.0f * scale;
        const float delete_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::DupDeleteExtras));
        const float vis_pad = 64.0f * scale;
        l.dup_group_cards.reserve(vm.dup_groups.size());
        l.dup_group_delete.reserve(vm.dup_groups.size());
        for (size_t g = 0; g < vm.dup_groups.size(); ++g) {
            const float card_h = 48.0f * scale +
                static_cast<float>(vm.dup_groups[g].files.size()) * file_h + 48.0f * scale;
            const D2D1_RECT_F card = D2D1::RectF(card_left, y, card_right, y + card_h);
            l.dup_group_cards.push_back(card);
            l.dup_group_delete.push_back(D2D1::RectF(
                card.right - inner - delete_w,
                card.top + 48.0f * scale +
                    static_cast<float>(vm.dup_groups[g].files.size()) * file_h + 8.0f * scale,
                card.right - inner,
                card.top + 48.0f * scale +
                    static_cast<float>(vm.dup_groups[g].files.size()) * file_h + 8.0f * scale +
                    btn_h));
            if (VisibleInContent(card, l.content, vis_pad)) {
                float fy = y + 48.0f * scale;
                l.dup_keep.reserve(l.dup_keep.size() + vm.dup_groups[g].files.size());
                l.dup_open.reserve(l.dup_open.size() + vm.dup_groups[g].files.size());
                for (size_t f = 0; f < vm.dup_groups[g].files.size(); ++f) {
                    l.dup_keep.push_back(D2D1::RectF(card.left + inner, fy,
                                                     card.left + inner + 88.0f * scale, fy + file_h));
                    l.dup_keep_group.push_back(static_cast<int>(g));
                    l.dup_keep_file.push_back(static_cast<int>(f));
                    l.dup_open.push_back(D2D1::RectF(card.left + inner + 92.0f * scale, fy,
                                                     card.right - inner, fy + file_h));
                    l.dup_open_group.push_back(static_cast<int>(g));
                    l.dup_open_file.push_back(static_cast<int>(f));
                    fy += file_h;
                }
            }
            y += card_h + 12.0f * scale;
        }
        if (vm.dup_show_delete_all) {
            const float all_w = label_btn_w(vm.dup_delete_all.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::DupDeleteAllExtras)
                : vm.dup_delete_all);
            l.dup_delete_all = D2D1::RectF(card_left, y, card_left + all_w, y + btn_h);
            y += 48.0f * scale;
        }
    }
    l.content_h = y - l.content_origin + pad;
    return l;
}

bool ContainsPt(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

HitTestResult::Region StatusBarHitRegion(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                         float x, float y, float scale, float status_height,
                                         Compositor* compositor) {
    IDWriteFactory2* factory = compositor ? compositor->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor ? compositor->SmallFormat() : nullptr;
    const StatusBarMetrics sb = MakeStatusBarMetrics(
        vm, rect, scale, status_height, factory, fmt);
    if (vm.status.query_cancellable && ContainsPt(sb.cancel_search, x, y)) return HitTestResult::StatusBarCancelSearch;
    if (sb.hint_action.right > sb.hint_action.left && ContainsPt(sb.hint_action, x, y))
        return HitTestResult::StatusHintAction;
    return !vm.status.query_active && !vm.status.task_is_update && ContainsPt(sb.task, x, y) ? HitTestResult::StatusBarTask
                                    : HitTestResult::StatusBar;
}

bool IsHovered(const WindowViewModel& vm, HitTestResult::Region region, int index = -1,
               int sub_index = -1) {
    return vm.hover_region == static_cast<int>(region) &&
        (index < 0 || vm.hover_control_index == index) &&
        (sub_index < 0 || vm.hover_sub_index == sub_index);
}

bool TabCloseVisible(const WindowViewModel& vm, int index, float tab_w, float scale) {
    if (index >= 0 && index < static_cast<int>(vm.tabs.size()) &&
        vm.tabs[static_cast<size_t>(index)].pinned)
        return false; // Chrome: pinned tabs have no close affordance
    if (tab_w >= kTabCloseAlwaysW * scale) return true;
    if (index >= 0 && index < static_cast<int>(vm.tabs.size()) && vm.tabs[static_cast<size_t>(index)].active)
        return true;
    return IsHovered(vm, HitTestResult::Tab, index) ||
           IsHovered(vm, HitTestResult::TabClose, index);
}

std::wstring FitFileName(Compositor* compositor, IDWriteFactory2* factory,
                                IDWriteTextFormat* fmt, const std::wstring& name, float max_w) {
    if (name.empty() || max_w <= 1.0f) return {};
    const auto measure = [&](const std::wstring& s) {
        return MeasureLayoutText(compositor, factory, fmt, s);
    };
    if (measure(name) <= max_w) return name;

    size_t dot = name.find_last_of(L'.');
    std::wstring ext;
    std::wstring stem = name;
    if (dot != std::wstring::npos && dot > 0 && dot + 1 < name.size()) {
        const size_t ext_len = name.size() - dot;
        if (ext_len >= 2 && ext_len <= 9) {
            bool ok = true;
            for (size_t i = dot + 1; i < name.size(); ++i) {
                if (!std::iswalnum(name[i])) { ok = false; break; }
            }
            if (ok) {
                ext = name.substr(dot);
                stem = name.substr(0, dot);
            }
        }
    }

    const std::wstring tail = std::wstring(1, L'\u2026') + ext;
    const float tail_w = measure(tail);
    if (tail_w >= max_w) {
        const std::wstring ellip(1, L'\u2026');
        size_t lo = 0, hi = name.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (measure(name.substr(0, mid) + ellip) <= max_w) lo = mid;
            else hi = mid - 1;
        }
        return lo == 0 ? ellip : name.substr(0, lo) + ellip;
    }

    size_t lo = 0, hi = stem.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(stem.substr(0, mid) + tail) <= max_w) lo = mid;
        else hi = mid - 1;
    }
    return stem.substr(0, lo) + tail;
}

float OverlapTagsWidth(int n, float diameter) {
    if (n <= 0) return 0.0f;
    return diameter + static_cast<float>(n - 1) * diameter * 0.52f;
}

float SpreadTagsWidth(int n, float diameter, float gap) {
    if (n <= 0) return 0.0f;
    return static_cast<float>(n) * diameter + static_cast<float>(n - 1) * gap;
}

float TagStepForLeftover(int n, float diameter, float spread_gap, float leftover) {
    if (n <= 1) return 0.0f;
    if (SpreadTagsWidth(n, diameter, spread_gap) <= leftover + 0.5f)
        return diameter + spread_gap;
    return diameter * 0.52f;
}

constexpr float kDetailsSnippetMinRowDip = 36.0f;

bool DetailsShowsSnippet(const PaneViewModel& vm) {
    if(vm.content_results && vm.view_mode == ViewMode::Details) return true;
    if (vm.view_mode != ViewMode::Details || !vm.search_snippets) return false;
    for (const auto& snippet : *vm.search_snippets) {
        if (!snippet.empty()) return true;
    }
    return false;
}

struct DetailsNameLine {
    float y = 0.0f;
    float h = 0.0f;
    float snippet_y = 0.0f;
    float snippet_h = 0.0f;
};

DetailsNameLine MakeDetailsNameLine(const D2D1_RECT_F& name_rc, const D2D1_RECT_F& cell,
                                    float scale, bool snippet) {
    DetailsNameLine line;
    line.y = name_rc.top;
    line.h = std::max(1.0f, name_rc.bottom - name_rc.top);
    if (!snippet) return line;
    const float row = std::max(1.0f, cell.bottom - cell.top);
    const float name_h = 16.0f * scale;
    const float snip_h = 14.0f * scale;
    const float top_pad = 1.0f * scale;
    const float bot_pad = 1.0f * scale;
    line.y = cell.top + top_pad;
    if (name_h + snip_h + top_pad + bot_pad <= row + 0.5f) {
        line.h = name_h;
        line.snippet_y = line.y + line.h;
        line.snippet_h = snip_h;
    } else {
        line.h = std::max(12.0f * scale, (row - top_pad - bot_pad) * (16.0f / 30.0f));
        line.snippet_y = line.y + line.h;
        line.snippet_h = std::max(10.0f * scale, cell.bottom - bot_pad - line.snippet_y);
    }
    return line;
}

// Name column: filename compresses first. Tags sit after the name — spread
// when leftover room fits every dot, otherwise overlap. Star / new tab / more
// dock to the column's right edge so trailing chrome stays a fixed width.
struct NameTrail {
    float name_x = 0.0f;
    float name_w = 0.0f;
    float line_w = 0.0f;
    int tag_n = 0;
    float tag_r = 0.0f;
    float tag_step = 0.0f;
    float tag_x0 = 0.0f;
    float tag_cy = 0.0f;
    D2D1_RECT_F badge{};
    D2D1_RECT_F star{};
    D2D1_RECT_F new_tab{};
    D2D1_RECT_F more{};
    bool show_star = false;
    bool show_new_tab = false;
    bool show_more = false;
};

float HighlightPaddingWidth(const std::wstring& name, const std::wstring& shown,
                            const std::vector<NameMatchRange>& matches, float scale) {
    return static_cast<float>(VisibleNameMatchRanges(name, shown, matches).size()) *
        2.0f * kNameHighlightPaddingDip * scale;
}

std::wstring FitHighlightedFileName(Compositor* compositor, IDWriteFactory2* factory,
                                   IDWriteTextFormat* fmt, const std::wstring& name, float budget,
                                   const std::vector<NameMatchRange>& matches, float scale) {
    float reserved = 0;
    for (;;) {
        const auto shown = FitFileName(compositor, factory, fmt, name, std::max(0.0f, budget - reserved));
        const float required = HighlightPaddingWidth(name, shown, matches, scale);
        if (required <= reserved) return shown;
        // Reserve only visible matches. A newly split match across ellipsis may
        // require one more iteration; the reservation grows monotonically.
        reserved = required;
    }
}

NameTrail LayoutNameTrail(float name_x, float text_y, float text_h,
                                 float col_right, float cell_top, float cell_bottom,
                                 float scale, const std::wstring& name, int tag_n,
                                 float badge_w,
                                 bool show_star, bool show_new_tab, bool show_more,
                                 Compositor* compositor, IDWriteFactory2* factory,
                                 IDWriteTextFormat* fmt, bool change_badge = false, int action_slots = 0,
                                 const std::vector<NameMatchRange>& matches = {},
                                 unsigned allowed_actions = 7u) {
    // allowed_actions: hover buttons the user keeps (bit 0 star, bit 1 new
    // tab, bit 2 more). A turned-off button is neither drawn nor reserved.
    show_star = show_star && (allowed_actions & 1u);
    show_new_tab = show_new_tab && (allowed_actions & 2u);
    show_more = show_more && (allowed_actions & 4u);
    NameTrail t;
    t.name_x = name_x;
    t.show_star = show_star;
    t.show_new_tab = show_new_tab;
    t.show_more = show_more;
    t.tag_n = std::clamp(tag_n, 0, 3);
    t.tag_r = 4.0f * scale;
    t.tag_cy = text_y + text_h * 0.5f;

    const float gap = 4.0f * scale;
    const float btn = 20.0f * scale;
    const float pad = 4.0f * scale;
    const float by = cell_top + (cell_bottom - cell_top - btn) * 0.5f;
    const float diameter = t.tag_r * 2.0f;

    // Star / new tab / more dock to the right edge of the name column so
    // every row lines up, independent of filename length.
    float dock = col_right - pad;
    bool reserve_actions = action_slots > 0 || (change_badge && badge_w > 0.0f);
    // Choose the action density from content, never hover state. Keep star and
    // more available; opening in a new tab is also in the more menu.
    const float full_name_width = MeasureLayoutText(compositor, factory, fmt, name) +
        HighlightPaddingWidth(name, name, matches, scale);
    const float content_width = full_name_width +
        badge_w + gap + (t.tag_n ? gap + OverlapTagsWidth(t.tag_n, diameter) : 0.0f);
    const bool compact_actions = action_slots == 2 || (reserve_actions &&
        content_width + 3 * (btn + gap) > dock - name_x);
    if (compact_actions) show_new_tab = false;
    const float readable_name = std::min(full_name_width, 96.0f * scale);
    const float minimum_tags = t.tag_n ? gap + OverlapTagsWidth(t.tag_n, diameter) : 0.0f;
    if (dock - name_x < readable_name + minimum_tags + 2 * (btn + gap)) {
        reserve_actions = false;
        show_star = show_new_tab = show_more = false;
    }
    t.show_star = show_star;
    t.show_more = show_more;
    t.show_new_tab = show_new_tab;
    if ((allowed_actions & 4u) && (show_more || reserve_actions)) {
        t.more = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.more.left - gap;
    }
    if ((allowed_actions & 1u) && (show_star || reserve_actions)) {
        t.star = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.star.left - gap;
    }
    if ((allowed_actions & 2u) && (show_new_tab || (reserve_actions && !compact_actions))) {
        t.new_tab = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.new_tab.left - gap;
    }
    t.line_w = std::max(0.0f, dock - name_x);

    while (t.tag_n > 0 && t.line_w < std::min(full_name_width, 24.0f * scale) +
        gap + OverlapTagsWidth(t.tag_n, diameter)) --t.tag_n;

    // Reserve the compact (overlapped) cluster so a long name still
    // compresses first. Spread only if leftover after the fitted name fits.
    badge_w = std::max(0.0f, badge_w);
    if (change_badge && dock - name_x < badge_w + 48.0f * scale +
        (t.tag_n ? gap + OverlapTagsWidth(t.tag_n, diameter) : 0.0f)) badge_w = 0.0f;
    const float badge_gap = badge_w > 0.0f ? gap : 0.0f;
    const float tags_w = OverlapTagsWidth(t.tag_n, diameter);
    const float tags_gap = t.tag_n > 0 ? gap : 0.0f;
    const float budget = std::max(0.0f,
        dock - name_x - badge_gap - badge_w - tags_gap - tags_w);
    const std::wstring fitted = FitHighlightedFileName(compositor, factory, fmt, name, budget, matches, scale);
    t.name_w = std::min(budget, MeasureLayoutText(compositor, factory, fmt, fitted) +
        HighlightPaddingWidth(name, fitted, matches, scale));
    float trail_x = name_x + t.name_w;
    if (badge_w > 0.0f && !change_badge) {
        trail_x += badge_gap;
        const float badge_h = std::min(20.0f * scale, text_h);
        const float badge_y = text_y + (text_h - badge_h) * 0.5f;
        t.badge = D2D1::RectF(trail_x, badge_y, trail_x + badge_w, badge_y + badge_h);
        trail_x += badge_w;
    }
    if (t.tag_n > 0) {
        t.tag_x0 = trail_x + gap;
        const float leftover = std::max(0.0f, dock - t.tag_x0 - (change_badge ? badge_w + badge_gap : 0.0f));
        t.tag_step = TagStepForLeftover(t.tag_n, diameter, gap, leftover);
    }
    if (change_badge && badge_w > 0.0f) {
        const float bx = t.tag_n > 0 ? t.tag_x0 + diameter + (t.tag_n - 1) * t.tag_step + gap : trail_x + gap;
        t.badge = D2D1::RectF(bx, text_y, std::min(dock, bx + badge_w), text_y + text_h);
    }
    if (!show_star) t.star = {};
    if (!show_new_tab) t.new_tab = {};
    if (!show_more) t.more = {};
    return t;
}

bool HasChangeBadge(const ChangeBadge& badge) {
    return !badge.label.empty() && badge.count > 0 && badge.status < 2;
}

float ChangeBadgeWidth(const ChangeBadge& badge, float scale, Compositor* compositor) {
    if (!HasChangeBadge(badge)) return 0.0f;
    return MeasureLayoutText(compositor, compositor->DwriteFactory(),
        compositor->SmallFormat(), badge.label) + (badge.has_deleted ? 48.0f : 34.0f) * scale;
}
D2D1_RECT_F ChangeTitleRect(const D2D1_RECT_F& bounds, float text_right, float height,
                          const ChangeBadge& badge, float scale, Compositor* compositor, const std::wstring& title,
                          float title_inset = 0.0f) {
    const float width = ChangeBadgeWidth(badge, scale, compositor);
    if (width <= 0 || text_right - bounds.left < width + 64.0f * scale) return {};
    const float title_width = MeasureLayoutText(compositor, compositor->DwriteFactory(), compositor->HeaderFormat(), title);
    const float left = std::min(text_right - width, bounds.left + 14.0f * scale + title_inset + title_width);
    return D2D1::RectF(left, bounds.top, left + width, bounds.top + height);
}
void DrawChangeBadge(Compositor* compositor, fluent::Painter&, const ChangeBadge& badge,
                     D2D1_RECT_F rc, const Theme& theme, float scale) {
    if (!HasChangeBadge(badge) || rc.right <= rc.left) return;
    auto* dc = compositor->Dc();
    ComPtr<ID2D1SolidColorBrush> brush;
    const bool dark = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
    const auto recent_color = HexColor(dark ? 0x65D9E8 : 0x087F99);
    const auto older_color = HexColor(dark ? 0x85ADB4 : 0x4A7580);
    const auto color = badge.status == 2 ? theme.text_secondary :
        badge.status == 1 ? older_color : recent_color;
    if (FAILED(dc->CreateSolidColorBrush(theme.stroke_card, &brush))) return;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    dc->DrawLine({rc.left + 2 * scale, cy - 6 * scale}, {rc.left + 2 * scale, cy + 6 * scale}, brush.get(), scale);
    brush->SetColor(color);
    const float cx = rc.left + 14 * scale;
    dc->DrawEllipse(D2D1::Ellipse({cx, cy}, 4.5f * scale, 4.5f * scale), brush.get(), scale);
    dc->DrawLine({cx, cy - 3 * scale}, {cx, cy}, brush.get(), scale);
    dc->DrawLine({cx, cy}, {cx + 2 * scale, cy + scale}, brush.get(), scale);
    auto text = rc; text.left += 23 * scale;
    if (badge.has_deleted) text.right -= 14 * scale;
    brush->SetColor(color);
    DrawTextEndEllipsis(dc, compositor->DwriteFactory(), compositor->SmallFormat(), brush.get(),
        badge.label, text.left, text.top, std::max(0.0f, text.right - text.left), text.bottom - text.top);
    if (badge.has_deleted) {
        brush->SetColor(badge.status == 2 ? theme.text_secondary : D2D1::ColorF(0xC58A38));
        dc->DrawLine({rc.right - 10 * scale, cy}, {rc.right - 4 * scale, cy}, brush.get(), 2 * scale);
    }
}
int ChangePopoverLineCount(const ChangePopover& popup) {
    return std::clamp(1 + static_cast<int>(std::count(popup.summary.begin(), popup.summary.end(), L'\n')), 1, 8);
}
D2D1_RECT_F ChangePopoverRect(const ChangePopover& popup, const D2D1_RECT_F& bounds, float scale) {
    const float w = std::min(320.0f * scale, bounds.right - bounds.left - 16 * scale);
    const float h = (54.0f + 24.0f * ChangePopoverLineCount(popup)) * scale;
    const float x = std::clamp(popup.x, bounds.left + 8 * scale, std::max(bounds.left + 8 * scale, bounds.right - w - 8 * scale));
    const float y = std::clamp(popup.y + 10 * scale, bounds.top + 8 * scale, std::max(bounds.top + 8 * scale, bounds.bottom - h - 8 * scale));
    return D2D1::RectF(x, y, x + w, y + h);
}
struct ScrollbarMetrics {
    float thumbY, thumbH;
    bool valid;
};

ScrollbarMetrics ComputeScrollbar(float viewH, float totalH, float scrollY, float rowH) {
    ScrollbarMetrics m{};
    if (totalH <= viewH || viewH <= 0) return m;
    m.valid = true;
    m.thumbH = std::min(viewH, std::max(rowH, viewH * (viewH / totalH)));
    m.thumbY = std::clamp(scrollY / (totalH - viewH), 0.0f, 1.0f) * (viewH - m.thumbH);
    return m;
}
} // namespace

} // namespace pulse::ui
