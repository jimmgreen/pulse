#include "toolbar_layout.h"
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
    const float w = m.Width(vm, static_cast<size_t>(index));
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

bool MainRenderer::SidebarRowRect(const WindowViewModel& vm, float w, float h, int section_id,
                                  int item, D2D1_RECT_F* out) const {
    if (!out || section_id < 0 || item < 0) return false;
    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, SidebarRect(w, h), scale_, slots);
    for (const auto& slot : slots) {
        if (slot.kind != SidebarSlot::Item && slot.kind != SidebarSlot::Drive &&
            slot.kind != SidebarSlot::Tag) continue;
        if (slot.group < 0 || slot.group >= static_cast<int>(vm.sidebar.size())) continue;
        if (vm.sidebar[slot.group].id != section_id || slot.item != item) continue;
        *out = slot.rc;
        return true;
    }
    return false;
}

int MainRenderer::SettingsSliderValueAt(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                        int which, float x) const {
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    return which == 0
        ? SettingsSliderValue(SettingsSlider(lay.wallpaper_look_row, scale_), x, kPanelTransparencyMax)
        : SettingsSliderValue(SettingsSlider(lay.wallpaper_blur_row, scale_), x, kWallpaperBlurMax);
}

HitTestResult MainRenderer::HitTest(const WindowViewModel& vm, const D2D1_RECT_F& rect, float x, float y) const {
    HitTestResult r;
    if (x < rect.left || x >= rect.right || y < rect.top || y >= rect.bottom) return r;
    // Embedded hosts own the title strip and footer controls.
    if (embedded_ && (y < title_bar_height_ || y >= rect.bottom - status_height_)) return r;

    if (vm.change_popover.visible) {
        const auto popup = ChangePopoverRect(vm.change_popover, rect, scale_);
        if (ContainsPt(popup, x, y)) {
            r.region = HitTestResult::ChangeOpen;
            r.index = vm.change_popover.row_index;
            r.pane_index = vm.change_popover.pane_index;
            return r;
        }
    }
    if (vm.teach.visible) {
        const TeachBubbleLayout t = MakeTeachBubbleLayout(rect, status_height_, scale_);
        if (t.card.right - t.card.left >= 200.0f * scale_ && ContainsPt(t.card, x, y)) {
            r.region = ContainsPt(t.primary, x, y) ? HitTestResult::TeachPrimary
                     : ContainsPt(t.close, x, y) ? HitTestResult::TeachDismiss
                     : ContainsPt(t.never, x, y) ? HitTestResult::TeachNever
                     : HitTestResult::TeachBubble;
            return r;
        }
    }
    // Title bar. With vertical tabs the address row sits up here; those
    // points fall through to the toolbar branch below.
    bool titleToolbar = false;
    if (vertical_tabs_ && y < title_bar_height_) {
        const float sbw = EffectiveSidebarWidth(rect.right);
        const auto row = ToolbarLayoutAt(rect.right, NewButtonWidthPx(rect.right - sbw < 600 * scale_));
        for (const auto& nav : row.navigation) titleToolbar = titleToolbar || ContainsPt(nav, x, y);
        titleToolbar = titleToolbar || ContainsPt(row.address, x, y) || ContainsPt(row.search, x, y) ||
            (vm.address_searching && ContainsPt(SearchBarRect(rect.right), x, y));
    }
    if (y < title_bar_height_ && !titleToolbar) {
        const float ctrlW = 46.0f * scale_;
        const TitleChrome chrome = MakeTitleChrome(rect.right, scale_, title_bar_height_);
        if (ContainsPt(SidebarToggleRect(rect.right), x, y)) {
            r.region = HitTestResult::SidebarToggle;
            return r;
        }
        const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
        auto hitTab = [&](int i) -> bool {
            if (vm.tabs[static_cast<size_t>(i)].hidden) return false;
            // A scrolling strip only shows tabs between its arrows.
            if (strip.overflow && (x < strip.view_left || x >= strip.view_right)) return false;
            const float tabW = strip.Width(vm, static_cast<size_t>(i));
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
            const bool compact = strip.compact && !vm.tabs[static_cast<size_t>(i)].pinned && i != strip.active;
            const bool show_close = !compact && TabCloseVisible(vm, i, tabW, scale_);
            const float close_hit = (kTabClosePadDip + kTabCloseSizeDip) * scale_;
            r.region = show_close && x >= tabLeft + tabW - close_hit
                ? HitTestResult::TabClose : HitTestResult::Tab;
            return true;
        };
        if (!vertical_tabs_) {
        if (strip.overflow && y >= strip.y && y < strip.y + strip.h) {
            if (x >= strip.view_left - strip.arrow_w && x < strip.view_left) {
                r.region = HitTestResult::TabScroll; r.index = -1; return r;
            }
            if (x >= strip.view_right && x < strip.view_right + strip.arrow_w) {
                r.region = HitTestResult::TabScroll; r.index = 1; return r;
            }
        }
        // Raised run is on top, so it wins overlapping hits.
        if (vm.tab_drag_index >= 0 && vm.tab_drag_index < static_cast<int>(vm.tabs.size())) {
            const int dragN = std::max(1, vm.tab_drag_count);
            for (int k = vm.tab_drag_index;
                 k < vm.tab_drag_index + dragN && k < static_cast<int>(vm.tabs.size()); ++k)
                if (hitTab(k)) return r;
        }
        // Group chips: strip slots between tabs, click opens the group popup.
        for (const auto& chip : strip.chips) {
            if (strip.overflow && (x < strip.view_left || x >= strip.view_right)) break;
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
        const float cx = strip.new_x;
        if (x >= cx && x < cx + 32.0f * scale_) {
            r.region = HitTestResult::TabNew;
            return r;
        }
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
            for(int i=0;i<4;++i) if(ContainsPt(lay.disclosure[i],x,y)) {
                r.region=HitTestResult::SettingsDisclosure;r.index=i;return r;
            }
            for(int i=0;i<kPreviewCodecCount;++i) if(ContainsPt(lay.preview_codec_button[i],x,y)) {
                r.region=HitTestResult::SettingsPreviewStore;r.index=i;return r;
            }
            for(int i=0;i<3;++i) if(ContainsPt(lay.theme_tile[i],x,y)) {
                const int values[]={1,2,0};r.region=HitTestResult::SettingsTheme;r.index=values[i];return r;
            }
            if(ContainsPt(lay.effect_choice,x,y) || ContainsPt(lay.language_choice,x,y)) {
                r.region=HitTestResult::SettingsDropdown;r.index=ContainsPt(lay.effect_choice,x,y) ? 0 : 1;return r;
            }
            if(ContainsPt(lay.performance_row,x,y)) {r.region=HitTestResult::SettingsToggle;r.index=4;return r;}
            for(int list_row=0;list_row<6;++list_row)
                if(ContainsPt(lay.list_style_row[list_row],x,y)) {r.region=HitTestResult::SettingsToggle;r.index=list_row<3 ? 17+list_row : (list_row==3 ? 22 : list_row==4 ? 33 : 34);return r;}
            const D2D1_RECT_F actions[]={lay.content_pause,lay.content_options,lay.content_rebuild};
            for(int i=0;i<3;++i) if(ContainsPt(actions[i],x,y)) {r.region=HitTestResult::SettingsContentAction;r.index=i+1;return r;}

            if (vm.settings_page == 0) {
                const D2D1_RECT_F integration_rows[]={lay.default_manager_row,lay.startup_row[2],
                    lay.win_e_row,lay.this_pc_row,lay.explorer_windows_row};
                for(int i=0;i<5;++i) if(ContainsPt(integration_rows[i],x,y)) {
                    r.region=HitTestResult::SettingsIntegration;r.index=i;return r;
                }
                if(vm.settings_integration_can_retry && ContainsPt(lay.integration_retry,x,y)) {
                    r.region=HitTestResult::SettingsIntegration;r.index=5;return r;
                }
                if(vm.settings_integration_can_restore && ContainsPt(lay.integration_restore,x,y)) {
                    r.region=HitTestResult::SettingsIntegration;r.index=6;return r;
                }
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
                    if (ContainsPt(lay.text_render_row[i], x, y)) {
                        r.region = HitTestResult::SettingsTextRender;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 4; ++i) {
                    if (ContainsPt(lay.ui_font_size_row[i], x, y)) {
                        r.region = HitTestResult::SettingsUiFontSize;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.folder_sort_row[i], x, y)) {
                        r.region = HitTestResult::SettingsFolderSort;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(lay.notify_icon_row[i], x, y)) {
                        r.region = HitTestResult::SettingsNotifyIcon;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (ContainsPt(lay.startup_open_row[i], x, y)) {
                        r.region = HitTestResult::SettingsStartupOpen;
                        r.index = i;
                        return r;
                    }
                    if (ContainsPt(lay.new_tab_open_row[i], x, y)) {
                        r.region = HitTestResult::SettingsNewTabOpen;
                        r.index = i;
                        return r;
                    }
                }
                if (ContainsPt(lay.home_folder_choose, x, y)) {
                    r.region = HitTestResult::SettingsHomeFolder;
                    r.index = 0;
                    return r;
                }
                if (ContainsPt(lay.home_folder_reset, x, y)) {
                    r.region = HitTestResult::SettingsHomeFolder;
                    r.index = 1;
                    return r;
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
                // Sliders: index carries the value under the pointer.
                const auto slider_zone = [&](const D2D1_RECT_F* cells) {
                    return D2D1::RectF(cells[0].left - 12.0f * scale_, cells[0].top - 8.0f * scale_,
                                       cells[2].right, cells[2].bottom + 8.0f * scale_);
                };
                if (ContainsPt(slider_zone(lay.wallpaper_look_row), x, y)) {
                    r.region = HitTestResult::SettingsWallpaperLook;
                    r.index = SettingsSliderValue(SettingsSlider(lay.wallpaper_look_row, scale_), x,
                                                  kPanelTransparencyMax);
                    return r;
                }
                if (ContainsPt(slider_zone(lay.wallpaper_blur_row), x, y)) {
                    r.region = HitTestResult::SettingsWallpaperBlur;
                    r.index = SettingsSliderValue(SettingsSlider(lay.wallpaper_blur_row, scale_), x,
                                                  kWallpaperBlurMax);
                    return r;
                }
                if (ContainsPt(lay.hidden_files_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle;
                    r.index = 5;
                    return r;
                }
                if (ContainsPt(lay.protected_files_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 16; return r;
                }
                if (ContainsPt(lay.pinned_names_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle;
                    r.index = 6;
                    return r;
                }
                if (ContainsPt(lay.vertical_tabs_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 23; return r;
                }
                if (ContainsPt(lay.hints_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 24; return r;
                }
                if (vm.settings_tips_seen && ContainsPt(lay.hints_reset_button, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 25; return r;
                }
                if (ContainsPt(lay.shell_tags_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 21; return r;
                }
                for (int i = 0; i < 3; ++i) if (ContainsPt(lay.blank_click_choice[i], x, y)) {
                    r.region = HitTestResult::SettingsBlankClick; r.index = i; return r;
                }
                if (ContainsPt(lay.change_tracking_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 8; return r;
                }
                for (int i = 0; i < 3; ++i) if (ContainsPt(lay.change_days[i], x, y)) {
                    r.region = HitTestResult::SettingsChangeDays; r.index = i; return r;
                }
                for (int i = 0; i < 2; ++i) {
                    if (ContainsPt(lay.startup_row[i], x, y)) {
                        r.region = HitTestResult::SettingsToggle;
                        r.index = i + 1;
                        return r;
                    }
                }
                if (ContainsPt(lay.close_last_tab_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 26; return r;
                }
                if (ContainsPt(lay.close_tab_double_click_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 35; return r;
                }
                if (ContainsPt(lay.confirm_delete_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 32; return r;
                }
                if (ContainsPt(lay.start_in_tray_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 27; return r;
                }
            } else if (vm.settings_page == 1) {
                if (ContainsPt(lay.global_search_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 15; return r;
                }
                if (ContainsPt(lay.global_search_hotkey_button, x, y)) {
                    r.region = HitTestResult::SettingsGlobalSearchHotkey; return r;
                }
                if (ContainsPt(lay.search_pinyin_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 9; return r;
                }
                if (ContainsPt(lay.content_index_row, x, y)) {
                    r.region = HitTestResult::SettingsContentIndex; return r;
                }
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
                // System folder switch shares the region: 1 switch row,
                // 2 customize, 10 + i group i (only the configurable 0-4).
                if (ContainsPt(lay.index_system_more, x, y)) {
                    r.region = HitTestResult::SettingsIndexExcludeAction;
                    r.index = 2;
                    return r;
                }
                if (ContainsPt(lay.index_system_row, x, y)) {
                    r.region = HitTestResult::SettingsIndexExcludeAction;
                    r.index = 1;
                    return r;
                }
                for (int g = 0; g < kSystemGroupConfigurable; ++g) {
                    if (ContainsPt(lay.index_system_group[g], x, y)) {
                        r.region = HitTestResult::SettingsIndexExcludeAction;
                        r.index = 10 + g;
                        return r;
                    }
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
                // Pulse menu card: presets 60-62, tabs 64-66, restore 67.
                for(int i=0;i<3;++i) {
                    if(ContainsPt(lay.pulse_preset[i],x,y)) {r.region=HitTestResult::SettingsToggle;r.index=60+i;return r;}
                    if(ContainsPt(lay.pulse_tab[i],x,y)) {r.region=HitTestResult::SettingsToggle;r.index=64+i;return r;}
                }
                if(ContainsPt(lay.pulse_restore,x,y)) {r.region=HitTestResult::SettingsToggle;r.index=67;return r;}
                // Preview rows: press to drag (kSettingsMenuRowHit + item);
                // the fixed rows only report hover. 68 restores the tab's order.
                if(ContainsPt(lay.pulse_order_reset,x,y)) {r.region=HitTestResult::SettingsToggle;r.index=kSettingsMenuOrderReset;return r;}
                if(ContainsPt(lay.pulse_preview,x,y)) {
                    WindowViewModel still=vm;still.settings_menu_drag=-1;
                    for(const auto& row:LayoutPulsePreview(still,PulsePreviewBox(lay.pulse_preview,scale_),scale_).rows)
                        if(ContainsPt(row.rect,x,y)) {r.region=HitTestResult::SettingsToggle;r.index=PulseMenuRowHit(row.row);return r;}
                }
                for(int g=0;g<SettingsLayout::kContextCards;++g) {
                    if(g<5 && ContainsPt(lay.context_toggle[g],x,y)) {r.region=HitTestResult::SettingsToggle;r.index=10+g;return r;}
                    if(ContainsPt(lay.context_header[g],x,y)) {r.region=HitTestResult::SettingsDisclosure;r.index=8+g;return r;}
                }
                for(size_t i=0;i<lay.context_rows.size();++i) if(ContainsPt(lay.context_rows[i],x,y)) {
                    r.region=HitTestResult::SettingsToggle;r.index=100+static_cast<int>(i);return r;
                }
                if(ContainsPt(lay.context_restore,x,y)) {r.region=HitTestResult::SettingsRestore;return r;}
            } else if (vm.settings_page == 3) {
                for (int i = 0; i < 3; ++i) {
                    if (ContainsPt(i < 2 ? lay.about_action[i] : lay.release_all, x, y)) {
                        r.region = HitTestResult::SettingsAboutAction;
                        r.index = i;
                        return r;
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (ContainsPt(lay.apps_row[i], x, y)) {
                        r.region = HitTestResult::SettingsAboutAction;
                        r.index = 3 + i;
                        return r;
                    }
                }
                for (size_t i = 0; i < lay.release_rows.size(); ++i) {
                    if (ContainsPt(lay.release_rows[i], x, y)) {
                        r.region = HitTestResult::SettingsReleaseNote;
                        r.index = static_cast<int>(i);
                        return r;
                    }
                }
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
                        ? vm.settings_update_enabled && !vm.settings_update_checking &&
                            !vm.settings_update_downloading && !vm.settings_update_installing
                        : vm.settings_update_available && !vm.settings_update_installing;
                    if (enabled && ContainsPt(lay.update_action[i], x, y)) {
                        r.region = HitTestResult::SettingsUpdateAction;
                        r.index = i;
                        return r;
                    }
                }
                if (ContainsPt(lay.update_auto_row, x, y)) {
                    r.region = HitTestResult::SettingsToggle; r.index = 31; return r;
                }
            } else if (vm.settings_page == 5) {
                // Buttons first: they sit inside the clickable rows.
                const std::pair<const D2D1_RECT_F*, PackAction> pack_targets[] = {
                    {&lay.pack_open, PackAction::OpenFolder},
                    {&lay.pack_primary, vm.settings_pack_media_installed && !vm.settings_pack_installing
                                            ? PackAction::Remove : PackAction::Install},
                    {&lay.pack_enable, PackAction::Enable},
                    {&lay.pack_images_primary, vm.settings_pack_images_installed && !vm.settings_pack_images_installing
                                                   ? PackAction::ImagesRemove : PackAction::ImagesInstall},
                    {&lay.pack_images_enable, PackAction::ImagesEnable},
                    {&lay.pack_raw_primary, vm.settings_pack_raw_installed && !vm.settings_pack_raw_installing
                        ? PackAction::RawRemove : PackAction::RawInstall},
                    {&lay.pack_raw_enable, PackAction::RawEnable},
                    {&lay.pack_archive_primary, vm.settings_pack_archive_installed && !vm.settings_pack_archive_installing
                        ? PackAction::ArchiveRemove : PackAction::ArchiveInstall},
                    {&lay.pack_archive_enable, PackAction::ArchiveEnable},
                    {&lay.pack_detect, PackAction::UseDetected},
                    {&lay.pack_browse, PackAction::Browse},
                    {&lay.pack_custom_row, PackAction::UseCustom},
                    {&lay.pack_remove_row, PackAction::RemoveOnUninstall},
                };
                for (const auto& [bounds, action] : pack_targets) {
                    if (action == PackAction::Install && !pack_text::PrimaryEnabled(vm.settings_pack_media_available,
                        vm.settings_pack_media_installed, vm.settings_pack_installing)) continue;
                    if (action == PackAction::ImagesInstall && !pack_text::PrimaryEnabled(vm.settings_pack_images_available,
                        vm.settings_pack_images_installed, vm.settings_pack_images_installing)) continue;
                    if (action == PackAction::RawInstall && !pack_text::PrimaryEnabled(vm.settings_pack_raw_available,
                        vm.settings_pack_raw_installed, vm.settings_pack_raw_installing)) continue;
                    if (action == PackAction::ArchiveInstall && !pack_text::PrimaryEnabled(vm.settings_pack_archive_available,
                        vm.settings_pack_archive_installed, vm.settings_pack_archive_installing)) continue;
                    if (bounds->right > bounds->left && ContainsPt(*bounds, x, y)) {
                        r.region = HitTestResult::SettingsPackAction;
                        r.index = static_cast<int>(action);
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

    // Drawing and hit testing share both toolbar rows.
    bool inPeek = false;
    if (SidebarPeekVisible(rect.right)) {
        const D2D1_RECT_F peek = SidebarRect(rect.right, rect.bottom);
        inPeek = x < peek.right && y >= peek.top && y < peek.bottom;
    }
    if (!inPeek && x >= EffectiveSidebarWidth(rect.right) && y < title_bar_height_ + toolbar_height_) {
        const auto toolbar = ToolbarLayoutAt(rect.right,
            NewButtonWidthPx(rect.right-EffectiveSidebarWidth(rect.right) < 600 * scale_), vm.pane.filter_expand);
        const HitTestResult::Region nav[] = {HitTestResult::NavBack, HitTestResult::NavForward,
            HitTestResult::NavUp, HitTestResult::NavRefresh};
        const bool searchOverlay=vm.address_searching && rect.right-EffectiveSidebarWidth(rect.right)<480*scale_;
        if (!searchOverlay)
            for (int i=0; i<4; ++i) if (ContainsPt(toolbar.navigation[i],x,y)) {r.region=nav[i]; return r;}
        // Hit-test the same rect the chrome is drawn in; toolbar.search can be wider,
        // which shifted every chip target (e.g. the options icon toggled the mode).
        const auto searchBounds=searchOverlay || vm.address_searching ? SearchBarRect(rect.right) : toolbar.search;
        if (ContainsPt(searchBounds,x,y)) {
            r.region = HitTestResult::AddressSearch;
            if (vm.address_searching) {
                const auto layout = LayoutAddressSearch(searchBounds,scale_);
                r.region = HitTestResult::AddressSearchInput;
                if (ContainsPt(layout.scope,x,y)) r.region=HitTestResult::AddressSearchScope;
                else if (ContainsPt(layout.name,x,y)) r.region=HitTestResult::AddressSearchMode;
                else if (ContainsPt(layout.content,x,y)) r.region=HitTestResult::AddressSearchContent;
                else if (ContainsPt(layout.close,x,y)) r.region=vm.address_editing && vm.address_search_has_text
                    ? HitTestResult::AddressSearchClear : HitTestResult::AddressSearchClose;
                else if (ContainsPt(layout.options,x,y)) r.region=HitTestResult::AddressSearchOptions;
                else if (vm.address_search_has_text && ContainsPt(layout.clear,x,y)) r.region=HitTestResult::AddressSearchClear;
            }
            return r;
        }
        if (ContainsPt(toolbar.address,x,y)) {
            r.region=HitTestResult::AddressBar;
            if (!vm.address_editing && x < toolbar.address.right-28*scale_) {
                std::vector<BreadcrumbPlaced> placed;
                BreadcrumbLayout(vm.pane,rect.right,placed);
                for (size_t i=0;i<placed.size();++i) if (ContainsPt(placed[i].rc,x,y)) {
                    r.region=HitTestResult::BreadcrumbSegment; r.index=(int)i; r.path=placed[i].path; break;
                }
            }
            return r;
        }
        if (ContainsPt(toolbar.sort,x,y)) {r.region=HitTestResult::ToolbarSort; return r;}
        if (toolbar.group.right>toolbar.group.left && ContainsPt(toolbar.group,x,y)) {
            const bool chip=toolbar_group_>0 && toolbar.group.right-toolbar.group.left>=60*scale_;
            r.region=chip && ContainsPt(ToolbarGroupClearRect(toolbar.group,scale_),x,y)
                ? HitTestResult::ToolbarGroupClear : HitTestResult::ToolbarGroup;
            return r;
        }
        if (ContainsPt(toolbar.overflow,x,y)) {r.region=HitTestResult::ToolbarMore; return r;}
        if (ContainsPt(toolbar.filter,x,y)) {
            r.region = !vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f &&
                ContainsPt(FilterClearRect(rect,vm.pane.filter_expand),x,y)
                ? HitTestResult::FilterClear : HitTestResult::FilterBox;
            return r;
        }
        if (ContainsPt(toolbar.create,x,y)) {r.region=HitTestResult::NewButton; return r;}
        const HitTestResult::Region commands[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
            HitTestResult::Rename,HitTestResult::Delete,HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
        for(int i=0;i<8;++i) if(ContainsPt(toolbar.commands[i],x,y)) {r.region=commands[i]; return r;}
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
        D2D1_RECT_F track{}, thumb{};
        float max_scroll = 0.0f;
        if (SidebarScrollbarGeometry(vm, rect.right, rect.bottom, track, thumb, max_scroll) &&
            ContainsPt(track, x, y)) {
            r.region = HitTestResult::Scrollbar;
            r.sub_index = 2; // Sidebar, independent of pane scrollbars.
            return r;
        }
        std::vector<SidebarSlot> slots;
        std::vector<SidebarGroupBand> bands;
        LayoutSidebar(vm, sb, scale_, slots, &bands);
        const bool compact = SidebarRailLayout(sb.right - sb.left, scale_);
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            const auto& slot = slots[i];
            if (!ContainsPt(slot.rc, x, y)) continue;
            if (slot.kind == SidebarSlot::RailMore) continue; // decoration only
            if (slot.kind == SidebarSlot::TrayRelease) {
                r.region = HitTestResult::TrayRelease;
                r.index = slot.batch;
                return r;
            }
            if (slot.kind == SidebarSlot::TrayClear) {
                r.region = HitTestResult::TrayClear;
                return r;
            }
            if (slot.kind == SidebarSlot::Rail) {
                // Narrow rail: a folded section is one icon row; clicking it
                // reopens the section.
                r.region = HitTestResult::SidebarHeader;
                r.index = slot.group;
                if (slot.group >= 0 && slot.group < static_cast<int>(vm.sidebar.size())) {
                    r.sidebar_section = vm.sidebar[slot.group].id;
                    r.label = vm.sidebar[slot.group].header;
                }
                return r;
            }
            if (slot.kind == SidebarSlot::Header) {
                const float action_left = slot.rc.right - 56.0f * scale_;
                const float action_right = slot.rc.right - 28.0f * scale_;
                r.sidebar_action = vm.sidebar[slot.group].add_action;
                r.region = r.sidebar_action != SidebarAddAction::None &&
                           x >= action_left && x < action_right
                    ? HitTestResult::SidebarHeaderAction : HitTestResult::SidebarHeader;
                r.index = slot.group;
                r.sidebar_section = vm.sidebar[slot.group].id;
                r.label = vm.sidebar[slot.group].header;
                const SidebarGroup& group = vm.sidebar[slot.group];
                if (r.region == HitTestResult::SidebarHeader && group.navigable) {
                    // #80: sub_index 1 = the title link; the rest folds.
                    const D2D1_RECT_F title = painter_.SidebarSectionHeaderTitleRect(
                        slot.rc, group.header, !group.icon_glyph.empty());
                    if (ContainsPt(title, x, y)) r.sub_index = 1;
                }
                return r;
            }
            if (slot.kind == SidebarSlot::TrayPanel) {
                // Card stack: only the top live card is interactive (the
                // peeking layers are decoration); ghosts are inert. Footer
                // pager buttons page through the stack.
                if (!compact) {
                    const TrayDeckView& deck = vm.tray_deck;
                    if (deck.live_count > 0) {
                        if (TrayDestRowH(deck, scale_) > 0.0f) {
                            for (int d = 0; d < static_cast<int>(deck.dests.size()); ++d) {
                                if (!ContainsPt(TrayDestChipRect(slot.rc, deck, scale_, d), x, y))
                                    continue;
                                r.region = HitTestResult::TrayDest;
                                r.index = d;
                                r.path = deck.dests[static_cast<size_t>(d)].path;
                                return r;
                            }
                        }
                        if (TrayStaleRowH(deck, scale_) > 0.0f) {
                            for (int b = 0; b < 2; ++b) {
                                if (!ContainsPt(TrayStaleButtonRect(slot.rc, deck, scale_, b), x, y))
                                    continue;
                                r.region = HitTestResult::TrayStale;
                                r.index = b;
                                return r;
                            }
                        }
                        const D2D1_RECT_F deck_rc = TrayDeckArea(slot.rc, deck, scale_);
                        const std::wstring index_text = TrayIndexText(deck);
                        const TrayFooterGeom f = TrayFooterGeometry(deck_rc, scale_,
                            deck.comparing ? 1 : deck.total_count, compositor_ ? MeasureTextWidth(
                                compositor_->DwriteFactory(), compositor_->SmallFormat(),
                                index_text) : 30.0f * scale_, deck.can_compare);
                        if (deck.can_compare && ContainsPt(f.compare, x, y)) {
                            r.region = HitTestResult::TrayCompare;
                            r.index = 0;
                            return r;
                        }
                        if (f.pager && ContainsPt(f.prev, x, y)) {
                            r.region = HitTestResult::TrayPrev;
                            return r;
                        }
                        if (f.pager && ContainsPt(f.next, x, y)) {
                            r.region = HitTestResult::TrayNext;
                            return r;
                        }
                        if (deck.comparing) {
                            // Compare table: only the content cell is live (before it runs).
                            const TrayCompareGeom cg = TrayCompareGeometry(deck_rc, scale_, deck.thumb_dip);
                            const bool can_diff = deck.compare.content == 3 && deck.compare.text == 1;
                            if ((deck.compare.content == 0 || can_diff) &&
                                ContainsPt(TrayCompareCell(cg, 4, 2), x, y)) {
                                r.region = HitTestResult::TrayCompare;
                                r.index = can_diff ? 2 : 1;
                            }
                            return r;
                        }
                        const TrayStackGeom g = TrayStackGeometry(deck_rc, scale_, deck.thumb_dip);
                        for (int c = 0; c < deck.live_count; ++c) {
                            const TrayCardView& card = deck.cards[static_cast<size_t>(c)];
                            if (card.ghost || card.depth > 0.5f) continue;
                            bool close_zone = false;
                            bool intent_zone = false;
                            if (!TrayCardHit(g, card, deck.spread, x, y, &close_zone,
                                             &intent_zone)) continue;
                            if (intent_zone && !close_zone && card.batch >= 0) {
                                r.region = HitTestResult::TrayIntent;
                                r.index = card.batch;
                                r.sub_index = c; // display index (hover identity)
                            } else if (close_zone) {
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
                }
                return r;
            }
            if (slot.kind == SidebarSlot::Item || slot.kind == SidebarSlot::Drive ||
                slot.kind == SidebarSlot::Tag) {
                if (slot.group >= 0 && slot.item >= 0) {
                    const auto& item = vm.sidebar[slot.group].items[slot.item];
                    if (!compact && SidebarItemHasUnpin(item) &&
                        ContainsPt(WorkspaceUnpinRect(slot.rc, scale_), x, y)) {
                        r.region = HitTestResult::SidebarItemAction;
                        r.index = slot.run;
                        r.path = item.path;
                        return r;
                    }
                    if (!compact && item.expandable &&
                        ContainsPt(SidebarExpandRect(slot.rc, scale_), x, y)) {
                        r.region = HitTestResult::SidebarItemExpand;
                        r.index = slot.run;
                        r.path = item.path;
                        return r;
                    }
                    r.path = item.path;
                }
                r.region = HitTestResult::SidebarItem;
                r.index = slot.run;
                if (slot.group >= 0 && slot.group < static_cast<int>(vm.sidebar.size())) {
                    r.sidebar_section = vm.sidebar[slot.group].id;
                    r.sidebar_item = slot.item;
                    if (slot.item >= 0 &&
                        slot.item < static_cast<int>(vm.sidebar[slot.group].items.size()))
                        r.label = vm.sidebar[slot.group].items[slot.item].label;
                }
                return r;
            }
        }
        // Empty space that sits inside a section reports that section: the
        // right-click menu must match what the cursor is over. Space below every
        // section keeps index -1 and falls back to the pane-wide menu.
        r.region = HitTestResult::SidebarBlank;
        for (const auto& band : bands) {
            if (y >= band.top && y < band.bottom) { r.index = band.group; break; }
        }
        // Report the section too, so callers can key off ids everywhere.
        if (r.index >= 0 && r.index < static_cast<int>(vm.sidebar.size()))
            r.sidebar_section = vm.sidebar[static_cast<size_t>(r.index)].id;
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
            const float previewH = vm.details.preview_enabled
                ? DetailsPreviewHeight(panel, scale_, vm.details.preview_expansion) : 0.0f;
            LayoutDetailsPanel(panel, scale_, vm.details,
                               compositor_ ? compositor_->DwriteFactory() : nullptr,
                               compositor_ ? compositor_->SmallFormat() : nullptr,
                               compositor_, previewH, hitRects);
            if (ContainsPt(hitRects.preview_enable, x, y)) { r.region = HitTestResult::DetailsPreviewEnable; return r; }
            if (ContainsPt(hitRects.preview, x, y)) { r.region = HitTestResult::DetailsPreview; return r; }
            if (ContainsPt(hitRects.preview_toggle, x, y)) { r.region = HitTestResult::DetailsPreviewToggle; return r; }
            if (vm.details.preview_only && vm.details.preview_enabled) return r;
            if (ContainsPt(hitRects.rename, x, y)) { r.region = HitTestResult::DetailsRename; return r; }
            if (ContainsPt(hitRects.open, x, y)) { r.region = HitTestResult::DetailsOpen; return r; }
            if (ContainsPt(hitRects.star, x, y)) { r.region = HitTestResult::DetailsStar; return r; }
            if (ContainsPt(hitRects.new_tab, x, y)) { r.region = HitTestResult::DetailsNewTab; return r; }
            if (ContainsPt(hitRects.copy_path, x, y)) { r.region = HitTestResult::DetailsCopyPath; return r; }
            if (ContainsPt(hitRects.more, x, y)) { r.region = HitTestResult::DetailsMore; return r; }
            if (ContainsPt(hitRects.tag_add, x, y)) { r.region = HitTestResult::DetailsTagAdd; return r; }
            if (ContainsPt(hitRects.attr_advanced, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 2; return r; }
            if (ContainsPt(hitRects.attr_readonly, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 0; return r; }
            if (ContainsPt(hitRects.attr_hidden, x, y)) { r.region = HitTestResult::DetailsAttrToggle; r.index = 1; return r; }
            if (ContainsPt(hitRects.security_change, x, y)) { r.region = HitTestResult::DetailsSecurityChange; return r; }
            for (size_t i = 0; i < hitRects.preset_chips.size(); ++i) {
                if (ContainsPt(hitRects.preset_chips[i], x, y)) {
                    r.region = HitTestResult::DetailsPresetTag;
                    r.index = hitRects.preset_ids[i];
                    return r;
                }
            }
            for (size_t i = 0; i < hitRects.section_headers.size(); ++i) {
                if (ContainsPt(hitRects.section_headers[i], x, y)) {
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

    // The swap button only exists while its divider (or the button) is hovered,
    // so it never takes clicks meant for the pane edges underneath.
    if (vm.hover_region == static_cast<int>(HitTestResult::Splitter) && !vm.splitter_pressed) {
        for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
            const auto& sp = vm.splitters[static_cast<size_t>(i)];
            if (sp.swap_rect.right <= sp.swap_rect.left) continue;
            if (vm.hover_control_index != i && vm.hover_control_index != kSplitterSwapIndex + i) continue;
            if (x >= sp.swap_rect.left && x < sp.swap_rect.right &&
                y >= sp.swap_rect.top && y < sp.swap_rect.bottom) {
                r.region = HitTestResult::Splitter;
                r.index = kSplitterSwapIndex + i;
                return r;
            }
        }
    }
    for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
        const auto& sp = vm.splitters[static_cast<size_t>(i)];
        if (x >= sp.hit_rect.left && x < sp.hit_rect.right &&
            y >= sp.hit_rect.top && y < sp.hit_rect.bottom) {
            r.region = HitTestResult::Splitter;
            r.index = i;
            return r;
        }
    }

    auto hitPaneBounds = [&](const PaneViewModel& paneVm, const D2D1_RECT_F& paneRect, int paneIndex, bool focused) -> HitTestResult {
        // Header controls use the full pane; below the header the column
        // view narrows the regular list to its body.
        D2D1_RECT_F paneRc = paneRect;
        HitTestResult out;
        out.pane_index = paneIndex;
        if (x < paneRc.left || x >= paneRc.right || y < paneRc.top || y >= paneRc.bottom)
            return out;
        const D2D1_RECT_F detailsRc = PaneDetailsRect(paneRc);
        if (compositor_ && ContainsPt(ChangeTitleRect(paneRc, detailsRc.left - 8 * scale_, pane_header_height_, paneVm.title_change_badge, scale_, compositor_, paneVm.header_text), x, y)) {
            out.region = HitTestResult::ChangeBadge; out.index = -1; return out;
        }
        if (focused && paneVm.header_controls_opacity > 0.05f) {
            const D2D1_RECT_F buttons[] = {detailsRc, PaneMediumIconsRect(paneRc), PaneViewButtonRect(paneRc)};
            const HitTestResult::Region actions[] = {HitTestResult::PaneDetails, HitTestResult::PaneMediumIcons, HitTestResult::PaneViewButton};
            for (int i=0;i<3;++i) if (ContainsPt(buttons[i],x,y)) {
                out.region=actions[i]; out.index=paneIndex; return out;
            }
        }
        if (y >= paneRect.top + pane_header_height_ && paneVm.column_strip.Active()) {
            if (HitTestColumnStrip(paneVm, paneRect, x, y, out)) return out;
            paneRc = PaneBodyBounds(paneVm, paneRect);
        }
        const float banner = PaneBannerHeight(paneVm, paneRc.right - paneRc.left, scale_, compositor_);
        if ((paneVm.is_content_search || paneVm.network_live_action) && banner > 0) {
            const float action_width = std::min(144.0f * scale_, (paneRc.right - paneRc.left - 16 * scale_) * 0.4f);
            if (x >= paneRc.right - 8 * scale_ - action_width && x < paneRc.right - 8 * scale_ &&
                y >= paneRc.top + pane_header_height_ && y < paneRc.top + pane_header_height_ + banner - 4 * scale_) {
                out.region = paneVm.is_content_search ? HitTestResult::ContentIndexManage
                                                      : HitTestResult::NetworkIndexAdd;
                out.index = paneIndex;
                return out;
            }
        }
        if (paneVm.compare_active && !paneVm.is_changes && !paneVm.is_content_search && banner > 0 &&
            y >= paneRc.top + pane_header_height_ && y < paneRc.top + pane_header_height_ + banner - 4 * scale_) {
            const D2D1_RECT_F bar = D2D1::RectF(paneRc.left + 8 * scale_, paneRc.top + pane_header_height_,
                paneRc.right - 8 * scale_, paneRc.top + pane_header_height_ + banner - 4 * scale_);
            D2D1_RECT_F diffRc{}, exitRc{};
            CompareBannerButtons(bar, scale_, diffRc, exitRc);
            if (ContainsPt(diffRc, x, y)) {
                out.region = HitTestResult::CompareDiffToggle;
                out.index = paneIndex;
                return out;
            }
            if (ContainsPt(exitRc, x, y)) {
                out.region = HitTestResult::CompareExit;
                out.index = paneIndex;
                return out;
            }
        }
        const float extra = PaneExtraTop(paneVm, scale_, paneRc.right - paneRc.left, compositor_);
        const float recentTop = paneRc.top + pane_header_height_ + banner;
        const float filterExtra = (paneVm.is_recent ? kRecentControlsDip * scale_ : 0.0f) +
                                  (paneVm.is_query_search ? kSearchFiltersDip * scale_ : 0.0f);
        const float changesTop = recentTop + filterExtra;
        if (paneVm.is_changes && y >= changesTop && y < changesTop + 36 * scale_) {
            const float cw = std::max(1.0f, (paneRc.right - paneRc.left - 16 * scale_) / 3);
            const int chip = static_cast<int>((x - paneRc.left - 8 * scale_) / cw);
            if (chip >= 0 && chip < 3 && (chip < 2 || paneVm.change_has_more)) {
                out.region = chip == 0 ? HitTestResult::ChangeTimeFilter : chip == 1 ? HitTestResult::ChangeTypeFilter : HitTestResult::ChangeMore;
                out.index = chip; return out;
            }
        }
        const float columnTop = changesTop + (paneVm.is_changes ? 36 * scale_ : 0);
        float listTop = paneRc.top + pane_header_height_ + extra +
                        (ShowsColumnHeader(paneVm.view_mode) ? column_header_height_ : 0.0f);
        if (y >= paneRc.top && y < paneRc.top + pane_header_height_) {
            out.region = HitTestResult::PaneHeader;
            return out;
        }
        if (paneVm.is_recent && y >= recentTop && y < columnTop) {
            for (int i = 0; i < 3; ++i) {
                if (ContainsPt(RecentFilterRect(
                        paneRc, pane_header_height_ + banner, scale_, i), x, y)) {
                    out.region = HitTestResult::RecentFilter;
                    out.index = i;
                    return out;
                }
            }
            if (ContainsPt(RecentClearRect(
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
                if (ContainsPt(SearchFilterRect(
                        paneRc, pane_header_height_ + banner, scale_, i, widths), x, y)) {
                    out.region = HitTestResult::SearchFilter;
                    out.index = i;
                    out.control_bounds = SearchFilterRect(
                        paneRc, pane_header_height_ + banner, scale_, i, widths);
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
            int col = 0;
            while (col < columns.count - 1 && x >= columns.DividerX(col)) ++col;
            switch (columns.kinds[static_cast<size_t>(col)]) {
            case ColumnKind::Name: out.column = SortColumn::Name; break;
            case ColumnKind::Path:
                if (paneVm.content_results) out.column = SortColumn::Path;
                else out.region = HitTestResult::Pane;
                break;
            case ColumnKind::Date: out.column = SortColumn::Mtime; break;
            case ColumnKind::Type: out.column = SortColumn::Type; break;
            case ColumnKind::Size: out.column = SortColumn::Size; break;
            case ColumnKind::Created: out.column = SortColumn::Created; break;
            case ColumnKind::Accessed: out.column = SortColumn::Accessed; break;
            }
            return out;
        }
        if (y >= listTop && y < paneRc.bottom) {
            if (paneVm.search_retaining_results) {
                out.region = HitTestResult::Pane;
                return out;
            }
            if (!paneVm.loading && paneVm.EntryCount() == 0 && !paneVm.filter_text.empty() &&
                !paneVm.is_changes) {
                const int first = paneVm.is_file_system ? 0 : 1;
                const SearchEmptyLayout emptyLayout = MakeSearchEmptyLayout(
                    D2D1::RectF(paneRc.left, listTop, paneRc.right, paneRc.bottom), scale_, 2 - first);
                for (int i = 0; i < 2 - first; ++i) {
                    const D2D1_RECT_F& rc = emptyLayout.buttons[i];
                    if (rc.right - rc.left < 40.0f * scale_) break;
                    if (ContainsPt(rc, x, y)) {
                        out.region = HitTestResult::FilterEmptyAction;
                        out.index = first + i;
                        return out;
                    }
                }
            }
            if (!paneVm.loading && paneVm.EntryCount() == 0 && paneVm.filter_text.empty() &&
                paneVm.is_query_search && !paneVm.is_changes) {
                app::SearchEmptyAction actions[3];
                const int count = app::SearchEmptyActions(paneVm.search_query, actions);
                const SearchEmptyLayout emptyLayout = MakeSearchEmptyLayout(
                    D2D1::RectF(paneRc.left, listTop, paneRc.right, paneRc.bottom), scale_, count);
                for (int i = 0; i < count; ++i) {
                    const D2D1_RECT_F& rc = emptyLayout.buttons[i];
                    if (rc.right - rc.left < 40.0f * scale_) break;
                    if (ContainsPt(rc, x, y)) {
                        out.region = HitTestResult::SearchEmptyAction;
                        out.index = i;
                        out.path = app::ApplySearchEmptyAction(paneVm.search_query, actions[i]);
                        return out;
                    }
                }
            }
            if (!paneVm.loading && paneVm.EntryCount() == 0 &&
                paneVm.filter_text.empty() && paneVm.is_file_system && paneVm.can_create) {
                const PaneEmptyLayout emptyLayout = MakePaneEmptyLayout(
                    D2D1::RectF(paneRc.left, listTop, paneRc.right, paneRc.bottom),
                    scale_, true);
                if (!paneVm.is_changes && emptyLayout.show_action && ContainsPt(emptyLayout.action, x, y)) {
                    out.region = HitTestResult::PaneEmptyNewFolder;
                    out.index = paneIndex;
                    return out;
                }
            }
            if (paneVm.view_mode == ViewMode::List &&
                y >= paneRc.bottom - 12.0f * scale_ &&
                MaxScrollXForPane(paneVm, paneRect) > 0.0f) {
                out.region = HitTestResult::Scrollbar;
                out.sub_index = 1;
                return out;
            }
            if (x >= paneRc.right - 14.0f * scale_) {
                out.region = HitTestResult::Scrollbar;
                return out;
            }
            if (const ListGroups* groups = paneVm.Groups()) {
                const D2D1_RECT_F list = PaneListRect(paneRc, extra, paneVm.view_mode);
                const ViewLayout grouped(paneVm.view_mode, list, paneVm.EntryCount(),
                                         paneVm.scroll_x, paneVm.scroll_y, scale_,
                                         ListRowHeightDip(paneVm, list), groups);
                D2D1_RECT_F header{};
                int g = grouped.StickyHeader(&header);
                if (g < 0 || !ContainsPt(header, x, y)) {
                    g = grouped.HeaderHitTest(x, y);
                    header = g >= 0 ? grouped.HeaderRect(g) : D2D1_RECT_F{};
                }
                if (g >= 0) {
                    const D2D1_RECT_F content = DetailsContentRect(header, scale_);
                    out.region = GroupSelectVisible(content, scale_) &&
                                 ContainsPt(GroupSelectRect(content, scale_), x, y)
                        ? HitTestResult::GroupSelect : HitTestResult::GroupHeader;
                    out.index = g;
                    out.pane_index = paneIndex;
                    return out;
                }
            }
            int idx = ItemFromPointInPane(paneVm, paneRect, x, y);
            if(idx>=0 && paneVm.content_results && !paneVm.content_results->Ready(static_cast<size_t>(idx))) {
                paneVm.content_results->Prefetch(static_cast<size_t>(idx));
                out.region=HitTestResult::Pane; return out;
            }
            if (idx >= 0) {
                if (idx != paneVm.rename_index) {
                    int viewRow = paneVm.ViewIndex(idx);
                    if (viewRow >= 0 && compositor_ && compositor_->DwriteFactory()) {
                        const D2D1_RECT_F list = PaneListRect(paneRc, extra, paneVm.view_mode);
                        const DetailsColumnLayout columns = DetailsColumns(list, paneVm);
                        ViewLayout layout(paneVm.view_mode, list, paneVm.EntryCount(),
                                          paneVm.scroll_x, paneVm.scroll_y, scale_,
                                          ListRowHeightDip(paneVm, list), paneVm.Groups());
                        const D2D1_RECT_F nameRc = layout.NameRect(viewRow);
                        const D2D1_RECT_F cell = layout.ItemRect(viewRow);
                        const ListEntryView& entry = MakeVisibleEntry(paneVm, static_cast<size_t>(idx));
                        const auto folder_size_rect = paneVm.view_mode == ViewMode::Details
                            ? D2D1::RectF(columns.Left(ColumnKind::Size), cell.top,
                                columns.Left(ColumnKind::Size) + columns.Width(ColumnKind::Size), cell.bottom)
                            : layout.FolderSizeRect(viewRow);
                        if (ShowsFolderSize(paneVm.view_mode) && paneVm.folder_size_actions.contains(idx) &&
                            ContainsPt(folder_size_rect, x, y)) {
                            out.region = HitTestResult::RowFolderSize; out.index = idx; return out;
                        }
                        const std::vector<int>* tagIndices = paneVm.tag_catalog
                            ? paneVm.tag_catalog->TagIndicesForPath(entry.path) : nullptr;
                        size_t tagCount = tagIndices ? tagIndices->size() : 0;
                        if (!tagIndices && paneVm.tag_dots) {
                            const auto found = paneVm.tag_dots->find(idx);
                            if (found != paneVm.tag_dots->end()) tagCount = found->second.size();
                        }
                        const bool rowHot = paneVm.hover_index == idx ||
                            (paneVm.selected_index == idx && paneVm.selected_count == 1);
                        const bool showActions = paneVm.view_mode == ViewMode::Details && idx != paneVm.rename_index &&
                            (rowHot || entry.starred);
                        const auto changeIt = paneVm.change_badges.find(idx);
                        const ChangeBadge* change = changeIt != paneVm.change_badges.end() && HasChangeBadge(changeIt->second) ? &changeIt->second : nullptr;
                        const bool grid = paneVm.view_mode == ViewMode::ExtraLargeIcons || paneVm.view_mode == ViewMode::LargeIcons || paneVm.view_mode == ViewMode::MediumIcons;
                        if (change && grid) {
                            const float bw = std::min(ChangeBadgeWidth(*change, scale_, compositor_), cell.right - cell.left - 12 * scale_);
                            const float bx = (cell.left + cell.right - bw) * 0.5f;
                            if (ContainsPt(D2D1::RectF(bx, cell.top + 2 * scale_, bx + bw, cell.top + 20 * scale_), x, y)) {
                                out.region = HitTestResult::ChangeBadge; out.index = idx; return out;
                            }
                        }
                        const float badgeW = change && !grid ? ChangeBadgeWidth(*change, scale_, compositor_) : !entry.badge.empty()
                            ? std::min(108.0f * scale_, painter_.MeasureTagWidth(entry.badge))
                            : 0.0f;
                        const bool rowSnippet = !entry.snippet.empty();
                        const DetailsNameLine nameLine =
                            MakeDetailsNameLine(nameRc, cell, scale_, rowSnippet);
                        const NameTrail trail = LayoutNameTrail(
                            nameRc.left, nameLine.y, nameLine.h,
                            paneVm.view_mode == ViewMode::Details ? columns.DividerX(0) - margin_ : nameRc.right, cell.top, cell.bottom, scale_,
                            entry.name, static_cast<int>(std::min<size_t>(3, tagCount)), badgeW,
                            showActions, showActions && paneVm.hover_index == idx && entry.is_dir,
                            showActions && rowHot,
                            compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(), change != nullptr,
                            paneVm.view_mode == ViewMode::Details ? (entry.is_dir ? 3 : 2) : 0,
                            NameMatchRanges(entry.name, NameHighlightTerms(paneVm.filter_text,
                                paneVm.is_search ? paneVm.search_query : L"")),
                            row_actions_);
                        if (change && !grid && ContainsPt(trail.badge, x, y)) {
                            out.region = HitTestResult::ChangeBadge; out.index = idx; return out;
                        }
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
                return hitPaneBounds(slot.pane, slot.rect, i, slot.focused);
            }
        }
        return r;
    }
    return hitPaneBounds(vm.pane, content, 0, true);
}

} // namespace pulse::ui
