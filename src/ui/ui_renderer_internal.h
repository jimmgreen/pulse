// ui_renderer_internal.h — Draw + HitTest shared geometry (not a public API).
#pragma once
#include "ui_renderer.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"
#include "typography.h"
#include "../app/places.h"
#include "../app/search_query.h"
#include "../common/text_format.h"
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
    constexpr const wchar_t* kIconTheme = L"\xE706";
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

        const fs::DirEntry& source = (*vm.snapshot)[index];
        const bool penetrated = !source.link_target.empty();
        ListEntryView entry;
        // Keep the on-disk name, including .lnk. Stripping hid the suffix and
        // made the icon cache treat resolved shortcuts as extensionless files.
        entry.name = source.name;
        entry.size_text = penetrated
            ? (source.link_target_is_dir ? L"" : pulse::format::ByteSize(source.link_target_size, true))
            : (source.is_dir ? L"" : pulse::format::ByteSize(source.size, true));
        entry.date_text = pulse::format::LocalFileTime(source.mtime);
        entry.path = !source.full_path.empty() ? source.full_path
                     : (fs::IsVirtualPath(vm.path) ? L"" : JoinDirName(vm.path, source.name));
        entry.attrs = source.attrs;
        entry.size_value = source.size;
        entry.modified_value = (static_cast<uint64_t>(source.mtime.dwHighDateTime) << 32) |
                               source.mtime.dwLowDateTime;
        entry.is_dir = penetrated ? source.link_target_is_dir : source.is_dir;
        entry.is_reparse = source.is_reparse;
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
        entry.starred = vm.tag_catalog && !entry.path.empty() &&
            vm.tag_catalog->IsStarred(entry.path);
        if (vm.search_snippets && index < vm.search_snippets->size())
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

    float SidebarItemHeight(const SidebarItem& item, const SidebarMetrics& m) {
        if (item.is_drive) return m.driveH;
        if (item.is_tag) return m.tagH;
        return m.itemH;
    }

    int TrayTotalCount(const WindowViewModel& vm) { return vm.tray_deck.total_count; }

    float ExpandedTrayHeight(const WindowViewModel& vm, const SidebarMetrics& m) {
        const float base = m.trayInner * 2.0f + m.trayHeaderH;
        if (vm.tray_deck.cards.empty())
            return std::max(base + m.trayHelperH + 8.0f * m.scale, 112.0f * m.scale);
        // Card deck: header + icon lane + footer (totals + clear action).
        return base + 4.0f * m.scale + m.trayDeckH + 18.0f * m.scale;
    }

    float SidebarContentHeight(const WindowViewModel& vm, const SidebarMetrics& m) {
        float height = m.pad;
        for (const auto& group : vm.sidebar) {
            if (group.items.empty() && group.add_action == SidebarAddAction::None) continue;
            height += m.headerH + 4.0f * m.scale;
            if (!group.collapsed) {
                for (const auto& item : group.items)
                    height += SidebarItemHeight(item, m) + m.itemGap;
                height += m.groupGap - m.itemGap;
            }
        }
        return height + m.pad;
    }

    void LayoutSidebar(const WindowViewModel& vm, const D2D1_RECT_F& sb, float scale,
                       std::vector<SidebarSlot>& out) {
        out.clear();
        const float width = sb.right - sb.left;
        const bool compact = width <= 60.0f * scale;
        const SidebarMetrics m = MakeSidebarMetrics(scale);

        if (compact) {
            const float rowH = 38.0f * scale;
            const float trayH = 44.0f * scale;
            float y = sb.top;
            int run = 0;
            for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
                const auto& group = vm.sidebar[g];
                if (group.collapsed) continue;
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
            if (group.items.empty() && group.add_action == SidebarAddAction::None) continue;
            if (y >= contentBottom) break;
            SidebarSlot header;
            header.kind = SidebarSlot::Header;
            header.rc = D2D1::RectF(innerL, y, innerR, y + m.headerH);
            header.group = g;
            if (header.rc.bottom > sb.top && header.rc.bottom <= contentBottom)
                out.push_back(header);
            y += m.headerH + 4.0f * scale;
            if (group.collapsed) continue;
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
            clear.rc = D2D1::RectF(innerR - inset - 72.0f * scale,
                                   tray.rc.bottom - m.trayInner - 22.0f * scale,
                                   tray.rc.right - inset,
                                   tray.rc.bottom - m.trayInner);
            out.push_back(clear);
        }
    }

    bool RectContains(const D2D1_RECT_F& rc, float x, float y) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    }

    float PaneExtraTop(const PaneViewModel& pane, float scale) {
        return (pane.banner_message.empty() ? 0.0f : 36.0f * scale) +
               (pane.is_recent ? kRecentControlsDip * scale : 0.0f) +
               (pane.is_query_search ? kSearchFiltersDip * scale : 0.0f);
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
            default:
                return pulse::l10n::Get(pulse::l10n::StringId::SearchChipSize);
            }
        };
        labels[0] = kind_label();
        labels[1] = date_label();
        labels[2] = size_label();
        labels[3] = spec.content.empty()
            ? pulse::l10n::Get(pulse::l10n::StringId::SearchChipContent)
            : spec.content;
        labels[4] = pulse::l10n::Get(pulse::l10n::StringId::AdvancedSearch);
    }

    void SearchFilterChipWidthsPx(const fluent::Painter& painter, float scale,
                                  const std::wstring labels[5], float widths[5]) {
        const float cap = 220.0f * scale;
        const float min_w = 32.0f * scale;
        for (int i = 0; i < 5; ++i) {
            float w = painter.MeasureButtonWidth(labels[i], {}, i < 3);
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
        for (int i = 0; i < index && i < 5; ++i) x += widths_px[i] + gap;
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
        return item.indent == 0 && item.path.starts_with(L"pulse:workspace:");
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

    bool PathIsSelfOrChild(const std::wstring& root, const std::wstring& path) {
        if (root.empty() || path.empty()) return false;
        if (_wcsicmp(root.c_str(), path.c_str()) == 0) return true;
        std::wstring prefix = root;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
        return path.size() >= prefix.size() &&
            _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    // ---------------------------------------------------------------------
    // Tray scatter deck geometry. Shared by DrawTrayDeck and HitTest so the
    // two can never disagree about where an icon is. Cards sit on eased
    // slots but each gets a deterministic per-card offset/rotation/size
    // jitter (keyed by path) for an irregular stacked look; slot spacing
    // never closes past half an icon, so every card keeps >=50% of its face
    // visible and clickable.
    // ---------------------------------------------------------------------
    struct TrayFanGeom {
        float icon = 0.0f;     // icon edge, DIPs
        float cx = 0.0f;       // deck center
        float cy = 0.0f;       // resting center y of slot 0
        float step_x = 0.0f;   // horizontal spacing per slot
        float tilt = 0.0f;     // max per-card rotation, degrees
        float hjit = 0.0f;     // horizontal jitter amplitude
        float vjit = 0.0f;     // vertical jitter amplitude
        float spread = 1.0f;   // 1 + drag-over scatter boost
    };

    TrayFanGeom TrayFanGeometry(const D2D1_RECT_F& panel, int live_count, float open,
                                float scale, float icon_dip) {
        TrayFanGeom g;
        const bool single = live_count <= 1;
        g.icon = (single ? icon_dip + 8.0f : icon_dip) * scale;
        const float deckTop = panel.top + 4.0f * scale + 22.0f * scale + 4.0f * scale;
        // The footer row (totals + clear) reserves the bottom 18px.
        const float deckBottom = panel.bottom - 8.0f * scale - 18.0f * scale - 4.0f * scale;
        g.cx = (panel.left + panel.right) * 0.5f;
        g.cy = deckTop + (deckBottom - deckTop) * 0.5f + (single ? -18.0f * scale : 0.0f);
        const float avail = (panel.right - panel.left) - 24.0f * scale - g.icon;
        g.step_x = live_count > 1
            ? std::min(g.icon * 0.72f, avail / static_cast<float>(live_count - 1)) : 0.0f;
        // Worst-case center distance is step_x - 2*hjit; clamp hjit so it
        // stays >= icon/2 (max 50% overlap between neighbors).
        g.hjit = std::clamp((g.step_x - g.icon * 0.5f) * 0.5f, 0.0f, g.icon * 0.08f);
        g.vjit = std::max(0.0f, (deckBottom - deckTop) * 0.5f - g.icon * 0.62f);
        g.spread = 1.0f + 0.25f * std::clamp(open, 0.0f, 1.0f);
        g.tilt = 13.0f;
        return g;
    }

    // Deterministic per-card scatter seed: keyed by path so a card keeps its
    // jitter for its whole lifetime (eased slot changes and ghosts included).
    uint32_t TrayCardSeed(const std::wstring& path) {
        uint32_t h = 2166136261u; // FNV-1a
        for (const wchar_t c : path) {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        return h;
    }

    // Independent hash stream -> [0, 1).
    float TraySeedFrac(uint32_t seed, uint32_t stream) {
        uint32_t x = seed + stream * 0x9E3779B9u;
        x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
        return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
    }

    struct TrayCardPose {
        D2D1_POINT_2F center{};
        float angle = 0.0f;
        float scale_f = 1.0f;
    };

    // Current visual pose of one deck entry; scale_f folds in enter/hover zoom.
    TrayCardPose TrayCardPoseOf(const TrayFanGeom& g, const TrayCardView& card, float scale) {
        const float hover = std::clamp(card.hover, 0.0f, 1.0f);
        const float appear = std::clamp(card.appear, 0.0f, 1.0f);
        const float opacity = std::clamp(card.opacity, 0.0f, 1.0f);
        const uint32_t seed = TrayCardSeed(card.path);
        const float jx = (TraySeedFrac(seed, 1) * 2.0f - 1.0f) * g.hjit;
        const float jy = (TraySeedFrac(seed, 2) * 2.0f - 1.0f) * g.vjit * g.spread;
        const float ja = (TraySeedFrac(seed, 3) * 2.0f - 1.0f) * g.tilt * g.spread;
        const float js = 0.94f + 0.12f * TraySeedFrac(seed, 4); // 0.94..1.06
        TrayCardPose p;
        p.center = D2D1::Point2F(
            g.cx + card.slot * g.step_x + jx,
            g.cy + jy - hover * 10.0f * scale
                 + (1.0f - opacity) * 14.0f * scale); // ghosts sink while fading
        p.angle = ja * (1.0f - 0.9f * hover); // straighten on hover
        p.scale_f = (0.55f + 0.45f * appear) * js * (1.0f + 0.10f * hover);
        return p;
    }

    // Back-to-front paint order: ghosts underneath, then by resting y so a
    // lower card overlaps the ones above it; the hovered card draws last.
    std::vector<int> TrayCardPaintOrder(const TrayDeckView& deck, const TrayFanGeom& g,
                                        float scale) {
        std::vector<int> order(deck.cards.size());
        std::vector<float> ys(deck.cards.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) {
            order[static_cast<size_t>(i)] = i;
            ys[static_cast<size_t>(i)] =
                TrayCardPoseOf(g, deck.cards[static_cast<size_t>(i)], scale).center.y;
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const TrayCardView& ca = deck.cards[static_cast<size_t>(a)];
            const TrayCardView& cb = deck.cards[static_cast<size_t>(b)];
            if (ca.ghost != cb.ghost) return ca.ghost && !cb.ghost;
            const bool ha = a == deck.hovered, hb = b == deck.hovered;
            if (ha != hb) return hb;
            return ys[static_cast<size_t>(a)] < ys[static_cast<size_t>(b)];
        });
        return order;
    }

    // Hit test a point against a posed icon (inverse-rotate around center).
    bool TrayCardHit(const TrayFanGeom& g, const TrayCardView& card, float scale,
                     float x, float y, bool* close_zone) {
        const TrayCardPose p = TrayCardPoseOf(g, card, scale);
        const float rad = -p.angle * 3.14159265f / 180.0f;
        const float dx = x - p.center.x, dy = y - p.center.y;
        const float lx = dx * std::cos(rad) - dy * std::sin(rad);
        const float ly = dx * std::sin(rad) + dy * std::cos(rad);
        const float half = g.icon * p.scale_f * 0.5f;
        if (std::abs(lx) > half || std::abs(ly) > half) return false;
        if (close_zone) {
            // × badge circle at the icon's top-right corner (local space).
            const float bx = half * 0.85f, by = -half * 0.85f;
            const float cx = lx - bx, cy = ly - by;
            *close_zone = (cx * cx + cy * cy) <= (11.0f * scale) * (11.0f * scale);
        }
        return true;
    }

    // ---------------------------------------------------------------------
    // Details panel: one layout for draw + hit-test (tray-deck idiom).
    // ---------------------------------------------------------------------

    // Fixed 16:10 band driven by the panel width; no manual resize.
    float DetailsPreviewHeight(const D2D1_RECT_F& panel, float scale) {
        const float w = panel.right - panel.left - 24.0f * scale - 24.0f * scale;
        const float h = w * 10.0f / 16.0f;
        return std::clamp(h, 160.0f * scale, 360.0f * scale);
    }
    float DetailsPreviewBand(float preview_h, float scale) {
        return preview_h + 12.0f * scale;
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

    // Details-view column widths (DIP). Type must fit "AutoCAD File" /
    // "Text document" after 8dip insets and Luma ink overhang.
    constexpr float kDetailsMinNameDip = 80.0f;
    constexpr float kDetailsMinPathDip = 110.0f;
    constexpr float kDetailsMinDateDip = 92.0f;
    constexpr float kDetailsMinTypeDip = 100.0f;
    constexpr float kDetailsMinSizeDip = 72.0f;
    constexpr float kDetailsDateDip = 130.0f;
    constexpr float kDetailsTypeDip = 128.0f;
    constexpr float kDetailsSizeDip = 90.0f;

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
        out.preview = D2D1::RectF(x, panel.top + pad, panel.right - 36.0f * s,
                                  panel.top + pad + preview_h);
        const float previewBottom = panel.top + pad + preview_h;
        float y = previewBottom + 12.0f * s - d.scroll_y * s;
        if (d.multi_count <= 1) {
            // Name row: star then rename pencil on the right edge.
            out.rename = D2D1::RectF(panel.right - pad - 22.0f * s, y,
                                     panel.right - pad, y + 22.0f * s);
            out.star = D2D1::RectF(out.rename.left - 4.0f * s - 22.0f * s, y,
                                   out.rename.left - 4.0f * s, y + 22.0f * s);
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

struct PaneEmptyLayout {
    D2D1_RECT_F art{};
    D2D1_RECT_F title{};
    D2D1_RECT_F message{};
    D2D1_RECT_F action{};
    bool show_message = false;
    bool show_action = false;
};

PaneEmptyLayout MakePaneEmptyLayout(const D2D1_RECT_F& bounds, float scale,
                                    bool can_create, float art_aspect = 512.0f / 360.0f) {
    PaneEmptyLayout out;
    const float width = std::max(0.0f, bounds.right - bounds.left);
    const float height = std::max(0.0f, bounds.bottom - bounds.top);
    out.show_message = height >= 220.0f * scale;
    out.show_action = can_create && height >= 280.0f * scale && width >= 180.0f * scale;

    const float titleH = 24.0f * scale;
    const float messageH = out.show_message ? 20.0f * scale : 0.0f;
    const float actionH = out.show_action ? 34.0f * scale : 0.0f;
    const float textGap = out.show_message ? 2.0f * scale : 0.0f;
    const float actionGap = out.show_action ? 14.0f * scale : 0.0f;
    const float fixedH = titleH + textGap + messageH + actionGap + actionH;
    const float aspect = art_aspect > 0.05f ? art_aspect : (512.0f / 360.0f);
    const float maxArtW = std::max(72.0f * scale,
        std::min(200.0f * scale, width - 32.0f * scale));
    const float maxArtH = std::max(60.0f * scale, height - fixedH - 44.0f * scale);
    const float artW = std::min(maxArtW, maxArtH * aspect);
    const float artH = artW / aspect;
    // The SVG viewBox already has bottom breathing room; keep only a small
    // layout gap so the illustration and copy read as one centered group.
    const float artGap = 2.0f * scale;
    const float totalH = artH + artGap + fixedH;
    float y = bounds.top + std::max(8.0f * scale, (height - totalH) * 0.5f);
    const float cx = (bounds.left + bounds.right) * 0.5f;
    out.art = D2D1::RectF(cx - artW * 0.5f, y, cx + artW * 0.5f, y + artH);
    y = out.art.bottom + artGap;
    out.title = D2D1::RectF(bounds.left + 12.0f * scale, y,
                            bounds.right - 12.0f * scale, y + titleH);
    y = out.title.bottom + textGap;
    out.message = D2D1::RectF(bounds.left + 12.0f * scale, y,
                              bounds.right - 12.0f * scale, y + messageH);
    y = out.message.bottom + actionGap;
    const float actionW = std::min(142.0f * scale, width - 32.0f * scale);
    out.action = D2D1::RectF(cx - actionW * 0.5f, y,
                             cx + actionW * 0.5f, y + actionH);
    return out;
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
    float pad = 0.0f;
    float right_reserved = 0.0f;
};

StatusBarMetrics MakeStatusBarMetrics(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                      float scale, float status_height,
                                      IDWriteFactory2* factory, IDWriteTextFormat* small_format) {
    StatusBarMetrics m;
    m.bar = D2D1::RectF(rect.left, rect.bottom - status_height, rect.right, rect.bottom);
    m.pad = kStatusBarPadDip * scale;
    m.right_reserved = m.pad;
    const std::wstring* trailing = nullptr;
    if (!vm.status.performance_text.empty()) {
        trailing = rect.right < 1100.0f * scale
            ? &vm.status.performance_compact_text : &vm.status.performance_text;
    } else if (!vm.status.hint_text.empty()) {
        trailing = &vm.status.hint_text;
    }
    if (trailing && factory && small_format) {
        const float perfWidth = std::min(rect.right * 0.50f,
            MeasureTextWidth(factory, small_format, *trailing) + 16.0f * scale);
        m.right_reserved = perfWidth + m.pad;
    }
    const bool has_task = !vm.status.task_text.empty() || vm.status.task_progress >= 0.0f;
    if (has_task) {
        m.task = D2D1::RectF(rect.right * 0.48f, m.bar.top,
                             rect.right - m.right_reserved, m.bar.bottom);
    }
    return m;
}

fluent::BadgeKind IndexVolumeBadgeKind(const std::wstring& state) {
    if (state.find(L"失败") != std::wstring::npos) return fluent::BadgeKind::Danger;
    if (state.find(L"非 NTFS") != std::wstring::npos ||
        state.find(L"不支持") != std::wstring::npos) return fluent::BadgeKind::Warning;
    if (state.find(L"正在") != std::wstring::npos ||
        state.find(L"等待") != std::wstring::npos) return fluent::BadgeKind::Accent;
    if (state.find(L"就绪") != std::wstring::npos ||
        state.find(L"实时") != std::wstring::npos) return fluent::BadgeKind::Success;
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
    c.theme_left = c.chrome_left - c.theme_w - 6.0f * scale;
    c.settings_left = c.theme_left - c.settings_w - 6.0f * scale;
    c.cmd_w = 0.0f;
    c.cmd_left = c.settings_left;
    const float cmd_pad = 8.0f * scale;
    c.cmd_top = cmd_pad;
    c.cmd_h = std::max(0.0f, title_h - cmd_pad * 2.0f);
    return c;
}

constexpr float kSettingsNavW = 200.0f;
constexpr int kSettingsNavCount = 5;

struct SettingsLayout {
    D2D1_RECT_F body{};
    D2D1_RECT_F nav{};
    D2D1_RECT_F content{};
    D2D1_RECT_F nav_row[kSettingsNavCount]{};
    D2D1_RECT_F accent_card{};
    D2D1_RECT_F accent_picker{};
    D2D1_RECT_F effect_card{};
    D2D1_RECT_F effect_row[kWindowEffectCount]{};
    D2D1_RECT_F density_card{};
    D2D1_RECT_F density_row[3]{};
    D2D1_RECT_F tray_icon_card{};
    D2D1_RECT_F tray_icon_row[3]{};
    D2D1_RECT_F language_card{};
    D2D1_RECT_F language_segment[3]{};
    D2D1_RECT_F wallpaper_card{};
    D2D1_RECT_F wallpaper_preview{};
    D2D1_RECT_F wallpaper_choose{};
    D2D1_RECT_F wallpaper_clear{};
    D2D1_RECT_F startup_row[3]{};
    D2D1_RECT_F hidden_files_row{};
    D2D1_RECT_F pinned_names_row{};
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
    D2D1_RECT_F diagnostics_card{};
    D2D1_RECT_F diagnostics_perf{};
    D2D1_RECT_F diagnostics_action[3]{};
    D2D1_RECT_F update_card{};
    D2D1_RECT_F update_action[2]{};
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

SettingsLayout MakeSettingsLayout(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                  float scale, float title_h, float status_h,
                                  const fluent::Painter* painter) {
    SettingsLayout l;
    l.body = D2D1::RectF(rect.left, title_h, rect.right, rect.bottom - status_h);
    l.nav = D2D1::RectF(l.body.left, l.body.top, l.body.left + kSettingsNavW * scale, l.body.bottom);
    l.content = D2D1::RectF(l.nav.right, l.body.top, l.body.right, l.body.bottom);
    const float row_h = 40.0f * scale;
    const float nav_pad = 12.0f * scale;
    for (int i = 0; i < kSettingsNavCount; ++i) {
        const float y = l.nav.top + nav_pad + 8.0f * scale + i * (row_h + 4.0f * scale);
        l.nav_row[i] = D2D1::RectF(l.nav.left + 8.0f * scale, y,
                                   l.nav.right - 8.0f * scale, y + row_h);
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
        y += 22.0f * scale;
        y += 8.0f * scale;
        const float radio_h = 36.0f * scale;
        const float effect_header = 56.0f * scale;
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;

        const float picker = kBloomPickerDip * scale;
        const float accent_h = 96.0f * scale;
        l.accent_card = D2D1::RectF(card_left, y, card_right, y + accent_h);
        l.accent_picker = D2D1::RectF(card_right - 16.0f * scale - picker,
                                      y + (accent_h - picker) * 0.5f,
                                      card_right - 16.0f * scale,
                                      y + (accent_h + picker) * 0.5f);
        y += accent_h + 12.0f * scale;

        const float effect_h = effect_header + kWindowEffectCount * radio_h + 8.0f * scale;
        l.effect_card = D2D1::RectF(card_left, y, card_right, y + effect_h);
        for (int i = 0; i < kWindowEffectCount; ++i) {
            const float ry = y + effect_header + static_cast<float>(i) * radio_h;
            l.effect_row[i] = D2D1::RectF(card_left, ry, card_right, ry + radio_h);
        }
        y += effect_h + 12.0f * scale;

        const float density_h = effect_header + 3 * radio_h + 8.0f * scale;
        l.density_card = D2D1::RectF(card_left, y, card_right, y + density_h);
        for (int i = 0; i < 3; ++i) {
            const float ry = y + effect_header + static_cast<float>(i) * radio_h;
            l.density_row[i] = D2D1::RectF(card_left, ry, card_right, ry + radio_h);
        }
        y += density_h + 12.0f * scale;

        l.tray_icon_card = D2D1::RectF(card_left, y, card_right, y + density_h);
        for (int i = 0; i < 3; ++i) {
            const float ry = y + effect_header + static_cast<float>(i) * radio_h;
            l.tray_icon_row[i] = D2D1::RectF(card_left, ry, card_right, ry + radio_h);
        }
        y += density_h + 12.0f * scale;

        const float language_h = 112.0f * scale;
        l.language_card = D2D1::RectF(card_left, y, card_right, y + language_h);
        const float segment_left = card_left + 16.0f * scale;
        const float segment_right = card_right - 16.0f * scale;
        const float segment_w = (segment_right - segment_left) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            l.language_segment[i] = D2D1::RectF(
                segment_left + static_cast<float>(i) * segment_w, y + 66.0f * scale,
                segment_left + static_cast<float>(i + 1) * segment_w, y + 98.0f * scale);
        }
        y += language_h + 12.0f * scale;

        const bool compact_wallpaper = card_right - card_left < 620.0f * scale;
        const float wall_h = (compact_wallpaper ? 132.0f : 88.0f) * scale;
        l.wallpaper_card = D2D1::RectF(card_left, y, card_right, y + wall_h);
        const float preview_w = 96.0f * scale;
        const float preview_h = 56.0f * scale;
        const float preview_top = compact_wallpaper ? y + 12.0f * scale
                                                     : y + (wall_h - preview_h) * 0.5f;
        l.wallpaper_preview = D2D1::RectF(card_left + 16.0f * scale, preview_top,
                                          card_left + 16.0f * scale + preview_w,
                                          preview_top + preview_h);
        const float btn_h = 32.0f * scale;
        const float btn_y = compact_wallpaper ? y + 88.0f * scale
                                               : y + (wall_h - btn_h) * 0.5f;
        const float clear_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::Clear));
        const float choose_w = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::ChooseImage));
        l.wallpaper_clear = D2D1::RectF(card_right - 16.0f * scale - clear_w, btn_y,
                                        card_right - 16.0f * scale, btn_y + btn_h);
        l.wallpaper_choose = D2D1::RectF(l.wallpaper_clear.left - 8.0f * scale - choose_w, btn_y,
                                         l.wallpaper_clear.left - 8.0f * scale, btn_y + btn_h);
        y += wall_h + 12.0f * scale;
        l.hidden_files_row = D2D1::RectF(card_left, y, card_right, y + 56.0f * scale);
        y += 68.0f * scale;
        l.pinned_names_row = D2D1::RectF(card_left, y, card_right, y + 56.0f * scale);
        y += 76.0f * scale;

        y += 22.0f * scale;
        y += 8.0f * scale;
        const float startup_h = 56.0f * scale;
        for (int i = 0; i < 3; ++i) {
            l.startup_row[i] = D2D1::RectF(card_left, y + static_cast<float>(i) * startup_h,
                                           card_right, y + static_cast<float>(i + 1) * startup_h);
        }
        y += startup_h * 3 + 24.0f * scale;
    } else if (vm.settings_page == 1) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
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
    } else if (vm.settings_page == 2) {
        int counts[5] = {};
        for (const auto& row : vm.settings_items) {
            if (row.group >= 0 && row.group < 5) ++counts[row.group];
        }
        for (int g = 0; g < 5; ++g) {
            y += 56.0f * scale + 36.0f * scale + counts[g] * 36.0f * scale
               + 8.0f * scale + 12.0f * scale;
        }
        y += 48.0f * scale;
    } else if (vm.settings_page == 3) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        l.about_card = D2D1::RectF(card_left, y, card_right, y + 104.0f * scale);
        y += 116.0f * scale;

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
        const float update_h = 174.0f * scale +
            (stack_updates ? 40.0f * scale : 0.0f);
        l.update_card = D2D1::RectF(card_left, y, card_right, y + update_h);
        const float check_y = y + update_h - (stack_updates ? 88.0f : 48.0f) * scale;
        l.update_action[0] = D2D1::RectF(card_left + 16.0f * scale,
                                         check_y,
                                         card_left + 16.0f * scale + check_w,
                                         check_y + 32.0f * scale);
        const float download_x = stack_updates ? l.update_action[0].left : l.update_action[0].right + gap;
        const float download_y = check_y + (stack_updates ? 40.0f * scale : 0.0f);
        l.update_action[1] = D2D1::RectF(download_x, download_y,
                                         download_x + download_w, download_y + 32.0f * scale);
        y += update_h + 24.0f * scale;
    } else if (vm.settings_page == 4) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        const float gap = 8.0f * scale;
        const float inner = 16.0f * scale;
        const float btn_h = 32.0f * scale;
        y += 28.0f * scale;
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
    return ContainsPt(sb.task, x, y) ? HitTestResult::StatusBarTask
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

bool TitleBarCompact(float window_w, float scale, size_t tab_count) {
    return window_w < 900.0f * scale || tab_count >= 4;
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

    const float stem_budget = max_w - tail_w;
    size_t lo = 0, hi = stem.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(stem.substr(0, mid)) <= stem_budget) lo = mid;
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

NameTrail LayoutNameTrail(float name_x, float text_y, float text_h,
                                 float col_right, float cell_top, float cell_bottom,
                                 float scale, const std::wstring& name, int tag_n,
                                 float badge_w,
                                 bool show_star, bool show_new_tab, bool show_more,
                                 Compositor* compositor, IDWriteFactory2* factory,
                                 IDWriteTextFormat* fmt) {
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
    if (show_more) {
        t.more = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.more.left - gap;
    }
    if (show_star) {
        t.star = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.star.left - gap;
    }
    if (show_new_tab) {
        t.new_tab = D2D1::RectF(dock - btn, by, dock, by + btn);
        dock = t.new_tab.left - gap;
    }
    t.line_w = std::max(0.0f, dock - name_x);

    // Reserve the compact (overlapped) cluster so a long name still
    // compresses first. Spread only if leftover after the fitted name fits.
    badge_w = std::max(0.0f, badge_w);
    const float badge_gap = badge_w > 0.0f ? gap : 0.0f;
    const float tags_w = OverlapTagsWidth(t.tag_n, diameter);
    const float tags_gap = t.tag_n > 0 ? gap : 0.0f;
    const float budget = std::max(0.0f,
        dock - name_x - badge_gap - badge_w - tags_gap - tags_w);
    const std::wstring fitted = FitFileName(compositor, factory, fmt, name, budget);
    t.name_w = std::min(budget, MeasureLayoutText(compositor, factory, fmt, fitted));
    float trail_x = name_x + t.name_w;
    if (badge_w > 0.0f) {
        trail_x += badge_gap;
        const float badge_h = std::min(20.0f * scale, text_h);
        const float badge_y = text_y + (text_h - badge_h) * 0.5f;
        t.badge = D2D1::RectF(trail_x, badge_y, trail_x + badge_w, badge_y + badge_h);
        trail_x += badge_w;
    }
    if (t.tag_n > 0) {
        t.tag_x0 = trail_x + gap;
        const float leftover = std::max(0.0f, dock - t.tag_x0);
        t.tag_step = TagStepForLeftover(t.tag_n, diameter, gap, leftover);
    }
    return t;
}
struct ScrollbarMetrics {
    float thumbY, thumbH;
    bool valid;
};

ScrollbarMetrics ComputeScrollbar(float viewH, float totalH, float scrollY, float rowH) {
    ScrollbarMetrics m{};
    if (totalH <= viewH || viewH <= 0) return m;
    m.valid = true;
    m.thumbH = std::max(rowH, viewH * (viewH / totalH));
    m.thumbY = (scrollY / (totalH - viewH)) * (viewH - m.thumbH);
    return m;
}
} // namespace

} // namespace pulse::ui
