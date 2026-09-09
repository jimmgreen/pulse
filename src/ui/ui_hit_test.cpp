// ui_hit_test.cpp — Hit testing and tab-strip queries.
#include "ui_renderer.h"
#include "address_search_layout.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
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
#include <string_view>

namespace pulse::ui {

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
        const float cx = strip.end_x;
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
                                                      status_height_, &painter_);
        if (y >= rect.bottom - status_height_) {
            r.region = StatusBarHitRegion(vm, rect, x, y, scale_, status_height_,
                                          compositor_);
            return r;
        }
        for (int i = 0; i < kSettingsNavCount; ++i) {
            if (ContainsPt(lay.nav_row[i], x, y)) {
                r.region = HitTestResult::SettingsNav;
                r.index = i;
                return r;
            }
        }
        if (ContainsPt(lay.content, x, y)) {
            if (vm.settings_page == 0) {
                if (vm.settings_bloom) {
                    vm.settings_bloom->SetDisk(lay.accent_picker);
                    const int dot = vm.settings_bloom->HitDot(x, y);
                    if (dot >= 0) {
                        r.region = HitTestResult::SettingsAccent;
                        r.index = dot;
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.density_row[i], x, y)) {
                        r.region = HitTestResult::SettingsDensity;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.tray_icon_row[i], x, y)) {
                        r.region = HitTestResult::SettingsTrayIcon;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.language_segment[i], x, y)) {
                        r.region = HitTestResult::SettingsLanguage;
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
                if (ContainsPt(lay.hidden_files_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle;
                    r.index = 5;
                    return r;
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.startup_row[i], x, y)) {
                        r.region = HitTestResult::SettingsToggle;
                        r.index = i + 1;
                        return r;
                    }
                }
            } else if (vm.settings_page == 1) {
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.index_action[i], x, y)) {
                        r.region = HitTestResult::SettingsIndexAction;
                        r.index = i;
                        return r;
                    }
                }
                for (size_t i = 0; i < lay.index_volume_rows.size(); ++i) {
                    if (ContainsPt(lay.index_volume_rows[i], x, y)) {
                        r.region = HitTestResult::SettingsIndexVolume;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
                if (ContainsPt(lay.index_exclude_action, x, y)) {
                    r.region = HitTestResult::SettingsIndexExcludeAction;
                    r.index = 0;
                    return r;
                }
                for (size_t i = 0; i < lay.index_exclude_remove.size(); ++i) {
                    if (ContainsPt(lay.index_exclude_remove[i], x, y)) {
                        r.region = HitTestResult::SettingsIndexExcludeRemove;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (ContainsPt(lay.network_action[i], x, y)) {
                        r.region = HitTestResult::SettingsNetworkAction;
                        r.index = i;
                        return r;
                    }
                }
                for (size_t i = 0; i < lay.network_remove.size(); ++i) {
                    if (ContainsPt(lay.network_remove[i], x, y)) {
                        r.region = HitTestResult::SettingsNetworkRemove;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
            } else if (vm.settings_page == 2) {
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
            } else if (vm.settings_page == 3) {
                if (ContainsPt(lay.diagnostics_perf, x, y)) {
                    r.region = HitTestResult::SettingsToggle;
                    r.index = 4;
                    return r;
                }
                for (int i = 0; i < 3; ++i) {
                    if (!vm.settings_diagnostics_exporting &&
                        ContainsPt(lay.diagnostics_action[i], x, y)) {
                        r.region = HitTestResult::SettingsDiagnosticsAction;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    const bool enabled = i == 0
                        ? vm.settings_update_enabled && !vm.settings_update_checking
                        : vm.settings_update_available;
                    if (enabled && ContainsPt(lay.update_action[i], x, y)) {
                        r.region = HitTestResult::SettingsUpdateAction;
                        r.index = i;
                        return r;
                    }
                }
            } else if (vm.settings_page == 4) {
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.dup_scope[i], x, y)) {
                        r.region = HitTestResult::SettingsDupScope;
                        r.index = i;
                        return r;
                    }
                }
                if (ContainsPt(lay.dup_browse, x, y)) {
                    r.region = HitTestResult::SettingsDupBrowse;
                    return r;
                }
                for (size_t i = 0; i < lay.dup_drives.size(); ++i) {
                    if (ContainsPt(lay.dup_drives[i], x, y)) {
                        r.region = HitTestResult::SettingsDupDrive;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.dup_min_size[i], x, y)) {
                        r.region = HitTestResult::SettingsDupMinSize;
                        r.index = i;
                        return r;
                    }
                }
                if (ContainsPt(lay.dup_scan, x, y)) {
                    r.region = HitTestResult::SettingsDupScan;
                    return r;
                }
                if (ContainsPt(lay.dup_cancel, x, y)) {
                    r.region = HitTestResult::SettingsDupCancel;
                    return r;
                }
                for (size_t i = 0; i < lay.dup_keep.size(); ++i) {
                    if (ContainsPt(lay.dup_keep[i], x, y)) {
                        r.region = HitTestResult::SettingsDupKeep;
                        r.index = lay.dup_keep_group[i];
                        r.sub_index = lay.dup_keep_file[i];
                        return r;
                    }
                }
                for (size_t i = 0; i < lay.dup_open.size(); ++i) {
                    if (ContainsPt(lay.dup_open[i], x, y)) {
                        r.region = HitTestResult::SettingsDupOpen;
                        r.index = lay.dup_open_group[i];
                        r.sub_index = lay.dup_open_file[i];
                        return r;
                    }
                }
                for (size_t i = 0; i < lay.dup_group_delete.size(); ++i) {
                    if (ContainsPt(lay.dup_group_delete[i], x, y)) {
                        r.region = HitTestResult::SettingsDupGroupDelete;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
                if (ContainsPt(lay.dup_delete_all, x, y)) {
                    r.region = HitTestResult::SettingsDupDeleteAll;
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
            if (vm.address_searching) {
                const auto layout = LayoutAddressSearch(addrRc, scale_);
                if (x < layout.scope.right) r.region = HitTestResult::AddressSearchScope;
                else if (x >= layout.close.left) r.region = HitTestResult::AddressSearchClose;
                else if (vm.address_search_has_text && layout.clear.right > layout.clear.left && x >= layout.clear.left)
                    r.region = HitTestResult::AddressSearchClear;
                else r.region = HitTestResult::AddressBar;
                return r;
            }
            if (vm.address_editing) {
                r.region = HitTestResult::AddressBar;
                return r;
            }
            if (x >= AddressSearchButtonRect(rect.right).left) {
                r.region = HitTestResult::AddressSearch;
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
        const float newW = NewButtonWidthPx(compact);
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

    // Status bar. Only the transfer summary (center-right) is a control;
    // the left folder/selection text must not reopen the copy dialog.
    if (y >= rect.bottom - status_height_) {
        r.region = StatusBarHitRegion(vm, rect, x, y, scale_, status_height_,
                                      compositor_);
        return r;
    }

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
                // Scatter deck: inverse-rotate the point into each icon's
                // local space; hovered (topmost) first, then bottom-up.
                // Ghosts are inert.
                if (!compact) {
                    const TrayFanGeom g = TrayFanGeometry(slot.rc, vm.tray_deck.live_count,
                                                          vm.tray_deck.open, scale_,
                                                          tray_icon_dip_);
                    const std::vector<int> order = TrayCardPaintOrder(vm.tray_deck, g, scale_);
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
                    if (!compact && item.expandable &&
                        RectContains(SidebarExpandRect(slot.rc, scale_), x, y)) {
                        r.region = HitTestResult::SidebarItemExpand;
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
                               compositor_, previewH, hitRects);
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
        const D2D1_RECT_F navBackRc = PaneNavBackRect(paneRc, paneVm.filter_expand);
        if (RectContains(navBackRc, x, y)) {
            out.region = HitTestResult::NavBack;
            out.index = paneIndex;
            return out;
        }
        const D2D1_RECT_F navForwardRc = PaneNavForwardRect(paneRc, paneVm.filter_expand);
        if (RectContains(navForwardRc, x, y)) {
            out.region = HitTestResult::NavForward;
            out.index = paneIndex;
            return out;
        }
        const D2D1_RECT_F navUpRc = PaneNavUpRect(paneRc, paneVm.filter_expand);
        if (RectContains(navUpRc, x, y)) {
            out.region = HitTestResult::NavUp;
            out.index = paneIndex;
            return out;
        }
        const float banner = paneVm.banner_message.empty() ? 0.0f : 36.0f * scale_;
        const float extra = PaneExtraTop(paneVm, scale_);
        const float recentTop = paneRc.top + pane_header_height_ + banner;
        const float filterExtra = (paneVm.is_recent ? kRecentControlsDip * scale_ : 0.0f) +
                                  (paneVm.is_query_search ? kSearchFiltersDip * scale_ : 0.0f);
        const float columnTop = recentTop + filterExtra;
        float listTop = paneRc.top + pane_header_height_ + extra +
                        (ShowsColumnHeader(paneVm.view_mode) ? column_header_height_ : 0.0f);
        if (y >= paneRc.top && y < paneRc.top + pane_header_height_) {
            out.region = HitTestResult::PaneHeader;
            return out;
        }
        if (paneVm.is_recent && y >= recentTop && y < columnTop) {
            for (int i = 0; i < 3; ++i) {
                if (RectContains(RecentFilterRect(
                        paneRc, pane_header_height_ + banner, scale_, i), x, y)) {
                    out.region = HitTestResult::RecentFilter;
                    out.index = i;
                    return out;
                }
            }
            if (RectContains(RecentClearRect(
                    paneRc, pane_header_height_ + banner, scale_), x, y)) {
                out.region = HitTestResult::RecentClear;
                out.index = paneIndex;
                return out;
            }
            out.region = HitTestResult::Pane;
            return out;
        }
        if (paneVm.is_query_search && y >= recentTop && y < recentTop + kSearchFiltersDip * scale_) {
            std::wstring labels[5];
            FillSearchFilterChipLabels(paneVm.search_query, labels);
            float widths[5]{};
            SearchFilterChipWidthsPx(painter_, scale_, labels, widths);
            for (int i = 0; i < 5; ++i) {
                if (RectContains(SearchFilterRect(
                        paneRc, pane_header_height_ + banner, scale_, i, widths), x, y)) {
                    out.region = HitTestResult::SearchFilter;
                    out.index = i;
                    return out;
                }
            }
            out.region = HitTestResult::Pane;
            return out;
        }
        if (ShowsColumnHeader(paneVm.view_mode) && y >= columnTop && y < listTop) {
            if (paneVm.curated_order) {
                out.region = HitTestResult::Pane;
                return out;
            }
            const DetailsColumnLayout columns = DetailsColumns(paneRc, paneVm);
            for (int divider = 0; divider < columns.count - 1; ++divider) {
                if (std::abs(x - columns.DividerX(divider)) <= 4.0f * scale_) {
                    out.region = HitTestResult::ColumnDivider;
                    out.index = divider;
                    return out;
                }
            }
            out.region = HitTestResult::ColumnHeader;
            if (x < columns.DividerX(0)) out.column = SortColumn::Name;
            else if (paneVm.is_search && x < columns.DividerX(1)) {
                // Path column header: not sortable.
                out.region = HitTestResult::Pane;
                return out;
            }
            else if (x < columns.DividerX(paneVm.is_search ? 2 : 1)) out.column = SortColumn::Mtime;
            else if (x < columns.DividerX(paneVm.is_search ? 3 : 2)) out.column = SortColumn::Type;
            else out.column = SortColumn::Size;
            return out;
        }
        if (y >= listTop && y < paneRc.bottom) {
            if (paneVm.search_retaining_results) {
                out.region = HitTestResult::Pane;
                return out;
            }
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
                        const DetailsColumnLayout columns = DetailsColumns(list, paneVm);
                        ViewLayout layout(paneVm.view_mode, list, paneVm.EntryCount(),
                                          paneVm.scroll_x, paneVm.scroll_y, scale_,
                                          ListRowHeightDip(paneVm));
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
                        const float badgeW = !entry.badge.empty()
                            ? std::min(108.0f * scale_, painter_.MeasureTagWidth(entry.badge))
                            : 0.0f;
                        const bool rowSnippet = !entry.snippet.empty();
                        const DetailsNameLine nameLine =
                            MakeDetailsNameLine(nameRc, cell, scale_, rowSnippet);
                        const NameTrail trail = LayoutNameTrail(
                            nameRc.left, nameLine.y, nameLine.h,
                            columns.DividerX(0) - margin_, cell.top, cell.bottom, scale_,
                            entry.name, static_cast<int>(std::min<size_t>(3, tagCount)), badgeW,
                            showActions, showActions && paneVm.hover_index == idx && entry.is_dir,
                            showActions && rowHot,
                            compositor_, compositor_->DwriteFactory(), compositor_->TextFormat());
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
