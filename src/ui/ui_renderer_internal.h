// ui_renderer_internal.h — Draw + HitTest shared geometry (not a public API).
#pragma once
#include "ui_renderer.h"
#include "layer_alphas.h"
#include "empty_state_layout.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"
#include "typography.h"
#include "name_highlight.h"
#include "settings_page_ids.h"
#include "../app/places.h"
#include "../app/device_registry.h"
#include "../app/folder_alias.h"
#include "../app/builtin_menu_items.h"
#include "../app/search_query.h"
#include "../common/text_format.h"
#include "../common/display_path.h"
#include "preview_format_catalog.h"
#include "link_type_text.h"
#include "recent_day_label.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include "known_extensions.h"

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

    // The unfocused frame of a pane card (and the docked AI panel's card):
    // a thin light card over a wallpaper takes a soft white edge instead of
    // the gray one, which reads as a dirty line on the image.
    inline D2D1_COLOR_F CardFrameColor(const Theme& theme, bool dark, float card_alpha) noexcept {
        if (!dark && card_alpha < 0.9f) return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f);
        return theme.stroke_card;
    }
    inline float WallpaperBlurDip(int wallpaper_blur) noexcept {
        return static_cast<float>(std::clamp(wallpaper_blur, 0, 40));
    }
    constexpr int kPanelTransparencyMax = 90;
    constexpr int kWallpaperBlurMax = 40;
    constexpr int kFrameTransparencyMax = 100;
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
    constexpr float kTabMinW = 96.0f;  // crowded tabs stop here and the strip scrolls
    constexpr float kTabMaxW = 240.0f;
    constexpr float kTabPinnedW = 36.0f; // Chrome pinned tab: icon-only square
    constexpr float kTabPinnedNamedW = 112.0f;
    constexpr float kTabCloseAlwaysW = 140.0f; // narrower inactive tabs show close on hover
    constexpr float kTabActiveMinW = 120.0f;  // crowded active tab: minimum + room for close
    constexpr float kTabScrollArrowW = 24.0f;
    constexpr float kTabClosePadDip = 10.0f;
    constexpr float kTabCloseSizeDip = 16.0f;
    constexpr float kCommandIconButtonDip = 32.0f;
    constexpr float kCommandIconStepDip = 34.0f;
    constexpr float kRecentControlsDip = 40.0f;
    constexpr float kSearchFiltersDip = 40.0f;
    constexpr float kTagQueryRowDip = 40.0f;      // tag view condition bar
    constexpr float kTagQueryRefineDip = 32.0f;   // its 细分 row, when there is one
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
    constexpr const wchar_t* kIconChevronLeft = L"\xE76B";
    constexpr const wchar_t* kIconChevronUp = L"\xE70E";
    constexpr const wchar_t* kIconChevronDown = L"\xE70D";
    constexpr const wchar_t* kIconCloseSmall = L"\xE711";
    constexpr const wchar_t* kIconPinFilled = L"\xE841";
    // The 别名 mark (14 DIP) and its gaps (pulse_alias.html .amark).
    constexpr float kAliasMarkDip = 14.0f;
    constexpr float kCrumbAliasMarkDip = 19.0f;   // breadcrumb: mark + 5
    constexpr float kListAliasMarkDip = 22.0f;    // list row: mark + 8

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

    // Protected operating-system items are hidden *and* system (Explorer's
    // "protected operating system files"). A folder with only the system bit is
    // just a customized folder and stays normal.
    inline bool IsProtectedSystemEntry(DWORD attrs) noexcept {
        constexpr DWORD kMask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
        return (attrs & kMask) == kMask;
    }

    // Hidden entries render faded (icon and name) so they read as background
    // content; protected system items fade further and carry a shield badge.
    inline float HiddenEntryAlpha(DWORD attrs) noexcept {
        if (IsProtectedSystemEntry(attrs)) return 0.42f;
        return (attrs & FILE_ATTRIBUTE_HIDDEN) ? 0.50f : 1.0f;
    }

    // Name color that matches HiddenEntryAlpha: secondary text for hidden items,
    // a step lighter for protected system items.
    inline D2D1_COLOR_F HiddenEntryNameColor(DWORD attrs, const Theme& theme,
                                             D2D1_COLOR_F normal) noexcept {
        if (IsProtectedSystemEntry(attrs)) {
            D2D1_COLOR_F c = theme.text_secondary;
            c.a *= 0.78f;
            return c;
        }
        if (attrs & FILE_ATTRIBUTE_HIDDEN) return theme.text_secondary;
        return normal;
    }

    // Opaque shield badge on the bottom-right of a protected item's artwork.
    inline void DrawProtectedBadge(ID2D1DeviceContext* dc, const D2D1_RECT_F& art,
                                   float scale, const Theme& theme) {
        if (!dc) return;
        const float art_size = (std::min)(art.right - art.left, art.bottom - art.top);
        if (art_size <= 0.0f) return;
        const float d = std::clamp(art_size * 0.42f, 11.0f * scale, 18.0f * scale);
        const float cx = art.right - d * 0.25f;
        const float cy = art.bottom - d * 0.25f;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        D2D1_COLOR_F plate = theme.surface_flyout;
        plate.a = 1.0f;
        if (FAILED(dc->CreateSolidColorBrush(plate, &brush))) return;
        const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), d * 0.5f, d * 0.5f);
        dc->FillEllipse(circle, brush.Get());
        D2D1_COLOR_F ring = theme.text;
        ring.a = 0.16f;
        brush->SetColor(ring);
        dc->DrawEllipse(circle, brush.Get(), (std::max)(1.0f, scale));
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> shield;
        if (!factory || FAILED(factory->CreatePathGeometry(&shield))) return;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(shield->Open(&sink))) return;
        // Shield outline in a 12-unit box, scaled into the badge.
        const float u = d * 0.62f / 12.0f;
        const float ox = cx - 6.0f * u;
        const float oy = cy - 6.0f * u;
        const auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * u, oy + y * u); };
        sink->BeginFigure(P(6.0f, 0.8f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(P(10.4f, 2.4f));
        sink->AddLine(P(10.4f, 5.7f));
        sink->AddBezier(D2D1::BezierSegment(P(10.4f, 8.5f), P(8.5f, 10.5f), P(6.0f, 11.2f)));
        sink->AddBezier(D2D1::BezierSegment(P(3.5f, 10.5f), P(1.6f, 8.5f), P(1.6f, 5.7f)));
        sink->AddLine(P(1.6f, 2.4f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        brush->SetColor(theme.text_secondary);
        dc->FillGeometry(shield.Get(), brush.Get());
    }

    const ListEntryView& MakeVisibleEntry(const PaneViewModel& vm, size_t index) {
        if (!vm.snapshot) return vm.entries[index];

        if (!vm.row_cache) vm.row_cache = std::make_shared<RowPresentationCache>();
        auto& cache = *vm.row_cache;
        const uint64_t alias_revision = app::AliasStore::Instance().Revision();
        const uint64_t extension_revision = KnownExtensionsRevision();
        if (cache.snapshot != vm.snapshot || cache.alias_revision != alias_revision ||
            cache.extension_revision != extension_revision) {
            cache.snapshot = vm.snapshot;
            cache.alias_revision = alias_revision;
            cache.extension_revision = extension_revision;
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
        // A folder in the Recycle Bin shows the total its $I record holds, as
        // Explorer does (an empty one reads 0 B, not a blank that looks
        // unknown); it is no longer at its path for a folder-size scan.
        entry.size_text = penetrated
            ? (source.link_target_is_dir ? L"" : pulse::format::ByteSize(source.link_target_size, true))
            : ((source.is_dir && source.recycle_path.empty()) || source.pc_launcher
                   ? L"" : pulse::format::ByteSize(source.size, source.recycle_path.empty() || !source.is_dir));
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
        if (!entry.path.empty()) entry.alias = app::AliasStore::Instance().Shown(entry.path);
        entry.thumb_path = !source.is_dir && fs::IsDevicePath(entry.path) ? source.shell_id : std::wstring();
        entry.attrs = source.attrs;
        entry.size_value = source.size;
        entry.modified_value = (static_cast<uint64_t>(source.mtime.dwHighDateTime) << 32) |
                               source.mtime.dwLowDateTime;
        entry.is_dir = penetrated ? source.link_target_is_dir : source.is_dir;
        entry.hidden_ext = HiddenExtensionLength(entry.name, entry.is_dir);
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
        if (source.drive_type != 0)
            entry.type_text = DriveTypeText(source.drive_remote_like ? static_cast<uint8_t>(DRIVE_REMOTE) : source.drive_type);
        if (source.drive_type != 0 && source.drive_locked)
            entry.type_text += L" \u00B7 " + pulse::l10n::Get(pulse::l10n::StringId::DriveBitLockerLocked);
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
        entry.drive_mounted = source.drive_type != 0 && source.drive_remote_like;
        entry.net_kind = source.net_kind;
        if (source.pc_launcher) {
            // 网盘程序入口: brand tint, and "打开百度网盘程序" as the type.
            entry.launcher_rgb = source.pc_launcher == 2 ? 0x2F8CF0u : source.pc_launcher == 3 ? 0x2F6FD6u : 0x60A5FAu;
            std::wstring text = pulse::l10n::Get(pulse::l10n::StringId::ThisPcLauncherOpensApp);
            if (const size_t at = text.find(L"{name}"); at != std::wstring::npos) {
                std::wstring name = source.name;
                // CJK text around a Latin name gets a space ("打开 WPS网盘程序").
                const auto latin = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
                if (!name.empty() && at > 0 && text[at - 1] >= 0x2E80 && latin(name.front())) name.insert(name.begin(), L' ');
                if (!name.empty() && at + 6 < text.size() && text[at + 6] >= 0x2E80 && latin(name.back())) name.push_back(L' ');
                text.replace(at, 6, name);
            }
            entry.type_text = std::move(text);
        }
        if (source.pc_device & fs::kPcDevicePlaceholder) {
            entry.device = source.pc_device;  // no phone: how to connect one
            entry.type_text = pulse::l10n::Get(pulse::l10n::StringId::DeviceNoneHint);
        } else if (source.pc_device) {
            // Phones: 手机 / iPhone · 只读, then 未解锁 while waiting for Allow / Trust.
            entry.device = source.pc_device;
            const bool iphone = (source.pc_device & fs::kPcDeviceIPhone) != 0;
            entry.type_text = iphone ? L"iPhone \u00B7 " + pulse::l10n::Get(pulse::l10n::StringId::DeviceReadOnlyChip)
                                     : pulse::l10n::Get(pulse::l10n::StringId::DeviceKindPhone);
            if (source.pc_device & fs::kPcDeviceLocked)
                entry.type_text += L" \u00B7 " + pulse::l10n::Get(pulse::l10n::StringId::DeviceLockedChip);
            // Storages from the registry's listing; This PC re-lists on every change.
            for (const auto& device : app::DeviceRegistry::Instance().Devices()) {
                if (fs::FormatDevicePath(fs::DevicePath{device.key, L"", {}}) != source.full_path) continue;
                for (const auto& storage : device.storages) {
                    ListEntryView::DeviceBar bar;
                    bar.label = storage.name;
                    if (storage.total > 0) {
                        const uint64_t free_bytes = std::min(storage.free, storage.total);
                        bar.used = static_cast<float>(static_cast<double>(storage.total - free_bytes) /
                                                      static_cast<double>(storage.total));
                        wchar_t left[96]{};
                        swprintf_s(left, pulse::l10n::Get(pulse::l10n::StringId::DeviceStorageFreeFormat).c_str(),
                                   pulse::format::ByteSize(free_bytes).c_str());
                        bar.free_text = left;
                    }
                    // FAT32 cards cannot take files over 4 GB (copy checks it, D5).
                    if (_wcsicmp(storage.file_system.c_str(), L"FAT32") == 0)
                        bar.free_text += bar.free_text.empty() ? L"FAT32" : L" \u00B7 FAT32";
                    entry.device_bars.push_back(std::move(bar));
                }
                break;
            }
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
            // Rail only: "+N" chip standing in for section rows that do not fit
            // (batch = how many rows it replaces). Drawn, never hit.
            RailMore,
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
        if (vm.tray_deck.hidden) return 0.0f;
        // Shelf redesign: the runtime springs the height between the layers
        // (down to 0 when the empty tray hides itself).
        return std::max(vm.tray_deck.hide_empty ? 0.0f : 40.0f, vm.tray_deck.height_dip) * m.scale;
    }

    // Space under the places list: the tray plus its padding. The padding
    // shrinks with a hiding tray so the list grows into the freed space.
    float TrayGap(float tray_h, const SidebarMetrics& m) {
        return m.pad * std::clamp(tray_h / (12.0f * m.scale), 0.0f, 1.0f);
    }

    // The open tray may cover the sidebar up to 20 dip from its top; the
    // other layers keep at least half of it for places.
    float TrayHeightCap(const WindowViewModel& vm, const D2D1_RECT_F& sb, float scale) {
        const auto open = static_cast<int>(TrayStage::Open);
        const bool open_like = vm.tray_deck.stage == open || vm.tray_deck.prev_stage == open;
        const float h = sb.bottom - sb.top;
        return std::max(40.0f * scale, open_like ? h - 20.0f * scale : h * 0.52f);
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
        const float tray_height = std::min(ExpandedTrayHeight(vm, m), TrayHeightCap(vm, sb, scale));
        const float bottom = std::max(sb.top, sb.bottom - tray_height - 2.0f * TrayGap(tray_height, m));
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
            const bool tray_gone = vm.tray_deck.hidden || (vm.tray_deck.hide_empty && vm.tray_deck.height_dip < 20.0f);
            const float trayH = tray_gone ? 0.0f : 44.0f * scale;
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
                    // Tags are a colored dot each, so their rail rows are shorter.
                    const float itemH = group.items[i].is_tag ? 32.0f * scale : rowH;
                    if (y + itemH > sb.bottom - trayH) {
                        if (group.items[i].is_tag) {
                            // The last fitting tag row becomes "+N" for the rest.
                            int rest = static_cast<int>(group.items.size()) - i;
                            if (!out.empty() && out.back().kind == SidebarSlot::Tag &&
                                out.back().group == g) {
                                out.back().kind = SidebarSlot::RailMore;
                                out.back().batch = rest + 1;
                            } else if (y + 26.0f * scale <= sb.bottom - trayH) {
                                SidebarSlot more;
                                more.kind = SidebarSlot::RailMore;
                                more.rc = D2D1::RectF(4.0f * scale, y, width - 4.0f * scale,
                                                      y + 26.0f * scale);
                                more.group = g;
                                more.batch = rest;
                                out.push_back(more);
                            }
                        }
                        break;
                    }
                    SidebarSlot slot;
                    slot.kind = group.items[i].is_drive ? SidebarSlot::Drive
                              : group.items[i].is_tag ? SidebarSlot::Tag : SidebarSlot::Item;
                    slot.rc = D2D1::RectF(4.0f * scale, y, width - 4.0f * scale, y + itemH);
                    slot.group = g;
                    slot.item = i;
                    slot.run = run++;
                    out.push_back(slot);
                    y += itemH;
                }
            }
            SidebarSlot tray;
            tray.kind = SidebarSlot::TrayPanel;
            tray.rc = D2D1::RectF(4.0f * scale, sb.bottom - trayH,
                                  width - 4.0f * scale, sb.bottom - 4.0f * scale);
            if (!tray_gone) out.push_back(tray);
            return;
        }

        float trayH = ExpandedTrayHeight(vm, m);
        trayH = std::min(trayH, TrayHeightCap(vm, sb, scale));
        const float trayGap = TrayGap(trayH, m);
        const float trayTop = sb.bottom - trayGap - trayH;
        const float contentBottom = trayTop - trayGap;
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
                const float h = SidebarItemHeight(item, m);
                if (y >= contentBottom) break;
                SidebarSlot slot;
                slot.kind = item.is_drive ? SidebarSlot::Drive
                          : item.is_tag ? SidebarSlot::Tag : SidebarSlot::Item;
                slot.rc = D2D1::RectF(innerL + item.indent * 16.0f * scale, y, innerR, y + h);
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

        if (vm.tray_deck.hidden || trayH < 1.0f * scale) return;
        SidebarSlot tray;
        tray.kind = SidebarSlot::TrayPanel;
        tray.rc = D2D1::RectF(innerL, trayTop, innerR, sb.bottom - trayGap);
        out.push_back(tray);

    }

    // Staging tray shelf (pulse_tray_redesign.html): every interactive rect
    // of the three layers, shared by drawing and hit-testing. Layers anchor
    // to the panel top, so a growing panel reveals them from the top down.
    struct TrayShelfGeom {
        float s = 1.0f;
        D2D1_RECT_F panel{};
        // Strip (empty).
        D2D1_RECT_F strip_icon{}, strip_title{}, strip_hint{}, strip_kbd{};
        bool strip_hint_short = false;   // no room for the chip: 拖入文件 without 或
        // Drop halves: 0 copy, 1 move.
        D2D1_RECT_F half[2]{};
        // Shelf rows.
        D2D1_RECT_F fan{}, meta{}, expand{}, more{};
        D2D1_RECT_F split{}, go{}, dd{}, zip{}, rename{};
        D2D1_RECT_F send_label{};
        std::vector<D2D1_RECT_F> chips;  // 送到 chips that fit
        std::vector<int> chip_dest;      // index into deck.dests per chip
        D2D1_RECT_F pick{};
        D2D1_RECT_F strip_clip{};        // 剪贴板 N 项 · 收入 button (strip)
        D2D1_RECT_F clip_row{};          // 剪贴板里有 N 项 row (shelf)
        bool has_expand = false;
    };

    // measure(text, small): width of text in the small (hint, chip) format
    // or the bold title format.
    template <class Measure>
    TrayShelfGeom TrayShelfGeometry(const TrayDeckView& deck, const D2D1_RECT_F& panel,
                                    float s, Measure&& measure) {
        TrayShelfGeom g;
        g.s = s;
        g.panel = panel;
        const float L = panel.left + 10.0f * s;
        const float R = panel.right - 10.0f * s;
        const float top = panel.top;
        const float inner = (R - L) / s;
        const bool narrow = inner < 230.0f;

        // Strip: icon, 暂存区, hint (+ kbd chip when it fits).
        {
            const float cy = top + 20.0f * s;
            const float x0 = panel.left + 13.0f * s;
            g.strip_icon = D2D1::RectF(x0, cy - 8.0f * s, x0 + 16.0f * s, cy + 8.0f * s);
            const float title_w = measure(pulse::l10n::Get(pulse::l10n::StringId::TrayShelfTitle), false);
            g.strip_title = D2D1::RectF(g.strip_icon.right + 9.0f * s, cy - 10.0f * s,
                                        g.strip_icon.right + 9.0f * s + title_w, cy + 10.0f * s);
            float hint_r = R;
            if (deck.clip_count > 0) {
                // padding 10 + icon 13 + gap 5 + label (11.5 bold) + padding 10.
                std::wstring label = pulse::l10n::Get(pulse::l10n::StringId::TrayClipStrip);
                if (size_t at = label.find(L"{n}"); at != std::wstring::npos)
                    label.replace(at, 3, std::to_wstring(deck.clip_count));
                const float w = std::min(R - g.strip_title.right - 9.0f * s,
                                         38.0f * s + measure(label, false) * (11.5f / 13.0f));
                g.strip_clip = D2D1::RectF(R - w, cy - 12.0f * s, R, cy + 12.0f * s);
                hint_r = g.strip_clip.left - 9.0f * s;
            }
            g.strip_hint = D2D1::RectF(g.strip_title.right + 9.0f * s, cy - 10.0f * s,
                                       std::max(g.strip_title.right + 9.0f * s, hint_r), cy + 10.0f * s);
            const float hint_w = measure(pulse::l10n::Get(pulse::l10n::StringId::TrayStripHint), true);
            const float kbd_w = measure(pulse::l10n::Get(pulse::l10n::StringId::TrayStripKbd), true) + 10.0f * s;
            const float kx = g.strip_hint.left + hint_w + 4.0f * s;
            if (kx + kbd_w <= hint_r)
                g.strip_kbd = D2D1::RectF(kx, cy - 8.5f * s, kx + kbd_w, cy + 8.5f * s);
            else
                g.strip_hint_short = true;   // "拖入文件，或" must not end on 或
        }
        // Drop: two halves, padding 10, gap 8.
        {
            const float bottom = std::max(top + 40.0f * s, panel.bottom - 10.0f * s);
            const float mid = (L + R) * 0.5f;
            g.half[0] = D2D1::RectF(L, top + 10.0f * s, mid - 4.0f * s, bottom);
            g.half[1] = D2D1::RectF(mid + 4.0f * s, top + 10.0f * s, R, bottom);
        }
        // Shelf: r1 (44) / r2 (34) / r3 (26), padding 10, gap 9.
        {
            const float ib = (narrow ? 28.0f : 30.0f) * s;
            float y1 = top + 10.0f * s;
            if (deck.clip_count > 0) {
                // .cliprow: 26 tall, margin-top -2, then the usual 9 gap.
                g.clip_row = D2D1::RectF(L, y1 - 2.0f * s, R, y1 + 24.0f * s);
                y1 += 34.0f * s;
            }
            const float c1 = y1 + 22.0f * s;
            g.fan = D2D1::RectF(L, y1, L + 56.0f * s, y1 + 44.0f * s);
            g.has_expand = true;
            g.more = D2D1::RectF(R - ib, c1 - ib * 0.5f, R, c1 + ib * 0.5f);
            float meta_r = g.more.left - (narrow ? 4.0f : 6.0f) * s;
            if (g.has_expand) {
                g.expand = D2D1::RectF(g.more.left - 2.0f * s - ib, g.more.top,
                                       g.more.left - 2.0f * s, g.more.bottom);
                meta_r = g.expand.left - (narrow ? 4.0f : 6.0f) * s;
            }
            g.meta = D2D1::RectF(g.fan.right + (narrow ? 8.0f : 11.0f) * s, y1, std::max(g.fan.right, meta_r),
                                 y1 + 44.0f * s);

            const float y2 = y1 + 44.0f * s + 9.0f * s;
            const float c2 = y2 + 17.0f * s;
            g.rename = D2D1::RectF(R - ib, c2 - ib * 0.5f, R, c2 + ib * 0.5f);
            g.zip = D2D1::RectF(g.rename.left - 4.0f * s - ib, g.rename.top,
                                g.rename.left - 4.0f * s, g.rename.bottom);
            g.split = D2D1::RectF(L, y2, std::max(L + 60.0f * s, g.zip.left - 4.0f * s), y2 + 34.0f * s);
            g.dd = D2D1::RectF(g.split.right - 30.0f * s, g.split.top, g.split.right, g.split.bottom);
            g.go = D2D1::RectF(g.split.left, g.split.top, g.dd.left, g.split.bottom);

            const float y3 = y2 + 34.0f * s + 9.0f * s;
            const float lb_w = measure(pulse::l10n::Get(pulse::l10n::StringId::TraySendTo), true);
            g.send_label = D2D1::RectF(L + 2.0f * s, y3, L + 2.0f * s + lb_w, y3 + 26.0f * s);
            float x = g.send_label.right + 2.0f * s + 5.0f * s;
            const float pick_w = 30.0f * s;
            int shown = 0;
            for (int d = 0; d < static_cast<int>(deck.dests.size()) && shown < 2; ++d) {
                const auto& dest = deck.dests[static_cast<size_t>(d)];
                if (dest.missing) continue;
                // padding 8 + folder 13 + gap 6 + label + padding 10.
                const float max_label = 120.0f * s;
                const float w = 8.0f * s + 13.0f * s + 6.0f * s +
                                std::min(max_label, measure(dest.label, true)) + 10.0f * s;
                const float min_w = 8.0f * s + 13.0f * s + 6.0f * s + 28.0f * s + 10.0f * s;
                const float room = R - pick_w - 5.0f * s - x;
                if (room < min_w) break;
                const float cw = std::min(w, room);
                g.chips.push_back(D2D1::RectF(x, y3, x + cw, y3 + 26.0f * s));
                g.chip_dest.push_back(d);
                x += cw + 5.0f * s;
                ++shown;
            }
            g.pick = D2D1::RectF(x, y3, x + pick_w, y3 + 26.0f * s);
        }
        return g;
    }

    // Open view (pulse_tray_redesign.html .op): header 47, filter chips 34,
    // grouped list, footer 80 (selection line + split button).
    struct TrayOpenGeom {
        D2D1_RECT_F title{}, count{}, collapse{};
        std::vector<D2D1_RECT_F> filters;
        std::vector<int> filter_id;
        D2D1_RECT_F list{};                  // clip of the scrolling rows
        std::vector<D2D1_RECT_F> rows;       // per deck.open_rows
        std::vector<D2D1_RECT_F> heads;      // source header above the row (empty: none)
        std::vector<D2D1_RECT_F> head_open;  // 打开 › on that header
        float content_h = 0.0f;              // rows + headers, unscrolled
        D2D1_RECT_F footer{}, selinfo{}, none_link{}, cmp{};
        D2D1_RECT_F split{}, go{}, dd{};
    };
    // measure(text, compact): 12 px regular (compact) or 13 px bold.
    template <class Measure>
    TrayOpenGeom TrayOpenGeometry(const TrayDeckView& d, const D2D1_RECT_F& panel, float s,
                                  Measure&& measure) {
        using pulse::l10n::StringId;
        TrayOpenGeom g;
        const float L = panel.left, R = panel.right, top = panel.top;
        // .oh: padding 11 10 6 14; 14 px bold title, 12 px count, collapse ib 30.
        const float cy = top + 26.0f * s;
        const std::wstring title = pulse::l10n::Get(StringId::TrayShelfTitle);
        const float tw = measure(title, false) * (14.0f / 13.0f) + 1.0f * s;
        g.title = D2D1::RectF(L + 14.0f * s, cy - 10.0f * s, L + 14.0f * s + tw, cy + 10.0f * s);
        g.collapse = D2D1::RectF(R - 10.0f * s - 30.0f * s, cy - 15.0f * s, R - 10.0f * s, cy + 15.0f * s);
        g.count = D2D1::RectF(g.title.right + 8.0f * s, cy - 10.0f * s,
                              std::max(g.title.right + 8.0f * s, g.collapse.left - 8.0f * s), cy + 10.0f * s);
        // .flt: padding 2 12 8; chips h24, padding 0 10, gap 5, 11.5 px.
        {
            const StringId names[4] = {StringId::TrayFilterAll, StringId::TrayFilterImages,
                                       StringId::TrayFilterDocs, StringId::TrayFilterOther};
            float x = L + 12.0f * s;
            const float y = top + 49.0f * s;
            // Narrow sidebars squeeze the chip padding (20 -> 10) before
            // dropping a chip.
            float text_w[4] = {};
            float need = 0.0f;
            int shown = 0;
            for (int f = 0; f < 4; ++f) {
                if (f > 0 && d.filter_counts[f] == 0) continue;
                text_w[f] = measure(pulse::l10n::Get(names[f]) + L" " + std::to_wstring(d.filter_counts[f]), true) *
                            (11.5f / 12.0f) + (d.filter == f ? 4.0f * s : 0.0f);
                need += text_w[f];
                ++shown;
            }
            const float avail = (R - 8.0f * s) - x - 5.0f * s * static_cast<float>(std::max(0, shown - 1));
            const float pad = shown > 0 ? std::clamp((avail - need) / static_cast<float>(shown), 10.0f * s, 20.0f * s)
                                        : 20.0f * s;
            for (int f = 0; f < 4; ++f) {
                if (f > 0 && d.filter_counts[f] == 0) continue;
                const float w = text_w[f] + pad;
                if (x + w > R - 8.0f * s + 0.5f) break;
                g.filters.push_back(D2D1::RectF(x, y, x + w, y + 24.0f * s));
                g.filter_id.push_back(f);
                x += w + 5.0f * s;
            }
        }
        // .of: border-top, padding 9 10 11; selinfo 18, gap 8, split 34.
        g.footer = D2D1::RectF(L, std::max(top + 120.0f * s, panel.bottom - 80.0f * s), R, panel.bottom);
        const float fy = g.footer.top + 9.0f * s;
        g.selinfo = D2D1::RectF(L + 10.0f * s, fy, R - 10.0f * s, fy + 18.0f * s);
        if (d.checked > 0) {
            std::wstring prefix = pulse::l10n::Get(StringId::TraySelectedFormat);
            if (size_t at = prefix.find(L"{n}"); at != std::wstring::npos) prefix.replace(at, 3, std::to_wstring(d.checked));
            prefix += L" \u00B7 ";
            const float px = g.selinfo.left + measure(prefix, true) * (11.5f / 12.0f);
            const float lw = measure(pulse::l10n::Get(StringId::Cancel), true) * (11.5f / 12.0f) + 2.0f * s;
            g.none_link = D2D1::RectF(px, fy - 2.0f * s, px + lw, fy + 20.0f * s);
        }
        if (d.checked == 2) {
            const float w = measure(pulse::l10n::Get(StringId::TrayCompareThese), true) * (11.5f / 12.0f) + 18.0f * s;
            g.cmp = D2D1::RectF(g.selinfo.right - w, fy, g.selinfo.right, fy + 18.0f * s);
        }
        const float sy = fy + 18.0f * s + 8.0f * s;
        g.split = D2D1::RectF(L + 10.0f * s, sy, R - 10.0f * s, sy + 34.0f * s);
        g.dd = D2D1::RectF(g.split.right - 30.0f * s, g.split.top, g.split.right, g.split.bottom);
        g.go = D2D1::RectF(g.split.left, g.split.top, g.dd.left, g.split.bottom);
        // .ol: padding 0 8; .og header 27 (padding 8 6 4), .oi rows 36.
        g.list = D2D1::RectF(L, top + 81.0f * s, R, g.footer.top);
        float y = g.list.top - d.open_scroll * s;
        const float y0 = y;
        g.rows.reserve(d.open_rows.size());
        for (const TrayOpenRow& row : d.open_rows) {
            const float k = row.out_t > 0.0f ? std::max(0.0f, 1.0f - row.out_t) : 1.0f;
            if (row.group_start) {
                g.heads.push_back(D2D1::RectF(L + 8.0f * s, y, R - 8.0f * s, y + 27.0f * s));
                const float ow = measure(pulse::l10n::Get(StringId::TrayGroupOpen), true) * (11.0f / 12.0f) + 4.0f * s;
                g.head_open.push_back(D2D1::RectF(R - 14.0f * s - ow, y + 6.0f * s, R - 12.0f * s, y + 25.0f * s));
                y += 27.0f * s;
            } else {
                g.heads.push_back(D2D1::RectF());
                g.head_open.push_back(D2D1::RectF());
            }
            g.rows.push_back(D2D1::RectF(L + 8.0f * s, y, R - 8.0f * s, y + 36.0f * s * k));
            y += 36.0f * s * k;
        }
        g.content_h = (y - y0) / s + 8.0f;
        return g;
    }

    // Compare card (#cmp): 380 wide, right of the sidebar, bottom near the
    // tray footer. Rows: header, 位置, 修改, 大小 (grid 64 / 1fr / 1fr).
    struct TrayCardGeom {
        D2D1_RECT_F box{}, title{}, close{}, res{};
        float row_y[4]{};
        float row_h = 0.0f, label_l = 0.0f, col_l[2]{}, col_w = 0.0f;
    };
    inline TrayCardGeom TrayCompareCardGeometry(const D2D1_RECT_F& panel, float s, float window_right) {
        TrayCardGeom g;
        const float w = 380.0f * s, h = 196.0f * s;
        float left = panel.right + 18.0f * s;
        if (left + w > window_right - 8.0f * s) left = std::max(panel.left, window_right - 8.0f * s - w);
        const float bottom = panel.bottom - 44.0f * s;
        g.box = D2D1::RectF(left, bottom - h, left + w, bottom);
        const float L = g.box.left + 16.0f * s, R = g.box.right - 16.0f * s;
        g.title = D2D1::RectF(L, g.box.top + 14.0f * s, R - 26.0f * s, g.box.top + 32.0f * s);
        g.close = D2D1::RectF(R - 22.0f * s, g.box.top + 11.0f * s, R + 2.0f * s, g.box.top + 35.0f * s);
        g.row_h = 18.0f * s;
        float y = g.title.bottom + 10.0f * s;
        for (int i = 0; i < 4; ++i) {
            g.row_y[i] = y;
            y += (i == 0 ? 20.0f : 18.0f) * s + 6.0f * s;
        }
        g.label_l = L;
        g.col_w = std::max(0.0f, (R - L - 84.0f * s) * 0.5f);
        g.col_l[0] = L + 74.0f * s;
        g.col_l[1] = g.col_l[0] + g.col_w + 10.0f * s;
        g.res = D2D1::RectF(L, y + 6.0f * s, R, y + 6.0f * s + 34.0f * s);
        return g;
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

    std::wstring PaneBannerActionLabel(PaneBannerAction action) {
        using I = pulse::l10n::StringId;
        switch (action) {
        case PaneBannerAction::Retry: return pulse::l10n::Get(I::IntegrationRetry);
        case PaneBannerAction::GoUp: return pulse::l10n::Get(I::BannerGoUp);
        case PaneBannerAction::Reconnect: return pulse::l10n::Get(I::BannerReconnect);
        case PaneBannerAction::Unlock: return pulse::l10n::Get(I::BannerUnlock);
        case PaneBannerAction::Refresh: return pulse::l10n::Get(I::Refresh);
        case PaneBannerAction::ManageIndex: return pulse::l10n::Get(I::ContentManageShort);
        case PaneBannerAction::AddNetworkIndex: return pulse::l10n::Get(I::NetworkLiveAdd);
        case PaneBannerAction::NetworkSettings: return pulse::l10n::Get(I::Settings);
        case PaneBannerAction::LanRescan: return pulse::l10n::Get(I::HintActLanRescan);
        default: return {};
        }
    }

    // The strip takes a second line when its message does not fit beside the
    // title and the buttons; measured with the window font, a little generously.
    bool PaneBannerTwoLine(const PaneViewModel& pane, float width, float scale, Compositor* compositor) {
        if (pane.is_changes || pane.compare_active || pane.banner_message.empty() || !compositor) return false;
        IDWriteTextFormat* const format = compositor->TextFormat();
        if (!format) return false;
        const auto measure = [&](std::wstring_view text) {
            return text.empty() ? 0.0f : typography::MeasureLine(compositor, format, text);
        };
        float room = width - 16 * scale - (12 + 16 + 8 + 4) * scale;
        bool any = false;
        for (const auto action : pane.banner_actions) {
            if (action == PaneBannerAction::None) continue;
            room -= measure(PaneBannerActionLabel(action)) * 1.06f + 20 * scale;
            any = true;
        }
        if (any) room -= 6 * scale;
        if (pane.banner_dismissible) room -= 28 * scale;
        const float need = measure(pane.banner_title) * 1.06f + 10 * scale + measure(pane.banner_message);
        return need > room;
    }

    float PaneBannerHeight(const PaneViewModel& pane, float width, float scale, Compositor* compositor) {
        if (!pane.is_changes) return pane.banner_message.empty() ? 0.0f
            : (PaneBannerTwoLine(pane, width, scale, compositor) ? 54 * scale : 36 * scale);
        ComPtr<IDWriteTextLayout> layout;
        return ChangeBannerLayout(pane, width, scale, compositor, layout);
    }

    // The strip above a file list: what it shows and where its buttons go.
    // The InfoBar spec views the labels, so fill it in place and keep it alive.
    struct PaneBannerParts {
        std::wstring labels[2];
        HitTestResult::Region regions[2] = {HitTestResult::None, HitTestResult::None};
        int indices[2] = {-1, -1};
        fluent::InfoBarSpec bar{};
    };

    void BuildPaneBanner(const PaneViewModel& pane, const D2D1_RECT_F& bounds, int pane_index,
                         PaneBannerParts& out) {
        out.bar.bounds = bounds;
        out.bar.kind = static_cast<fluent::InfoBarKind>(std::clamp(pane.banner_kind, 0, 4));
        out.bar.show_close = false;
        if (pane.compare_active && !pane.is_content_search) {
            // Counts are drawn as colored chips by the caller, so narrow panes still fit.
            out.bar.kind = fluent::InfoBarKind::Neutral;
            out.labels[0] = pulse::l10n::Get(pane.compare_diff_only ? pulse::l10n::StringId::CompareShowAll
                                                                    : pulse::l10n::StringId::CompareDiffOnly);
            out.labels[1] = pulse::l10n::Get(pulse::l10n::StringId::CompareExit);
            out.regions[0] = HitTestResult::CompareDiffToggle;
            out.regions[1] = HitTestResult::CompareExit;
            out.indices[0] = out.indices[1] = pane_index;
        } else {
            out.bar.title = pane.banner_title;
            out.bar.message = pane.banner_message;
            out.bar.show_close = pane.banner_dismissible;
            for (int i = 0; i < 2; ++i) {
                const PaneBannerAction action = pane.banner_actions[i];
                if (action == PaneBannerAction::None) continue;
                out.labels[i] = PaneBannerActionLabel(action);
                out.regions[i] = action == PaneBannerAction::ManageIndex ? HitTestResult::ContentIndexManage
                    : action == PaneBannerAction::AddNetworkIndex ? HitTestResult::NetworkIndexAdd
                    : HitTestResult::PaneBannerButton;
                out.indices[i] = out.regions[i] == HitTestResult::PaneBannerButton ? static_cast<int>(action) : pane_index;
            }
        }
        for (int i = 0; i < 2; ++i) out.bar.actions[i] = out.labels[i];
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

    float TagQueryBarHeight(const PaneViewModel& pane, float scale) {
        if (!pane.is_tag_query) return 0.0f;
        return (kTagQueryRowDip + (pane.tag_query_refine.empty() ? 0.0f : kTagQueryRefineDip)) * scale;
    }

    float PaneExtraTop(const PaneViewModel& pane, float scale, float width, Compositor* compositor) {
        return PaneBannerHeight(pane, width, scale, compositor) + TagQueryBarHeight(pane, scale) +
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

    // 收藏夹 root: 改成横向收藏栏, just left of the chevron.
    D2D1_RECT_F SidebarBarSwitchRect(const D2D1_RECT_F& row, float scale) {
        const D2D1_RECT_F expand = SidebarExpandRect(row, scale);
        const float right = expand.left - 2.0f * scale;
        return D2D1::RectF(right - (expand.right - expand.left), expand.top, right, expand.bottom);
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

    // The user's first day of the week, Sunday = 0 like SYSTEMTIME::wDayOfWeek.
    int FirstDayOfWeek() {
        DWORD value = 0;  // LOCALE_IFIRSTDAYOFWEEK counts from Monday = 0
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                             reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(wchar_t)) ||
            value > 6)
            return 1;
        return static_cast<int>((value + 1) % 7);
    }

    std::wstring LastWeekdayName(int day) {
        wchar_t text[64];
        swprintf_s(text, pulse::l10n::Get(pulse::l10n::StringId::DateLastWeekFormat).c_str(),
                   WeekdayName(day).c_str());
        return text;
    }

    // Recent times read faster relative to today: "今天 14:32", "昨天 09:10",
    // "周三 18:00" this week, "上周五 18:00" earlier in the last 7 days, this
    // year "4月13日 22:28", older "2024-04-02".
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
        switch (ClassifyRecentDay(diff, now.wDayOfWeek, FirstDayOfWeek())) {
        case RecentDayLabel::Today: return pulse::l10n::Get(StringId::DateToday) + L" " + hm;
        case RecentDayLabel::Yesterday: return pulse::l10n::Get(StringId::DateYesterday) + L" " + hm;
        case RecentDayLabel::ThisWeek: return WeekdayName(st.wDayOfWeek) + L" " + hm;
        case RecentDayLabel::LastWeek: return LastWeekdayName(st.wDayOfWeek) + L" " + hm;
        case RecentDayLabel::Date: break;
        }
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
        for (int day = 0; day < 7; ++day) {
            out.push_back(WeekdayName(day) + L" 23:59");
            out.push_back(LastWeekdayName(day) + L" 23:59");
        }
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

    // 下载并预览 button: centred near the bottom of the preview well; the
    // explanation sits above it. Empty when the well is too short for both.
    inline D2D1_RECT_F DetailsCloudButtonRect(const D2D1_RECT_F& preview, float s, float label_w) {
        if (preview.bottom - preview.top < 132.0f * s) return {};
        const float w = std::min(preview.right - preview.left - 24.0f * s, label_w + 16.0f * s + 6.0f * s + 32.0f * s);
        if (w < 60.0f * s) return {};
        const float cx = (preview.left + preview.right) * 0.5f;
        const float bottom = preview.bottom - 16.0f * s;
        return D2D1::RectF(std::round(cx - w * 0.5f), bottom - 30.0f * s, std::round(cx + w * 0.5f), bottom);
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
        if (d.cloud_online && d.preview_enabled && d.multi_count <= 1 && d.cloud_download_percent < 0) {
            const std::wstring label = pulse::l10n::Get(pulse::l10n::StringId::CloudDownloadPreview);
            const float label_w = dwrite && small_fmt
                ? MeasureLayoutText(compositor, dwrite, small_fmt, label) : 90.0f * s;
            out.cloud_download = DetailsCloudButtonRect(out.preview, s, label_w);
        }
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
    D2D1_RECT_F ai_chip{};       // the AI chip at the far left; empty when hidden
    bool ai_chip_text = false;   // the chip has room for its words (else "✦ AI" only)
    float content_left = 0.0f;   // where everything else starts (right of the AI chip)
    float content_right = 0.0f;  // where everything else ends
    float pad = 0.0f;
    float right_reserved = 0.0f;
};

constexpr float kAiChipMaxTextDip = 240.0f;

StatusBarMetrics MakeStatusBarMetrics(const WindowViewModel& vm, const D2D1_RECT_F& full,
                                      float scale, float status_height,
                                      IDWriteFactory2* factory, IDWriteTextFormat* small_format) {
    const bool centered_progress = vm.status.query_active || vm.status.task_is_update;
    StatusBarMetrics m;
    m.bar = D2D1::RectF(full.left, full.bottom - status_height, full.right, full.bottom);
    m.pad = kStatusBarPadDip * scale;
    // The AI chip (Pulse 助手) takes the far left; the status text starts
    // right of it.
    D2D1_RECT_F rect = full;
    m.content_left = m.pad;
    if (vm.status.ai_state != StatusAiState::Hidden) {
        // Widths measured in the small face, scaled to the chip's 12.5 DIP.
        float k = 1.0f;
        if (small_format && small_format->GetFontSize() > 0.0f) k = 12.5f * scale / small_format->GetFontSize();
        const std::wstring label = vm.status.ai_label.empty() ? std::wstring(L"AI") : vm.status.ai_label;
        const float label_w = factory && small_format ? MeasureTextWidth(factory, small_format, label) * k * 1.06f + 1.0f
                                                       : 14.0f * scale;
        m.ai_chip_text = vm.status.ai_state != StatusAiState::Idle && !vm.status.ai_text.empty() &&
                         full.right - full.left >= 640.0f * scale && factory && small_format;
        const bool bold_text = vm.status.ai_state == StatusAiState::Waiting || vm.status.ai_state == StatusAiState::Done ||
                               vm.status.ai_state == StatusAiState::Error || vm.status.ai_state == StatusAiState::Warn;
        const float text_w = m.ai_chip_text
            ? (std::min)(kAiChipMaxTextDip * scale,
                         MeasureTextWidth(factory, small_format, vm.status.ai_text) * k * (bold_text ? 1.06f : 1.0f) + 1.0f)
            : 0.0f;
        const float width = AiStatusChipWidth(label_w, text_w, m.ai_chip_text,
                                              vm.status.ai_state == StatusAiState::Busy && vm.status.ai_progress >= 0.0f,
                                              scale);
        const float h = 24.0f * scale;
        const float top = std::round(m.bar.top + (status_height - h) * 0.5f);
        const float left = full.left + m.pad - 4.0f * scale;
        m.ai_chip = D2D1::RectF(left, top, left + width, top + h);
        m.content_left = m.ai_chip.right + 12.0f * scale;
    }
    m.content_right = rect.right;
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

// Main-window variant: LumaText draws the line (with its own end ellipsis)
// so labels match the file list; otherwise DirectWrite trims a layout whose
// glyphs LumaText still rasterizes. Paragraph-centred formats only reach
// LumaText's line path, which always centres vertically.
void DrawTextEndEllipsis(Compositor* compositor, IDWriteTextFormat* fmt,
    ID2D1SolidColorBrush* br, const std::wstring& text, float x, float y, float w, float h) {
    if (!compositor || !compositor->Dc() || !fmt || !br || text.empty() || w <= 1.0f || h <= 0.0f) return;
    IDWriteFactory2* factory = compositor->DwriteFactory();
    if (!IsHighContrast() && fmt->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_CENTER &&
        compositor->DrawLumaText(text, fmt, D2D1::RectF(x, y, x + w, y + h), br->GetColor(),
                                 D2D1::ColorF(0, 0.0f), fmt->GetTextAlignment())) {
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (!factory || FAILED(factory->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
                                                     w, h, &layout)) || !layout.get()) {
        DrawTextEndEllipsis(compositor->Dc(), factory, fmt, br, text, x, y, w, h);
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(fmt->GetTextAlignment());
    if (MeasureTextWidth(factory, fmt, text) > w) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    compositor->DrawTextLayout(D2D1::Point2F(x, y), layout.get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
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

// Main-window tab titles through LumaText, keeping the DirectWrite version's
// rule that a short extension stays visible when the stem is elided.
void DrawTabTitle(Compositor* compositor, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
                  const std::wstring& text, float x, float y, float w, float h, float scale) {
    if (!compositor || !compositor->Dc() || !fmt || !br || text.empty() || w <= 1.0f || h <= 1.0f) return;
    IDWriteFactory2* factory = compositor->DwriteFactory();
    const auto luma = [&](const std::wstring& s, float left, float width, DWRITE_TEXT_ALIGNMENT alignment) {
        return !IsHighContrast() && width > 1.0f &&
            compositor->DrawLumaText(s, fmt, D2D1::RectF(left, y, left + width, y + h),
                                     br->GetColor(), D2D1::ColorF(0, 0.0f), alignment);
    };
    const size_t dot = text.find_last_of(L'.');
    if (factory && MeasureTextWidth(factory, fmt, text) > w && dot != std::wstring::npos && dot > 0 &&
        dot + 1 < text.size() && text.size() - dot <= 12) {
        const std::wstring ext = text.substr(dot);
        const float extW = MeasureTextWidth(factory, fmt, ext);
        if (extW + 20.0f * scale < w &&
            luma(text.substr(0, dot), x, w - extW, DWRITE_TEXT_ALIGNMENT_LEADING)) {
            // A little slack: the extension box came from DirectWrite widths.
            const float pad = 4.0f * scale;
            if (!luma(ext, x + w - extW - pad, extW + pad, DWRITE_TEXT_ALIGNMENT_TRAILING)) {
                const D2D1_RECT_F ext_rc = D2D1::RectF(x + w - extW, y, x + w, y + h);
                compositor->Dc()->DrawText(ext.c_str(), (UINT32)ext.size(), fmt, &ext_rc, br,
                                           D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            return;
        }
    }
    if (luma(text, x, w, fmt->GetTextAlignment())) return;
    DrawTabTitle(compositor->Dc(), factory, fmt, br, text, x, y, w, h, scale);
}

// Edge-style: a title too long for its tab fades out over its last few
// pixels instead of ending in "...", so narrow tabs keep more of the name.
void DrawTabTitleFaded(Compositor* compositor, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
                       const std::wstring& text, float x, float y, float w, float h, float scale) {
    if (!compositor || !compositor->Dc() || !fmt || !br || text.empty() || w <= 1.0f || h <= 1.0f) return;
    IDWriteFactory2* factory = compositor->DwriteFactory();
    ID2D1DeviceContext* dc = compositor->Dc();
    const float fullW = factory ? MeasureTextWidth(factory, fmt, text) : 0.0f;
    const float fade = std::min(20.0f * scale, w * 0.4f);
    ComPtr<ID2D1GradientStopCollection> stops;
    ComPtr<ID2D1LinearGradientBrush> mask;
    if (fullW > w && !IsHighContrast() && fmt->GetReadingDirection() == DWRITE_READING_DIRECTION_LEFT_TO_RIGHT) {
        const D2D1_GRADIENT_STOP s[] = {{0.0f, D2D1::ColorF(0, 0, 0, 1.0f)},
                                        {(w - fade) / w, D2D1::ColorF(0, 0, 0, 1.0f)},
                                        {1.0f, D2D1::ColorF(0, 0, 0, 0.0f)}};
        if (SUCCEEDED(dc->CreateGradientStopCollection(s, 3, &stops)) && stops.get())
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(x, 0.0f), D2D1::Point2F(x + w, 0.0f)), stops.get(), &mask);
    }
    if (!mask.get()) {
        DrawTabTitle(compositor, fmt, br, text, x, y, w, h, scale);
        return;
    }
    dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(x, y, x + w, y + h), nullptr,
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                         1.0f, mask.get(), D2D1_LAYER_OPTIONS1_NONE), nullptr);
    // Laid out at full width so nothing trims; the layer clips and fades it.
    DrawTabTitle(compositor, fmt, br, text, x, y, fullW + 8.0f * scale, h, scale);
    dc->PopLayer();
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
// 5: 快速预览, 6: 系统右键菜单, 7: 快捷键, 8: 增强包; 0 and 9-12 split the former 通用 page:
// 0 外观, 9 文件列表, 10 标签页与窗口, 11 启动与集成, 12 文件操作.
constexpr int kSettingsNavCount = 13;
inline bool IsGeneralSettingsPage(int page) noexcept { return page == 0 || (page >= 9 && page <= 12); }
// Sidebar order, top to bottom, in five groups (关于 3 sits at the bottom, 6 has no row).
constexpr int kSettingsNavOrder[] = {0, 9, 10, 11, 12, 1, 5, 8, 2, 7, 4};
constexpr int kSettingsNavGroupStart[] = {0, 1, 3, 5, 10};  // indexes into kSettingsNavOrder
constexpr int kSettingsNavGroups = 5;

// System folder groups shown under Settings > Exclusions: 0-4 follow
// index::kSystemExclusionGroups, 5-6 are always skipped by the index.
constexpr int kSystemGroupRows = 7;
constexpr int kSystemGroupConfigurable = 5;
inline pulse::l10n::StringId SystemGroupLabel(int group) {
    using I = pulse::l10n::StringId;
    static constexpr I kLabels[kSystemGroupRows] = {I::SystemGroupWindows, I::SystemGroupTemp,
                                    I::SystemGroupOld, I::SystemGroupProgramData,
                                    I::SystemGroupNodeModules, I::SystemGroupRecycle,
                                    I::SystemGroupComponents};
    return kLabels[group < 0 || group >= kSystemGroupRows ? 0 : group];
}
inline const wchar_t* SystemGroupDetail(int group) {
    static constexpr const wchar_t* kDetails[] = {
        L"C:\\Windows",
        L"Windows\\Temp \u00b7 AppData\\Local\\Temp \u00b7 INetCache \u00b7 CrashDumps",
        L"Windows.old \u00b7 $WINDOWS.~BT \u00b7 $Windows.~WS \u00b7 $WinREAgent",
        L"C:\\ProgramData",
        L"node_modules",
        L"$Recycle.Bin \u00b7 System Volume Information",
        L"WinSxS \u00b7 servicing"};
    static_assert(sizeof(kDetails) / sizeof(kDetails[0]) == kSystemGroupRows);
    return kDetails[group < 0 || group >= kSystemGroupRows ? 0 : group];
}
inline bool SystemGroupEnabled(const WindowViewModel& vm, int group) {
    return group >= kSystemGroupConfigurable || (vm.settings_index_system_groups & (1u << group)) != 0;
}
inline bool SystemGroupChipVisible(const WindowViewModel& vm, int group) {
    return SystemGroupEnabled(vm, group);
}

inline app::BuiltinMenuSurface PulseMenuSurface(const WindowViewModel& vm) {
    return vm.settings_context_tab == 1 ? app::BuiltinMenuSurface::Background
         : vm.settings_context_tab == 2 ? app::BuiltinMenuSurface::RowButtons
                                        : app::BuiltinMenuSurface::Item;
}
// Preview metrics (DIP), shared by layout and drawing.
inline constexpr float kPulsePreviewTitle = 24.0f, kPulsePreviewRow = 34.0f,
    kPulsePreviewSwatches = 34.0f, kPulsePreviewSeparator = 9.0f, kPulsePreviewPad = 5.0f;
// Hint line under the preview box ("drag to reorder").
inline constexpr float kPulsePreviewHint = 26.0f;
// Switch rows of the Pulse menu card (系统右键菜单 rows are kSysCtxRowDip).
inline constexpr float kPulseMenuRowDip = 48.0f;
inline const wchar_t* PulseMenuSideNote() {
    return l10n::Get(l10n::StringId::ThePreviewUsesTheSameDataAsTheRealMenuCommandsFromWindowsAnd).c_str();
}
inline const app::BuiltinMenuOrder& PulseMenuOrder(const WindowViewModel& vm) {
    return vm.settings_builtin_order[static_cast<size_t>(PulseMenuSurface(vm))];
}
inline bool PulseMenuOrderCustom(const WindowViewModel& vm, app::BuiltinMenuSurface surface) {
    const auto& order = vm.settings_builtin_order[static_cast<size_t>(surface)];
    return !order.empty() && app::NormalizeBuiltinMenuOrder(surface, order) != app::BuiltinMenuDefaultOrder(surface);
}
inline float PulseMenuPreviewHeight(const WindowViewModel& vm, float scale) {
    if (vm.settings_context_tab == 2) return (kPulsePreviewTitle + 56.0f + kPulsePreviewHint) * scale;
    float h = 2 * kPulsePreviewPad;
    for (const auto& r : app::BuiltinMenuVisibleRows(PulseMenuSurface(vm), vm.settings_builtin_hidden, PulseMenuOrder(vm))) {
        h += kPulsePreviewRow;
        if (r.row.item == app::BuiltinMenuItem::Tags) h += kPulsePreviewSwatches;
        if (r.separator_after) h += kPulsePreviewSeparator;
    }
    return (kPulsePreviewTitle + h + kPulsePreviewHint) * scale;
}
// The menu box inside l.pulse_preview (below the title, above the hint).
inline D2D1_RECT_F PulsePreviewBox(const D2D1_RECT_F& preview, float scale) {
    return D2D1::RectF(preview.left, preview.top + kPulsePreviewTitle * scale,
                       preview.right, preview.bottom - kPulsePreviewHint * scale);
}
inline int PulseMenuRowHit(const app::BuiltinMenuRow& row) {
    return row.fixed != app::BuiltinFixedRow::None ? kSettingsMenuFixedHit + static_cast<int>(row.fixed)
                                                   : kSettingsMenuRowHit + static_cast<int>(row.item);
}

// One preview row (the tag swatches belong to the 标签... row) or, on the
// row-buttons tab, one hover button.
struct PulsePreviewRow {
    app::BuiltinMenuRow row{};
    D2D1_RECT_F rect{};
    bool separator_after = false;
};
struct PulsePreviewLayout {
    std::vector<PulsePreviewRow> rows; // without the dragged row while dragging
    bool dragging = false;
    app::BuiltinMenuRow drag_row{};
    D2D1_RECT_F ghost{}, gap{};        // floating row; the slot it would land in
    app::BuiltinMenuOrder order;       // order a drop now would give
};
inline std::vector<PulsePreviewRow> LayoutPulsePreviewRows(const WindowViewModel& vm, const app::BuiltinMenuOrder& order,
                                                           const D2D1_RECT_F& box, float s) {
    std::vector<PulsePreviewRow> out;
    if (vm.settings_context_tab == 2) {
        const auto visible = app::BuiltinMenuVisibleRows(app::BuiltinMenuSurface::RowButtons, vm.settings_builtin_hidden, order);
        const auto row = D2D1::RectF(box.left + 8 * s, box.top + 12 * s, box.right - 8 * s, box.bottom - 12 * s);
        out.resize(visible.size());
        float bx = row.right - 28 * s;
        for (size_t k = visible.size(); k-- > 0;) {
            out[k] = { visible[k].row, D2D1::RectF(bx, row.top + 4 * s, bx + 24 * s, row.bottom - 4 * s), false };
            bx -= 26 * s;
        }
        return out;
    }
    float y = box.top + kPulsePreviewPad * s;
    const float x0 = box.left + 6 * s, x1 = box.right - 6 * s;
    for (const auto& v : app::BuiltinMenuVisibleRows(PulseMenuSurface(vm), vm.settings_builtin_hidden, order)) {
        const float h = (kPulsePreviewRow + (v.row.item == app::BuiltinMenuItem::Tags ? kPulsePreviewSwatches : 0.0f)) * s;
        out.push_back({ v.row, D2D1::RectF(x0, y, x1, y + h), v.separator_after });
        y += h;
        if (v.separator_after) y += kPulsePreviewSeparator * s;
    }
    return out;
}
// While a row is dragged, every landing place is tried and the one whose
// slot is nearest the floating row wins; the rows are laid out as that drop
// would leave them, so separators and groups preview truthfully.
inline PulsePreviewLayout LayoutPulsePreview(const WindowViewModel& vm, const D2D1_RECT_F& box, float s) {
    PulsePreviewLayout l;
    const auto surface = PulseMenuSurface(vm);
    const bool horizontal = vm.settings_context_tab == 2;
    l.order = app::NormalizeBuiltinMenuOrder(surface, PulseMenuOrder(vm));
    l.rows = LayoutPulsePreviewRows(vm, l.order, box, s);
    if (vm.settings_menu_drag < 0 || vm.settings_menu_drag >= app::kBuiltinMenuItemCount) return l;
    const auto dragged = static_cast<app::BuiltinMenuItem>(vm.settings_menu_drag);
    auto movable = [&](const PulsePreviewRow& r) { return r.row.fixed == app::BuiltinFixedRow::None && r.row.item < app::BuiltinMenuItem::Count; };
    const auto held = std::find_if(l.rows.begin(), l.rows.end(), [&](const PulsePreviewRow& r) { return movable(r) && r.row.item == dragged; });
    if (held == l.rows.end()) return l;
    app::BuiltinMenuOrder rest = l.order;
    rest.erase(std::remove(rest.begin(), rest.end(), dragged), rest.end());
    std::vector<app::BuiltinMenuItem> rest_visible;
    for (const auto& r : l.rows) if (movable(r) && r.row.item != dragged) rest_visible.push_back(r.row.item);
    if (rest_visible.empty()) return l;
    const float size = horizontal ? held->rect.right - held->rect.left : held->rect.bottom - held->rect.top;
    const float lo = horizontal ? box.left : box.top, hi = horizontal ? box.right : box.bottom;
    const float start = std::clamp(vm.settings_menu_drag_pos - vm.settings_menu_drag_grab, lo, std::max(lo, hi - size));
    float best = 1e30f;
    for (size_t p = 0; p <= rest_visible.size(); ++p) {
        app::BuiltinMenuOrder candidate = rest;
        auto at = p < rest_visible.size() ? std::find(candidate.begin(), candidate.end(), rest_visible[p])
                                          : std::find(candidate.begin(), candidate.end(), rest_visible.back()) + 1;
        candidate.insert(at, dragged);
        auto rows = LayoutPulsePreviewRows(vm, candidate, box, s);
        const auto slot = std::find_if(rows.begin(), rows.end(), [&](const PulsePreviewRow& r) { return movable(r) && r.row.item == dragged; });
        if (slot == rows.end()) continue;
        const float d = std::abs((horizontal ? slot->rect.left : slot->rect.top) - start);
        if (d >= best) continue;
        best = d;
        l.order = std::move(candidate);
        l.gap = slot->rect;
        l.drag_row = slot->row;
        rows.erase(slot);
        l.rows = std::move(rows);
    }
    l.dragging = true;
    l.ghost = horizontal ? D2D1::RectF(start, l.gap.top, start + size, l.gap.bottom)
                         : D2D1::RectF(l.gap.left, start, l.gap.right, start + size);
    return l;
}

} // namespace

// Settings page geometry, shared by drawing and hit testing. Kept out of the
// anonymous namespace so MakeSettingsLayout (ui_settings_layout.cpp) exists
// once instead of being compiled into every settings file.
struct SettingsLayout {
    // Rows of vm.settings_items; only Pulse's own commands (group 5) are laid
    // out. Windows and other software are managed on 系统右键菜单 (page 6).
    std::vector<D2D1_RECT_F> context_rows;
    // Three columns like 系统右键菜单: pulse_tabs is the left card with the
    // surfaces (pulse_tab) under group label pulse_group[0]; pulse_card the
    // switch list (head, presets 3 = 自定义 display only, fixed rows);
    // pulse_side the live preview card, present on wide pages only.
    D2D1_RECT_F pulse_card{}, pulse_restore{}, pulse_preset[4]{}, pulse_tabs{}, pulse_tab[3]{},
        pulse_fixed[2]{}, pulse_preview{}, pulse_order_reset{};
    D2D1_RECT_F pulse_group[2]{}, pulse_head{}, pulse_rows_top{}, pulse_side{}, pulse_side_tab{},
        pulse_side_note{};
    // The left card's last row, under pulse_group[1]: where the entries of
    // Windows and other software went (opens page 6).
    D2D1_RECT_F context_system_hint{};
    D2D1_RECT_F duplicate_options{};
    D2D1_RECT_F section[8]{}, group[8]{}, footer{};
    // General pages (IsGeneralSettingsPage): the title of each section[i], the
    // tinted bands behind dependent rows, and the dividers between rows.
    l10n::StringId section_title[8]{};
    int section_count = 0;
    std::vector<D2D1_RECT_F> child_bands;
    std::vector<D2D1_RECT_F> dividers;      // left, top (= y), right; bottom unused
    // Sidebar group headings (empty when the sidebar is too short or compact)
    // and the thin separators used instead.
    D2D1_RECT_F nav_group_label[kSettingsNavGroups]{};
    D2D1_RECT_F nav_separator[kSettingsNavGroups]{};
    // 主题色 › 自定义: the button, the preview row and the three slider rows.
    D2D1_RECT_F accent_custom_button{}, accent_custom_head{}, accent_custom_swatch{};
    D2D1_RECT_F accent_slider_row[3]{}, accent_slider_cells[3][3]{};
    // 关于与诊断 › 新手提示.
    D2D1_RECT_F tips_card{};
    D2D1_RECT_F theme_row{}, theme_tile[3]{}, effect_choice{}, language_choice{};
    D2D1_RECT_F performance_row{}, disclosure[4]{}, filename_status{};
    D2D1_RECT_F integration_card{}, integration_status{}, integration_summary{}, integration_hint{};
    D2D1_RECT_F integration_retry{}, integration_restore{};
    D2D1_RECT_F integration_section{}, integration_badge{}, integration_bar{}, integration_list_head{};
    D2D1_RECT_F integration_chip[4]{};
    D2D1_RECT_F integration_cover[3]{}, integration_options_head{}, integration_try{}, integration_hint_text{};
    float integration_text_right = 0.0f;
    // Quick Look page: 预览方式 (hover card or Space) and the hover delay.
    D2D1_RECT_F trigger_section{}, trigger_row{}, trigger_choice[2]{}, hover_delay_row{}, hover_delay_choice[3]{};
    D2D1_RECT_F hover_size_row{}, hover_size_choice[3]{};  // 卡片尺寸: 小 / 中 / 大
    // Quick Look page: formats, source legend and system extensions.
    D2D1_RECT_F preview_section{}, preview_group{}, preview_formats{}, preview_legend{}, preview_legend_note{};
    D2D1_RECT_F preview_header_title{}, preview_header_summary{};
    D2D1_RECT_F preview_legend_chip[3]{}, preview_codec_section{}, preview_codec_group{}, preview_codec_hint{}, pack_section{};
    D2D1_RECT_F preview_codec_text[kPreviewCodecCount]{}, preview_codec_badge[kPreviewCodecCount]{};
    D2D1_RECT_F preview_codec_row[kPreviewCodecCount]{}, preview_codec_button[kPreviewCodecCount]{};
    // 预览增强包 page (5), see LayoutSettingsPacks.
    D2D1_RECT_F pack_update_all{}, pack_manage{};
    // 增强包 page, 搜索: the 内容搜索 module and the planned OCR one.
    D2D1_RECT_F pack_search_section{}, pack_content_card{}, pack_content_badge{}, pack_content_enable{},
        pack_content_primary{}, pack_content_update{}, pack_content_notice{};
    D2D1_RECT_F pack_ocr_card{}, pack_ocr_badge{}, pack_ocr_enable{}, pack_ocr_primary{}, pack_ocr_update{},
        pack_ocr_notice{};
    // 增强包 page, AI: the AI pack (MCP server and, later, the assistant).
    D2D1_RECT_F pack_ai_section{}, pack_ai_card{}, pack_ai_badge{}, pack_ai_enable{}, pack_ai_primary{},
        pack_ai_update{}, pack_ai_notice{};
    float pack_ai_desc_h = 0.0f;
    float pack_content_desc_h = 0.0f, pack_ocr_desc_h = 0.0f;   // 增强包 page: 全部更新; 快速预览 page: 管理增强包
    D2D1_RECT_F pack_summary{}, pack_open{}, pack_media_section{}, pack_card{}, pack_badge{}, pack_enable{},
        pack_primary{}, pack_update{}, pack_notice{}, pack_advanced_section{}, pack_custom_row{}, pack_path_row{},
        pack_detect{}, pack_browse{}, pack_remove_row{}, pack_group{}, pack_note{}, pack_source_group{}, pack_source_status{},
        pack_auto_update_row{};
    float pack_desc_h = 0.0f;
    // The image pack card (same parts as the FFmpeg one).
    D2D1_RECT_F pack_images_section{}, pack_images_card{}, pack_images_badge{}, pack_images_enable{},
        pack_images_primary{}, pack_images_update{}, pack_images_notice{};
    float pack_images_desc_h = 0.0f;
    D2D1_RECT_F pack_raw_card{}, pack_raw_badge{}, pack_raw_enable{}, pack_raw_primary{}, pack_raw_update{}, pack_raw_notice{};
    float pack_raw_desc_h = 0.0f;
    D2D1_RECT_F pack_archive_section{}, pack_archive_card{}, pack_archive_badge{}, pack_archive_enable{}, pack_archive_primary{},
        pack_archive_update{}, pack_archive_notice{};
    float pack_archive_desc_h = 0.0f;
    D2D1_RECT_F content_header{}, content_types{}, content_pause{}, content_options{}, content_rebuild{}, content_empty{};
    D2D1_RECT_F body{};
    D2D1_RECT_F nav{};
    D2D1_RECT_F content{};
    D2D1_RECT_F nav_row[kSettingsNavCount]{};
    // 右键菜单 (page 2): intro and the layer cards, 0 = Pulse 右键菜单 (current),
    // 1 = 系统右键菜单; page 6 has its own (sysctx.local_card / system_card).
    D2D1_RECT_F ctx_intro{}, ctx_layer[2]{};
    SysCtxLayout sysctx;  // page 6, see LayoutSettingsSysCtx
    ShortcutsLayout shortcuts;  // page 7, see LayoutSettingsShortcuts
    AiPageLayout ai;            // kSettingsPageAi, see LayoutSettingsAi
    D2D1_RECT_F accent_card{};
    D2D1_RECT_F accent_picker{};
    D2D1_RECT_F effect_card{};
    D2D1_RECT_F effect_row[kWindowEffectCount]{};
    D2D1_RECT_F menu_acrylic_row{};
    D2D1_RECT_F density_card{};
    D2D1_RECT_F list_style_row[6]{};
    // 修改时间提示: the style picker; range, colors and switches while it is on.
    D2D1_RECT_F mtime_style_card{}, mtime_style_row[4]{};
    D2D1_RECT_F mtime_range_card{}, mtime_range_row[4]{};
    D2D1_RECT_F mtime_palette_card{}, mtime_palette_row[6]{};
    D2D1_RECT_F mtime_toggle_row[5]{};   // fade, bold, folders, other views, flash
    D2D1_RECT_F sync_row[2]{};           // OneDrive, SVN
    // 鼠标与选择: open mode; name band and hover selection while single-click; check boxes.
    D2D1_RECT_F click_open_card{}, click_open_row[2]{};
    D2D1_RECT_F name_highlight_card{}, name_highlight_row[2]{};
    D2D1_RECT_F hover_select_row{}, item_checkbox_row{};
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
    D2D1_RECT_F external_open_card{};
    D2D1_RECT_F external_open_row[3]{};
    D2D1_RECT_F close_last_tab_row{};
    D2D1_RECT_F close_tab_double_click_row{};
    D2D1_RECT_F confirm_delete_row{};
    D2D1_RECT_F start_in_tray_row{};
    D2D1_RECT_F text_render_card{};
    D2D1_RECT_F text_render_row[3]{};
    D2D1_RECT_F ui_font_size_card{};
    D2D1_RECT_F ui_font_size_row[4]{};
    D2D1_RECT_F ui_font_family_card{};
    D2D1_RECT_F ui_font_family_choice{};
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
    D2D1_RECT_F frame_look_card{};     // 窗口效果 › 侧边栏与标题栏
    D2D1_RECT_F frame_look_row[3]{};
    D2D1_RECT_F startup_row[3]{};
    D2D1_RECT_F hidden_files_row{};
    D2D1_RECT_F protected_files_row{};
    D2D1_RECT_F pinned_names_row{};
    D2D1_RECT_F alias_mark_row{};
    D2D1_RECT_F tree_sticky_row{};
    D2D1_RECT_F thumb_cache_row{};     // 缩略图缓存: usage bar and 清除缓存
    D2D1_RECT_F thumb_cache_button{};
    D2D1_RECT_F vertical_tabs_row{};
    D2D1_RECT_F tray_hide_row{};
    D2D1_RECT_F lan_discovery_row{}, network_recent_row{}, hidden_shares_row{};
    D2D1_RECT_F lan_cache_card{}, lan_cache_row[3]{};
    D2D1_RECT_F network_clear_row{}, network_clear_button{};
    D2D1_RECT_F hints_row{};
    D2D1_RECT_F hints_reset_row{};
    D2D1_RECT_F hints_reset_button{};
    D2D1_RECT_F blank_click_row{};
    D2D1_RECT_F blank_click_choice[3]{};
    D2D1_RECT_F win_e_row{};
    D2D1_RECT_F this_pc_row{};
    D2D1_RECT_F recycle_bin_row{};
    D2D1_RECT_F explorer_windows_row{};
    D2D1_RECT_F folders_compat_row{};
    D2D1_RECT_F default_manager_row{};
    D2D1_RECT_F shell_tags_row{};
    D2D1_RECT_F change_tracking_row{}, change_days_row{}, change_days[3]{};
    D2D1_RECT_F search_pinyin_row{};
    D2D1_RECT_F search_master_section{}, search_master_row{};
    D2D1_RECT_F global_search_row{}, global_search_hotkey_row{}, global_search_hotkey_button{};
    D2D1_RECT_F global_search_double_ctrl_row{}, global_search_gap_card{}, global_search_gap[3]{};
    D2D1_RECT_F global_search_apps_row{}, dialog_jump_row{}, dialog_jump_auto_row{};
    D2D1_RECT_F jump_history_row{}, jump_history_clear{};
    D2D1_RECT_F content_index_row{};
    D2D1_RECT_F index_info{};
    D2D1_RECT_F index_status{};
    D2D1_RECT_F index_path{};
    D2D1_RECT_F index_action[3]{};
    std::vector<D2D1_RECT_F> index_volume_rows;
    D2D1_RECT_F index_exclude_action{};
    D2D1_RECT_F index_exclude_empty{};
    // System folder switch: card spans the switch row, chips, group rows and
    // the user's exclusion rows below them.
    D2D1_RECT_F index_system_card{}, index_system_row{}, index_system_more{};
    D2D1_RECT_F index_system_chip[kSystemGroupRows]{};
    D2D1_RECT_F index_system_group[kSystemGroupRows]{};
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
    D2D1_RECT_F transfer_card{};       // 备份与迁移
    D2D1_RECT_F transfer_action[3]{};  // export, import, restore / restart (empty: hidden)
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

SettingsLayout MakeSettingsLayout(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                  float scale, float title_h, float status_h,
                                  const fluent::Painter* painter);

namespace {

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

// Tagged release notes (release_note_tags.h): a badge column in front of the
// tagged items, headings in the section style with room above them, and a row
// of per-tag counts under the summary line. Layout and painting both measure
// through MeasureReleaseLine, so the expanded body is as tall as what is drawn.
constexpr float kReleaseHeadingGapDip = 8.0f;
constexpr float kReleaseBadgeHeightDip = 18.0f;
constexpr float kReleaseBadgeGapDip = 8.0f;
constexpr float kReleaseLegendHeightDip = 28.0f;

struct ReleaseTagSummary {
    int count[kReleaseTagCount]{};
    const std::wstring* label[kReleaseTagCount]{};  // first label of each tag
    bool any = false;
};

inline ReleaseTagSummary SummarizeReleaseTags(const ReleaseNoteView& note) {
    ReleaseTagSummary summary;
    for (size_t k = 0; k < note.tags.size() && k < note.tag_labels.size(); ++k) {
        const int tag = static_cast<int>(note.tags[k]);
        if (tag <= 0 || tag >= kReleaseTagCount) continue;
        if (summary.count[tag]++ == 0) summary.label[tag] = &note.tag_labels[k];
        summary.any = true;
    }
    return summary;
}

inline bool ReleaseLineTagged(const ReleaseNoteView& note, size_t k) {
    return k < note.tags.size() && note.tags[k] != ReleaseTag::None && k < note.tag_labels.size();
}

inline bool ReleaseLineHeading(const ReleaseNoteView& note, size_t k) {
    return k < note.headings.size() && note.headings[k];
}

// The widest tag of the note plus a gap; 0 for an untagged note.
inline float ReleaseTagColumn(const fluent::Painter* painter, const ReleaseNoteView& note, float scale) {
    float widest = 0.0f;
    for (size_t k = 0; painter && k < note.lines.size(); ++k)
        if (ReleaseLineTagged(note, k))
            widest = (std::max)(widest, painter->MeasureBadgeWidth(note.tag_labels[k]));
    return widest > 0.0f ? widest + kReleaseBadgeGapDip * scale : 0.0f;
}

struct ReleaseLineBox {
    float gap_above = 0.0f;  // headings after the first line
    float text_h = 0.0f;
    float legend_h = 0.0f;   // the count row under the summary line
    float Total() const { return gap_above + text_h + legend_h; }
};

inline ReleaseLineBox MeasureReleaseLine(const fluent::Painter* painter, const ReleaseNoteView& note,
                                         size_t k, float text_w, float tag_col,
                                         const ReleaseTagSummary& summary, float scale) {
    ReleaseLineBox box;
    const std::wstring& line = note.lines[k];
    if (ReleaseLineHeading(note, k)) {
        if (k > 0) box.gap_above = kReleaseHeadingGapDip * scale;
        const float h = painter ? painter->MeasureWrappedSectionHeight(line, text_w) : 0.0f;
        box.text_h = h > 0.0f ? h : 20.0f * scale;
    } else if (tag_col > 0.0f && ReleaseLineTagged(note, k)) {
        const float width = (std::max)(40.0f * scale, text_w - tag_col);
        box.text_h = (std::max)(kReleaseBadgeHeightDip * scale, ReleaseLineHeight(painter, line, width, scale));
    } else {
        box.text_h = ReleaseLineHeight(painter, line, text_w, scale);
    }
    const bool plain_first = k == 0 && !(k < note.bullets.size() && note.bullets[k]) && !ReleaseLineHeading(note, k);
    if (plain_first && summary.any) box.legend_h = kReleaseLegendHeightDip * scale;
    return box;
}

// 新功能 accent, 新设置 purple, 改进 green, 修复 amber; high contrast keeps system colours.
inline void StyleReleaseBadge(fluent::BadgeSpec& spec, ReleaseTag tag, bool dark, bool high_contrast) {
    spec.use_custom_colors = false;
    switch (tag) {
    case ReleaseTag::New: spec.kind = fluent::BadgeKind::Accent; break;
    case ReleaseTag::Improved: spec.kind = fluent::BadgeKind::Success; break;
    case ReleaseTag::Fixed: spec.kind = fluent::BadgeKind::Warning; break;
    case ReleaseTag::Setting:
        spec.kind = high_contrast ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral;
        if (!high_contrast) {
            spec.use_custom_colors = true;
            spec.custom_background = dark ? D2D1::ColorF(0x352A55) : D2D1::ColorF(0xEEE6FF);
            spec.custom_foreground = dark ? D2D1::ColorF(0xC9AEFF) : D2D1::ColorF(0x6B2FD6);
        }
        break;
    default: spec.kind = fluent::BadgeKind::Neutral; break;
    }
}

#include "settings_layout_sections.h"
#include "settings_sysctx_layout.h"
#include "settings_shortcuts_layout.h"
#include "settings_ai_layout.h"


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
    if (sb.ai_chip.right > sb.ai_chip.left && ContainsPt(sb.ai_chip, x, y)) return HitTestResult::StatusBarAi;
    if (vm.status.query_cancellable && ContainsPt(sb.cancel_search, x, y)) return HitTestResult::StatusBarCancelSearch;
    if (sb.hint_action.right > sb.hint_action.left && ContainsPt(sb.hint_action, x, y))
        return HitTestResult::StatusHintAction;
    return !vm.status.query_active && !vm.status.task_is_update && ContainsPt(sb.task, x, y) ? HitTestResult::StatusBarTask
                                    : HitTestResult::StatusBar;
}

// Tag view condition bar (pulse_tag_redesign §5). Row 1: the terms, the 位置
// chip, ＋ 条件 and 保存视图 (kept at the end of the row; terms that do not fit before them
// get an empty rect); neighboring 任一 chips have 或 between them. Row 2:
// 细分, then the refinement chips that fit.
struct TagQueryBarLayout {
    std::vector<D2D1_RECT_F> chips;     // per tag_query_chips
    std::vector<D2D1_RECT_F> removes;   // the × of term and 位置 chips (empty for ＋ / 保存)
    std::vector<D2D1_RECT_F> or_marks;  // 或
    std::vector<D2D1_RECT_F> refine;    // per tag_query_refine
    D2D1_RECT_F refine_label{};
};

TagQueryBarLayout LayoutTagQueryBar(Compositor* compositor, const PaneViewModel& pane,
                                    const D2D1_RECT_F& bounds, float top, float scale) {
    using Chip = TagQueryChipView;
    TagQueryBarLayout out;
    IDWriteFactory2* factory = compositor ? compositor->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor ? compositor->SmallFormat() : nullptr;
    const auto text_w = [&](const std::wstring& text) {
        const float w = factory && fmt ? MeasureTextWidth(factory, fmt, text)
                                       : 7.0f * scale * static_cast<float>(text.size());
        return (std::min)(w, 160.0f * scale);
    };
    const auto count_w = [&](int count) {
        return count < 0 ? 0.0f : text_w(std::to_wstring(count)) + 6.0f * scale;
    };
    const float gap = 6.0f * scale;
    const float right = bounds.right - 10.0f * scale;
    const float chip_h = 28.0f * scale;
    const float y = top + (kTagQueryRowDip * scale - chip_h) * 0.5f;
    const auto chip_w = [&](const Chip& chip) {
        switch (chip.kind) {
        case Chip::Add:
        case Chip::Save: return (10.0f + 12.0f + 6.0f + 10.0f) * scale + text_w(chip.label);
        case Chip::Location: return (10.0f + 14.0f + 6.0f + 4.0f + 16.0f + 6.0f) * scale + text_w(chip.label);
        default: return (10.0f + 8.0f + 6.0f + 4.0f + 16.0f + 6.0f) * scale + text_w(chip.label);
        }
    };
    const size_t n = pane.tag_query_chips.size();
    out.chips.assign(n, D2D1_RECT_F{});
    out.removes.assign(n, D2D1_RECT_F{});
    // ＋ 条件 and 保存视图 end the row; their room is kept first.
    float limit = right;
    for (const Chip& chip : pane.tag_query_chips)
        if (chip.kind >= Chip::Add) limit -= chip_w(chip) + gap;
    const float or_w = text_w(pulse::l10n::Get(pulse::l10n::StringId::TagOr)) + 8.0f * scale;
    float x = bounds.left + 10.0f * scale;
    bool full = false;
    for (size_t i = 0; i < n; ++i) {
        const Chip& chip = pane.tag_query_chips[i];
        const bool add = chip.kind >= Chip::Add;
        const bool after_or = !add && i > 0 && chip.kind == Chip::Any &&
                              pane.tag_query_chips[i - 1].kind == Chip::Any;
        const float left = after_or ? x + or_w - gap * 0.5f : x;
        const float w = chip_w(chip);
        if (!add && (full || left + w > limit)) {
            full = true;
            continue;
        }
        if (after_or) out.or_marks.push_back(D2D1::RectF(x - gap * 0.5f, y, left, y + chip_h));
        out.chips[i] = D2D1::RectF(left, y, left + w, y + chip_h);
        if (!add) {
            const float cy = y + chip_h * 0.5f;
            out.removes[i] = D2D1::RectF(left + w - 24.0f * scale, cy - 10.0f * scale,
                                         left + w - 4.0f * scale, cy + 10.0f * scale);
        }
        x = left + w + gap;
    }

    const size_t m = pane.tag_query_refine.size();
    out.refine.assign(m, D2D1_RECT_F{});
    if (m == 0) return out;
    const float refine_h = 24.0f * scale;
    const float ry = top + kTagQueryRowDip * scale + (kTagQueryRefineDip * scale - refine_h) * 0.5f - 4.0f * scale;
    const std::wstring& label = pulse::l10n::Get(pulse::l10n::StringId::TagRefineLabel);
    float rx = bounds.left + 12.0f * scale;
    out.refine_label = D2D1::RectF(rx, ry, rx + text_w(label) + 2.0f * scale, ry + refine_h);
    rx = out.refine_label.right + 8.0f * scale;
    for (size_t i = 0; i < m; ++i) {
        const Chip& chip = pane.tag_query_refine[i];
        const float lead = chip.kind == Chip::RefineLocation ? 14.0f : 8.0f;
        const float w = (8.0f + lead + 6.0f + 8.0f) * scale + text_w(chip.label) + count_w(chip.count);
        if (rx + w > right) break;
        out.refine[i] = D2D1::RectF(rx, ry, rx + w, ry + refine_h);
        rx += w + 4.0f * scale;
    }
    return out;
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
    // Left-to-right order from bits 3-8 (0 star, 1 new tab, 2 more); the
    // rightmost button docks first. Default: new tab, star, more.
    int order[3] = { 1, 0, 2 };
    if (const unsigned code = (allowed_actions >> 3) & 0x3Fu; code != 0)
        for (int i = 0; i < 3; ++i) order[i] = static_cast<int>((code >> (2 * i)) & 3u);
    for (int i = 2; i >= 0; --i) {
        if (order[i] == 2 && (allowed_actions & 4u) && (show_more || reserve_actions)) {
            t.more = D2D1::RectF(dock - btn, by, dock, by + btn);
            dock = t.more.left - gap;
        } else if (order[i] == 0 && (allowed_actions & 1u) && (show_star || reserve_actions)) {
            t.star = D2D1::RectF(dock - btn, by, dock, by + btn);
            dock = t.star.left - gap;
        } else if (order[i] == 1 && (allowed_actions & 2u) &&
                   (show_new_tab || (reserve_actions && !compact_actions))) {
            t.new_tab = D2D1::RectF(dock - btn, by, dock, by + btn);
            dock = t.new_tab.left - gap;
        }
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
