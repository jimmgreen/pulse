// ui_renderer.cpp — Full-window Fluent rendering.
#include "ui_renderer.h"
#include "tab_shape.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace pulse::ui {

static float MeasureTextWidth(IDWriteFactory3* factory, IDWriteTextFormat* fmt,
                              const std::wstring& text);
static std::wstring FitFileName(IDWriteFactory3* factory, IDWriteTextFormat* fmt,
                                const std::wstring& name, float max_w);
static float OverlapTagsWidth(int n, float diameter);
static float SpreadTagsWidth(int n, float diameter, float gap);
static float TagStepForLeftover(int n, float diameter, float spread_gap, float leftover);

namespace {
    // Transparent fill: button resting state (hover fill drawn on interaction).
    constexpr D2D1_COLOR_F kTransparent{0.0f, 0.0f, 0.0f, 0.0f};
    constexpr float kTabMinW = 72.0f;
    constexpr float kTabMaxW = 240.0f;
    constexpr float kTabPinnedW = 36.0f; // Chrome pinned tab: icon-only square
    constexpr float kTabCloseAlwaysW = 96.0f;
    constexpr float kTabClosePadDip = 10.0f;
    constexpr float kTabCloseSizeDip = 16.0f;
    constexpr float kCommandIconButtonDip = 32.0f;
    constexpr float kCommandIconStepDip = 34.0f;
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
        if (isDir) return L"\u6587\u4EF6\u5939"; // 文件夹
        const size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"\u6587\u4EF6"; // 文件
        std::wstring ext = name.substr(dot + 1);
        for (auto& c : ext) c = std::towlower(c);
        if (ext == L"txt" || ext == L"md" || ext == L"log") return L"\u6587\u672C\u6587\u6863";
        if (ext == L"png" || ext == L"jpg" || ext == L"jpeg" || ext == L"gif" || ext == L"bmp" || ext == L"webp") return L"\u56FE\u50CF";
        if (ext == L"mp4" || ext == L"mkv" || ext == L"avi" || ext == L"mov") return L"\u89C6\u9891";
        if (ext == L"mp3" || ext == L"wav" || ext == L"flac") return L"\u97F3\u9891";
        if (ext == L"zip" || ext == L"rar" || ext == L"7z") return L"\u538B\u7F29\u6587\u4EF6";
        if (ext == L"exe" || ext == L"msi") return L"\u5E94\u7528\u7A0B\u5E8F";
        if (ext == L"dwg") return L"AutoCAD \u56FE\u7EB8";
        return L"\u6587\u4EF6";
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
        entry.name = penetrated ? fs::StripLnkSuffix(source.name) : source.name;
        entry.size_text = penetrated
            ? (source.link_target_is_dir ? L"" : pulse::format::ByteSize(source.link_target_size, true))
            : (source.is_dir ? L"" : pulse::format::ByteSize(source.size, true));
        entry.date_text = pulse::format::LocalFileTime(source.mtime);
        entry.type_text = FormatListType(entry.name,
            penetrated ? source.link_target_is_dir : source.is_dir);
        entry.path = !source.full_path.empty() ? source.full_path
                     : (fs::IsVirtualPath(vm.path) ? L"" : JoinDirName(vm.path, source.name));
        entry.attrs = source.attrs;
        entry.size_value = source.size;
        entry.modified_value = (static_cast<uint64_t>(source.mtime.dwHighDateTime) << 32) |
                               source.mtime.dwLowDateTime;
        entry.is_dir = source.is_dir;
        entry.is_reparse = source.is_reparse;
        entry.cloud_recall = source.cloud_recall;
        entry.starred = vm.tag_catalog && !entry.path.empty() &&
            vm.tag_catalog->IsStarred(entry.path);
        constexpr size_t kMaxCachedRows = 512;
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
        float headerH = 26.0f;
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
        m.headerH = 26.0f * scale;
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
        m.actionW = 44.0f * scale;
        return m;
    }

    float SidebarItemHeight(const SidebarItem& item, const SidebarMetrics& m) {
        if (item.is_drive) return m.driveH;
        if (item.is_tag) return m.tagH;
        return m.itemH;
    }

    int TrayTotalCount(const WindowViewModel& vm) { return vm.tray_deck.total_count; }

    std::vector<int> TrayCardPaintOrder(const TrayDeckView& deck) {
        std::vector<int> order(deck.cards.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i)
            order[static_cast<size_t>(i)] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const TrayCardView& ca = deck.cards[static_cast<size_t>(a)];
            const TrayCardView& cb = deck.cards[static_cast<size_t>(b)];
            if (ca.ghost != cb.ghost) return ca.ghost && !cb.ghost;
            const bool ha = a == deck.hovered, hb = b == deck.hovered;
            if (ha != hb) return hb;
            return std::abs(ca.slot) > std::abs(cb.slot);
        });
        return order;
    }

    float ExpandedTrayHeight(const WindowViewModel& vm, const SidebarMetrics& m) {
        const float base = m.trayInner * 2.0f + m.trayHeaderH;
        if (vm.tray_deck.cards.empty())
            return std::max(base + m.trayHelperH + 8.0f * m.scale, 112.0f * m.scale);
        // Fan deck: header + icon lane + footer (totals + clear action).
        return base + 4.0f * m.scale + m.trayDeckH + 18.0f * m.scale;
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

        float y = sb.top + m.pad;
        int run = 0;
        for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
            const auto& group = vm.sidebar[g];
            if (group.items.empty() && !group.add_action) continue;
            if (y + m.headerH > contentBottom) break;
            SidebarSlot header;
            header.kind = SidebarSlot::Header;
            header.rc = D2D1::RectF(innerL, y, innerR, y + m.headerH);
            header.group = g;
            out.push_back(header);
            y += m.headerH + 4.0f * scale;
            if (group.collapsed) continue;
            for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
                const auto& item = group.items[i];
                if (g == vm.tag_drag_group && i == vm.tag_drag_item) continue; // drawn floating
                const float h = SidebarItemHeight(item, m);
                if (y + h > contentBottom) break;
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
                        minTop = std::min(minTop, slot.rc.top);
                        maxBottom = std::max(maxBottom, slot.rc.bottom);
                    }
                }
                float cy = vm.tag_drag_y;
                if (minTop <= maxBottom)
                    cy = std::clamp(cy, minTop + m.tagH * 0.5f, maxBottom - m.tagH * 0.5f);
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
            clear.rc = D2D1::RectF(innerR - inset - 44.0f * scale,
                                   tray.rc.bottom - m.trayInner - 22.0f * scale,
                                   tray.rc.right - inset,
                                   tray.rc.bottom - m.trayInner);
            out.push_back(clear);
        }
    }

    bool RectContains(const D2D1_RECT_F& rc, float x, float y) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
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

    bool PathIsSelfOrChild(const std::wstring& root, const std::wstring& path) {
        if (root.empty() || path.empty()) return false;
        if (_wcsicmp(root.c_str(), path.c_str()) == 0) return true;
        std::wstring prefix = root;
        if (prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
        return path.size() >= prefix.size() &&
            _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    // ---------------------------------------------------------------------
    // Tray fan deck geometry. Shared by DrawTrayDeck and HitTest so the two
    // can never disagree about where an icon is.
    // ---------------------------------------------------------------------
    struct TrayFanGeom {
        float icon = 0.0f;     // icon edge, DIPs
        float cx = 0.0f;       // fan center
        float cy = 0.0f;       // resting center y of slot 0
        float step_x = 0.0f;   // horizontal spacing per slot
        float tilt = 0.0f;     // degrees per slot
        float droop = 0.0f;    // edge icons sit slot^2 * droop lower
    };

    TrayFanGeom TrayFanGeometry(const D2D1_RECT_F& panel, int live_count, float open,
                                float scale) {
        TrayFanGeom g;
        const bool single = live_count <= 1;
        g.icon = (single ? 56.0f : 48.0f) * scale;
        const float deckTop = panel.top + 4.0f * scale + 22.0f * scale + 4.0f * scale;
        // The footer row (totals + clear) reserves the bottom 18px.
        const float deckBottom = panel.bottom - 8.0f * scale - 18.0f * scale - 4.0f * scale;
        g.cx = (panel.left + panel.right) * 0.5f;
        g.cy = deckTop + (deckBottom - deckTop) * 0.5f + (single ? -18.0f * scale : 0.0f);
        const float avail = (panel.right - panel.left) - 24.0f * scale - g.icon;
        g.step_x = live_count > 1
            ? std::min(g.icon * 0.72f, avail / static_cast<float>(live_count - 1)) : 0.0f;
        g.tilt = 8.5f * (1.0f + 0.30f * std::clamp(open, 0.0f, 1.0f));
        g.droop = 3.0f * scale;
        return g;
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
        TrayCardPose p;
        p.center = D2D1::Point2F(
            g.cx + card.slot * g.step_x,
            g.cy + card.slot * card.slot * g.droop - hover * 10.0f * scale
                 + (1.0f - opacity) * 14.0f * scale); // ghosts sink while fading
        p.angle = card.slot * g.tilt * (1.0f - 0.9f * hover); // straighten on hover
        p.scale_f = (0.55f + 0.45f * appear) * (1.0f + 0.10f * hover);
        return p;
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

    struct DetailsSectionDef { int id; const wchar_t* label; };
    constexpr DetailsSectionDef kDetailsSections[] = {
        { 0, L"基本信息" }, { 1, L"属性" }, { 2, L"标签" },
        { 3, L"安全" }, { 4, L"其他" },
    };

    // Button row labels, shared between layout (width measurement) and drawing.
    constexpr const wchar_t* kDetailsButtonLabels[4] = {
        L"打开", L"在新标签打开", L"复制路径", L"更多"
    };

    void LayoutDetailsPanel(const D2D1_RECT_F& panel, float scale,
                            const DetailsPanelView& d, IDWriteFactory3* dwrite,
                            IDWriteTextFormat* small_fmt, float preview_h,
                            DetailsHitRects& out) {
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
                    bw[i] = MeasureTextWidth(dwrite, small_fmt, kDetailsButtonLabels[i])
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
                case 1: // 属性: 只读/隐藏 checkbox rows (24 DIP) + 高级… on row two
                    out.attr_readonly = D2D1::RectF(x, y, x + w, y + 24.0f * s);
                    out.attr_hidden = D2D1::RectF(x, y + 24.0f * s, x + w,
                                                  y + 48.0f * s);
                    out.attr_advanced = D2D1::RectF(x + w - 64.0f * s, y + 24.0f * s,
                                                    x + w, y + 48.0f * s);
                    y += 48.0f * s + 8.0f * s;
                    break;
                case 2: { // 标签: 添加标签 row + preset chip grid (wraps)
                    out.tag_add = D2D1::RectF(x, y, x + w, y + 26.0f * s);
                    y += 26.0f * s + 6.0f * s;
                    float cx = x;
                    const float chipH = 22.0f * s;
                    bool any = false;
                    for (const auto& chip : d.preset_tags) {
                        const float tw = std::min(72.0f * s,
                            MeasureTextWidth(dwrite, small_fmt, chip.name));
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
}

MainRenderer::MainRenderer() = default;

void MainRenderer::SetCompositor(Compositor* comp) {
    empty_state_svg_.reset();
    no_selection_svg_.reset();
    empty_state_svg_dc_.reset();
    compositor_ = comp;
    material_.SetCompositor(comp);
    painter_.SetCompositor(comp);
    if (!comp) {
        icon_cache_.Reset();
        thumbnail_cache_.Reset();
        preview_handler_.Reset();
        preview_mono_format_.reset();
    }
    else { icon_cache_.SetDeviceContext(comp->Dc()); thumbnail_cache_.SetDeviceContext(comp->Dc()); }
}

bool MainRenderer::EnsureEmptyStateSvg() {
    if (empty_state_svg_.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;

    if (FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_))))
        return false;

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_EMPTY_FOLDER_SVG), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byteCount == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byteCount);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(512.0f, 360.0f), &empty_state_svg_))) {
        empty_state_svg_.reset();
        return false;
    }

    ComPtr<ID2D1SvgElement> background;
    if (SUCCEEDED(empty_state_svg_->FindElementById(L"background", &background)) &&
        background.get()) {
        background->SetAttributeValue(L"display", D2D1_SVG_DISPLAY_NONE);
    }
    return true;
}

bool MainRenderer::DrawEmptyStateSvg(const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureEmptyStateSvg()) return false;
    ComPtr<ID2D1SvgElement> root;
    empty_state_svg_->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float availableWidth = std::max(0.0f, bounds.right - bounds.left);
    const float artWidth = std::min(availableWidth, 280.0f * scale_);
    if (artWidth <= 1.0f) return false;
    const float artHeight = artWidth * 360.0f / 512.0f;
    const float left = (bounds.left + bounds.right - artWidth) * 0.5f;
    const float top = (bounds.top + bounds.bottom - artHeight) * 0.5f;

    empty_state_svg_->SetViewportSize(D2D1::SizeF(512.0f, 360.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(artWidth / 512.0f, artHeight / 360.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(empty_state_svg_.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

bool MainRenderer::EnsureNoSelectionSvg() {
    if (no_selection_svg_.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;

    if (!empty_state_svg_dc_.get() &&
        FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_))))
        return false;

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_NO_SELECTION_SVG), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byteCount == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byteCount);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(320.0f, 240.0f), &no_selection_svg_))) {
        no_selection_svg_.reset();
        return false;
    }
    return true;
}

bool MainRenderer::DrawNoSelectionSvg(const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureNoSelectionSvg()) return false;
    ComPtr<ID2D1SvgElement> root;
    no_selection_svg_->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float availableWidth = std::max(0.0f, bounds.right - bounds.left);
    const float artWidth = std::min(availableWidth, 180.0f * scale_);
    if (artWidth <= 1.0f) return false;
    const float artHeight = artWidth * 240.0f / 320.0f;
    const float left = (bounds.left + bounds.right - artWidth) * 0.5f;
    const float top = (bounds.top + bounds.bottom - artHeight) * 0.5f;

    no_selection_svg_->SetViewportSize(D2D1::SizeF(320.0f, 240.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(artWidth / 320.0f, artHeight / 240.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(no_selection_svg_.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

void MainRenderer::SetIconNotifyWindow(HWND hwnd) {
    notify_hwnd_ = hwnd;
    icon_cache_.SetNotifyWindow(hwnd);
    thumbnail_cache_.SetNotifyWindow(hwnd);
    preview_handler_.SetNotifyWindow(hwnd);
}

void MainRenderer::SetScale(float scale) {
    if (scale_ != scale) preview_mono_format_.reset();
    scale_ = scale;
    title_bar_height_ = kTitleBarHeight * scale;
    toolbar_height_ = 44.0f * scale;
    status_height_ = 28.0f * scale;
    sidebar_width_ = 224.0f * scale;
    pane_header_height_ = 40.0f * scale;
    column_header_height_ = 32.0f * scale;
    row_height_ = row_height_dip_ * scale;
    margin_ = 4.0f * scale;
    control_gap_ = 4.0f * scale;
    painter_.SetScale(scale);
    icon_cache_.SetScale(scale);
}

float MainRenderer::EffectiveSidebarWidth(float window_width) const {
    return window_width < 900.0f * scale_ ? 48.0f * scale_ : sidebar_width_;
}

D2D1_RECT_F MainRenderer::ContentRect(float w, float h) const {
    float left = EffectiveSidebarWidth(w) + margin_;
    float top = title_bar_height_ + toolbar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    return D2D1::RectF(left, top, w - margin_ - DetailsPanelWidth(w), bottom);
}

D2D1_RECT_F MainRenderer::DetailsPanelRect(float w, float h) const {
    if (DetailsPanelWidth(w) <= 0.0f) return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    const float pw = details_width_ * scale_;
    const float top = title_bar_height_ + toolbar_height_ + margin_;
    const float bottom = h - status_height_ - margin_;
    return D2D1::RectF(w - margin_ - pw, top, w - margin_, bottom);
}

D2D1_RECT_F MainRenderer::PaneListRect(const D2D1_RECT_F& pane_bounds, float extra_top,
                                       ViewMode mode) const {
    D2D1_RECT_F list = pane_bounds;
    list.top += pane_header_height_ + extra_top +
                (ShowsColumnHeader(mode) ? column_header_height_ : 0.0f);
    if (list.top > list.bottom) list.top = list.bottom;
    return list;
}

D2D1_RECT_F MainRenderer::FilterBoxRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    const float w = pane_bounds.right - pane_bounds.left;
    const float expandedW = std::max(32.0f * scale_,
        std::min(220.0f * scale_, w * 0.34f));
    const float t = std::clamp(expand, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t);
    const float filterW = 32.0f * scale_ +
        (expandedW - 32.0f * scale_) * eased;
    const float right = pane_bounds.right - 8.0f * scale_;
    return D2D1::RectF(right - filterW,
                       pane_bounds.top + 4 * scale_,
                       right,
                       pane_bounds.top + pane_header_height_ - 4 * scale_);
}

D2D1_RECT_F MainRenderer::PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                              float filter_expand) const {
    const D2D1_RECT_F filter = FilterBoxRect(pane_bounds, filter_expand);
    const float right = filter.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_;
    return D2D1::RectF(right - kCommandIconButtonDip * scale_, filter.top, right, filter.bottom);
}

D2D1_RECT_F MainRenderer::PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                               float filter_expand) const {
    const D2D1_RECT_F view = PaneViewButtonRect(pane_bounds, filter_expand);
    return D2D1::RectF(view.left - kCommandIconStepDip * scale_, view.top,
                       view.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_,
                       view.bottom);
}

D2D1_RECT_F MainRenderer::FilterEditRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    D2D1_RECT_F rc = FilterBoxRect(pane_bounds, expand);
    rc.left += 34.0f * scale_;
    rc.right -= 10.0f * scale_;
    if (rc.right < rc.left + 24.0f * scale_) rc.right = rc.left + 24.0f * scale_;
    return rc;
}

MainRenderer::DetailsColumnLayout MainRenderer::DetailsColumns(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 3>& dividers) const {
    DetailsColumnLayout out;
    out.left = pane_bounds.left + margin_;
    out.right = std::max(out.left, pane_bounds.right - margin_ * 3.0f);
    const float total = out.right - out.left;
    if (total <= 0.0f) return out;

    std::array<float, 4> minimums{
        80.0f * scale_, 92.0f * scale_, 64.0f * scale_, 72.0f * scale_ };
    const float minimumTotal = minimums[0] + minimums[1] +
                               minimums[2] + minimums[3];
    if (minimumTotal > total) {
        const float shrink = total / minimumTotal;
        for (float& width : minimums) width *= shrink;
    }

    const bool customized = dividers[0] > 0.0f &&
        dividers[1] > dividers[0] && dividers[2] > dividers[1] &&
        dividers[2] < 1.0f;
    std::array<float, 3> desired{};
    if (customized) {
        for (size_t i = 0; i < desired.size(); ++i)
            desired[i] = dividers[i] * total;
    } else {
        desired[0] = total - (130.0f + 90.0f + 90.0f) * scale_;
        desired[1] = total - (90.0f + 90.0f) * scale_;
        desired[2] = total - 90.0f * scale_;
    }

    const float edge0 = std::clamp(
        desired[0], minimums[0],
        total - minimums[1] - minimums[2] - minimums[3]);
    const float edge1 = std::clamp(
        desired[1], edge0 + minimums[1],
        total - minimums[2] - minimums[3]);
    const float edge2 = std::clamp(
        desired[2], edge1 + minimums[2], total - minimums[3]);
    out.widths = {edge0, edge1 - edge0, edge2 - edge1, total - edge2};
    return out;
}

std::array<float, 3> MainRenderer::ResizeDetailsColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 3>& dividers,
    int divider_index, float cursor_x) const {
    DetailsColumnLayout layout = DetailsColumns(pane_bounds, dividers);
    const float total = layout.right - layout.left;
    if (divider_index < 0 || divider_index >= 3 || total <= 0.0f)
        return dividers;

    const float adjacentTotal = layout.widths[static_cast<size_t>(divider_index)] +
        layout.widths[static_cast<size_t>(divider_index + 1)];
    const float outerLeft = divider_index == 0
        ? layout.left : layout.DividerX(divider_index - 1);
    const float minScale = std::min(1.0f, total /
        ((80.0f + 92.0f + 64.0f + 72.0f) * scale_));
    static constexpr float minimumDip[4] = {80.0f, 92.0f, 64.0f, 72.0f};
    const float leftMinimum = minimumDip[divider_index] * scale_ * minScale;
    const float rightMinimum = minimumDip[divider_index + 1] * scale_ * minScale;
    const float divider = std::clamp(
        cursor_x, outerLeft + leftMinimum,
        outerLeft + adjacentTotal - rightMinimum);

    std::array<float, 3> result{};
    for (int i = 0; i < 3; ++i)
        result[static_cast<size_t>(i)] = layout.DividerX(i);
    result[static_cast<size_t>(divider_index)] = divider;
    for (float& edge : result) edge = (edge - layout.left) / total;
    return result;
}

D2D1_RECT_F MainRenderer::NameCellRect(const D2D1_RECT_F& pane_bounds, int view_row, float scroll_y,
                                       float extra_top, ViewMode mode, float scroll_x,
                                       size_t item_count,
                                       const std::array<float, 3>& column_dividers) const {
    const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, mode);
    if (mode != ViewMode::Details) {
        ViewLayout layout(mode, list, item_count, scroll_x, scroll_y, scale_, row_height_dip_);
        return layout.NameRect(view_row);
    }
    const float list_x = list.left;
    const float list_y = list.top;
    const float name_w = DetailsColumns(list, column_dividers).widths[0];
    const float icon_size = 16.0f * scale_;
    const float row_y = list_y + static_cast<float>(view_row) * row_height_ - scroll_y;
    const float name_x = list_x + margin_ + icon_size + margin_ + 4.0f * scale_;
    const float name_avail = name_w - icon_size - margin_ * 3;
    const float inset = 1.0f * scale_;
    return D2D1::RectF(name_x, row_y + inset, name_x + std::max(40.0f * scale_, name_avail),
                       row_y + row_height_ - inset);
}

bool MainRenderer::PointInItemName(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                   int source_index, float x, float y) const {
    if (!compositor_ || !compositor_->DwriteFactory() || source_index < 0) return false;
    const int view_index = vm.ViewIndex(source_index);
    if (view_index < 0) return false;

    const ListEntryView& entry = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
    if (entry.name.empty()) return false;
    D2D1_RECT_F name = NameCellRect(
        pane_bounds, view_index, vm.scroll_y,
        vm.banner_message.empty() ? 0.0f : 36.0f * scale_,
        vm.view_mode, vm.scroll_x, vm.EntryCount(), vm.details_column_dividers);
    const bool icon_grid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                           vm.view_mode == ViewMode::LargeIcons ||
                           vm.view_mode == ViewMode::MediumIcons;
    float available = std::max(0.0f, name.right - name.left);
    const std::vector<D2D1_COLOR_F>* tag_dots = nullptr;
    const std::vector<int>* tag_indices = vm.tag_catalog
        ? vm.tag_catalog->TagIndicesForPath(entry.path) : nullptr;
    if (!tag_indices && vm.tag_dots) {
        const auto found = vm.tag_dots->find(source_index);
        if (found != vm.tag_dots->end()) tag_dots = &found->second;
    }
    const size_t tag_count = tag_indices ? tag_indices->size() : (tag_dots ? tag_dots->size() : 0);
    const int visible_dots = static_cast<int>(std::min<size_t>(3, tag_count));
    const float diameter = 8.0f * scale_;
    const float tag_gap = 4.0f * scale_;
    const float overlap_w = OverlapTagsWidth(visible_dots, diameter);
    if (overlap_w > 0.0f)
        available = std::max(24.0f * scale_, available - overlap_w - tag_gap);

    const std::wstring fitted = FitFileName(
        compositor_->DwriteFactory(), compositor_->TextFormat(), entry.name, available);
    const float text_width = MeasureTextWidth(
        compositor_->DwriteFactory(), compositor_->TextFormat(), fitted);
    float text_left = name.left;
    if (icon_grid) {
        const float leftover = std::max(0.0f, (name.right - name.left) - text_width - tag_gap);
        const float dots_width = visible_dots > 0 &&
            SpreadTagsWidth(visible_dots, diameter, tag_gap) <= leftover + 0.5f
            ? SpreadTagsWidth(visible_dots, diameter, tag_gap)
            : overlap_w;
        const float group_width = text_width + (dots_width > 0.0f ? tag_gap + dots_width : 0.0f);
        text_left += std::max(0.0f, (name.right - name.left - group_width) * 0.5f);
    }
    const float slop = 2.0f * scale_;
    return x >= text_left - slop && x < text_left + text_width + slop &&
           y >= name.top && y < name.bottom;
}

D2D1_RECT_F MainRenderer::SidebarRect(float w, float h) const {
    float top = title_bar_height_ + toolbar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    return D2D1::RectF(0.0f, top, EffectiveSidebarWidth(w), bottom);
}

D2D1_RECT_F MainRenderer::StagingTrayRect(const WindowViewModel& vm, float w, float h) const {
    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, SidebarRect(w, h), scale_, slots);
    for (const auto& slot : slots) {
        if (slot.kind == SidebarSlot::TrayPanel) return slot.rc;
    }
    return D2D1::RectF();
}

D2D1_RECT_F MainRenderer::TitleBarRect(float w) const {
    return D2D1::RectF(0, 0, w, title_bar_height_);
}

D2D1_RECT_F MainRenderer::ToolbarRect(float w) const {
    return D2D1::RectF(0, title_bar_height_, w, title_bar_height_ + toolbar_height_);
}

D2D1_RECT_F MainRenderer::AddressBarRect(float w) const {
    const bool compact = w < 900.0f * scale_;
    const float nav_count = compact ? 3.0f : 4.0f;
    float x = margin_ + 34.0f * scale_ * nav_count + margin_;
    float right = w - margin_;
    const float actions = compact ? 112.0f : 380.0f;
    float addrW = std::max(104.0f * scale_, right - x - actions * scale_);
    return D2D1::RectF(x, title_bar_height_ + 4 * scale_,
                       x + addrW, title_bar_height_ + toolbar_height_ - 4 * scale_);
}

std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path) {
    std::vector<BreadcrumbSegment> out;
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p.empty()) return out;

    std::wstring prefix; // full path of the segments emitted so far
    size_t i = 0;
    if (p.size() >= 2 && p[1] == L':') {
        // Drive root, e.g. "C:\": single segment with the drive icon text.
        prefix = p.substr(0, 2);
        BreadcrumbSegment seg;
        seg.text = prefix;
        seg.path = prefix + L"\\";
        out.push_back(seg);
        i = 2;
        while (i < p.size() && p[i] == L'\\') ++i;
        prefix += L"\\";
    } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        // UNC: collapse \\server\share into one segment.
        auto s3 = p.find(L'\\', 2);           // after server
        auto s4 = s3 == std::wstring::npos ? std::wstring::npos
                                           : p.find(L'\\', s3 + 1); // after share
        std::wstring root = (s4 == std::wstring::npos) ? p : p.substr(0, s4);
        BreadcrumbSegment seg;
        seg.text = root;
        seg.path = root;
        out.push_back(seg);
        prefix = root;
        i = (s4 == std::wstring::npos) ? p.size() : s4 + 1;
    }
    while (i <= p.size() && i < p.size()) {
        auto sep = p.find(L'\\', i);
        std::wstring part = (sep == std::wstring::npos) ? p.substr(i)
                                                        : p.substr(i, sep - i);
        if (!part.empty()) {
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
            prefix += part;
            BreadcrumbSegment seg;
            seg.text = part;
            seg.path = prefix;
            out.push_back(seg);
        }
        if (sep == std::wstring::npos) break;
        i = sep + 1;
    }
    return out;
}

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out) const {
    out.clear();
    auto segments = SplitBreadcrumb(vm.path);
    if (segments.empty()) return;
    D2D1_RECT_F addr = AddressBarRect(w);
    const float segPad = 8.0f * scale_;
    const float chevronW = 14.0f * scale_;
    const float hint = painter_.OmnibarHintReservePx();
    const float avail = std::max(0.0f, addr.right - addr.left - 2 * margin_ - hint);

    IDWriteFactory3* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->AddressFormat() : nullptr;

    struct Measured { float w; };
    std::vector<float> widths(segments.size(), 0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        widths[i] = MeasureTextWidth(dwrite, fmt, segments[i].text) + segPad * 2;
        total += widths[i] + (i ? chevronW : 0.0f);
    }
    // Collapse leading segments until the rest fits (last segment always kept).
    size_t first = 0;
    while (total > avail && first + 1 < segments.size()) {
        total -= widths[first] + chevronW;
        ++first;
    }

    float x = addr.left + margin_;
    for (size_t i = first; i < segments.size(); ++i) {
        if (i > first) x += chevronW;
        BreadcrumbPlaced p;
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + widths[i], addr.bottom - 2 * scale_);
        p.text = segments[i].text;
        p.path = segments[i].path;
        out.push_back(p);
        x += widths[i];
    }
}

static void FillRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float x, float y, float w, float h) {
    dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), br);
}

static void FillRoundedRect(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
    float x, float y, float w, float h, float r) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r);
    dc->FillRoundedRectangle(&rr, br);
}

static void DrawTextRect(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
    const std::wstring& text, float x, float y, float w, float h,
    D2D1_DRAW_TEXT_OPTIONS opts = D2D1_DRAW_TEXT_OPTIONS_CLIP) {
    D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    dc->DrawText(text.c_str(), (UINT32)text.size(), fmt, &rc, br, opts, DWRITE_MEASURING_MODE_NATURAL);
}

static float MeasureTextWidth(IDWriteFactory3* factory, IDWriteTextFormat* fmt, const std::wstring& text) {
    if (!factory || !fmt || text.empty()) return 0.0f;
    const float fallback = fmt->GetFontSize() * static_cast<float>(text.size());
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt,
        10000.0f, 100.0f, &layout)) || !layout.get()) return fallback;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return fallback;
    DWRITE_OVERHANG_METRICS om{};
    layout->GetOverhangMetrics(&om);
    return m.widthIncludingTrailingWhitespace + std::max(0.0f, om.right) + 1.0f;
}

static void DrawTabTitle(ID2D1DeviceContext* dc, IDWriteFactory3* factory,
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

static void MakeBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c, ComPtr<ID2D1SolidColorBrush>& br) {
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

struct SettingsLayout {
    D2D1_RECT_F body{};
    D2D1_RECT_F nav{};
    D2D1_RECT_F content{};
    D2D1_RECT_F nav_row[2]{};
    D2D1_RECT_F effect_card{};
    D2D1_RECT_F effect_row[kWindowEffectCount]{};
    D2D1_RECT_F density_card{};
    D2D1_RECT_F density_row[3]{};
    D2D1_RECT_F wallpaper_card{};
    D2D1_RECT_F wallpaper_preview{};
    D2D1_RECT_F wallpaper_choose{};
    D2D1_RECT_F wallpaper_clear{};
    D2D1_RECT_F startup_row[2]{};
    float content_origin = 0.0f;
    float content_h = 0.0f;
};

static std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

SettingsLayout MakeSettingsLayout(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                  float scale, float title_h, float status_h) {
    SettingsLayout l;
    l.body = D2D1::RectF(rect.left, title_h, rect.right, rect.bottom - status_h);
    l.nav = D2D1::RectF(l.body.left, l.body.top, l.body.left + kSettingsNavW * scale, l.body.bottom);
    l.content = D2D1::RectF(l.nav.right, l.body.top, l.body.right, l.body.bottom);
    const float row_h = 40.0f * scale;
    const float nav_pad = 12.0f * scale;
    for (int i = 0; i < 2; ++i) {
        const float y = l.nav.top + nav_pad + 8.0f * scale + i * (row_h + 4.0f * scale);
        l.nav_row[i] = D2D1::RectF(l.nav.left + 8.0f * scale, y,
                                   l.nav.right - 8.0f * scale, y + row_h);
    }
    const float pad = 20.0f * scale;
    l.content_origin = l.content.top - vm.settings_scroll;
    float y = l.content_origin + pad;
    y += 36.0f * scale;
    y += 8.0f * scale;
    if (vm.settings_page == 0) {
        y += 22.0f * scale;
        y += 8.0f * scale;
        const float radio_h = 36.0f * scale;
        const float effect_header = 56.0f * scale;
        const float effect_h = effect_header + kWindowEffectCount * radio_h + 8.0f * scale;
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
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

        const float wall_h = 88.0f * scale;
        l.wallpaper_card = D2D1::RectF(card_left, y, card_right, y + wall_h);
        const float preview_w = 96.0f * scale;
        const float preview_h = 56.0f * scale;
        l.wallpaper_preview = D2D1::RectF(card_left + 16.0f * scale,
                                          y + (wall_h - preview_h) * 0.5f,
                                          card_left + 16.0f * scale + preview_w,
                                          y + (wall_h + preview_h) * 0.5f);
        const float btn_w = 88.0f * scale;
        const float btn_h = 32.0f * scale;
        const float btn_y = y + (wall_h - btn_h) * 0.5f;
        l.wallpaper_clear = D2D1::RectF(card_right - 16.0f * scale - btn_w, btn_y,
                                        card_right - 16.0f * scale, btn_y + btn_h);
        l.wallpaper_choose = D2D1::RectF(l.wallpaper_clear.left - 8.0f * scale - btn_w, btn_y,
                                         l.wallpaper_clear.left - 8.0f * scale, btn_y + btn_h);
        y += wall_h + 20.0f * scale;

        y += 22.0f * scale;
        y += 8.0f * scale;
        const float startup_h = 56.0f * scale;
        for (int i = 0; i < 2; ++i) {
            l.startup_row[i] = D2D1::RectF(card_left, y + static_cast<float>(i) * startup_h,
                                           card_right, y + static_cast<float>(i + 1) * startup_h);
        }
        y += startup_h * 2 + 24.0f * scale;
    } else {
        int counts[5] = {};
        for (const auto& row : vm.settings_items) {
            if (row.group >= 0 && row.group < 5) ++counts[row.group];
        }
        for (int g = 0; g < 5; ++g) {
            y += 56.0f * scale + 36.0f * scale + counts[g] * 36.0f * scale
               + 8.0f * scale + 12.0f * scale;
        }
        y += 48.0f * scale;
    }
    l.content_h = y - l.content_origin + pad;
    return l;
}

static bool ContainsPt(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

static bool IsHovered(const WindowViewModel& vm, HitTestResult::Region region, int index = -1) {
    return vm.hover_region == static_cast<int>(region) &&
        (index < 0 || vm.hover_control_index == index);
}

static bool TabCloseVisible(const WindowViewModel& vm, int index, float tab_w, float scale) {
    if (index >= 0 && index < static_cast<int>(vm.tabs.size()) &&
        vm.tabs[static_cast<size_t>(index)].pinned)
        return false; // Chrome: pinned tabs have no close affordance
    if (tab_w >= kTabCloseAlwaysW * scale) return true;
    if (index >= 0 && index < static_cast<int>(vm.tabs.size()) && vm.tabs[static_cast<size_t>(index)].active)
        return true;
    return IsHovered(vm, HitTestResult::Tab, index) ||
           IsHovered(vm, HitTestResult::TabClose, index);
}

static bool TitleBarCompact(float window_w, float scale, size_t tab_count) {
    return window_w < 900.0f * scale || tab_count >= 4;
}

void MainRenderer::UpdateBrushes(const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    MakeBrush(dc, theme.bg, brBg_);
    MakeBrush(dc, theme.text, brText_);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    MakeBrush(dc, theme.text_disabled, brTextDisabled_);
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.fill_pressed, brFillPressed_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    MakeBrush(dc, theme.fill_input, brFillInput_);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    MakeBrush(dc, theme.accent, brAccent_);
    MakeBrush(dc, theme.accent_hover, brAccentHover_);
    MakeBrush(dc, theme.accent_text, brAccentText_);
    MakeBrush(dc, theme.danger, brDanger_);
    MakeBrush(dc, theme.danger_hover, brDangerHover_);
    MakeBrush(dc, theme.scrollbar_thumb, brScrollbar_);
    MakeBrush(dc, theme.icon_folder, brIconFolder_);
    MakeBrush(dc, theme.icon_file, brIconFile_);
    MakeBrush(dc, theme.fps_bg, brFpsBg_);
    MakeBrush(dc, theme.fps_text, brFpsText_);
}

void MainRenderer::DrawIconText(float x, float y, float w, float h,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& color, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteTextFormat* iconFmt = compositor_->IconFormat();
    std::wstring txt = glyph;
    IDWriteTextFormat* fmt = iconFmt;
    if (!fmt) {
        fmt = compositor_->TextFormat();
        txt = fallback;
    }
    ComPtr<IDWriteTextFormat> sizedFmt;
    if (iconFmt && size_factor != 1.0f) {
        float size = 16.0f * scale_ * size_factor;
        compositor_->DwriteFactory()->CreateTextFormat(L"Segoe Fluent Icons", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"en-us", &sizedFmt);
        if (sizedFmt.get()) {
            sizedFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            sizedFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmt = sizedFmt.get();
        }
    }
    MakeBrush(dc, color, brText_);
    DrawTextRect(dc, fmt, brText_.get(), txt, x, y, w, h);
}

void MainRenderer::DrawButton(const D2D1_RECT_F& rc, const Theme& theme, const D2D1_COLOR_F& bg,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& fg, bool /*round_right*/, bool /*round_left*/, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (bg.a > 0.0f) { // transparent = resting state; hover fill is drawn on interaction only
        MakeBrush(dc, bg, brFillHover_);
        float r = theme.radius_control * scale_;
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r);
    }
    DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, glyph, fallback, fg,
        size_factor);
}

void MainRenderer::DrawFolderIcon(float x, float y, float size, const Theme& theme) {
    (void)theme;
    DrawIconText(x, y, size, size, kIconFolder, L"[dir]", brIconFolder_->GetColor(), 1.0f);
}

void MainRenderer::DrawFileIcon(float x, float y, float size, const Theme& theme) {
    (void)theme;
    DrawIconText(x, y, size, size, kIconFile, L"[file]", brIconFile_->GetColor(), 1.0f);
}

void MainRenderer::DrawEntryIcon(const ListEntryView& entry, float x, float y, float size,
                                 const Theme& theme) {
    const auto dest = D2D1::RectF(x, y, x + size, y + size);
    if (compositor_ && compositor_->Dc() &&
        icon_cache_.Draw(compositor_->Dc(), dest, entry.path, entry.name, entry.is_dir, entry.attrs)) {
        return;
    }
    if (entry.is_dir) DrawFolderIcon(x, y, size, theme);
    else DrawFileIcon(x, y, size, theme);
}

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
    UpdateBrushes(theme);
    painter_.BeginFrame(theme, IsHighContrast());

    // None effect + selected image: the wallpaper itself covers the window
    // base; a scrim plus the translucent panels (backdrop_enabled) let it
    // show through.
    const bool image_mode = vm.window_effect == WindowEffect::None &&
                            !vm.background_image.empty() && !IsHighContrast();
    bool backdrop_drawn = false;
    if (image_mode) {
        backdrop_drawn = material_.DrawSourceCover(dc, rect, vm.background_image);
    } else {
        backdrop_drawn = material_.DrawBackdrop(
            dc, rect, vm.window_effect, vm.dark,
            (vm.window_effect == WindowEffect::None) ? std::wstring{} : vm.background_image);
    }
    D2D1_COLOR_F micaTint = theme.bg;
    if (image_mode) {
        // Decode failure must fall back to opaque, never a hole to the desktop.
        micaTint.a = backdrop_drawn ? (vm.dark ? 0.55f : 0.60f) : 1.0f;
    } else if (backdrop_drawn) {
        micaTint.a = 0.0f;
    } else if (vm.backdrop_enabled) {
        micaTint.a = vm.dark ? 0.72f : 0.78f;
    }
    if (micaTint.a > 0.0f) {
        MakeBrush(dc, micaTint, brBg_);
        FillRect(dc, brBg_.get(), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }

    DrawTitleBar(vm, rect, theme);
    if (vm.settings_open) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        DrawSettings(vm, rect, theme);
        DrawStatusBar(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        DrawSidebar(vm, rect, theme);
        DrawPane(vm, rect, theme);
        if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
        else preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                                   theme.bg, theme.text, false);
        DrawStatusBar(vm, rect, theme);
    }

    // Drag action badge (ui.md §7.8): tooltip-style flyout near the cursor.
    if (!vm.drag_badge.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        float tw = MeasureTextWidth(compositor_->DwriteFactory(), fmt, vm.drag_badge);
        float bw = tw + 20 * scale_;
        float bh = 24 * scale_;
        float bx = std::min(vm.drag_badge_x + 14 * scale_, rect.right - bw - margin_);
        float by = std::min(vm.drag_badge_y + 16 * scale_, rect.bottom - bh - margin_);
        D2D1_RECT_F brc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), brc.left, brc.top, bw, bh, 4 * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(brc, 4 * scale_, 4 * scale_), brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, fmt, brText_.get(), vm.drag_badge, bx + 10 * scale_, by, tw + 2, bh);
    }

    if (vm.drag_badge.empty() && !vm.tooltip_text.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        const float tw = MeasureTextWidth(compositor_->DwriteFactory(), fmt, vm.tooltip_text);
        const float bw = std::min(tw + 20.0f * scale_, rect.right - 16.0f * scale_);
        const float bh = 28.0f * scale_;
        const float bx = std::clamp(vm.tooltip_x + 12.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.right - bw - 8.0f * scale_));
        const float by = std::clamp(vm.tooltip_y + 18.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.bottom - bh - 8.0f * scale_));
        const D2D1_RECT_F tipRc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bx, by, bw, bh, 4.0f * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(tipRc, 4.0f * scale_, 4.0f * scale_),
            brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, fmt, brText_.get(), vm.tooltip_text,
            bx + 10.0f * scale_, by, bw - 20.0f * scale_, bh);
    }

}

ID2D1Bitmap* MainRenderer::LogoBitmap() {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return nullptr;
    if (logo_bitmap_.get() && logo_dc_ == dc && std::abs(logo_scale_ - scale_) <= 0.001f) {
        return logo_bitmap_.get();
    }
    logo_bitmap_.reset();
    logo_dc_ = nullptr;
    // Decode well above the on-screen size so the mark stays crisp at high DPI.
    const int px = std::max(32, static_cast<int>(48.0f * scale_ + 0.5f));
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon) return nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wicBitmap;
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &wicBitmap)) &&
        SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeMedianCut))) {
        dc->CreateBitmapFromWicBitmap(converter.get(), nullptr, &logo_bitmap_);
    }
    DestroyIcon(icon);
    if (logo_bitmap_.get()) {
        logo_dc_ = dc;
        logo_scale_ = scale_;
    }
    return logo_bitmap_.get();
}

void MainRenderer::DrawTitleBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float y = 0.0f;
    const float h = title_bar_height_;
    const float right = rect.right;
    const bool compact = TitleBarCompact(rect.right, scale_, vm.tabs.size());

    // Product mark: the packaged app icon; the monogram is the fallback.
    float x = 12.0f * scale_;
    const float mark = 20.0f * scale_;
    const float markY = (h - mark) * 0.5f;
    if (ID2D1Bitmap* logo = LogoBitmap()) {
        dc->DrawBitmap(logo, D2D1::RectF(x, markY, x + mark, markY + mark), 1.0f,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } else {
        MakeBrush(dc, theme.accent, brAccent_);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + mark * 0.5f, markY + mark * 0.5f),
                                      mark * 0.5f, mark * 0.5f), brAccent_.get());
        ComPtr<IDWriteTextFormat> markFmt;
        compositor_->DwriteFactory()->CreateTextFormat(L"Segoe UI Variable", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.0f * scale_, L"en-us", &markFmt);
        if (markFmt.get()) {
            markFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            markFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            MakeBrush(dc, theme.accent_text, brAccentText_);
            DrawTextRect(dc, markFmt.get(), brAccentText_.get(), L"P", x, markY, mark, mark);
        }
    }
    x += mark + 8.0f * scale_;
    if (!compact) {
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->HeaderFormat(), brTextSecondary_.get(), L"Pulse",
            x, 0.0f, 64.0f * scale_, h);
        x += 72.0f * scale_;
    } else {
        x += 4.0f * scale_;
    }

    const float ctrlW = 46.0f * scale_;
    const TitleChrome chrome = MakeTitleChrome(right, scale_, h);

    const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
    const float tabH = strip.h;
    const float tabY = strip.y;
    auto drawTab = [&](size_t i, float left, bool raised) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = pinned ? kTabPinnedW * scale_ : strip.w;
        const bool hovered = IsHovered(vm, HitTestResult::Tab, static_cast<int>(i)) ||
                             IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i));
        const bool connect = active || raised;
        ChromeTabShape shape;
        shape.top_radius = theme.radius_control * scale_;
        shape.bottom_radius = 8.0f * scale_;
        shape.connect_bottom = connect;
        const float tabTop = tabY;
        const float tabBottom = connect ? (h + 1.0f) : (tabY + tabH);
        const D2D1_RECT_F tabRc = D2D1::RectF(left, tabTop, left + tabW, tabBottom);
        if (raised) {
            D2D1_RECT_F shadow = tabRc;
            shadow.left += 1.0f * scale_;
            shadow.top += 2.0f * scale_;
            shadow.right -= 1.0f * scale_;
            shadow.bottom += 1.0f * scale_;
            MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, vm.dark ? 0.09f : 0.07f), brFillPressed_);
            FillChromeTab(dc, brFillPressed_.get(), shadow, shape);
            D2D1_COLOR_F fill = theme.header_bg;
            if (vm.backdrop_enabled) fill.a = vm.dark ? 0.78f : 0.84f;
            MakeBrush(dc, fill, brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else if (active) {
            D2D1_COLOR_F fill = theme.header_bg;
            if (vm.backdrop_enabled) fill.a = vm.dark ? 0.78f : 0.84f;
            MakeBrush(dc, fill, brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else {
            // Grouped tabs get a tinted body; ungrouped keep the stock look.
            const bool has_color = vm.tabs[i].color_rgb != 0;
            if (has_color) {
                D2D1_COLOR_F tint = HexColor(vm.tabs[i].color_rgb);
                tint.a *= hovered ? 0.16f : 0.10f;
                MakeBrush(dc, tint, brFillHover_);
            } else {
                MakeBrush(dc, hovered ? theme.fill_hover : theme.tab_bg, brFillHover_);
            }
            FillChromeTab(dc, brFillHover_.get(), tabRc, shape);
        }
        // Grouped tabs draw the same top strip as the ungrouped active tab,
        // just in their group color instead of the accent.
        const float r = shape.top_radius;
        const bool has_color = vm.tabs[i].color_rgb != 0;
        if (has_color) {
            D2D1_COLOR_F line = HexColor(vm.tabs[i].color_rgb);
            if (!active) line.a *= 0.55f;
            MakeBrush(dc, line, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        } else if (active) {
            MakeBrush(dc, theme.accent, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        }
        if (pinned) {
            // Chrome pinned tab: centered icon, no title, no close button.
            DrawIconText(left, tabY, tabW, tabH,
                vm.tabs[i].title.empty() ? kIconFolder
                    : vm.tabs[i].title == L"设置" ? kIconSettings : kIconFolder, L"[]",
                active ? theme.icon_folder : theme.text_secondary, 0.85f);
            return;
        }
        DrawIconText(left + 6.0f * scale_, tabY, 16.0f * scale_, tabH,
            vm.tabs[i].title == L"\u8BBE\u7F6E" ? kIconSettings : kIconFolder, L"[]",
            active ? theme.icon_folder : theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_;
        DrawTabTitle(dc, compositor_->DwriteFactory(), compositor_->TabFormat(), brText_.get(),
                     vm.tabs[i].title, titleLeft, tabY,
                     std::max(0.0f, tabW - 24.0f * scale_ - closeReserve), tabH, scale_);
        if (show_close) {
            const float closeY = tabY + (tabH - closeSz) * 0.5f;
            const float closeX = left + tabW - closePad - closeSz;
            if (IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillPressed_);
                FillRoundedRect(dc, brFillPressed_.get(), closeX, closeY, closeSz, closeSz, r);
            }
            DrawIconText(closeX, closeY, closeSz, closeSz,
                kIconCloseSmall, L"x", theme.text_secondary, 0.62f);
        }
    };
    const int dragI = vm.tab_drag_index;
    // Group chips sit at run starts: Edge-style solid blocks, not pill badges.
    auto brighten = [&](const D2D1_COLOR_F& c) {
        // Toward white (dark theme) or black (light) for readable chip text.
        D2D1_COLOR_F out = c;
        const float t = 0.35f;
        const float target = vm.dark ? 1.0f : 0.0f;
        out.r += (target - out.r) * t;
        out.g += (target - out.g) * t;
        out.b += (target - out.b) * t;
        out.a = 1.0f;
        return out;
    };
    for (const auto& chip : strip.chips) {
        if (chip.group < 0 || chip.group >= static_cast<int>(vm.tab_groups.size())) continue;
        const TabGroupView& gv = vm.tab_groups[static_cast<size_t>(chip.group)];
        const D2D1_COLOR_F gc = HexColor(gv.color_rgb);
        const float ch = strip.h - 8.0f * scale_;
        // Chip drag: the group's chip floats with its run (alone if collapsed).
        float chipLeft = chip.left + gv.x_offset;
        if (vm.tab_drag_chip && dragI >= 0 && dragI < static_cast<int>(vm.tabs.size()) &&
            vm.tabs[static_cast<size_t>(dragI)].group == chip.group) {
            chipLeft = gv.collapsed ? vm.tab_drag_x
                                    : vm.tab_drag_x - chip.width - 4.0f * scale_;
        }
        const D2D1_RECT_F rc = D2D1::RectF(chipLeft, strip.y + 4.0f * scale_,
                                           chipLeft + chip.width,
                                           strip.y + 4.0f * scale_ + ch);
        const bool chip_hovered = IsHovered(vm, HitTestResult::TabGroup, chip.group);
        const bool named = !gv.name.empty();
        D2D1_COLOR_F fill = gc;
        fill.a *= named ? (chip_hovered ? 0.42f : 0.32f)
                        : (chip_hovered ? 1.0f : 0.85f);
        MakeBrush(dc, fill, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, chip.width, ch,
                        5.0f * scale_);
        if (named) {
            MakeBrush(dc, brighten(gc), brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), gv.name,
                rc.left + 8.0f * scale_, rc.top, chip.width - 16.0f * scale_, ch);
        }
    }
    int activeI = -1;
    const int dragN = std::max(1, vm.tab_drag_count);
    auto inDragRun = [&](int i) { return dragI >= 0 && i >= dragI && i < dragI + dragN; };
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].hidden) continue;
        if (inDragRun(static_cast<int>(i))) continue;
        if (vm.tabs[i].active) {
            activeI = static_cast<int>(i);
            continue;
        }
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        drawTab(i, left, false);
    }
    if (activeI >= 0 && !inDragRun(activeI) && !vm.tabs[static_cast<size_t>(activeI)].hidden) {
        const size_t i = static_cast<size_t>(activeI);
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        drawTab(i, left, false);
    }
    // The dragged run floats as one block (browser group drag); collapsed
    // members stay hidden and do not take float width.
    if (dragI >= 0 && dragI < static_cast<int>(vm.tabs.size())) {
        float floatX = vm.tab_drag_x;
        for (int k = 0; k < dragN && dragI + k < static_cast<int>(vm.tabs.size()); ++k) {
            if (vm.tabs[static_cast<size_t>(dragI + k)].hidden) continue;
            drawTab(static_cast<size_t>(dragI + k), floatX, true);
            floatX += strip.pitch;
        }
    }

    auto tab_left_at = [&](int i) -> float {
        if (inDragRun(i)) return vm.tab_drag_x + static_cast<float>(i - dragI) * strip.pitch;
        const float extra = i < static_cast<int>(strip.extra.size())
            ? strip.extra[static_cast<size_t>(i)] : 0.0f;
        return strip.x0
            + (static_cast<float>(i) + vm.tabs[static_cast<size_t>(i)].x_offset) * strip.pitch
            + extra;
    };
    const int connected = dragI >= 0 ? dragI : activeI;
    if (connected >= 0 && connected < static_cast<int>(vm.tabs.size())) {
        const float connW = vm.tabs[static_cast<size_t>(connected)].pinned
            ? kTabPinnedW * scale_ : strip.w;
        const float shoulder = 8.0f * scale_;
        const float cut_l = tab_left_at(connected) - shoulder;
        const float cut_r = tab_left_at(connected) + connW + shoulder;
        if (cut_l > 0.0f)
            FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, cut_l, 1.0f);
        if (cut_r < rect.right)
            FillRect(dc, brStrokeDivider_.get(), cut_r, h - 1.0f, rect.right - cut_r, 1.0f);
    } else {
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, rect.right, 1.0f);
    }

    const float extraEnd = strip.extra.empty() ? 0.0f : strip.extra.back();
    x = strip.x0 + static_cast<float>(vm.tabs.size()) * strip.pitch + extraEnd;
    // New tab button follows the final rest slot (not the sliding tabs).
    D2D1_RECT_F newRc = D2D1::RectF(x, tabY, x + 32 * scale_, tabY + tabH);
    DrawButton(newRc, theme, IsHovered(vm, HitTestResult::TabNew) ? theme.fill_hover : kTransparent,
        kIconAdd, L"+", theme.text_secondary, true, true);

    const D2D1_RECT_F settingsRc = D2D1::RectF(chrome.settings_left, tabY,
        chrome.settings_left + chrome.settings_w, tabY + tabH);
    DrawButton(settingsRc, theme,
        IsHovered(vm, HitTestResult::SettingsButton) ? theme.fill_hover : kTransparent,
        kIconSettings, L"S", theme.text_secondary, true, true);

    const D2D1_RECT_F themeRc = D2D1::RectF(chrome.theme_left, tabY,
        chrome.theme_left + chrome.theme_w, tabY + tabH);
    DrawButton(themeRc, theme, IsHovered(vm, HitTestResult::ThemeToggle) ? theme.fill_hover : kTransparent,
        kIconTheme, L"T", theme.text_secondary, true, true);

    // Window controls, right-aligned in Win11 order: min, max/restore, close.
    const float ctrlY = y;
    const float ctrlH = h;
    float cx = right;
    cx -= ctrlW;
    D2D1_RECT_F closeRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Close)) {
        MakeBrush(dc, theme.danger, brDanger_);
        FillRect(dc, brDanger_.get(), closeRc.left, closeRc.top, ctrlW, ctrlH);
    }
    DrawIconText(closeRc.left, closeRc.top, ctrlW, ctrlH, kIconClose, L"x",
        IsHovered(vm, HitTestResult::Close) ? HexColor(0xFFFFFF) : theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F maxRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Maximize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), maxRc.left, maxRc.top, ctrlW, ctrlH);
    }
    DrawIconText(maxRc.left, maxRc.top, ctrlW, ctrlH,
        vm.maximized ? kIconRestore : kIconMaximize, vm.maximized ? L"[]" : L"\u25A1",
        theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F minRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Minimize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), minRc.left, minRc.top, ctrlW, ctrlH);
    }
    DrawIconText(minRc.left, minRc.top, ctrlW, ctrlH, kIconMinimize, L"_", theme.text, 0.66f);
}

void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float y = title_bar_height_;
    const float h = toolbar_height_;
    const bool compact = rect.right < 900.0f * scale_;
    float x = margin_;
    const float commandHeight = kCommandIconButtonDip * scale_;
    const float commandTop = y + (h - commandHeight) * 0.5f;
    const float commandBottom = commandTop + commandHeight;

    D2D1_COLOR_F toolbarBackground = theme.header_bg;
    if (vm.backdrop_enabled) toolbarBackground.a = vm.dark ? 0.78f : 0.84f;
    MakeBrush(dc, toolbarBackground, brFillInput_);
    FillRect(dc, brFillInput_.get(), 0.0f, y, rect.right, h);
    FillRect(dc, brStrokeDivider_.get(), 0.0f, y + h - 1.0f, rect.right, 1.0f);

    auto navBtn = [&](const wchar_t* glyph, const wchar_t* fallback, bool enabled,
                      HitTestResult::Region region) {
        D2D1_RECT_F rc = D2D1::RectF(x, commandTop,
                                     x + kCommandIconButtonDip * scale_, commandBottom);
        DrawButton(rc, theme, IsHovered(vm, region) ? theme.fill_hover : kTransparent,
            glyph, fallback, enabled ? theme.text : theme.text_disabled, true, true, 0.8f);
        x += kCommandIconStepDip * scale_;
    };
    navBtn(kIconBack, L"<", vm.can_go_back, HitTestResult::NavBack);
    if (!compact) navBtn(kIconForward, L">", vm.can_go_forward, HitTestResult::NavForward);
    navBtn(kIconUp, L"^", true, HitTestResult::NavUp);
    navBtn(kIconRefresh, L"R", true, HitTestResult::NavRefresh);

    x += margin_;

    // Breadcrumb address bar: segments clickable, empty area -> edit mode.
    D2D1_RECT_F addrRc = AddressBarRect(rect.right);
    fluent::ControlState addrState{};
    addrState.focused = vm.address_editing;
    addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
    painter_.DrawTextFieldFrame(addrRc, addrState);
    if (!vm.address_editing) {
        std::vector<BreadcrumbPlaced> placed;
        BreadcrumbLayout(vm.pane, rect.right, placed);
        for (size_t i = 0; i < placed.size(); ++i) {
            const auto& seg = placed[i];
            if ((int)i == vm.breadcrumb_drop) {
                // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                dc->DrawRoundedRectangle(
                    D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                    brAccent_.get(), 2.0f * scale_);
            } else if ((int)i == vm.breadcrumb_hover) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                    seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                    theme.radius_control * scale_);
            }
            if (i > 0) {
                // Chevron separator.
                float chX = seg.rc.left - 14.0f * scale_;
                DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                    kIconChevronRight, L">", theme.text_secondary, 0.55f);
            }
            MakeBrush(dc, theme.text, brText_);
            // Width measured exactly; let the ink use the right padding as slack
            // so the trailing glyph is not shaved by the clip rect.
            DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                seg.rc.bottom - seg.rc.top);
        }
        if (placed.empty() && !vm.pane.path.empty()) {
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                addrRc.left + 12.0f * scale_, addrRc.top,
                addrRc.right - addrRc.left - 12.0f * scale_ - painter_.OmnibarHintReservePx(),
                addrRc.bottom - addrRc.top);
        }
        painter_.DrawOmnibarHints(addrRc);
    }
    x = addrRc.right + margin_;

    // New primary button.
    const float newW = compact ? 62.0f * scale_ : 78.0f * scale_;
    D2D1_RECT_F newRc = D2D1::RectF(x, commandTop, x + newW, commandBottom);
    MakeBrush(dc, IsHovered(vm, HitTestResult::NewButton) ? theme.accent_hover : theme.accent,
        brAccent_);
    FillRoundedRect(dc, brAccent_.get(), newRc.left, newRc.top, newRc.right - newRc.left, newRc.bottom - newRc.top, theme.radius_control * scale_);
    if (compact) {
        DrawIconText(newRc.left, newRc.top, newW, newRc.bottom - newRc.top,
            kIconAdd, L"+", theme.accent_text, 0.88f);
    } else {
        DrawIconText(newRc.left + 4.0f * scale_, newRc.top, 24.0f * scale_,
            newRc.bottom - newRc.top, kIconAdd, L"+", theme.accent_text, 0.82f);
        MakeBrush(dc, theme.accent_text, brAccentText_);
        DrawTextRect(dc, compositor_->AddressFormat(), brAccentText_.get(), L"\u65B0\u5EFA",
            newRc.left + 28.0f * scale_, newRc.top, newW - 44.0f * scale_, newRc.bottom - newRc.top);
        DrawIconText(newRc.right - 20.0f * scale_, newRc.top, 16.0f * scale_,
            newRc.bottom - newRc.top, kIconChevronDown, L"v", theme.accent_text, 0.48f);
    }
    x = newRc.right + margin_;

    const bool hasSelection = vm.pane.selected_count > 0;
    auto opBtn = [&](const wchar_t* glyph, const wchar_t* fallback, bool enabled,
                     HitTestResult::Region region) {
        D2D1_RECT_F rc = D2D1::RectF(x, commandTop,
                                     x + kCommandIconButtonDip * scale_, commandBottom);
        DrawButton(rc, theme, IsHovered(vm, region) ? theme.fill_hover : kTransparent, glyph, fallback,
            enabled ? theme.text : theme.text_disabled, true, true, 0.8f);
        x += kCommandIconStepDip * scale_;
    };
    if (!compact) {
        x += 4.0f * scale_;
        FillRect(dc, brStrokeDivider_.get(), x, y + 10.0f * scale_, 1.0f, h - 20.0f * scale_);
        x += 7.0f * scale_;
        opBtn(kIconCut, L"Cut", hasSelection, HitTestResult::Cut);
        opBtn(kIconCopy, L"Copy", hasSelection, HitTestResult::Copy);
        opBtn(kIconPaste, L"Paste", true, HitTestResult::Paste);
        opBtn(kIconRename, L"Ren", hasSelection, HitTestResult::Rename);
        opBtn(kIconDelete, L"Del", hasSelection, HitTestResult::Delete);
        x += 4.0f * scale_;
        FillRect(dc, brStrokeDivider_.get(), x, y + 10.0f * scale_, 1.0f, h - 20.0f * scale_);
        x += 7.0f * scale_;
        D2D1_RECT_F splitRc = D2D1::RectF(x, commandTop,
                                          x + kCommandIconButtonDip * scale_, commandBottom);
        DrawButton(splitRc, theme, IsHovered(vm, HitTestResult::SplitButton) ? theme.fill_hover : kTransparent,
            kIconSplit, L"Spl", theme.text, true, true, 0.8f);
        x += kCommandIconStepDip * scale_;
        D2D1_RECT_F detailsRc = D2D1::RectF(x, commandTop,
                                             x + kCommandIconButtonDip * scale_,
                                             commandBottom);
        fluent::ControlState detailsState;
        detailsState.hovered = IsHovered(vm, HitTestResult::DetailsToggle);
        detailsState.checked = vm.details_visible;
        painter_.DrawButton({detailsRc, {},
            vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,
            fluent::ButtonKind::TransparentToggle, detailsState, true});
        x += kCommandIconStepDip * scale_;
    }
}

void MainRenderer::DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const float w = sb.right - sb.left;

    D2D1_COLOR_F sidebarBackground = theme.tab_bg;
    if (vm.backdrop_enabled) sidebarBackground.a = vm.dark ? 0.62f : 0.70f;
    MakeBrush(dc, sidebarBackground, brFillHover_);
    FillRect(dc, brFillHover_.get(), sb.left, sb.top, w, sb.bottom - sb.top);
    FillRect(dc, brStrokeDivider_.get(), sb.right - 1, sb.top, 1, sb.bottom - sb.top);

    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, sb, scale_, slots);
    const bool compact = w <= 60.0f * scale_;
    painter_.BeginFrame(theme, IsHighContrast());

    if (compact) {
        for (const auto& slot : slots) {
            if (slot.kind == SidebarSlot::TrayPanel) {
                MakeBrush(dc, vm.tray_drop ? theme.fill_selected : theme.surface_flyout, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), slot.rc.left, slot.rc.top,
                    slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                    theme.radius_control * scale_);
                DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                    slot.rc.bottom - slot.rc.top, kIconTray, L"Tray", theme.accent, 0.9f);
                if (vm.tray_deck.total_count > 0) {
                    const float badge = 14.0f * scale_;
                    MakeBrush(dc, theme.accent, brAccent_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(slot.rc.right - 6.0f * scale_,
                        slot.rc.top + 7.0f * scale_), badge * 0.5f, badge * 0.5f), brAccent_.get());
                }
                continue;
            }
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            const bool selected = PathIsSelfOrChild(item.path, vm.pane.path);
            const bool hovered = IsHovered(vm, HitTestResult::SidebarItem, slot.run);
            if (selected || hovered || slot.run == vm.sidebar_drop_index) {
                MakeBrush(dc, selected ? theme.fill_selected : theme.fill_hover, brFillSelected_);
                FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                    slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                    theme.radius_control * scale_);
            }
            const D2D1_COLOR_F iconColor = item.icon_color.a > 0 ? item.icon_color
                                         : item.tag_dot.a > 0 ? item.tag_dot : theme.text;
            DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                slot.rc.bottom - slot.rc.top, item.icon_glyph, item.fallback_text,
                iconColor, 0.92f);
        }
        return;
    }

    wchar_t countLabel[24]{};
    const int trayCount = TrayTotalCount(vm);
    if (trayCount > 0) swprintf_s(countLabel, L"%d \u9879", trayCount); // N 项

    for (const auto& slot : slots) {
        if (slot.kind == SidebarSlot::Header) {
            fluent::SidebarSectionHeaderSpec header;
            header.bounds = slot.rc;
            header.text = vm.sidebar[slot.group].header;
            header.expanded = !vm.sidebar[slot.group].collapsed;
            header.state.hovered = IsHovered(vm, HitTestResult::SidebarHeader, slot.group);
            painter_.DrawSidebarSectionHeader(header);
            if (vm.sidebar[slot.group].add_action) {
                DrawIconText(slot.rc.right - 52.0f * scale_, slot.rc.top,
                    24.0f * scale_, slot.rc.bottom - slot.rc.top,
                    kIconAdd, L"+", theme.text_secondary, 0.72f);
            }
            continue;
        }
        if (slot.kind == SidebarSlot::TrayPanel) {
            fluent::StagingTrayPanelSpec tray;
            tray.bounds = slot.rc;
            tray.title = L"\u4E34\u65F6\u6682\u5B58\u6258\u76D8"; // 临时暂存托盘
            tray.helper = vm.tray_deck.cards.empty() ? L"\u62D6\u5165\u6587\u4EF6\uFF0C\u96C6\u4E2D\u590D\u5236/\u79FB\u52A8" : L""; // helper only while the deck is empty
            tray.count_label = countLabel;
            tray.action_text = vm.tray_deck.total_count == 0 ? L"" : L"\u91CA\u653E"; // 释放
            tray.action_hovered =
                vm.hover_region == static_cast<int>(HitTestResult::TrayRelease);
            tray.item_count = trayCount;
            tray.state.hovered = vm.tray_drop;
            tray.expanded = true;
            painter_.DrawStagingTrayPanel(tray);
            DrawTrayDeck(vm, slot.rc, theme);
            continue;
        }
        if (slot.kind == SidebarSlot::TrayRelease) continue;
        if (slot.group < 0 || slot.item < 0) continue;

        const auto& item = vm.sidebar[slot.group].items[slot.item];
        fluent::ControlState state;
        state.selected = PathIsSelfOrChild(item.path, vm.pane.path);
        state.hovered = IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
        if (vm.tag_drag_group >= 0) state.hovered = false; // run indices shift mid-drag
        if (slot.kind == SidebarSlot::Drive) {
            fluent::DriveSidebarItemSpec drive;
            drive.bounds = slot.rc;
            drive.name = item.label;
            drive.detail = item.detail;
            drive.glyph = item.icon_glyph;
            drive.capacity = item.used_ratio;
            drive.state = state;
            drive.drop_target = slot.run == vm.sidebar_drop_index;
            drive.icon_color = item.icon_color;
            if (item.danger) drive.bar_color = theme.danger;
            painter_.DrawDriveSidebarItem(drive);
        } else {
            const bool draggedTag =
                slot.group == vm.tag_drag_group && slot.item == vm.tag_drag_item;
            if (draggedTag) {
                // Raised while dragging (QFluent TabBar: raise + soft drop
                // shadow, no outline) — real gaussian shadow via command list.
                const float r = theme.radius_control * scale_;
                ComPtr<ID2D1CommandList> card;
                dc->CreateCommandList(&card);
                if (card.get()) {
                    ComPtr<ID2D1Image> prev;
                    dc->GetTarget(&prev);
                    dc->SetTarget(card.get());
                    MakeBrush(dc, vm.dark ? HexColor(0x282828) : HexColor(0xF9F9F9), brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(),
                        slot.rc.left + 1.0f, slot.rc.top + 1.0f,
                        slot.rc.right - slot.rc.left - 2.0f,
                        slot.rc.bottom - slot.rc.top - 2.0f, r);
                    dc->SetTarget(prev.get());
                    card->Close();
                    ComPtr<ID2D1Effect> shadow;
                    if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow)) && shadow.get()) {
                        shadow->SetInput(0, card.get());
                        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 3.0f * scale_);
                        shadow->SetValue(D2D1_SHADOW_PROP_COLOR,
                            D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.22f));
                        dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 1.0f * scale_),
                            D2D1_INTERPOLATION_MODE_LINEAR);
                    }
                    dc->DrawImage(card.get());
                    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(slot.rc, r, r),
                        brStrokeCard_.get(), 1.0f);
                }
            }
            fluent::SidebarItemSpec row;
            row.bounds = slot.rc;
            row.text = item.label;
            row.glyph = item.icon_glyph;
            row.badge_text = item.badge;
            row.state = state;
            row.badge_count = item.count;
            row.show_count = item.show_count;
            row.drop_target = slot.run == vm.sidebar_drop_index;
            row.tag_dot = item.is_tag;
            row.tag_color = item.tag_dot;
            row.status_dot = item.status_dot;
            row.status_color = item.status_color;
            row.icon_color = item.icon_color;
            row.suppress_text = item.editing;
            const bool unpin = SidebarItemHasUnpin(item);
            const auto unpin_rc = WorkspaceUnpinRect(slot.rc, scale_);
            if (unpin)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - unpin_rc.left) - 6.0f * scale_);
            painter_.DrawSidebarItem(row);
            if (unpin) {
                const bool unpin_hot = IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
                if (unpin_hot) {
                    MakeBrush(dc, WithAlpha(theme.accent, vm.dark ? 0.22f : 0.16f), brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), unpin_rc.left, unpin_rc.top,
                        unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                        4.0f * scale_);
                }
                DrawIconText(unpin_rc.left, unpin_rc.top,
                    unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                    kIconPinFilled, L"P", unpin_hot ? theme.accent_hover : theme.accent, 0.72f);
            }
        }
    }
}

void MainRenderer::DrawTrayDeck(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc,
                                const Theme& theme) {
    if (vm.tray_deck.cards.empty() || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const TrayFanGeom g = TrayFanGeometry(panel_rc, vm.tray_deck.live_count,
                                          vm.tray_deck.open, scale_);

    // Ghosts (exiting) underneath; live icons outermost-first so the fan
    // center stays on top; the hovered entry always draws last.
    const std::vector<int> order = TrayCardPaintOrder(vm.tray_deck);

    for (const int idx : order) {
        const TrayCardView& card = vm.tray_deck.cards[static_cast<size_t>(idx)];
        const bool hovered = !card.ghost && idx == vm.tray_deck.hovered;
        const TrayCardPose pose = TrayCardPoseOf(g, card, scale_);
        const float opacity = std::clamp(card.opacity, 0.0f, 1.0f);
        const float half = g.icon * pose.scale_f * 0.5f;
        const D2D1_RECT_F dest = D2D1::RectF(pose.center.x - half, pose.center.y - half,
                                             pose.center.x + half, pose.center.y + half);
        D2D1_MATRIX_3X2_F old;
        dc->GetTransform(&old);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(pose.angle, pose.center) * old);
        const bool drewThumbnail = !card.missing && !card.is_dir &&
            thumbnail_cache_.Draw(dc, dest, card.path, card.attrs,
                                  static_cast<uint32_t>(std::clamp(
                                      std::lround(g.icon), 32l, 256l)),
                                  0, 0, 0, opacity) == PreviewDrawResult::Bitmap;
        ID2D1Bitmap* bitmap = drewThumbnail || card.missing ? nullptr
            : icon_cache_.BitmapFor(card.path, card.name, card.is_dir, card.attrs, g.icon);
        if (!drewThumbnail && bitmap) {
            // Soft drop shadow straight off the bitmap silhouette, no card.
            ComPtr<ID2D1Effect> shadow;
            if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow)) && shadow.get()) {
                shadow->SetInput(0, bitmap);
                shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                                 (hovered ? 5.0f : 3.0f) * scale_);
                shadow->SetValue(D2D1_SHADOW_PROP_COLOR,
                    D2D1::Vector4F(0.0f, 0.0f, 0.0f, (vm.dark ? 0.55f : 0.30f) * opacity));
                dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 2.0f * scale_),
                              D2D1_INTERPOLATION_MODE_LINEAR);
            }
            dc->DrawBitmap(bitmap, &dest, opacity,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        } else if (!drewThumbnail) {
            DrawIconText(dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top,
                         card.is_dir ? kIconFolder : kIconFile,
                         card.is_dir ? L"[dir]" : L"[file]",
                         card.missing ? theme.text_disabled
                                      : (card.is_dir ? brIconFolder_->GetColor()
                                                     : brIconFile_->GetColor()),
                         1.6f);
        }
        if (hovered) {
            // × badge pinned to the icon's top-right corner.
            const float badge_r = 8.0f * scale_;
            const auto bc = D2D1::Point2F(pose.center.x + half * 0.85f,
                                          pose.center.y - half * 0.85f);
            const bool close_hot =
                vm.hover_region == static_cast<int>(HitTestResult::TrayItemRemove);
            D2D1_COLOR_F badge_fill = theme.danger;
            badge_fill.a *= close_hot ? 1.0f : (vm.dark ? 0.30f : 0.15f);
            D2D1_COLOR_F badge_border = theme.danger;
            badge_border.a *= close_hot ? 1.0f : 0.45f;
            ComPtr<ID2D1SolidColorBrush> brBadge, brBadgeBorder;
            dc->CreateSolidColorBrush(badge_fill, &brBadge);
            dc->CreateSolidColorBrush(badge_border, &brBadgeBorder);
            dc->FillEllipse(D2D1::Ellipse(bc, badge_r, badge_r), brBadge.get());
            dc->DrawEllipse(D2D1::Ellipse(bc, badge_r - 0.5f, badge_r - 0.5f),
                            brBadgeBorder.get(), 1.0f);
            DrawIconText(bc.x - badge_r, bc.y - badge_r, badge_r * 2.0f, badge_r * 2.0f,
                         kIconCloseSmall, L"x",
                         close_hot ? HexColor(0xFFFFFF) : theme.danger, 0.55f);
        }
        dc->SetTransform(old);
    }

    // Centered single-line label with ellipsis trimming (DrawCenteredIconName
    // idiom, but with a caller-picked format).
    auto draw_label = [&](const std::wstring& text, const D2D1_RECT_F& rc,
                          const D2D1_COLOR_F& color, IDWriteTextFormat* fmt) {
        if (!fmt || text.empty()) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), fmt,
                std::max(1.0f, rc.right - rc.left), std::max(1.0f, rc.bottom - rc.top),
                &layout)) || !layout.get())
            return;
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        compositor_->DwriteFactory()->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
        layout->SetTrimming(&trimming, ellipsis.get());
        MakeBrush(dc, color, brText_);
        dc->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.get(), brText_.get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Small rounded badge pinned near an icon corner (overflow indicators).
    // Local brushes only: member brushes are reused by later drawing code.
    auto corner_pill = [&](D2D1_POINT_2F center, const std::wstring& text,
                           const D2D1_COLOR_F& fill, const D2D1_COLOR_F& fg) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        const float tw = MeasureTextWidth(compositor_->DwriteFactory(), fmt, text);
        const float w = std::max(18.0f * scale_, tw + 10.0f * scale_);
        const float h = 16.0f * scale_;
        const D2D1_RECT_F rc = D2D1::RectF(center.x - w * 0.5f, center.y - h * 0.5f,
                                           center.x + w * 0.5f, center.y + h * 0.5f);
        ComPtr<ID2D1SolidColorBrush> brPill, brPillBorder;
        dc->CreateSolidColorBrush(fill, &brPill);
        dc->CreateSolidColorBrush(theme.stroke_card, &brPillBorder);
        FillRoundedRect(dc, brPill.get(), rc.left, rc.top, w, h, h * 0.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, h * 0.5f, h * 0.5f),
                                 brPillBorder.get(), 1.0f);
        draw_label(text, rc, fg, fmt);
    };

    // Wheel-scroll overflow: "+N" on the rightmost icon, "‹" on the leftmost
    // once scrolled away from the newest items.
    const int live = vm.tray_deck.live_count;
    const int remaining = vm.tray_deck.total_count - vm.tray_deck.offset - live;
    if (live > 0 && remaining > 0) {
        const TrayCardView& last = vm.tray_deck.cards[static_cast<size_t>(live - 1)];
        const TrayCardPose pose = TrayCardPoseOf(g, last, scale_);
        const float half = g.icon * pose.scale_f * 0.5f;
        wchar_t more[20];
        swprintf_s(more, L"+%d", remaining);
        corner_pill(D2D1::Point2F(pose.center.x + half * 0.9f, pose.center.y + half * 0.85f),
                    more, theme.accent, theme.accent_text);
    }
    if (live > 0 && vm.tray_deck.offset > 0) {
        const TrayCardView& first = vm.tray_deck.cards.front();
        const TrayCardPose pose = TrayCardPoseOf(g, first, scale_);
        const float half = g.icon * pose.scale_f * 0.5f;
        corner_pill(D2D1::Point2F(pose.center.x - half * 0.9f, pose.center.y + half * 0.85f),
                    L"\u2039", // ‹
                    D2D1::ColorF(vm.dark ? 0x2A3446u : 0xFFFFFFu, vm.dark ? 0.94f : 0.97f),
                    theme.text_secondary);
    }

    // Footer: aggregate size (+ batch count) left, 清空 right. Skip while
    // only exiting ghosts remain (tray already cleared).
    if (live > 0) {
        wchar_t footer[96]{};
        const std::wstring size_text = pulse::format::ByteSize(vm.tray_deck.total_size, true);
        if (vm.tray_deck.batch_count > 1)
            swprintf_s(footer, L"\u5171 %s \u00B7 %d \u6279", size_text.c_str(), vm.tray_deck.batch_count);
        else
            swprintf_s(footer, L"\u5171 %s", size_text.c_str());
        const float footer_h = 22.0f * scale_;
        const float footerTop = panel_rc.bottom - 8.0f * scale_ - footer_h;
        painter_.DrawText(footer,
                          D2D1::RectF(panel_rc.left + 10.0f * scale_, footerTop,
                                      panel_rc.right - 60.0f * scale_, footerTop + footer_h),
                          compositor_->SmallFormat(), theme.text_secondary);
        const bool clear_hovered =
            vm.hover_region == static_cast<int>(HitTestResult::TrayClear);
        // Ghost button: outlined, fills on hover — distinct from both the
        // solid release button and the soft count badge.
        const float clear_w = 44.0f * scale_;
        const D2D1_RECT_F clear_rc =
            D2D1::RectF(panel_rc.right - 10.0f * scale_ - clear_w,
                        footerTop,
                        panel_rc.right - 10.0f * scale_, footerTop + footer_h);
        const float clear_r = (clear_rc.bottom - clear_rc.top) * 0.5f;
        if (clear_hovered) {
            D2D1_COLOR_F hot_fill = theme.danger;
            hot_fill.a *= 0.12f;
            painter_.FillRoundedRect(clear_rc, clear_r, hot_fill);
        }
        D2D1_COLOR_F danger_border = theme.danger;
        danger_border.a *= clear_hovered ? 0.9f : 0.45f;
        painter_.StrokeRoundedRect(clear_rc, clear_r, danger_border, 1.0f);
        painter_.DrawText(L"\u6E05\u7A7A", clear_rc,
                          compositor_->SmallFormat(),
                          theme.danger,
                          fluent::HorizontalAlignment::Center);
    }

    // Name text: a single staged item always shows name + size/type; with a
    // fan out, only the hovered icon gets a small pill label.
    if (vm.tray_deck.live_count == 1 && !vm.tray_deck.cards.empty() &&
        !vm.tray_deck.cards.front().ghost) {
        const TrayCardView& card = vm.tray_deck.cards.front();
        const TrayCardPose pose = TrayCardPoseOf(g, card, scale_);
        const float half = g.icon * pose.scale_f * 0.5f;
        const float text_w = panel_rc.right - panel_rc.left - 24.0f * scale_;
        const float name_y = pose.center.y + half + 6.0f * scale_;
        draw_label(card.name,
                   D2D1::RectF(g.cx - text_w * 0.5f, name_y,
                               g.cx + text_w * 0.5f, name_y + 18.0f * scale_),
                   card.missing ? theme.text_disabled : theme.text,
                   compositor_->TextFormat());
        std::wstring sub;
        if (!card.is_dir && card.batch_total_size > 0)
            sub = pulse::format::ByteSize(card.batch_total_size, true) + L" \xB7 ";
        sub += FormatListType(card.name, card.is_dir);
        if (!sub.empty())
            draw_label(sub,
                       D2D1::RectF(g.cx - text_w * 0.5f, name_y + 18.0f * scale_,
                                   g.cx + text_w * 0.5f, name_y + 32.0f * scale_),
                       theme.text_secondary, compositor_->SmallFormat());
    } else if (vm.tray_deck.hovered >= 0 &&
               vm.tray_deck.hovered < vm.tray_deck.live_count) {
        const TrayCardView& card =
            vm.tray_deck.cards[static_cast<size_t>(vm.tray_deck.hovered)];
        const TrayCardPose pose = TrayCardPoseOf(g, card, scale_);
        const float half = g.icon * pose.scale_f * 0.5f;
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        const float max_text = panel_rc.right - panel_rc.left - 40.0f * scale_;
        const float tw = std::min(MeasureTextWidth(compositor_->DwriteFactory(), fmt,
                                                   card.name),
                                  max_text);
        const float pill_w = tw + 16.0f * scale_;
        const float pill_h = 20.0f * scale_;
        const float pcx = std::clamp(pose.center.x,
            panel_rc.left + 6.0f * scale_ + pill_w * 0.5f,
            panel_rc.right - 6.0f * scale_ - pill_w * 0.5f);
        const float pcy = std::min(pose.center.y + half + 6.0f * scale_ + pill_h * 0.5f,
                                   panel_rc.bottom - 6.0f * scale_ - pill_h * 0.5f);
        const D2D1_RECT_F pill = D2D1::RectF(pcx - pill_w * 0.5f, pcy - pill_h * 0.5f,
                                             pcx + pill_w * 0.5f, pcy + pill_h * 0.5f);
        ComPtr<ID2D1SolidColorBrush> brPill, brPillBorder;
        dc->CreateSolidColorBrush(D2D1::ColorF(vm.dark ? 0x2A3446u : 0xFFFFFFu,
                                               vm.dark ? 0.94f : 0.97f), &brPill);
        dc->CreateSolidColorBrush(theme.stroke_card, &brPillBorder);
        FillRoundedRect(dc, brPill.get(), pill.left, pill.top, pill_w, pill_h,
                        pill_h * 0.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, pill_h * 0.5f, pill_h * 0.5f),
                                 brPillBorder.get(), 1.0f);
        draw_label(card.name, pill, theme.text, fmt);
    }
}

void MainRenderer::DrawDetailsPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                    const Theme& theme) {
    const D2D1_RECT_F panel = DetailsPanelRect(rect.right, rect.bottom);
    if (panel.right - panel.left <= 1.0f || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    const DetailsPanelView& d = vm.details;
    const float previewH = DetailsPreviewHeight(panel, s);
    DetailsHitRects hit;
    LayoutDetailsPanel(panel, s, d, compositor_->DwriteFactory(),
                       compositor_->SmallFormat(), previewH, hit);

    // Same surface as the list; a left rule separates the column.
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    FillRect(dc, brStrokeCard_.get(), panel.left, panel.top, 1.0f, panel.bottom - panel.top);

    fluent::SplitterSpec resizeSplitter;
    resizeSplitter.bounds = D2D1::RectF(panel.left - 4.0f * s, panel.top,
                                        panel.left + 4.0f * s, panel.bottom);
    resizeSplitter.vertical = true;
    resizeSplitter.state.hovered =
        vm.hover_region == static_cast<int>(HitTestResult::DetailsResize);
    resizeSplitter.state.pressed = vm.details_resize_pressed;
    painter_.DrawSplitter(resizeSplitter);

    auto text = [&](const std::wstring& str, const D2D1_RECT_F& rc,
                    IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                    bool right = false) {
        MakeBrush(dc, color, brText_);
        if (!fmt) return;
        const auto old = fmt->GetTextAlignment();
        fmt->SetTextAlignment(right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                    : DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawTextRect(dc, fmt, brText_.get(), str, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top);
        fmt->SetTextAlignment(old);
    };
    auto section = [&](const wchar_t* label, float y, int id) {
        const bool hovered = IsHovered(vm, HitTestResult::DetailsSection, id);
        if (hovered) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), panel.left + 6.0f * s, y,
                            panel.right - panel.left - 12.0f * s, 28.0f * s, 6.0f * s);
        }
        // Vector chevron: icon fonts on some machines lack the right-chevron glyph.
        const bool collapsed = (d.collapsed_mask >> id) & 1u;
        MakeBrush(dc, theme.text_secondary, brText_);
        const float cx = panel.left + 17.0f * s;
        const float cy = y + 14.0f * s;
        const float t = 1.3f * s;
        if (collapsed) {
            dc->DrawLine(D2D1::Point2F(cx - 2.0f * s, cy - 4.0f * s),
                         D2D1::Point2F(cx + 2.0f * s, cy), brText_.get(), t);
            dc->DrawLine(D2D1::Point2F(cx + 2.0f * s, cy),
                         D2D1::Point2F(cx - 2.0f * s, cy + 4.0f * s), brText_.get(), t);
        } else {
            dc->DrawLine(D2D1::Point2F(cx - 4.0f * s, cy - 2.0f * s),
                         D2D1::Point2F(cx, cy + 2.0f * s), brText_.get(), t);
            dc->DrawLine(D2D1::Point2F(cx, cy + 2.0f * s),
                         D2D1::Point2F(cx + 4.0f * s, cy - 2.0f * s), brText_.get(), t);
        }
        text(label, D2D1::RectF(panel.left + 28.0f * s, y + 4.0f * s,
                                panel.right - 12.0f * s, y + 24.0f * s),
             compositor_->SmallFormat(), theme.accent);
    };
    auto centeredText = [&](const std::wstring& str, const D2D1_RECT_F& rc,
                            IDWriteTextFormat* fmt, const D2D1_COLOR_F& color) {
        if (!fmt) return;
        MakeBrush(dc, color, brText_);
        const auto oldAlignment = fmt->GetTextAlignment();
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawTextRect(dc, fmt, brText_.get(), str, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top);
        fmt->SetTextAlignment(oldAlignment);
    };
    auto ghostButton = [&](const D2D1_RECT_F& rc, const wchar_t* glyph,
                           const std::wstring& label, bool hovered) {
        if (hovered) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
        }
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                 brStrokeCard_.get(), 1.0f);
        if (glyph) DrawIconText(rc.left + 6.0f * s, rc.top, 20.0f * s,
                                rc.bottom - rc.top, glyph, L"?", theme.text_secondary, 0.72f);
        text(label, D2D1::RectF(rc.left + 28.0f * s, rc.top, rc.right - 4.0f * s, rc.bottom),
             compositor_->SmallFormat(), theme.text);
    };

    if (!d.has_selection) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        const D2D1_RECT_F content = D2D1::RectF(
            panel.left + 16.0f * s, panel.top + 16.0f * s,
            panel.right - 16.0f * s, panel.bottom - 16.0f * s);
        const float availableWidth = std::max(0.0f, content.right - content.left);
        const float artWidth = std::min(180.0f * s, availableWidth);
        const float artHeight = artWidth * 240.0f / 320.0f;
        const float titleHeight = 24.0f * s;
        const float gap = 2.0f * s;
        const float totalHeight = artHeight + gap + titleHeight;
        const float top = content.top + std::max(
            0.0f, ((content.bottom - content.top) - totalHeight) * 0.5f);
        const D2D1_RECT_F art = D2D1::RectF(
            (content.left + content.right - artWidth) * 0.5f, top,
            (content.left + content.right + artWidth) * 0.5f, top + artHeight);
        const float svgOpacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
        if (!DrawNoSelectionSvg(art, svgOpacity)) {
            fluent::EmptyStateSpec empty;
            empty.bounds = content;
            empty.glyph = kIconFile;
            empty.title = L"\u672A\u9009\u62E9\u9879\u76EE";
            painter_.DrawEmptyState(empty);
            return;
        }
        centeredText(L"\u672A\u9009\u62E9\u9879\u76EE",
            D2D1::RectF(content.left, art.bottom + gap,
                        content.right, art.bottom + gap + titleHeight),
            compositor_->HeaderFormat(), theme.text_secondary);
        return;
    }

    const float pad = 12.0f * s;
    dc->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float previewTop = panel.top + pad;
    float y = previewTop;

    // Direct preview stays pinned so the HWND overlay can track it.
    {
        const D2D1_RECT_F previewRc = D2D1::RectF(panel.left + pad, previewTop,
            panel.right - 36.0f * s, previewTop + previewH);
        const bool placeholderOnly = d.is_dir || d.multi_count > 1;
        const bool handlerPreview = !placeholderOnly &&
            PreviewHandlerHost::CanHost(d.path);
        if (!placeholderOnly) {
            MakeBrush(dc, theme.fill_hover, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), previewRc.left, previewRc.top,
                            previewRc.right - previewRc.left,
                            previewRc.bottom - previewRc.top, 6.0f * s);
        }

        const float grip = 8.0f * s;
        const D2D1_RECT_F overlayRc = D2D1::RectF(previewRc.left + 2.0f * s,
            previewRc.top + 2.0f * s, previewRc.right - 2.0f * s,
            previewRc.bottom - grip);
        const D2D1_RECT_F contentRc = overlayRc;
        std::wstring previewText;
        bool truncated = false;
        uint32_t bytesRead = 0;
        std::wstring previewError;
        PreviewDrawResult previewResult = PreviewDrawResult::Failed;
        if (!d.is_dir && d.multi_count <= 1) {
            std::vector<PreviewProperty> ignored;
            thumbnail_cache_.Properties(d.path, d.attrs, d.view_generation,
                                        d.modified_value, d.size_value, ignored);
            if (!handlerPreview) {
                float panX = d.preview_pan_x, panY = d.preview_pan_y;
                previewResult = thumbnail_cache_.Draw(dc, contentRc, d.path, d.attrs, 512u,
                    d.view_generation, d.modified_value, d.size_value, 1.0f,
                    &previewText, &truncated, &bytesRead, true, &previewError,
                    &panX, &panY, &cover_max_pan_x_, &cover_max_pan_y_);
            }
        }

        preview_handler_.Sync(notify_hwnd_, overlayRc, d.path, d.attrs, d.view_generation,
                              d.modified_value, d.size_value, vm.dark, theme.bg, theme.text,
                              handlerPreview);
        const auto handlerState = preview_handler_.state();
        if (handlerPreview) {
            if (handlerState == PreviewHandlerHost::State::Shown) {
                previewResult = PreviewDrawResult::Pending;
            } else if (handlerState == PreviewHandlerHost::State::Loading ||
                       handlerState == PreviewHandlerHost::State::Idle) {
                previewResult = PreviewDrawResult::Pending;
            } else {
                previewResult = PreviewDrawResult::Failed;
                previewError = L"handler-failed";
            }
        }

        if (!handlerPreview && (previewResult == PreviewDrawResult::Text ||
            previewResult == PreviewDrawResult::Hex)) {
            if (!preview_mono_format_.get() && compositor_->DwriteFactory()) {
                compositor_->DwriteFactory()->CreateTextFormat(L"Cascadia Mono", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 11.0f * s, L"zh-CN",
                    &preview_mono_format_);
                if (!preview_mono_format_.get())
                    compositor_->DwriteFactory()->CreateTextFormat(L"Consolas", nullptr,
                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, 11.0f * s, L"zh-CN",
                        &preview_mono_format_);
                if (preview_mono_format_.get()) {
                    preview_mono_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    preview_mono_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }
            }
            if (previewText.empty()) {
                text(L"空文件", contentRc, compositor_->SmallFormat(), theme.text_secondary);
            } else if (preview_mono_format_.get()) {
                MakeBrush(dc, theme.text, brText_);
                D2D1_RECT_F clipRc = contentRc;
                clipRc.bottom -= 18.0f * s;
                D2D1_RECT_F textRc = clipRc;
                textRc.top -= d.preview_scroll_y * s;
                textRc.bottom += 4000.0f * s;
                dc->PushAxisAlignedClip(clipRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                dc->DrawTextW(previewText.data(), static_cast<UINT32>(previewText.size()),
                              preview_mono_format_.get(), textRc, brText_.get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
                dc->PopAxisAlignedClip();
            }
            std::wstring footer = previewResult == PreviewDrawResult::Hex
                ? L"十六进制 · " : L"文本 · ";
            footer += pulse::format::ByteSize(bytesRead, true);
            if (truncated) footer += L" · 已截断";
            text(footer, D2D1::RectF(contentRc.left, contentRc.bottom - 18.0f * s,
                                     contentRc.right, contentRc.bottom),
                 compositor_->SmallFormat(), theme.text_secondary);
        } else if (handlerPreview && handlerState == PreviewHandlerHost::State::Shown) {
            // System preview handler paints into the overlay HWND.
        } else if (previewResult != PreviewDrawResult::Bitmap) {
            const bool filePlaceholder = !d.is_dir && d.multi_count <= 1;
            std::wstring state;
            if (d.multi_count > 1)
                state = std::to_wstring(d.multi_count) + L" 个项目";
            else if ((d.attrs & (0x00040000u | 0x00400000u)) &&
                     !(d.attrs & 0x00080000u))
                state = L"联机后可预览";
            else if (previewResult == PreviewDrawResult::Pending)
                state = L"正在加载预览…";
            else if (previewError == L"path-unavailable")
                state = L"文件已移动或删除";
            else if (previewError == L"image-decode-failed")
                state = L"图片文件无法解码";
            else if (previewError == L"provider-failed")
                state = L"系统未提供此格式的预览";
            else if (previewError == L"handler-failed")
                state = L"预览加载失败";
            else
                state = L"此格式暂不支持预览";
            if (filePlaceholder) {
                centeredText(state, contentRc, compositor_->SmallFormat(),
                             theme.text_secondary);
            } else {
                const float icon = 58.0f * s;
                const bool showState = !d.is_dir || d.multi_count > 1;
                const float stateGap = 8.0f * s;
                const float stateHeight = 20.0f * s;
                const float groupHeight = icon + (showState ? stateGap + stateHeight : 0.0f);
                const float contentCenterX = (contentRc.left + contentRc.right) * 0.5f;
                const float iconTop = contentRc.top +
                    ((contentRc.bottom - contentRc.top) - groupHeight) * 0.5f;
                const D2D1_RECT_F iconRc = D2D1::RectF(contentCenterX - icon * 0.5f,
                    iconTop, contentCenterX + icon * 0.5f, iconTop + icon);
                if (ID2D1Bitmap* bmp = icon_cache_.BitmapFor(d.path, d.name, d.is_dir, d.attrs,
                                                             icon)) {
                    dc->DrawBitmap(bmp, &iconRc, 1.0f,
                                   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                } else if (d.is_dir) {
                    DrawFolderIcon(iconRc.left, iconRc.top, icon, theme);
                } else {
                    DrawFileIcon(iconRc.left, iconRc.top, icon, theme);
                }
                if (showState && !state.empty())
                    centeredText(state,
                        D2D1::RectF(contentRc.left, iconRc.bottom + stateGap,
                                    contentRc.right, iconRc.bottom + stateGap + stateHeight),
                        compositor_->SmallFormat(), theme.text_secondary);
            }
        }
    }
    y = previewTop + DetailsPreviewBand(previewH, s) - d.scroll_y * s;
    const D2D1_RECT_F restClip = D2D1::RectF(
        panel.left, previewTop + previewH + 4.0f * s,
        panel.right, panel.bottom);
    dc->PushAxisAlignedClip(restClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Name + star + rename pencil + subtitle.
    {
        if (d.multi_count <= 1) {
            const bool starHot = IsHovered(vm, HitTestResult::DetailsStar);
            const bool renHot = IsHovered(vm, HitTestResult::DetailsRename);
            if (starHot || d.starred) {
                MakeBrush(dc, d.starred ? WithAlpha(theme.accent, starHot ? 0.28f : 0.18f)
                                        : theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), hit.star.left, hit.star.top,
                                22.0f * s, 22.0f * s, 5.0f * s);
            }
            if (renHot) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), hit.rename.left, hit.rename.top,
                                22.0f * s, 22.0f * s, 5.0f * s);
            }
            DrawIconText(hit.star.left, hit.star.top, 22.0f * s, 22.0f * s,
                         d.starred ? L"\xE735" : L"\xE734", L"*",
                         d.starred ? theme.accent : theme.text_secondary, 1.0f);
            DrawIconText(hit.rename.left, hit.rename.top, 22.0f * s, 22.0f * s,
                         L"\xE8AC", L"ren", theme.text_secondary, 0.72f);
        }
        std::wstring shown = d.multi_count > 1
            ? (std::wstring(L"\u5DF2\u9009\u4E2D ") + std::to_wstring(d.multi_count) + L" \u9879")
            : d.name;
        const float nameRight = d.multi_count <= 1 ? hit.star.left - 4.0f * s
                                                   : panel.right - pad;
        text(shown, D2D1::RectF(panel.left + pad, y, nameRight,
                                y + 22.0f * s),
             compositor_->HeaderFormat(), theme.text);
        std::wstring subtitle = d.subtitle_text;
        if (subtitle.empty())
            subtitle = d.type_text.empty() ? FormatListType(d.name, d.is_dir) : d.type_text;
        if (!subtitle.empty())
            text(subtitle, D2D1::RectF(panel.left + pad, y + 22.0f * s,
                                       panel.right - pad, y + 38.0f * s),
                 compositor_->SmallFormat(), theme.text_secondary);
    }
    y += 22.0f * s + 16.0f * s + 8.0f * s;

    // Button row: 打开 / 在新标签打开 / 复制路径 / 更多 (icon over label).
    if (d.multi_count <= 1) {
        auto rowButton = [&](const D2D1_RECT_F& rc, const wchar_t* glyph,
                             const wchar_t* fallback, const wchar_t* label, bool hovered,
                             bool primary) {
            if (primary) {
                MakeBrush(dc, hovered ? theme.accent_hover : theme.accent, brAccent_);
                FillRoundedRect(dc, brAccent_.get(), rc.left, rc.top, rc.right - rc.left,
                                rc.bottom - rc.top, 6.0f * s);
            } else {
                if (hovered) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                    rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
                }
                MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                         brStrokeCard_.get(), 1.0f);
            }
            const D2D1_COLOR_F fg = primary ? theme.accent_text : theme.text;
            DrawIconText(rc.left, rc.top + 5.0f * s, rc.right - rc.left, 18.0f * s,
                         glyph, fallback, primary ? theme.accent_text
                                                  : theme.text_secondary, 1.0f);
            IDWriteTextFormat* fmt = compositor_->SmallFormat();
            if (!fmt) return;
            // Draw through a private text layout so the shared format is never
            // mutated; narrow cells ellipsize (hover tooltip keeps full text).
            const float textW = rc.right - rc.left - 4.0f * s;
            const float textH = rc.bottom - rc.top - 28.0f * s;
            ComPtr<IDWriteTextLayout> tl;
            if (compositor_->DwriteFactory() &&
                SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                    label, static_cast<UINT32>(wcslen(label)), fmt,
                    textW, textH, &tl)) && tl.get()) {
                tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                const DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                tl->SetTrimming(&trim, nullptr);
                MakeBrush(dc, fg, brText_);
                dc->DrawTextLayout(D2D1::Point2F(rc.left + 2.0f * s, rc.top + 26.0f * s),
                                   tl.get(), brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        };
        rowButton(hit.open, L"\xE8B7", L"O", kDetailsButtonLabels[0],
                  IsHovered(vm, HitTestResult::DetailsOpen), true);
        rowButton(hit.new_tab, L"\xE8A0", L"T", kDetailsButtonLabels[1],
                  IsHovered(vm, HitTestResult::DetailsNewTab), false);
        rowButton(hit.copy_path, L"\xE8C8", L"C", kDetailsButtonLabels[2],
                  IsHovered(vm, HitTestResult::DetailsCopyPath), false);
        rowButton(hit.more, L"\xE712", L"...", kDetailsButtonLabels[3],
                  IsHovered(vm, HitTestResult::DetailsMore), false);
        y += 48.0f * s + 4.0f * s;
    }

    auto infoRow = [&](const wchar_t* label, const std::wstring& value, float& iy) {
        text(label, D2D1::RectF(panel.left + pad, iy, panel.left + pad + 96.0f * s,
                                iy + 18.0f * s),
             compositor_->SmallFormat(), theme.text_secondary);
        text(value, D2D1::RectF(panel.left + pad + 100.0f * s, iy, panel.right - pad,
                                iy + 18.0f * s),
             compositor_->SmallFormat(), theme.text);
        iy += 18.0f * s;
    };

    if (d.multi_count > 1) {
        text(L"\u4FE1\u606F", D2D1::RectF(panel.left + 12.0f * s, y, panel.right - 12.0f * s,
                                          y + 20.0f * s),
             compositor_->SmallFormat(), theme.accent);
        float iy = y + 20.0f * s;
        infoRow(L"位置", d.location_text, iy);
        infoRow(L"已知大小", d.size_text, iy);
        y = iy + 8.0f * s;
    }

    if (d.multi_count <= 1) {
        for (const auto& def : kDetailsSections) {
            // Section separator (matches the 8 DIP gap in LayoutDetailsPanel).
            y += 8.0f * s;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRect(dc, brStrokeCard_.get(), panel.left + pad, y - 4.5f * s,
                     panel.right - panel.left - pad * 2.0f, 1.0f);
            section(def.label, y, def.id);
            y += 28.0f * s;
            if ((d.collapsed_mask >> def.id) & 1u) { y += 6.0f * s; continue; }
            switch (def.id) {
            case 0: { // 基本信息
                float iy = y;
                infoRow(L"位置", d.location_text, iy);
                infoRow(L"类型", !d.type_text.empty()
                            ? d.type_text : FormatListType(d.name, d.is_dir), iy);
                infoRow(L"大小", d.size_pending ? std::wstring(L"\u8BA1\u7B97\u4E2D\u2026")
                                                : d.size_text, iy);
                if (d.is_dir) infoRow(L"\u5305\u542B", d.contains_text, iy);
                infoRow(L"\u521B\u5EFA\u65F6\u95F4", d.created_text, iy);
                infoRow(L"\u4FEE\u6539\u65F6\u95F4", d.modified_text, iy);
                infoRow(L"\u6700\u540E\u8BBF\u95EE", d.accessed_text, iy);
                for (const auto& property : d.preview_properties)
                    infoRow(property.label.c_str(), property.value, iy);
                y = iy + 8.0f * s;
                break;
            }
            case 1: { // 属性: writable checkboxes + 高级…
                fluent::ControlState roState;
                roState.checked = (d.attrs & FILE_ATTRIBUTE_READONLY) != 0;
                roState.hovered = IsHovered(vm, HitTestResult::DetailsAttrToggle, 0);
                painter_.DrawCheckBox(D2D1::RectF(panel.left + pad, y, panel.right - pad,
                                                  y + 24.0f * s),
                                      L"\u53EA\u8BFB", roState);
                fluent::ControlState hidState;
                hidState.checked = (d.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
                hidState.hovered = IsHovered(vm, HitTestResult::DetailsAttrToggle, 1);
                painter_.DrawCheckBox(D2D1::RectF(panel.left + pad, y + 24.0f * s,
                                                  panel.right - pad, y + 48.0f * s),
                                      L"\u9690\u85CF", hidState);
                {
                    const bool hot = IsHovered(vm, HitTestResult::DetailsAttrToggle, 2);
                    const auto& rc = hit.attr_advanced;
                    if (hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                        rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
                    }
                    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                             brStrokeCard_.get(), 1.0f);
                    IDWriteTextFormat* fmt = compositor_->SmallFormat();
                    if (fmt) {
                        const auto old = fmt->GetTextAlignment();
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        MakeBrush(dc, theme.text, brText_);
                        DrawTextRect(dc, fmt, brText_.get(), L"\u9AD8\u7EA7\u2026",
                                     rc.left, rc.top, rc.right - rc.left,
                                     rc.bottom - rc.top);
                        fmt->SetTextAlignment(old);
                    }
                }
                y += 48.0f * s + 8.0f * s;
                break;
            }
            case 2: { // 标签: 添加标签 row + preset chips (toggle on click)
                ghostButton(hit.tag_add, kIconAdd, L"\u6DFB\u52A0\u6807\u7B7E",
                            IsHovered(vm, HitTestResult::DetailsTagAdd));
                for (size_t i = 0; i < hit.preset_chips.size(); ++i) {
                    const auto& rc = hit.preset_chips[i];
                    const auto& chip = d.preset_tags[i];
                    const bool hot = IsHovered(vm, HitTestResult::DetailsPresetTag,
                                               chip.tag_index);
                    D2D1_COLOR_F fill = chip.color;
                    fill.a *= chip.assigned ? (hot ? 0.34f : 0.24f)
                                            : (hot ? 0.10f : 0.05f);
                    MakeBrush(dc, fill, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                    rc.right - rc.left, rc.bottom - rc.top, 11.0f * s);
                    D2D1_COLOR_F border = chip.color;
                    border.a *= chip.assigned ? 0.55f : 0.22f;
                    MakeBrush(dc, border, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 11.0f * s, 11.0f * s),
                                             brStrokeCard_.get(), 1.0f);
                    MakeBrush(dc, chip.color, brAccent_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rc.left + 11.0f * s,
                        (rc.top + rc.bottom) * 0.5f), 3.5f * s, 3.5f * s), brAccent_.get());
                    text(chip.name, D2D1::RectF(rc.left + 19.0f * s, rc.top,
                                                rc.right - 4.0f * s, rc.bottom),
                         compositor_->SmallFormat(),
                         chip.assigned ? theme.text : theme.text_secondary);
                }
                y = hit.preset_chips.empty() ? (y + 26.0f * s + 6.0f * s + 8.0f * s)
                    : (hit.preset_chips.back().bottom + 8.0f * s);
                break;
            }
            case 3: { // 安全
                float iy = y;
                infoRow(L"\u6240\u6709\u8005",
                        d.owner_text.empty() ? std::wstring(L"\u52A0\u8F7D\u4E2D\u2026")
                                             : d.owner_text, iy);
                {
                    const bool hot = IsHovered(vm, HitTestResult::DetailsSecurityChange);
                    text(L"\u66F4\u6539",
                         D2D1::RectF(hit.security_change.left, y,
                                     hit.security_change.right, y + 18.0f * s),
                         compositor_->SmallFormat(),
                         hot ? theme.accent_hover : theme.accent, true);
                }
                infoRow(L"\u6743\u9650",
                        d.permissions_text.empty() ? std::wstring(L"\u52A0\u8F7D\u4E2D\u2026")
                                                   : d.permissions_text, iy);
                y = iy + 8.0f * s;
                break;
            }
            case 4: { // 其他
                float iy = y;
                infoRow(L"\u9A71\u52A8\u5668",
                        d.drive_text.empty() ? std::wstring(L"\u52A0\u8F7D\u4E2D\u2026")
                                             : d.drive_text, iy);
                infoRow(L"\u6587\u4EF6\u7CFB\u7EDF",
                        d.fs_text.empty() ? std::wstring(L"\u52A0\u8F7D\u4E2D\u2026")
                                          : d.fs_text, iy);
                infoRow(L"\u53EF\u7528\u7A7A\u95F4",
                        d.free_space_text.empty() ? std::wstring(L"\u52A0\u8F7D\u4E2D\u2026")
                                                  : d.free_space_text, iy);
                y = iy + 8.0f * s;
                break;
            }
            default: break;
            }
        }
    }
    dc->PopAxisAlignedClip();
    dc->PopAxisAlignedClip();
}

float MainRenderer::DetailsContentHeightDip(const WindowViewModel& vm, float w, float h) {
    const D2D1_RECT_F panel = DetailsPanelRect(w, h);
    if (panel.right - panel.left <= 1.0f) return 0.0f;
    DetailsHitRects hit;
    LayoutDetailsPanel(panel, scale_, vm.details,
                       compositor_ ? compositor_->DwriteFactory() : nullptr,
                       compositor_ ? compositor_->SmallFormat() : nullptr,
                       DetailsPreviewHeight(panel, scale_), hit);
    return hit.content_height_dip;
}

void MainRenderer::DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    if (!vm.pane_slots.empty()) {
        for (int i = 0; i < static_cast<int>(vm.pane_slots.size()); ++i) {
            const auto& slot = vm.pane_slots[static_cast<size_t>(i)];
            DrawSinglePane(vm, slot.pane, slot.rect, i, slot.focused, slot.target, theme);
        }
        for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
            const auto& sp = vm.splitters[static_cast<size_t>(i)];
            fluent::SplitterSpec spec;
            spec.bounds = sp.hit_rect;
            spec.vertical = sp.vertical;
            spec.state.hovered = vm.hover_region == static_cast<int>(HitTestResult::Splitter) &&
                                 vm.hover_control_index == i;
            spec.state.pressed = vm.splitter_pressed && spec.state.hovered;
            painter_.DrawSplitter(spec);
        }
        return;
    }
    DrawSinglePane(vm, vm.pane, ContentRect(rect.right, rect.bottom), 0, true, false, theme);
}

namespace {
struct PaneEmptyLayout {
    D2D1_RECT_F art{};
    D2D1_RECT_F title{};
    D2D1_RECT_F message{};
    D2D1_RECT_F action{};
    bool show_message = false;
    bool show_action = false;
};

PaneEmptyLayout MakePaneEmptyLayout(const D2D1_RECT_F& bounds, float scale,
                                    bool can_create) {
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
    const float maxArtW = std::max(72.0f * scale,
        std::min(200.0f * scale, width - 32.0f * scale));
    const float maxArtH = std::max(60.0f * scale, height - fixedH - 44.0f * scale);
    const float artW = std::min(maxArtW, maxArtH * 512.0f / 360.0f);
    const float artH = artW * 360.0f / 512.0f;
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
} // namespace

void MainRenderer::DrawPaneEmptyState(const WindowViewModel& vm, const PaneViewModel& pane,
                                      const D2D1_RECT_F& bounds, int pane_index,
                                      const Theme& theme) {
    if (!pane.filter_text.empty()) {
        fluent::EmptyStateSpec empty;
        empty.bounds = bounds;
        empty.glyph = kIconSearch;
        empty.title = L"\u6CA1\u6709\u5339\u914D\u9879";
        painter_.DrawEmptyState(empty);
        return;
    }
    if (!pane.is_file_system) {
        fluent::EmptyStateSpec empty;
        empty.bounds = bounds;
        empty.glyph = kIconFile;
        empty.title = L"\u6682\u65E0\u5185\u5BB9";
        painter_.DrawEmptyState(empty);
        return;
    }

    const PaneEmptyLayout layout = MakePaneEmptyLayout(bounds, scale_, pane.can_create);
    const float svgOpacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
    if (!DrawEmptyStateSvg(layout.art, svgOpacity)) {
        const float icon = std::min(72.0f * scale_, layout.art.bottom - layout.art.top);
        DrawFolderIcon((layout.art.left + layout.art.right - icon) * 0.5f,
                       (layout.art.top + layout.art.bottom - icon) * 0.5f, icon, theme);
    }
    painter_.DrawText(L"\u6B64\u6587\u4EF6\u5939\u4E3A\u7A7A", layout.title,
                      compositor_->HeaderFormat(), theme.text,
                      fluent::HorizontalAlignment::Center);
    if (layout.show_message) {
        painter_.DrawText(L"\u6CA1\u6709\u4EFB\u4F55\u6587\u4EF6\u6216\u6587\u4EF6\u5939", layout.message,
                          compositor_->SmallFormat(), theme.text_secondary,
                          fluent::HorizontalAlignment::Center);
    }
    if (layout.show_action) {
        const bool hovered = vm.hover_region == static_cast<int>(HitTestResult::PaneEmptyNewFolder) &&
                             vm.hover_control_index == pane_index;
        painter_.FillRoundedRect(layout.action, 6.0f * scale_,
                                 hovered ? theme.fill_input_hover : theme.fill_input);
        painter_.StrokeRoundedRect(layout.action, 6.0f * scale_, theme.stroke_card);
        const float iconW = 32.0f * scale_;
        painter_.DrawText(L"\u65B0\u5EFA\u6587\u4EF6\u5939",
            D2D1::RectF(layout.action.left + 8.0f * scale_, layout.action.top,
                        layout.action.right - iconW, layout.action.bottom),
            compositor_->SmallFormat(), theme.text, fluent::HorizontalAlignment::Center);
        MakeBrush(compositor_->Dc(), theme.stroke_divider, brStrokeDivider_);
        compositor_->Dc()->DrawLine(
            D2D1::Point2F(layout.action.right - iconW, layout.action.top + 6.0f * scale_),
            D2D1::Point2F(layout.action.right - iconW, layout.action.bottom - 6.0f * scale_),
            brStrokeDivider_.get(), 1.0f);
        DrawIconText(layout.action.right - iconW, layout.action.top, iconW,
                     layout.action.bottom - layout.action.top,
                     kIconAdd, L"+", theme.text, 0.72f);
    }
}

void MainRenderer::DrawSinglePane(const WindowViewModel& vm, const PaneViewModel& pane,
                                  const D2D1_RECT_F& bounds, int pane_index, bool focused, bool target,
                                  const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float x = bounds.left;
    const float y0 = bounds.top;
    const float w = bounds.right - bounds.left;
    const float bottom = bounds.bottom;
    if (w <= 1.0f || bottom - y0 <= 1.0f) return;

    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = y0;
    const std::wstring& title = pane.header_text;
    const D2D1_RECT_F filterRc = FilterBoxRect(bounds, pane.filter_expand);
    const D2D1_RECT_F mediumRc = PaneMediumIconsRect(bounds, pane.filter_expand);
    const D2D1_RECT_F viewRc = PaneViewButtonRect(bounds, pane.filter_expand);
    const float textLeft = x + 8.0f * scale_;
    const float textRight = std::max(textLeft, mediumRc.left - 8.0f * scale_);
    const float textWidth = textRight - textLeft;
    MakeBrush(dc, theme.text, brText_);
    DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), title,
        textLeft, y, textWidth, pane_header_height_);

    auto headerIconButton = [&](const D2D1_RECT_F& rc, HitTestResult::Region region,
                                const wchar_t* glyph, const wchar_t* fallback,
                                bool active, float glyphScale) {
        const bool hovered = vm.hover_region == static_cast<int>(region) &&
                             vm.hover_control_index == pane_index;
        const D2D1_COLOR_F fill = active
            ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f)
            : (hovered ? theme.fill_hover : kTransparent);
        DrawButton(rc, theme, fill, glyph, fallback,
                   active ? theme.accent : theme.text,
                   true, true, glyphScale);
    };
    headerIconButton(mediumRc, HitTestResult::PaneMediumIcons,
                     L"\xE7F4", L"M", pane.view_mode == ViewMode::MediumIcons, 0.66f);
    headerIconButton(viewRc, HitTestResult::PaneViewButton,
                     kIconView, L"=", false, 0.76f);

    if (pane.filter_expand <= 0.015f && !(focused && vm.filter_editing)) {
        const bool activeFilter = !pane.filter_text.empty();
        const bool hovered = vm.hover_region == static_cast<int>(HitTestResult::FilterBox) &&
                             vm.hover_control_index == pane_index;
        const D2D1_COLOR_F fill = activeFilter
            ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f)
            : (hovered ? theme.fill_hover : kTransparent);
        DrawButton(filterRc, theme, fill, kIconFilter, L"F",
                   activeFilter ? theme.accent : theme.text,
                   true, true, 0.76f);
    } else {
        fluent::TextFieldSpec filter{};
        filter.bounds = filterRc;
        filter.placeholder = L"\u6309\u540D\u79F0/\u6807\u7B7E\u8FC7\u6EE4...";
        filter.text = pane.filter_text;
        filter.leading_glyph = kIconFilter;
        filter.compact_leading_glyph = true;
        filter.suppress_text = focused && vm.filter_editing;
        filter.state.focused = focused && vm.filter_editing;
        filter.state.hovered = vm.hover_region == static_cast<int>(HitTestResult::FilterBox) &&
                               vm.hover_control_index == pane_index;
        painter_.DrawTextField(filter);
    }
    y += pane_header_height_;

    const float bannerH = pane.banner_message.empty() ? 0.0f : 36.0f * scale_;
    if (bannerH > 0.0f) {
        fluent::InfoBarSpec bar;
        bar.bounds = D2D1::RectF(x + 8.0f * scale_, y, x + w - 8.0f * scale_, y + bannerH - 4.0f * scale_);
        bar.title = pane.banner_title;
        bar.message = pane.banner_message;
        bar.kind = static_cast<fluent::InfoBarKind>(std::clamp(pane.banner_kind, 0, 3));
        bar.show_close = false;
        painter_.DrawInfoBar(bar);
        y += bannerH;
    }

    if (ShowsColumnHeader(pane.view_mode)) {
        // Re-set the brush: earlier drawing (tray deck pills, toolbar) may
        // have left a different color on this shared member.
        MakeBrush(dc, theme.fill_input, brFillInput_);
        FillRect(dc, brFillInput_.get(), x, y, w, column_header_height_);
        FillRect(dc, brStrokeDivider_.get(), x, y + column_header_height_ - 1, w, 1);
        const DetailsColumnLayout columns = DetailsColumns(
            bounds, pane.details_column_dividers);
        float cx = columns.left;
        auto drawCol = [&](const std::wstring& label, SortColumn col, float cw, bool right = false) {
            const bool active = pane.sort_column == col;
            MakeBrush(dc, active ? theme.accent : theme.text, brText_);
            IDWriteTextFormat* fmt = compositor_->HeaderFormat();
            const float textInset = 8.0f * scale_;
            const float contentLeft = cx + textInset;
            const float availableW = std::max(0.0f, cw - textInset * 2.0f);
            const float iconW = active ? 12.0f * scale_ : 0.0f;
            const float iconGap = active ? 3.0f * scale_ : 0.0f;
            const float labelW = std::min(
                MeasureTextWidth(compositor_->DwriteFactory(), fmt, label),
                std::max(0.0f, availableW - iconW - iconGap));
            const float groupW = labelW + iconGap + iconW;
            const float groupLeft = right
                ? contentLeft + availableW - groupW : contentLeft;
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            DrawTextRect(dc, fmt, brText_.get(), label, groupLeft, y,
                         labelW, column_header_height_);
            if (active) {
                DrawIconText(groupLeft + labelW + iconGap, y, iconW,
                             column_header_height_,
                             pane.sort_direction == SortDirection::Asc
                                 ? kIconChevronUp : kIconChevronDown,
                             pane.sort_direction == SortDirection::Asc ? L"^" : L"v",
                             theme.accent, 0.75f);
            }
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cx += cw;
        };
        drawCol(L"\u540D\u79F0", SortColumn::Name, columns.widths[0], false);
        drawCol(L"\u4FEE\u6539\u65E5\u671F", SortColumn::Mtime, columns.widths[1], false);
        drawCol(L"\u7C7B\u578B", SortColumn::Type, columns.widths[2], false);
        drawCol(L"\u5927\u5C0F", SortColumn::Size, columns.widths[3], true);
        if ((vm.hover_region == static_cast<int>(HitTestResult::ColumnDivider) ||
             vm.column_resize_pressed) && vm.hover_pane_index == pane_index) {
            const int divider = std::clamp(vm.hover_control_index, 0, 2);
            const float dividerX = columns.DividerX(divider);
            FillRect(dc, brAccent_.get(), dividerX - scale_, y + 4.0f * scale_,
                     2.0f * scale_, column_header_height_ - 8.0f * scale_);
        }
        y += column_header_height_;
    }
    float listH = bottom - y;
    dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (!pane.loading && pane.EntryCount() == 0)
        DrawPaneEmptyState(vm, pane, D2D1::RectF(x, y, x + w, bottom), pane_index, theme);
    else
        DrawList(pane, x, y, w, listH, theme, vm.hover_region, vm.hover_control_index);
    dc->PopAxisAlignedClip();

    const float radius = theme.radius_control * scale_;
    D2D1_COLOR_F frameColor = theme.stroke_card;
    if (focused) frameColor = WithAlpha(theme.accent, 0.28f);
    else if (target) frameColor = WithAlpha(theme.accent, 0.22f);
    MakeBrush(dc, frameColor, brAccent_);
    const float strokeW = focused ? 1.5f * scale_ : 1.0f * scale_;
    D2D1_ROUNDED_RECT frame = D2D1::RoundedRect(
        D2D1::RectF(bounds.left + 0.5f * scale_, bounds.top + 0.5f * scale_,
                    bounds.right - 0.5f * scale_, bounds.bottom - 0.5f * scale_),
        radius, radius);
    if (target && !focused) {
        if (!dashStroke_.get() && dc) {
            ID2D1Factory* factory = nullptr;
            dc->GetFactory(&factory);
            if (factory) {
                D2D1_STROKE_STYLE_PROPERTIES props{};
                props.dashStyle = D2D1_DASH_STYLE_DASH;
                props.dashCap = D2D1_CAP_STYLE_FLAT;
                factory->CreateStrokeStyle(props, nullptr, 0, &dashStroke_);
                factory->Release();
            }
        }
        dc->DrawRoundedRectangle(frame, brAccent_.get(), 1.5f * scale_, dashStroke_.get());
    } else {
        dc->DrawRoundedRectangle(frame, brAccent_.get(), strokeW);
    }

    dc->PopAxisAlignedClip();
}

void MainRenderer::DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                                     const Theme& theme, bool selected) {
    (void)selected;
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() ||
        !compositor_->TextFormat() || name.empty() || w <= 1.0f) {
        return;
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory3* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->TextFormat();
    MakeBrush(dc, theme.text, brText_);

    const auto old_wrap = fmt->GetWordWrapping();
    const auto old_align = fmt->GetTextAlignment();
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    const std::wstring shown = FitFileName(factory, fmt, name, w);
    const D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                 D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    fmt->SetWordWrapping(old_wrap);
    fmt->SetTextAlignment(old_align);
}

void MainRenderer::DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color) {
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() || name.empty()) return;
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(
            name.c_str(), static_cast<UINT32>(name.size()), compositor_->TextFormat(),
            width, height, &layout)) || !layout.get()) {
        return;
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    compositor_->DwriteFactory()->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.get());
    MakeBrush(compositor_->Dc(), color, brText_);
    compositor_->Dc()->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(),
                                      brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static std::wstring FitFileName(IDWriteFactory3* factory, IDWriteTextFormat* fmt,
                                const std::wstring& name, float max_w) {
    if (name.empty() || max_w <= 1.0f) return {};
    if (MeasureTextWidth(factory, fmt, name) <= max_w) return name;

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
    const float tail_w = MeasureTextWidth(factory, fmt, tail);
    if (tail_w >= max_w) {
        const std::wstring ellip(1, L'\u2026');
        size_t lo = 0, hi = name.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (MeasureTextWidth(factory, fmt, name.substr(0, mid) + ellip) <= max_w) lo = mid;
            else hi = mid - 1;
        }
        return lo == 0 ? ellip : name.substr(0, lo) + ellip;
    }

    const float stem_budget = max_w - tail_w;
    size_t lo = 0, hi = stem.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (MeasureTextWidth(factory, fmt, stem.substr(0, mid)) <= stem_budget) lo = mid;
        else hi = mid - 1;
    }
    return stem.substr(0, lo) + tail;
}

static float OverlapTagsWidth(int n, float diameter) {
    if (n <= 0) return 0.0f;
    return diameter + static_cast<float>(n - 1) * diameter * 0.52f;
}

static float SpreadTagsWidth(int n, float diameter, float gap) {
    if (n <= 0) return 0.0f;
    return static_cast<float>(n) * diameter + static_cast<float>(n - 1) * gap;
}

static float TagStepForLeftover(int n, float diameter, float spread_gap, float leftover) {
    if (n <= 1) return 0.0f;
    if (SpreadTagsWidth(n, diameter, spread_gap) <= leftover + 0.5f)
        return diameter + spread_gap;
    return diameter * 0.52f;
}

// Name column: filename compresses first. Tags sit after the name — spread
// when leftover room fits every dot, otherwise overlap. Star / new tab / more
// dock to the column's right edge so trailing chrome stays a fixed width.
struct NameTrail {
    float name_x = 0.0f;
    float name_w = 0.0f;
    int tag_n = 0;
    float tag_r = 0.0f;
    float tag_step = 0.0f;
    float tag_x0 = 0.0f;
    float tag_cy = 0.0f;
    D2D1_RECT_F star{};
    D2D1_RECT_F new_tab{};
    D2D1_RECT_F more{};
    bool show_star = false;
    bool show_new_tab = false;
    bool show_more = false;
};

static NameTrail LayoutNameTrail(float name_x, float text_y, float text_h,
                                 float col_right, float cell_top, float cell_bottom,
                                 float scale, const std::wstring& name, int tag_n,
                                 bool show_star, bool show_new_tab, bool show_more,
                                 IDWriteFactory3* factory, IDWriteTextFormat* fmt) {
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

    // Reserve the compact (overlapped) cluster so a long name still
    // compresses first. Spread only if leftover after the fitted name fits.
    const float tags_w = OverlapTagsWidth(t.tag_n, diameter);
    const float tags_gap = t.tag_n > 0 ? gap : 0.0f;
    const float budget = std::max(0.0f, dock - name_x - tags_gap - tags_w);
    const std::wstring fitted = FitFileName(factory, fmt, name, budget);
    t.name_w = std::min(budget, MeasureTextWidth(factory, fmt, fitted));
    if (t.tag_n > 0) {
        t.tag_x0 = name_x + t.name_w + gap;
        const float leftover = std::max(0.0f, dock - t.tag_x0);
        t.tag_step = TagStepForLeftover(t.tag_n, diameter, gap, leftover);
    }
    return t;
}

D2D1_RECT_F MainRenderer::RenameFieldRect(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                          int source_index) {
    // Mirror of the frame geometry in DrawList for vm.rename_index — keep in sync.
    if (!compositor_ || !compositor_->DwriteFactory() || source_index < 0) return {};
    const int view = vm.ViewIndex(source_index);
    if (view < 0) return {};
    const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                      scale_, row_height_dip_);
    const D2D1_RECT_F cell = layout.ItemRect(view);
    const D2D1_RECT_F nameRc = layout.NameRect(view);
    const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                          vm.view_mode == ViewMode::LargeIcons ||
                          vm.view_mode == ViewMode::MediumIcons;
    float nameX = nameRc.left;
    float textY = nameRc.top;
    float textH = std::max(1.0f, nameRc.bottom - nameRc.top);
    const float dot = 8.0f * scale_;
    const std::vector<D2D1_COLOR_F>* tagDots = nullptr;
    const std::vector<int>* tagIndices = vm.tag_catalog
        ? vm.tag_catalog->TagIndicesForPath(e.path) : nullptr;
    if (!tagIndices && vm.tag_dots) {
        const auto found = vm.tag_dots->find(source_index);
        if (found != vm.tag_dots->end()) tagDots = &found->second;
    }
    const size_t totalTags = tagIndices ? tagIndices->size() : (tagDots ? tagDots->size() : 0);
    const int tagDotCount = static_cast<int>(std::min<size_t>(3, totalTags));
    const float nameColRight = vm.view_mode == ViewMode::Details
        ? DetailsColumns(list, vm.details_column_dividers).DividerX(0) - margin_
        : nameRc.right;
    if (iconGrid && tagDotCount > 0) {
        const float fullNameW = MeasureTextWidth(
            compositor_->DwriteFactory(), compositor_->TextFormat(), e.name);
        const float nameGap = 4.0f * scale_;
        const float cellW = nameRc.right - nameRc.left;
        const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
        const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
            ? SpreadTagsWidth(tagDotCount, dot, nameGap)
            : OverlapTagsWidth(tagDotCount, dot);
        const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
        nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
        const float lineH = std::min(textH, 24.0f * scale_);
        textY = nameRc.top + (textH - lineH) * 0.5f;
        textH = lineH;
    }
    const NameTrail trail = LayoutNameTrail(
        nameX, textY, textH, nameColRight, cell.top, cell.bottom, scale_,
        e.name, tagDotCount, false, false, false,
        compositor_->DwriteFactory(), compositor_->TextFormat());
    const float field_w = std::max(40.0f * scale_, trail.name_w);
    const float field_h = std::max(22.0f * scale_, std::min(textH, 30.0f * scale_));
    return D2D1::RectF(nameX, textY, nameX + field_w, textY + field_h);
}

void MainRenderer::DrawList(const PaneViewModel& vm, float x, float y, float w, float h,
                            const Theme& theme, int hover_region, int hover_control_index) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    MakeBrush(dc, theme.accent, brAccent_);
    if (vm.loading) {
        // Directory enumeration is asynchronous. Leave the list quiet for its
        // brief initial frame instead of presenting it like a search request.
        return;
    }
    const size_t entryCount = vm.EntryCount();
    if (entryCount == 0) return;
    const D2D1_RECT_F viewport = D2D1::RectF(x, y, x + w, y + h);
    ViewLayout layout(vm.view_mode, viewport, entryCount, vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    const auto [startIdx, endIdx] = layout.VisibleRange();
    const DetailsColumnLayout detailsColumns = DetailsColumns(
        viewport, vm.details_column_dividers);
    const float dateW = detailsColumns.widths[1];
    const float typeW = detailsColumns.widths[2];
    const float sizeW = detailsColumns.widths[3];

    for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
        const D2D1_RECT_F cell = layout.ItemRect(i);
        if (cell.right < x || cell.left > x + w || cell.bottom < y || cell.top > y + h) continue;
        const int src = vm.SourceIndex(i);
        if (src < 0) continue;
        const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));

        bool selected = vm.IsRowSelected(src);
        bool focused = (src == vm.selected_index);
        bool hover = (src == vm.hover_index);
        bool cut = vm.cut_names.contains(e.name);

        const float inset = 4.0f * scale_;
        if (hover) {
            FillRoundedRect(dc, brFillHover_.get(), cell.left + inset, cell.top + scale_,
                std::max(0.0f, cell.right - cell.left - inset * 2),
                std::max(0.0f, cell.bottom - cell.top - scale_ * 2),
                theme.radius_control * scale_);
        }
        if (selected) {
            const D2D1_RECT_F sel = D2D1::RectF(
                cell.left + inset, cell.top + scale_,
                cell.right - inset, cell.bottom - scale_);
            FillRoundedRect(dc, brFillSelected_.get(), sel.left, sel.top,
                std::max(0.0f, sel.right - sel.left),
                std::max(0.0f, sel.bottom - sel.top),
                theme.radius_control * scale_);
            if (focused) {
                FillRoundedAccent(dc, brAccent_.get(), sel,
                    theme.radius_control * scale_, 3.0f * scale_, AccentEdge::Left);
            }
        }
        if (src == vm.drop_target_index) {
            D2D1_RECT_F rc = D2D1::RectF(cell.left + inset, cell.top + scale_,
                                         cell.right - inset, cell.bottom - scale_);
            dc->DrawRoundedRectangle(
                D2D1::RoundedRect(rc, theme.radius_control * scale_, theme.radius_control * scale_),
                brAccent_.get(), 2.0f * scale_);
        }

        const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                              vm.view_mode == ViewMode::LargeIcons ||
                              vm.view_mode == ViewMode::MediumIcons;
        D2D1_RECT_F nameRc = layout.NameRect(i);
        D2D1_RECT_F iconRect = layout.IconRect(i);
        const float snappedIconW = std::max(1.0f, std::round(iconRect.right - iconRect.left));
        const float snappedIconH = std::max(1.0f, std::round(iconRect.bottom - iconRect.top));
        iconRect.left = std::round(iconRect.left);
        iconRect.top = std::round(iconRect.top);
        iconRect.right = iconRect.left + snappedIconW;
        iconRect.bottom = iconRect.top + snappedIconH;
        const float iconX = iconRect.left;
        const float iconY = iconRect.top;
        const float renderedIconSize = std::min(snappedIconW, snappedIconH);
        long requestedPixels = std::lround(renderedIconSize);
        if (vm.view_mode == ViewMode::ExtraLargeIcons || vm.view_mode == ViewMode::LargeIcons)
            requestedPixels = std::max(256l, requestedPixels);
        else if (vm.view_mode == ViewMode::MediumIcons)
            requestedPixels = std::max(128l, requestedPixels);
        const bool drewThumbnail = UsesThumbnails(vm.view_mode) &&
            thumbnail_cache_.Draw(dc, iconRect, e.path, e.attrs,
                static_cast<uint32_t>(std::clamp(requestedPixels, 32l, 512l)),
                vm.view_generation, e.modified_value, e.size_value)
                == PreviewDrawResult::Bitmap;
        if (!drewThumbnail) DrawEntryIcon(e, iconX, iconY, renderedIconSize, theme);

        float nameX = nameRc.left;
        float textY = nameRc.top;
        float textH = std::max(1.0f, nameRc.bottom - nameRc.top);
        const float dot = 8.0f * scale_;
        const std::vector<D2D1_COLOR_F>* tagDots = nullptr;
        const std::vector<int>* tagIndices = vm.tag_catalog
            ? vm.tag_catalog->TagIndicesForPath(e.path) : nullptr;
        if (!tagIndices && vm.tag_dots) {
            const auto tagIt = vm.tag_dots->find(src);
            if (tagIt != vm.tag_dots->end()) tagDots = &tagIt->second;
        }
        const size_t totalTags = tagIndices ? tagIndices->size() : (tagDots ? tagDots->size() : 0);
        const int tagDotCount = static_cast<int>(std::min<size_t>(3, totalTags));
        const bool showRowActions = vm.view_mode == ViewMode::Details &&
            src != vm.rename_index &&
            (hover || (selected && vm.selected_count == 1) || e.starred);
        const float nameColRight = vm.view_mode == ViewMode::Details
            ? detailsColumns.DividerX(0) - margin_
            : nameRc.right;
        const bool showStar = showRowActions;
        const bool showNewTab = showRowActions && hover && e.is_dir;
        const bool showMore = showRowActions && (hover || (selected && vm.selected_count == 1));
        if (iconGrid && tagDotCount > 0) {
            const float fullNameW = MeasureTextWidth(
                compositor_->DwriteFactory(), compositor_->TextFormat(), e.name);
            const float nameGap = 4.0f * scale_;
            const float cellW = nameRc.right - nameRc.left;
            const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
            const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
                ? SpreadTagsWidth(tagDotCount, dot, nameGap)
                : OverlapTagsWidth(tagDotCount, dot);
            const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
            nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
            const float lineH = std::min(textH, 24.0f * scale_);
            textY = nameRc.top + (textH - lineH) * 0.5f;
            textH = lineH;
        }
        const NameTrail trail = LayoutNameTrail(
            nameX, textY, textH, nameColRight, cell.top, cell.bottom, scale_,
            e.name, tagDotCount, showStar, showNewTab, showMore,
            compositor_->DwriteFactory(), compositor_->TextFormat());
        if (src == vm.rename_index) {
            const D2D1_RECT_F fieldRc = RenameFieldRect(vm, viewport, src);
            fluent::ControlState fieldState{};
            fieldState.focused = true;
            painter_.DrawTextFieldFrame(fieldRc, fieldState);
        } else {
            D2D1_COLOR_F nameColor = cut ? WithAlpha(theme.text, 0.55f) : theme.text;
            MakeBrush(dc, nameColor, brText_);
            if (iconGrid && tagDotCount == 0) {
                DrawCenteredIconName(e.name, nameRc, nameColor);
            } else {
                DrawTruncatedName(e.name, trail.name_x, textY, trail.name_w, textH, theme, selected);
            }
            D2D1_COLOR_F halo = theme.bg;
            halo.a = 1.0f;
            for (int d = trail.tag_n - 1; d >= 0; --d) {
                D2D1_COLOR_F color{};
                if (tagIndices && d < static_cast<int>(tagIndices->size())) {
                    const int tagIndex = (*tagIndices)[static_cast<size_t>(d)];
                    if (tagIndex >= 0 && tagIndex < static_cast<int>(vm.tag_catalog->tags.size()))
                        color = HexColor(vm.tag_catalog->tags[static_cast<size_t>(tagIndex)].rgb);
                } else if (tagDots) {
                    color = (*tagDots)[static_cast<size_t>(d)];
                }
                const float cx = trail.tag_x0 + trail.tag_r + static_cast<float>(d) * trail.tag_step;
                const float cy = trail.tag_cy;
                MakeBrush(dc, halo, brFillInput_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy),
                    trail.tag_r + 1.5f * scale_, trail.tag_r + 1.5f * scale_), brFillInput_.get());
                MakeBrush(dc, color, brAccent_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), trail.tag_r, trail.tag_r),
                                brAccent_.get());
                if (IsHighContrast()) {
                    MakeBrush(dc, theme.text, brText_);
                    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), trail.tag_r, trail.tag_r),
                                    brText_.get(), 1.0f * scale_);
                }
            }
        }

        MakeBrush(dc, cut ? WithAlpha(theme.text_secondary, 0.55f) : theme.text_secondary, brTextSecondary_);
        if (vm.view_mode == ViewMode::Details) {
            float colX = detailsColumns.DividerX(0);
            const float textInset = 8.0f * scale_;
            DrawTextRect(dc, compositor_->TextFormat(), brTextSecondary_.get(), e.date_text,
                colX + textInset, textY, std::max(0.0f, dateW - textInset * 2.0f), textH);
            colX += dateW;
            DrawTextRect(dc, compositor_->TextFormat(), brTextSecondary_.get(), e.type_text,
                colX + textInset, textY, std::max(0.0f, typeW - textInset * 2.0f), textH);
            colX += typeW;
            IDWriteTextFormat* fmt = compositor_->TextFormat();
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            DrawTextRect(dc, fmt, brTextSecondary_.get(), e.size_text,
                colX + textInset, textY, std::max(0.0f, sizeW - textInset * 2.0f), textH);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        } else if (vm.view_mode == ViewMode::Tiles || vm.view_mode == ViewMode::Content) {
            std::wstring meta = e.type_text;
            if (!e.size_text.empty()) meta += (meta.empty() ? L"" : L" \u00B7 ") + e.size_text;
            if (vm.view_mode == ViewMode::Content && !e.date_text.empty())
                meta += (meta.empty() ? L"" : L" \u00B7 ") + e.date_text;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), meta,
                nameRc.left, nameRc.top + 25.0f * scale_,
                std::max(0.0f, nameRc.right - nameRc.left), 22.0f * scale_);
        }

        if (trail.show_star || trail.show_new_tab || trail.show_more) {
            const bool starHot = hover_region == static_cast<int>(HitTestResult::RowStar) &&
                                 hover_control_index == src;
            const bool newTabHot = hover_region == static_cast<int>(HitTestResult::RowNewTab) &&
                                   hover_control_index == src;
            const bool moreHot = hover_region == static_cast<int>(HitTestResult::RowMore) &&
                                 hover_control_index == src;
            auto draw_action = [&](const D2D1_RECT_F& rc, bool hot, const wchar_t* glyph,
                                   const wchar_t* fallback, const D2D1_COLOR_F& color, float size) {
                if (rc.right <= rc.left) return;
                if (hot) {
                    MakeBrush(dc, theme.fill_selected, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                        rc.right - rc.left, rc.bottom - rc.top, 5.0f * scale_);
                }
                DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                             glyph, fallback, color, size);
            };
            if (trail.show_star) {
                if (e.starred) {
                    MakeBrush(dc, WithAlpha(theme.accent, starHot ? 0.28f : 0.18f), brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), trail.star.left, trail.star.top,
                        trail.star.right - trail.star.left, trail.star.bottom - trail.star.top,
                        5.0f * scale_);
                }
                draw_action(trail.star, starHot && !e.starred,
                            e.starred ? L"\xE735" : L"\xE734", L"*",
                            e.starred ? theme.accent : theme.text_secondary,
                            1.0f);
            }
            if (trail.show_new_tab)
                draw_action(trail.new_tab, newTabHot, L"\xE8A0", L"\x2197",
                            theme.text_secondary, 0.9f);
            if (trail.show_more)
                draw_action(trail.more, moreHot, L"\xE712", L"...", theme.text_secondary, 0.72f);
        }
    }

    if (vm.marquee_active) {
        D2D1_RECT_F clip = D2D1::RectF(x, y, x + w, y + h);
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_RECT_F rc = vm.marquee_rect;
        const float mw = rc.right - rc.left;
        const float mh = rc.bottom - rc.top;
        if (mw > 0.0f && mh > 0.0f) {
            MakeBrush(dc, WithAlpha(theme.accent, 0.16f), brAccent_);
            FillRect(dc, brAccent_.get(), rc.left, rc.top, mw, mh);
            MakeBrush(dc, theme.accent, brAccent_);
            dc->DrawRectangle(rc, brAccent_.get(), 1.0f * scale_);
        }
        dc->PopAxisAlignedClip();
    }

    DrawScrollbar(vm, x, y, w, h, theme);
    if (vm.view_mode == ViewMode::List && layout.MaxScrollX() > 0.0f) {
        const float viewW = std::max(1.0f, w);
        const float totalW = layout.ContentWidth();
        const float thumbW = std::max(28.0f * scale_, viewW * (viewW / totalW));
        const float travel = std::max(1.0f, viewW - thumbW);
        const float thumbX = x + (vm.scroll_x / layout.MaxScrollX()) * travel;
        FillRoundedRect(dc, brScrollbar_.get(), thumbX, y + h - 8.0f * scale_,
                        thumbW, 6.0f * scale_, 3.0f * scale_);
    }
}

struct ScrollbarMetrics {
    float thumbY, thumbH;
    bool valid;
};

static ScrollbarMetrics ComputeScrollbar(float viewH, float totalH, float scrollY, float rowH) {
    ScrollbarMetrics m{};
    if (totalH <= viewH || viewH <= 0) return m;
    m.valid = true;
    m.thumbH = std::max(rowH, viewH * (viewH / totalH));
    m.thumbY = (scrollY / (totalH - viewH)) * (viewH - m.thumbH);
    return m;
}

void MainRenderer::DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme) {
    (void)theme;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ViewLayout layout(vm.view_mode, D2D1::RectF(x, y, x + w, y + h), vm.EntryCount(),
                      vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    const float totalH = layout.ContentHeight();
    auto sb = ComputeScrollbar(h, totalH, vm.scroll_y, layout.Metrics().cell_height);
    if (!sb.valid) return;
    float thumbW = 6 * scale_;
    FillRoundedRect(dc, brScrollbar_.get(), x + w - 10.0f * scale_, y + sb.thumbY,
        thumbW, sb.thumbH, thumbW * 0.5f);
}

void MainRenderer::DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    float y = rect.bottom - status_height_;
    D2D1_COLOR_F statusBackground = theme.status_bg;
    if (vm.backdrop_enabled) statusBackground.a = vm.dark ? 0.76f : 0.82f;
    MakeBrush(dc, statusBackground, brFillInput_);
    FillRect(dc, brFillInput_.get(), 0, y, rect.right, status_height_);
    FillRect(dc, brStrokeDivider_.get(), 0, y, rect.right, 1);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.status.status_text,
        margin_, y, rect.right * 0.30f, status_height_);
    const bool compact = rect.right < 900.0f * scale_;
    std::wstring selectionText = vm.status.selection_text;
    if (compact && vm.pane.selected_count > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L"\u5DF2\u9009\u4E2D %d \u9879", vm.pane.selected_count); // 已选中 N 项
        selectionText = buf;
    }
    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), selectionText,
        rect.right * 0.30f, y, rect.right * 0.18f, status_height_);

    float rightReserved = 170.0f * scale_;
    if (!vm.status.performance_text.empty()) {
        const std::wstring& perfText = rect.right < 1100.0f * scale_
            ? vm.status.performance_compact_text : vm.status.performance_text;
        const float perfWidth = std::min(rect.right * 0.50f,
            MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), perfText)
                + 16.0f * scale_);
        rightReserved = perfWidth + margin_;
        IDWriteTextFormat* perfFormat = compositor_->SmallFormat();
        perfFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, perfFormat, brTextSecondary_.get(), perfText,
            rect.right - rightReserved, y, perfWidth, status_height_);
        perfFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.status.mode_text,
            rect.right - rightReserved, y, rightReserved - 10.0f * scale_, status_height_);
    }

    // Ops task summary + self-drawn Fluent progress bar.
    float taskX = rect.right * 0.48f;
    float taskRight = rect.right - rightReserved - margin_;
    if (!vm.status.task_text.empty() || vm.status.task_progress >= 0.0f) {
        float barW = (vm.status.task_progress >= 0.0f) ? (100 * scale_ + margin_ * 2) : 0.0f;
        MakeBrush(dc, theme.accent, brAccentText_);
        DrawTextRect(dc, compositor_->SmallFormat(), brAccentText_.get(), vm.status.task_text,
            taskX, y, std::max(0.0f, taskRight - taskX - barW), status_height_);
        if (barW > 0.0f) {
            float trackH = 4 * scale_;
            float trackX = taskRight - 100 * scale_;
            float trackY = y + (status_height_ - trackH) * 0.5f;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRoundedRect(dc, brStrokeCard_.get(), trackX, trackY, 100 * scale_, trackH, trackH * 0.5f);
            float fillW = 100 * scale_ * std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
            if (fillW > trackH) {
                MakeBrush(dc, theme.accent, brAccent_);
                FillRoundedRect(dc, brAccent_.get(), trackX, trackY, fillW, trackH, trackH * 0.5f);
            }
        }
    }

}

void MainRenderer::DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_);
    D2D1_COLOR_F nav_bg = theme.tab_bg;
    if (vm.backdrop_enabled) nav_bg.a = vm.dark ? 0.62f : 0.70f;
    MakeBrush(dc, nav_bg, brFillHover_);
    FillRect(dc, brFillHover_.get(), lay.nav.left, lay.nav.top,
             lay.nav.right - lay.nav.left, lay.nav.bottom - lay.nav.top);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.right - 1.0f, lay.nav.top, 1.0f,
             lay.nav.bottom - lay.nav.top);

    static constexpr const wchar_t* kNav[] = { L"\u901A\u7528", L"\u672C\u5730\u53F3\u952E\u83DC\u5355" };
    static constexpr const wchar_t* kNavIcon[] = { kIconHome, kIconSettings };
    for (int i = 0; i < 2; ++i) {
        const bool active = vm.settings_page == i;
        const bool hovered = IsHovered(vm, HitTestResult::SettingsNav, i);
        const auto& rc = lay.nav_row[i];
        if (active || hovered) {
            MakeBrush(dc, active ? theme.fill_selected : theme.fill_hover, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * scale_);
        }
        DrawIconText(rc.left + 8.0f * scale_, rc.top, 22.0f * scale_, rc.bottom - rc.top,
                     kNavIcon[i], L"*", active ? theme.accent : theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), kNav[i],
                     rc.left + 36.0f * scale_, rc.top, rc.right - rc.left - 44.0f * scale_,
                     rc.bottom - rc.top);
    }

    dc->PushAxisAlignedClip(lay.content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float pad = 20.0f * scale_;
    const float origin = lay.content_origin;
    const float switch_w = 42.0f * scale_;
    const float switch_h = 32.0f * scale_;

    const wchar_t* page_title = vm.settings_page == 0 ? L"\u901A\u7528" : L"\u672C\u5730\u53F3\u952E\u83DC\u5355";
    MakeBrush(dc, theme.text, brText_);
    DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), page_title,
                 lay.content.left + pad, origin + pad,
                 lay.content.right - lay.content.left - pad * 2, 32.0f * scale_);

    if (vm.settings_page == 0) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), L"\u5916\u89c2",
                     lay.content.left + pad, origin + pad + 44.0f * scale_,
                     200.0f * scale_, 22.0f * scale_);
        draw_card(lay.effect_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), L"\u7a97\u53e3\u6548\u679c",
                     lay.effect_card.left + 16.0f * scale_, lay.effect_card.top + 10.0f * scale_,
                     lay.effect_card.right - lay.effect_card.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     L"\u6539\u53d8\u7a97\u53e3\u7684\u663e\u793a\u6548\u679c\uff0c\u7acb\u5373\u751f\u6548",
                     lay.effect_card.left + 16.0f * scale_, lay.effect_card.top + 32.0f * scale_,
                     lay.effect_card.right - lay.effect_card.left - 32.0f * scale_, 18.0f * scale_);
        for (int i = 0; i < kWindowEffectCount; ++i) {
            const auto effect = static_cast<WindowEffect>(i);
            const auto& row = lay.effect_row[i];
            if (IsHovered(vm, HitTestResult::SettingsEffect, i)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            fluent::ControlState st{};
            st.checked = vm.window_effect == effect;
            st.hovered = IsHovered(vm, HitTestResult::SettingsEffect, i);
            painter_.DrawRadioButton(D2D1::RectF(row.left + 16.0f * scale_, row.top,
                                                 row.right - 16.0f * scale_, row.bottom),
                                     WindowEffectLabel(effect), st);
        }

        draw_card(lay.density_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), L"列表行高",
                     lay.density_card.left + 16.0f * scale_, lay.density_card.top + 10.0f * scale_,
                     lay.density_card.right - lay.density_card.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     L"调整文件列表每一行的高度，立即生效",
                     lay.density_card.left + 16.0f * scale_, lay.density_card.top + 32.0f * scale_,
                     lay.density_card.right - lay.density_card.left - 32.0f * scale_, 18.0f * scale_);
        static constexpr const wchar_t* kDensityLabels[] = {
            L"紧凑（28 像素）", L"标准（34 像素）", L"舒适（40 像素）"
        };
        static constexpr int kDensityDips[] = { 28, 34, 40 };
        for (int i = 0; i < 3; ++i) {
            const auto& row = lay.density_row[i];
            if (IsHovered(vm, HitTestResult::SettingsDensity, i)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            fluent::ControlState st{};
            st.checked = vm.settings_row_height == kDensityDips[i];
            st.hovered = IsHovered(vm, HitTestResult::SettingsDensity, i);
            painter_.DrawRadioButton(D2D1::RectF(row.left + 16.0f * scale_, row.top,
                                                 row.right - 16.0f * scale_, row.bottom),
                                     kDensityLabels[i], st);
        }

        draw_card(lay.wallpaper_card);
        const auto& preview = lay.wallpaper_preview;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), preview.left, preview.top,
                        preview.right - preview.left, preview.bottom - preview.top, 6.0f * scale_);
        if (!vm.background_image.empty())
            material_.DrawSourceCover(dc, preview, vm.background_image);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6.0f * scale_, 6.0f * scale_),
                                 brStrokeCard_.get(), 1.0f);
        const float text_left = preview.right + 12.0f * scale_;
        const float text_right = lay.wallpaper_choose.left - 12.0f * scale_;
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), L"\u80cc\u666f\u56fe",
                     text_left, lay.wallpaper_card.top + 18.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 22.0f * scale_);
        const std::wstring wallpaper_desc = vm.background_image.empty()
            ? L"\u7a97\u53e3\u6548\u679c\u9009\u300c\u65e0\u300d\u65f6\u900f\u51fa\u663e\u793a\uff1b\u4e9a\u514b\u529b / Mica \u5219\u4ee5\u5176\u53d6\u6837"
            : FileNameOf(vm.background_image);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), wallpaper_desc,
                     text_left, lay.wallpaper_card.top + 42.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 18.0f * scale_);
        fluent::ControlState choose{};
        choose.hovered = IsHovered(vm, HitTestResult::SettingsWallpaper, 0);
        painter_.DrawButton({ lay.wallpaper_choose, L"\u9009\u62e9\u56fe\u7247", {},
                              fluent::ButtonKind::Standard, choose });
        fluent::ControlState clear{};
        clear.enabled = !vm.background_image.empty();
        clear.hovered = clear.enabled && IsHovered(vm, HitTestResult::SettingsWallpaper, 1);
        painter_.DrawButton({ lay.wallpaper_clear, L"\u6e05\u9664", {},
                              fluent::ButtonKind::Standard, clear });

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), L"\u542F\u52A8\u4E0E\u5173\u95ED",
                     lay.content.left + pad, lay.startup_row[0].top - 30.0f * scale_,
                     200.0f * scale_, 22.0f * scale_);
        const D2D1_RECT_F startup_card = D2D1::RectF(lay.startup_row[0].left, lay.startup_row[0].top,
                                                     lay.startup_row[1].right, lay.startup_row[1].bottom);
        draw_card(startup_card);
        auto draw_row = [&](const D2D1_RECT_F& row, const wchar_t* title, const wchar_t* desc,
                            bool on, int hit) {
            if (IsHovered(vm, HitTestResult::SettingsToggle, hit)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), title,
                         row.left + 16.0f * scale_, row.top + 8.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), desc,
                         row.left + 16.0f * scale_, row.top + 30.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 18.0f * scale_);
            fluent::ControlState st{};
            st.checked = on;
            st.hovered = IsHovered(vm, HitTestResult::SettingsToggle, hit);
            painter_.DrawSwitch(D2D1::RectF(row.right - 16.0f * scale_ - switch_w,
                                            row.top + (56.0f * scale_ - switch_h) * 0.5f,
                                            row.right - 16.0f * scale_,
                                            row.top + (56.0f * scale_ + switch_h) * 0.5f),
                                L"", st);
        };
        draw_row(lay.startup_row[0], L"\u5F00\u673A\u81EA\u542F",
                 L"\u767B\u5F55 Windows \u540E\u81EA\u52A8\u6253\u5F00 Pulse",
                 vm.settings_launch_on_startup, 1);
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), startup_card.left + 16.0f * scale_,
                 lay.startup_row[0].bottom, startup_card.right - startup_card.left - 32.0f * scale_, 1.0f);
        draw_row(lay.startup_row[1], L"\u5173\u95ED\u540E\u7EE7\u7EED\u8FD0\u884C",
                 L"\u70B9\u5173\u95ED\u65F6\u7F29\u5230\u6258\u76D8\uFF0C\u590D\u5236\u7B49\u4EFB\u52A1\u7EE7\u7EED",
                 vm.settings_keep_running, 2);
    } else {
        float y = origin + pad + 44.0f * scale_;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     L"\u9009\u62E9\u878D\u5408\u533A\u663E\u793A\u54EA\u4E9B Explorer \u52A8\u8BCD\u3002\u6539\u52A8\u5728\u4E0B\u6B21\u53F3\u952E\u751F\u6548\u3002",
                     lay.content.left + pad, y, lay.content.right - lay.content.left - pad * 2,
                     22.0f * scale_);
        y += 30.0f * scale_;
        static constexpr const wchar_t* kGroupTitle[] = {
            L"\u8F6F\u4EF6\u529F\u80FD", L"\u6253\u5F00\u65B9\u5F0F", L"\u5206\u4EAB", L"\u7CFB\u7EDF\u9879", L"\u6253\u5370"
        };
        static constexpr const wchar_t* kGroupDesc[] = {
            L"\u7B2C\u4E09\u65B9\u8F6F\u4EF6\u7684\u5B50\u83DC\u5355\u548C\u7C7B\u578B\u52A8\u8BCD\u3002\u9ED8\u8BA4\u4FDD\u7559\u3002",
            L"\u6CE8\u518C\u8868\u300C\u7528\u67D0\u5E94\u7528\u6253\u5F00\u300D\u548C\u300C\u6253\u5F00\u65B9\u5F0F\u2026\u300D\u3002COM \u91CD\u590D\u9879\u9ED8\u8BA4\u5173\u95ED\u3002",
            L"\u53D1\u9001\u5230\u3001\u5373\u65F6\u901A\u8BAF\u5206\u4EAB\u3002\u9ED8\u8BA4\u9690\u85CF\u3002",
            L"\u58C1\u7EB8\u3001\u65CB\u8F6C\u3001\u5FEB\u6377\u65B9\u5F0F\u3001\u4EE5\u524D\u7684\u7248\u672C\u7B49\u3002\u9ED8\u8BA4\u9690\u85CF\u3002",
            L"\u6253\u5370\u3002\u9ED8\u8BA4\u4FDD\u7559\u3002"
        };
        for (int g = 0; g < 5; ++g) {
            std::vector<int> rows;
            for (int i = 0; i < static_cast<int>(vm.settings_items.size()); ++i)
                if (vm.settings_items[static_cast<size_t>(i)].group == g) rows.push_back(i);
            const float header = 56.0f * scale_;
            const float desc = 36.0f * scale_;
            const float row_h = 36.0f * scale_;
            const float card_h = header + desc + rows.size() * row_h + 8.0f * scale_;
            const D2D1_RECT_F card = D2D1::RectF(lay.content.left + pad, y,
                                                 lay.content.right - pad, y + card_h);
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), kGroupTitle[g],
                         card.left + 16.0f * scale_, y + 10.0f * scale_,
                         card.right - card.left - 80.0f * scale_, 22.0f * scale_);
            fluent::ControlState gst{};
            gst.checked = vm.settings_group_on[g];
            gst.hovered = IsHovered(vm, HitTestResult::SettingsToggle, 10 + g);
            painter_.DrawSwitch(D2D1::RectF(card.right - 16.0f * scale_ - switch_w,
                                            y + (header - switch_h) * 0.5f,
                                            card.right - 16.0f * scale_,
                                            y + (header + switch_h) * 0.5f),
                                L"", gst);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), kGroupDesc[g],
                         card.left + 16.0f * scale_, y + header - 4.0f * scale_,
                         card.right - card.left - 32.0f * scale_, desc - 8.0f * scale_);
            float iy = y + header + desc;
            for (int idx : rows) {
                const auto& row = vm.settings_items[static_cast<size_t>(idx)];
                if (IsHovered(vm, HitTestResult::SettingsToggle, 100 + idx)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), card.left + 4.0f * scale_, iy,
                                    card.right - card.left - 8.0f * scale_, row_h, 4.0f * scale_);
                }
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), row.text,
                             card.left + 16.0f * scale_, iy,
                             card.right - card.left - 80.0f * scale_, row_h);
                fluent::ControlState ist{};
                ist.checked = row.on;
                ist.hovered = IsHovered(vm, HitTestResult::SettingsToggle, 100 + idx);
                painter_.DrawSwitch(D2D1::RectF(card.right - 16.0f * scale_ - switch_w,
                                                iy + (row_h - switch_h) * 0.5f,
                                                card.right - 16.0f * scale_,
                                                iy + (row_h + switch_h) * 0.5f),
                                    L"", ist);
                iy += row_h;
            }
            y += card_h + 12.0f * scale_;
        }
        fluent::ControlState restore{};
        restore.hovered = IsHovered(vm, HitTestResult::SettingsRestore);
        painter_.DrawButton({ D2D1::RectF(lay.content.left + pad, y,
                                          lay.content.left + pad + 120.0f * scale_,
                                          y + 32.0f * scale_),
                              L"\u6062\u590D\u9ED8\u8BA4", {}, fluent::ButtonKind::Standard, restore });
    }
    dc->PopAxisAlignedClip();

    const float view = lay.content.bottom - lay.content.top;
    if (lay.content_h > view + 1.0f) {
        fluent::ScrollbarSpec bar;
        bar.viewport = D2D1::RectF(lay.content.right - 10.0f * scale_, lay.content.top,
                                   lay.content.right - 2.0f * scale_, lay.content.bottom);
        bar.offset = vm.settings_scroll;
        bar.viewport_extent = view;
        bar.content_extent = lay.content_h;
        bar.expand_progress = 1.0f;
        painter_.DrawScrollbar(bar);
    }
}

float MainRenderer::SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const {
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, window_w, window_h);
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_);
    const float view = (std::max)(0.0f, lay.content.bottom - lay.content.top);
    return (std::max)(0.0f, lay.content_h - view);
}

float MainRenderer::MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.MaxScrollY();
}

float MainRenderer::MaxScrollXForPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.MaxScrollX();
}

D2D1_RECT_F MainRenderer::ItemRectInPane(const PaneViewModel& vm,
                                         const D2D1_RECT_F& pane_bounds,
                                         int view_index) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.ItemRect(view_index);
}

int MainRenderer::MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                int current, int dx, int dy) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.MoveIndex(current, dx, dy);
}

int MainRenderer::PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.PageDelta();
}

std::pair<int, int> MainRenderer::VisibleRangeInPane(
    const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    return layout.VisibleRange();
}

int MainRenderer::RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const {
    return ItemFromPointInPane(vm, pane_bounds, pane_bounds.left + 1.0f, y);
}

int MainRenderer::ItemFromPointInPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds,
                                      float x, float y) const {
    const float extra = vm.banner_message.empty() ? 0.0f : 36.0f * scale_;
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, row_height_dip_);
    int idx = layout.HitTest(x, y);
    if (idx < 0) return -1;
    return vm.SourceIndex(idx);
}

MainRenderer::TabStripMetrics MainRenderer::ComputeTabStrip(
    const WindowViewModel& vm, float window_w) const {
    TabStripMetrics m;
    const bool compact = TitleBarCompact(window_w, scale_, vm.tabs.size());
    const TitleChrome chrome = MakeTitleChrome(window_w, scale_, title_bar_height_);
    m.x0 = compact ? 44.0f * scale_ : 112.0f * scale_;
    const float tabsRight = chrome.settings_left - 8.0f * scale_;

    // Group chips: one at the start of each consecutive same-group run. Their
    // widths come out of the strip budget before tabs are sized; positions
    // are resolved in the second pass once the tab pitch is known.
    const float chipGap = 4.0f * scale_;
    float chipsTotal = 0.0f;
    IDWriteTextFormat* chipFmt = compositor_ ? compositor_->SmallFormat() : nullptr;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (!runStart) continue;
        float cw = 8.0f * scale_; // unnamed group: slim color bar
        if (g < static_cast<int>(vm.tab_groups.size()) &&
            !vm.tab_groups[static_cast<size_t>(g)].name.empty()) {
            const float tw = std::min(88.0f * scale_,
                MeasureTextWidth(compositor_->DwriteFactory(), chipFmt,
                                 vm.tab_groups[static_cast<size_t>(g)].name));
            cw = tw + 16.0f * scale_; // Edge-style block: text + side padding
        }
        TabStripMetrics::Chip chip;
        chip.width = cw;
        chip.group = g;
        chipsTotal += cw + chipGap;
        m.chips.push_back(chip);
    }
    m.extra.assign(vm.tabs.size(), 0.0f);

    const float available = std::max(0.0f, tabsRight - m.x0 - 36.0f * scale_ - chipsTotal);
    size_t visibleCount = 0;
    size_t pinnedCount = 0; // visible pinned tabs get a fixed narrow slot
    for (const auto& t : vm.tabs) {
        if (t.hidden) continue;
        if (t.pinned) ++pinnedCount; else ++visibleCount;
    }
    const float pinnedW = kTabPinnedW * scale_;
    const float pinnedTotal = static_cast<float>(pinnedCount) * (pinnedW + control_gap_);
    m.w = visibleCount == 0 ? 0.0f
        : std::min(kTabMaxW * scale_, std::max(kTabMinW * scale_,
            std::max(0.0f, available - pinnedTotal)
                / static_cast<float>(visibleCount) - control_gap_));
    m.y = 4.0f * scale_;
    m.h = title_bar_height_ - 8.0f * scale_;
    m.pitch = m.w + control_gap_;

    // Final pass: per-tab extra offset + definitive chip positions. Collapsed
    // members contribute zero width (chip stays visible at the fold point);
    // pinned tabs use the fixed narrow slot.
    float acc = 0.0f;
    size_t chipIdx = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (runStart && chipIdx < m.chips.size()) {
            TabStripMetrics::Chip& chip = m.chips[chipIdx++];
            chip.left = m.x0 + static_cast<float>(i) * m.pitch + acc;
            acc += chip.width + chipGap;
        }
        if (vm.tabs[i].hidden) acc -= m.pitch;
        else if (vm.tabs[i].pinned) acc += pinnedW - m.w; // narrower than a slot
        m.extra[i] = acc;
    }
    return m;
}

bool MainRenderer::TabItemRect(const WindowViewModel& vm, float window_w, int index,
                               D2D1_RECT_F* out) const {
    if (!out || index < 0 || index >= static_cast<int>(vm.tabs.size())) return false;
    if (vm.tabs[static_cast<size_t>(index)].hidden) return false; // collapsed group
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    const float left = m.x0 + static_cast<float>(index) * m.pitch
        + (index < static_cast<int>(m.extra.size()) ? m.extra[static_cast<size_t>(index)] : 0.0f);
    const float w = vm.tabs[static_cast<size_t>(index)].pinned
        ? kTabPinnedW * scale_ : m.w;
    *out = D2D1::RectF(left, m.y, left + w, m.y + m.h);
    return true;
}

bool MainRenderer::TabGroupChipRect(const WindowViewModel& vm, float window_w,
                                    int group_index, D2D1_RECT_F* out) const {
    if (!out) return false;
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    for (const auto& chip : m.chips) {
        if (chip.group != group_index) continue;
        *out = D2D1::RectF(chip.left, m.y + 4.0f * scale_, chip.left + chip.width,
                           m.y + m.h - 4.0f * scale_);
        return true;
    }
    return false;
}

float MainRenderer::TabPitchPx(const WindowViewModel& vm, float window_w) const {
    return ComputeTabStrip(vm, window_w).pitch;
}

bool MainRenderer::TagItemRect(const WindowViewModel& vm, float w, float h, int group,
                               int item, D2D1_RECT_F* out) const {
    if (!out) return false;
    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, SidebarRect(w, h), scale_, slots);
    for (const auto& slot : slots) {
        if (slot.kind == SidebarSlot::Tag && slot.group == group && slot.item == item) {
            *out = slot.rc;
            return true;
        }
    }
    return false;
}

HitTestResult MainRenderer::HitTest(const WindowViewModel& vm, const D2D1_RECT_F& rect, float x, float y) const {
    HitTestResult r;
    if (x < rect.left || x >= rect.right || y < rect.top || y >= rect.bottom) return r;

    // Title bar.
    if (y < title_bar_height_) {
        const float ctrlW = 46.0f * scale_;
        const TitleChrome chrome = MakeTitleChrome(rect.right, scale_, title_bar_height_);
        const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
        auto hitTab = [&](int i) -> bool {
            if (vm.tabs[static_cast<size_t>(i)].hidden) return false;
            const float tabW = vm.tabs[static_cast<size_t>(i)].pinned
                ? kTabPinnedW * scale_ : strip.w;
            const float extra = i < static_cast<int>(strip.extra.size())
                ? strip.extra[static_cast<size_t>(i)] : 0.0f;
            float tabLeft = strip.x0
                + (static_cast<float>(i) + vm.tabs[static_cast<size_t>(i)].x_offset) * strip.pitch
                + extra;
            if (vm.tab_drag_index >= 0 && i >= vm.tab_drag_index &&
                i < vm.tab_drag_index + std::max(1, vm.tab_drag_count)) {
                // Floating run: positions follow the drag cursor, skipping
                // hidden (collapsed) members just like the draw path.
                float fx = vm.tab_drag_x;
                for (int k = vm.tab_drag_index; k < i; ++k)
                    if (!vm.tabs[static_cast<size_t>(k)].hidden) fx += strip.pitch;
                tabLeft = fx;
            }
            if (x < tabLeft || x >= tabLeft + tabW) return false;
            r.index = i;
            const bool show_close = TabCloseVisible(vm, i, tabW, scale_);
            const float close_hit = (kTabClosePadDip + kTabCloseSizeDip) * scale_;
            r.region = show_close && x >= tabLeft + tabW - close_hit
                ? HitTestResult::TabClose : HitTestResult::Tab;
            return true;
        };
        // Raised run is on top, so it wins overlapping hits.
        if (vm.tab_drag_index >= 0 && vm.tab_drag_index < static_cast<int>(vm.tabs.size())) {
            const int dragN = std::max(1, vm.tab_drag_count);
            for (int k = vm.tab_drag_index;
                 k < vm.tab_drag_index + dragN && k < static_cast<int>(vm.tabs.size()); ++k)
                if (hitTab(k)) return r;
        }
        // Group chips: strip slots between tabs, click opens the group popup.
        for (const auto& chip : strip.chips) {
            if (x >= chip.left && x < chip.left + chip.width &&
                y >= strip.y + 4.0f * scale_ && y < strip.y + strip.h - 4.0f * scale_) {
                r.region = HitTestResult::TabGroup;
                r.index = chip.group;
                return r;
            }
        }
        for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
            if (vm.tab_drag_index >= 0 && i >= vm.tab_drag_index &&
                i < vm.tab_drag_index + std::max(1, vm.tab_drag_count)) continue;
            if (hitTab(i)) return r;
        }
        const float extraEnd = strip.extra.empty() ? 0.0f : strip.extra.back();
        float cx = strip.x0 + static_cast<float>(vm.tabs.size()) * strip.pitch + extraEnd;
        if (x >= cx && x < cx + 32.0f * scale_) {
            r.region = HitTestResult::TabNew;
            return r;
        }
        if (x >= chrome.settings_left && x < chrome.settings_left + chrome.settings_w) {
            r.region = HitTestResult::SettingsButton;
            return r;
        }
        if (x >= chrome.theme_left && x < chrome.theme_left + chrome.theme_w) {
            r.region = HitTestResult::ThemeToggle;
            return r;
        }
        float ctrlX = rect.right - ctrlW;
        if (x >= ctrlX) { r.region = HitTestResult::Close; return r; }
        ctrlX -= ctrlW;
        if (x >= ctrlX) { r.region = HitTestResult::Maximize; return r; }
        ctrlX -= ctrlW;
        if (x >= ctrlX) { r.region = HitTestResult::Minimize; return r; }
        return r;
    }

    if (vm.settings_open) {
        const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                      status_height_);
        if (y >= rect.bottom - status_height_) {
            r.region = HitTestResult::StatusBar;
            return r;
        }
        for (int i = 0; i < 2; ++i) {
            if (ContainsPt(lay.nav_row[i], x, y)) {
                r.region = HitTestResult::SettingsNav;
                r.index = i;
                return r;
            }
        }
        if (ContainsPt(lay.content, x, y)) {
            if (vm.settings_page == 0) {
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.density_row[i], x, y)) {
                        r.region = HitTestResult::SettingsDensity;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < kWindowEffectCount; ++i) {
                    if (ContainsPt(lay.effect_row[i], x, y)) {
                        r.region = HitTestResult::SettingsEffect;
                        r.index = i;
                        return r;
                    }
                }
                if (ContainsPt(lay.wallpaper_choose, x, y)) {
                    r.region = HitTestResult::SettingsWallpaper;
                    r.index = 0;
                    return r;
                }
                if (ContainsPt(lay.wallpaper_clear, x, y)) {
                    r.region = HitTestResult::SettingsWallpaper;
                    r.index = 1;
                    return r;
                }
                for (int i = 0; i < 2; ++i) {
                    if (ContainsPt(lay.startup_row[i], x, y)) {
                        r.region = HitTestResult::SettingsToggle;
                        r.index = i + 1;
                        return r;
                    }
                }
            } else {
                const float pad = 20.0f * scale_;
                float cy = lay.content_origin + pad + 44.0f * scale_;
                cy += 30.0f * scale_;
                for (int g = 0; g < 5; ++g) {
                    size_t n = 0;
                    for (const auto& row : vm.settings_items)
                        if (row.group == g) ++n;
                    const float header = 56.0f * scale_;
                    const float desc = 36.0f * scale_;
                    const float row_h = 36.0f * scale_;
                    const float card_h = header + desc + n * row_h + 8.0f * scale_;
                    const float card_left = lay.content.left + pad;
                    const float card_right = lay.content.right - pad;
                    if (x >= card_left && x < card_right && y >= cy && y < cy + header) {
                        r.region = HitTestResult::SettingsToggle;
                        r.index = 10 + g;
                        return r;
                    }
                    float iy = cy + header + desc;
                    for (size_t i = 0; i < vm.settings_items.size(); ++i) {
                        if (vm.settings_items[i].group != g) continue;
                        if (x >= card_left && x < card_right && y >= iy && y < iy + row_h) {
                            r.region = HitTestResult::SettingsToggle;
                            r.index = 100 + static_cast<int>(i);
                            return r;
                        }
                        iy += row_h;
                    }
                    cy += card_h + 12.0f * scale_;
                }
                const D2D1_RECT_F restore = D2D1::RectF(lay.content.left + pad, cy,
                    lay.content.left + pad + 120.0f * scale_, cy + 32.0f * scale_);
                if (ContainsPt(restore, x, y)) {
                    r.region = HitTestResult::SettingsRestore;
                    return r;
                }
            }
        }
        r.region = HitTestResult::Pane;
        return r;
    }

    // Toolbar.
    if (y < title_bar_height_ + toolbar_height_) {
        const bool compact = rect.right < 900.0f * scale_;
        float tx = margin_;
        auto hitBtn = [&](float w, HitTestResult::Region reg) -> bool {
            if (x >= tx && x < tx + w) { r.region = reg; return true; }
            tx += kCommandIconStepDip * scale_;
            return false;
        };
        const float commandButtonWidth = kCommandIconButtonDip * scale_;
        if (hitBtn(commandButtonWidth, HitTestResult::NavBack)) return r;
        if (!compact && hitBtn(commandButtonWidth, HitTestResult::NavForward)) return r;
        if (hitBtn(commandButtonWidth, HitTestResult::NavUp)) return r;
        if (hitBtn(commandButtonWidth, HitTestResult::NavRefresh)) return r;
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        if (x >= addrRc.left && x < addrRc.right) {
            if (vm.address_editing) {
                r.region = HitTestResult::AddressBar;
                return r;
            }
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            const float edit_zone = 28.0f * scale_;
            if (x >= addrRc.right - edit_zone) {
                r.region = HitTestResult::AddressBar;
                return r;
            }
            for (size_t i = 0; i < placed.size(); ++i) {
                if (x >= placed[i].rc.left && x < placed[i].rc.right) {
                    r.region = HitTestResult::BreadcrumbSegment;
                    r.index = (int)i;
                    r.path = placed[i].path;
                    return r;
                }
            }
            r.region = HitTestResult::AddressBar;
            return r;
        }
        tx = addrRc.right + margin_;
        const float newW = (compact ? 62.0f : 78.0f) * scale_;
        if (x >= tx && x < tx + newW) { r.region = HitTestResult::NewButton; return r; }
        tx += newW + margin_;
        if (!compact) {
            tx += 12.0f * scale_;
            if (hitBtn(commandButtonWidth, HitTestResult::Cut)) return r;
            if (hitBtn(commandButtonWidth, HitTestResult::Copy)) return r;
            if (hitBtn(commandButtonWidth, HitTestResult::Paste)) return r;
            if (hitBtn(commandButtonWidth, HitTestResult::Rename)) return r;
            if (hitBtn(commandButtonWidth, HitTestResult::Delete)) return r;
            tx += 12.0f * scale_;
            if (hitBtn(commandButtonWidth, HitTestResult::SplitButton)) return r;
            if (hitBtn(commandButtonWidth, HitTestResult::DetailsToggle)) return r;
        }
        return r;
    }

    // Status bar.
    if (y >= rect.bottom - status_height_) { r.region = HitTestResult::StatusBar; return r; }

    // Sidebar.
    D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    if (x >= sb.left && x < sb.right && y >= sb.top && y < sb.bottom) {
        std::vector<SidebarSlot> slots;
        LayoutSidebar(vm, sb, scale_, slots);
        const bool compact = (sb.right - sb.left) <= 60.0f * scale_;
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            const auto& slot = slots[i];
            if (!RectContains(slot.rc, x, y)) continue;
            if (slot.kind == SidebarSlot::TrayRelease) {
                r.region = HitTestResult::TrayRelease;
                r.index = slot.batch;
                return r;
            }
            if (slot.kind == SidebarSlot::TrayClear) {
                r.region = HitTestResult::TrayClear;
                return r;
            }
            if (slot.kind == SidebarSlot::Header) {
                const float action_left = slot.rc.right - 56.0f * scale_;
                const float action_right = slot.rc.right - 28.0f * scale_;
                r.region = vm.sidebar[slot.group].add_action &&
                           x >= action_left && x < action_right
                    ? HitTestResult::SidebarHeaderAction : HitTestResult::SidebarHeader;
                r.index = slot.group;
                return r;
            }
            if (slot.kind == SidebarSlot::TrayPanel) {
                // Fan deck: inverse-rotate the point into each icon's local
                // space; hovered (topmost) first, then center-out. Ghosts
                // are inert.
                if (!compact) {
                    const TrayFanGeom g = TrayFanGeometry(slot.rc, vm.tray_deck.live_count,
                                                          vm.tray_deck.open, scale_);
                    const std::vector<int> order = TrayCardPaintOrder(vm.tray_deck);
                    for (auto it = order.rbegin(); it != order.rend(); ++it) {
                        const int c = *it;
                        const TrayCardView& card = vm.tray_deck.cards[static_cast<size_t>(c)];
                        if (card.ghost) continue;
                        bool close_zone = false;
                        if (!TrayCardHit(g, card, scale_, x, y, &close_zone)) continue;
                        if (close_zone && c == vm.tray_deck.hovered) {
                            r.region = HitTestResult::TrayItemRemove;
                            r.index = card.batch;
                            r.sub_index = card.sub;
                        } else {
                            r.region = HitTestResult::TrayCard;
                            r.index = c; // display index (hover identity)
                        }
                        return r;
                    }
                }
                return r;
            }
            if (slot.kind == SidebarSlot::Item || slot.kind == SidebarSlot::Drive ||
                slot.kind == SidebarSlot::Tag) {
                if (slot.group >= 0 && slot.item >= 0) {
                    const auto& item = vm.sidebar[slot.group].items[slot.item];
                    if (!compact && SidebarItemHasUnpin(item) &&
                        RectContains(WorkspaceUnpinRect(slot.rc, scale_), x, y)) {
                        r.region = HitTestResult::SidebarItemAction;
                        r.index = slot.run;
                        r.path = item.path;
                        return r;
                    }
                    r.path = item.path;
                }
                r.region = HitTestResult::SidebarItem;
                r.index = slot.run;
                return r;
            }
        }
        return r;
    }

    // Details panel (right edge): its interactive rects come from the same
    // layout the draw path uses.
    if (vm.details_visible) {
        const D2D1_RECT_F panel = DetailsPanelRect(rect.right, rect.bottom);
        if (panel.right > panel.left && std::abs(x - panel.left) <= 4.0f * scale_ &&
            y >= panel.top && y < panel.bottom) {
            r.region = HitTestResult::DetailsResize;
            return r;
        }
        if (panel.right > panel.left && x >= panel.left && x < panel.right &&
            y >= panel.top && y < panel.bottom) {
            DetailsHitRects hitRects;
            const float previewH = DetailsPreviewHeight(panel, scale_);
            LayoutDetailsPanel(panel, scale_, vm.details,
                               compositor_ ? compositor_->DwriteFactory() : nullptr,
                               compositor_ ? compositor_->SmallFormat() : nullptr,
                               previewH, hitRects);
            if (RectContains(hitRects.preview, x, y)) { r.region = HitTestResult::DetailsPreview; return r; }
            if (RectContains(hitRects.rename, x, y)) { r.region = HitTestResult::DetailsRename; return r; }
            if (RectContains(hitRects.open, x, y)) { r.region = HitTestResult::DetailsOpen; return r; }
            if (RectContains(hitRects.star, x, y)) { r.region = HitTestResult::DetailsStar; return r; }
            if (RectContains(hitRects.new_tab, x, y)) { r.region = HitTestResult::DetailsNewTab; return r; }
            if (RectContains(hitRects.copy_path, x, y)) { r.region = HitTestResult::DetailsCopyPath; return r; }
            if (RectContains(hitRects.more, x, y)) { r.region = HitTestResult::DetailsMore; return r; }
            if (RectContains(hitRects.tag_add, x, y)) { r.region = HitTestResult::DetailsTagAdd; return r; }
            if (RectContains(hitRects.attr_advanced, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 2; return r; }
            if (RectContains(hitRects.attr_readonly, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 0; return r; }
            if (RectContains(hitRects.attr_hidden, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 1; return r; }
            if (RectContains(hitRects.security_change, x, y)) { r.region = HitTestResult::DetailsSecurityChange; return r; }
            for (size_t i = 0; i < hitRects.preset_chips.size(); ++i) {
                if (RectContains(hitRects.preset_chips[i], x, y)) {
                    r.region = HitTestResult::DetailsPresetTag;
                    r.index = hitRects.preset_ids[i];
                    return r;
                }
            }
            for (size_t i = 0; i < hitRects.section_headers.size(); ++i) {
                if (RectContains(hitRects.section_headers[i], x, y)) {
                    r.region = HitTestResult::DetailsSection;
                    r.index = hitRects.section_ids[i];
                    return r;
                }
            }
            return r; // panel surface swallows the click
        }
    }

    // Pane content (one or more split leaves).
    D2D1_RECT_F content = ContentRect(rect.right, rect.bottom);
    if (x < content.left || x >= content.right || y < content.top || y >= content.bottom) return r;

    for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
        const auto& sp = vm.splitters[static_cast<size_t>(i)];
        if (x >= sp.hit_rect.left && x < sp.hit_rect.right &&
            y >= sp.hit_rect.top && y < sp.hit_rect.bottom) {
            r.region = HitTestResult::Splitter;
            r.index = i;
            return r;
        }
    }

    auto hitPaneBounds = [&](const PaneViewModel& paneVm, const D2D1_RECT_F& paneRc, int paneIndex) -> HitTestResult {
        HitTestResult out;
        out.pane_index = paneIndex;
        if (x < paneRc.left || x >= paneRc.right || y < paneRc.top || y >= paneRc.bottom)
            return out;
        D2D1_RECT_F filterRc = FilterBoxRect(paneRc, paneVm.filter_expand);
        if (RectContains(filterRc, x, y)) {
            out.region = HitTestResult::FilterBox;
            out.index = paneIndex;
            return out;
        }
        const D2D1_RECT_F mediumRc = PaneMediumIconsRect(paneRc, paneVm.filter_expand);
        if (RectContains(mediumRc, x, y)) {
            out.region = HitTestResult::PaneMediumIcons;
            out.index = paneIndex;
            return out;
        }
        const D2D1_RECT_F viewRc = PaneViewButtonRect(paneRc, paneVm.filter_expand);
        if (RectContains(viewRc, x, y)) {
            out.region = HitTestResult::PaneViewButton;
            out.index = paneIndex;
            return out;
        }
        const float extra = paneVm.banner_message.empty() ? 0.0f : 36.0f * scale_;
        float listTop = paneRc.top + pane_header_height_ + extra +
                        (ShowsColumnHeader(paneVm.view_mode) ? column_header_height_ : 0.0f);
        if (y >= paneRc.top && y < paneRc.top + pane_header_height_) {
            out.region = HitTestResult::Pane;
            return out;
        }
        if (ShowsColumnHeader(paneVm.view_mode) &&
            y >= paneRc.top + pane_header_height_ && y < listTop) {
            const DetailsColumnLayout columns = DetailsColumns(
                paneRc, paneVm.details_column_dividers);
            for (int divider = 0; divider < 3; ++divider) {
                if (std::abs(x - columns.DividerX(divider)) <= 4.0f * scale_) {
                    out.region = HitTestResult::ColumnDivider;
                    out.index = divider;
                    return out;
                }
            }
            out.region = HitTestResult::ColumnHeader;
            if (x < columns.DividerX(0)) out.column = SortColumn::Name;
            else if (x < columns.DividerX(1)) out.column = SortColumn::Mtime;
            else if (x < columns.DividerX(2)) out.column = SortColumn::Type;
            else out.column = SortColumn::Size;
            return out;
        }
        if (y >= listTop && y < paneRc.bottom) {
            if (!paneVm.loading && paneVm.EntryCount() == 0 &&
                paneVm.filter_text.empty() && paneVm.is_file_system && paneVm.can_create) {
                const PaneEmptyLayout emptyLayout = MakePaneEmptyLayout(
                    D2D1::RectF(paneRc.left, listTop, paneRc.right, paneRc.bottom),
                    scale_, true);
                if (emptyLayout.show_action && RectContains(emptyLayout.action, x, y)) {
                    out.region = HitTestResult::PaneEmptyNewFolder;
                    out.index = paneIndex;
                    return out;
                }
            }
            if (paneVm.view_mode == ViewMode::List &&
                y >= paneRc.bottom - 12.0f * scale_ &&
                MaxScrollXForPane(paneVm, paneRc) > 0.0f) {
                out.region = HitTestResult::Scrollbar;
                out.sub_index = 1;
                return out;
            }
            if (x >= paneRc.right - 14.0f * scale_) {
                out.region = HitTestResult::Scrollbar;
                return out;
            }
            int idx = ItemFromPointInPane(paneVm, paneRc, x, y);
            if (idx >= 0) {
                if (paneVm.view_mode == ViewMode::Details && idx != paneVm.rename_index) {
                    int viewRow = paneVm.ViewIndex(idx);
                    if (viewRow >= 0 && compositor_ && compositor_->DwriteFactory()) {
                        const D2D1_RECT_F list = PaneListRect(paneRc, extra, paneVm.view_mode);
                        const DetailsColumnLayout columns = DetailsColumns(
                            list, paneVm.details_column_dividers);
                        ViewLayout layout(paneVm.view_mode, list, paneVm.EntryCount(),
                                          paneVm.scroll_x, paneVm.scroll_y, scale_, row_height_dip_);
                        const D2D1_RECT_F nameRc = layout.NameRect(viewRow);
                        const D2D1_RECT_F cell = layout.ItemRect(viewRow);
                        const ListEntryView& entry = MakeVisibleEntry(paneVm, static_cast<size_t>(idx));
                        const std::vector<int>* tagIndices = paneVm.tag_catalog
                            ? paneVm.tag_catalog->TagIndicesForPath(entry.path) : nullptr;
                        size_t tagCount = tagIndices ? tagIndices->size() : 0;
                        if (!tagIndices && paneVm.tag_dots) {
                            const auto found = paneVm.tag_dots->find(idx);
                            if (found != paneVm.tag_dots->end()) tagCount = found->second.size();
                        }
                        const bool rowHot = paneVm.hover_index == idx ||
                            (paneVm.selected_index == idx && paneVm.selected_count == 1);
                        const bool showActions = idx != paneVm.rename_index &&
                            (rowHot || entry.starred);
                        const NameTrail trail = LayoutNameTrail(
                            nameRc.left, nameRc.top, nameRc.bottom - nameRc.top,
                            columns.DividerX(0) - margin_, cell.top, cell.bottom, scale_,
                            entry.name, static_cast<int>(std::min<size_t>(3, tagCount)),
                            showActions, showActions && paneVm.hover_index == idx && entry.is_dir,
                            showActions && rowHot,
                            compositor_->DwriteFactory(), compositor_->TextFormat());
                        if (ContainsPt(trail.star, x, y)) {
                            out.region = HitTestResult::RowStar;
                            out.index = idx;
                            return out;
                        }
                        if (ContainsPt(trail.new_tab, x, y)) {
                            out.region = HitTestResult::RowNewTab;
                            out.index = idx;
                            return out;
                        }
                        if (ContainsPt(trail.more, x, y)) {
                            out.region = HitTestResult::RowMore;
                            out.index = idx;
                            return out;
                        }
                    }
                }
                out.region = HitTestResult::Row;
                out.index = idx;
            } else {
                out.region = HitTestResult::Pane;
            }
            return out;
        }
        out.region = HitTestResult::Pane;
        return out;
    };

    if (!vm.pane_slots.empty()) {
        for (int i = 0; i < static_cast<int>(vm.pane_slots.size()); ++i) {
            const auto& slot = vm.pane_slots[static_cast<size_t>(i)];
            if (x >= slot.rect.left && x < slot.rect.right &&
                y >= slot.rect.top && y < slot.rect.bottom) {
                return hitPaneBounds(slot.pane, slot.rect, i);
            }
        }
        return r;
    }
    return hitPaneBounds(vm.pane, content, 0);
}

} // namespace pulse::ui
