// ui_settings_layout.cpp — MakeSettingsLayout: the geometry of every settings
// page, shared by drawing (ui_settings_*.cpp) and hit testing (ui_hit_test.cpp).
// It used to live in ui_renderer_internal.h's anonymous namespace, which put a
// copy of it (and of the page layouts it calls) into each of those files.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"

namespace pulse::ui {

SettingsLayout MakeSettingsLayout(const WindowViewModel& vm, const D2D1_RECT_F& window,
                                  float scale, float title_h, float status_h,
                                  const fluent::Painter* painter) {
    // The docked 助手 column takes the left edge (vm.settings_inset_left).
    const D2D1_RECT_F rect = D2D1::RectF((std::min)(window.right, window.left + vm.settings_inset_left),
                                         window.top, window.right, window.bottom);
    SettingsLayout l;
    l.body = D2D1::RectF(rect.left, title_h, rect.right, rect.bottom - status_h);
    const float nav_width = (rect.right - rect.left < 760 * scale ? 64.0f :
        rect.right - rect.left < 1000 * scale ? 200.0f : 220.0f) * scale;
    l.nav = D2D1::RectF(l.body.left, l.body.top, l.body.left + nav_width, l.body.bottom);
    l.content = D2D1::RectF(l.nav.right, l.body.top, l.body.right, l.body.bottom);
    l.search_box = D2D1::RectF(l.nav.left + 8*scale, l.nav.top + 10*scale, l.nav.right - 8*scale, l.nav.top + 46*scale);
    if (nav_width < 100*scale && vm.settings_search_edit_visible) l.search_box.right = (std::min)(l.body.right-8*scale, l.nav.left+252*scale);
    if (!vm.settings_search_query.empty() && (nav_width >= 100*scale || vm.settings_search_edit_visible)) l.search_clear = D2D1::RectF(l.search_box.right-30*scale, l.search_box.top+4*scale, l.search_box.right-4*scale, l.search_box.bottom-4*scale);
    const float nav_top = l.nav.top + 52*scale;
    {
        // 关于 (3) stays at the bottom; 系统右键菜单 (6) has no row of its own: the
        // 右键菜单 (2) layer cards switch between the two, see ctx_layer.
        const bool compact = nav_width < 100.0f * scale;
        const float about_top = l.nav.bottom - 92.0f * scale;
        l.nav_row[3] = D2D1::RectF(l.nav.left + 8*scale, about_top, l.nav.right - 8*scale, about_top + 40*scale);
        const float avail = about_top - 12.0f * scale - nav_top;
        const int rows = static_cast<int>(std::size(kSettingsNavOrder));
        // Roomiest arrangement that fits: headings, then separators, then tighter rows.
        float pitch = 44.0f, row_h = 38.0f, head = 28.0f, top_pad = 10.0f;
        bool headings = !compact;
        auto needed = [&]() {
            return (top_pad + rows * pitch + (headings ? kSettingsNavGroups * head : (kSettingsNavGroups - 1) * 13.0f)) * scale;
        };
        if (needed() > avail) headings = false;
        if (needed() > avail) { pitch = 40.0f; row_h = 36.0f; }
        if (needed() > avail) { pitch = 36.0f; row_h = 33.0f; top_pad = 6.0f; }
        // Still too short (a low window): 关于 joins the list as its last row
        // after a separator and the version line gives way (关于 shows it), then
        // rows and separators tighten, and as a last resort share the height.
        float sep = 13.0f;
        const bool about_in_list = needed() > avail;
        if (about_in_list) {
            const float room = (l.nav.bottom - nav_top) / scale - 6.0f;
            auto fits = [&]() { return top_pad + (rows + 1) * pitch + kSettingsNavGroups * sep <= room; };
            if (!fits()) { pitch = 34.0f; row_h = 31.0f; sep = 11.0f; }
            if (!fits()) { pitch = 32.0f; row_h = 30.0f; sep = 9.0f; top_pad = 4.0f; }
            if (!fits()) { pitch = 30.0f; row_h = 28.0f; sep = 7.0f; }
            if (!fits()) {
                pitch = (std::max)(26.0f, (room - top_pad - kSettingsNavGroups * sep) / static_cast<float>(rows + 1));
                row_h = pitch - 2.0f;
            }
        }
        float y = nav_top + top_pad * scale;
        int group = 0;
        for (int k = 0; k < rows; ++k) {
            if (group < kSettingsNavGroups && kSettingsNavGroupStart[group] == k) {
                if (headings) {
                    l.nav_group_label[group] = D2D1::RectF(l.nav.left + 20*scale, y + 6*scale, l.nav.right - 12*scale, y + head*scale);
                    y += head * scale;
                } else if (group > 0) {
                    const float mid = std::floor(sep * 0.5f);
                    l.nav_separator[group] = D2D1::RectF(l.nav.left + 16*scale, y + mid*scale, l.nav.right - 16*scale, y + (mid + 1.0f)*scale);
                    y += sep * scale;
                }
                ++group;
            }
            const int page = kSettingsNavOrder[k];
            const float top = y + (pitch - row_h) * 0.5f * scale;
            l.nav_row[page] = D2D1::RectF(l.nav.left + 8*scale, top, l.nav.right - 8*scale, top + row_h*scale);
            y += pitch * scale;
        }
        if (about_in_list) {
            // The divider above 关于 is drawn 12 px over its row (settings view).
            y += sep * scale;
            const float top = (std::max)(y + (pitch - row_h) * 0.5f * scale, y - sep * scale + 13.0f * scale);
            l.nav_row[3] = D2D1::RectF(l.nav.left + 8*scale, top, l.nav.right - 8*scale, top + row_h*scale);
        }
        l.nav_row[6] = {};
        l.nav_row[kSettingsPageAi] = {};
    }
    const float pad = 20.0f * scale;
    auto label_btn_w = [&](std::wstring_view label) {
        if (painter) return painter->MeasureButtonWidth(label);
        return 88.0f * scale;
    };
    l.content_origin = l.content.top - (vm.settings_page == kSettingsPageNativeExtensions && vm.settings_search_query.empty() ? 0.0f : vm.settings_scroll);
    if (!vm.settings_search_query.empty()) {
        if (vm.settings_search_results.empty()) l.search_empty_clear = D2D1::RectF(l.content.left+20*scale, l.content_origin+132*scale, (std::min)(l.content.right-20*scale,l.content.left+92*scale), l.content_origin+164*scale);
        float result_y = l.content_origin + 76*scale;
        for (size_t i = 0; i < vm.settings_search_results.size(); ++i) {
            l.search_results.push_back(D2D1::RectF(l.content.left + pad, result_y, l.content.right - pad, result_y + 64*scale));
            result_y += 72*scale;
        }
        l.content_h = result_y - l.content_origin + pad;
        return l;
    }
    float y = l.content_origin + pad;
    l.page_heading=D2D1::RectF(l.content.left+pad,y,l.content.right-pad,y+36.0f*scale);
    y = l.page_heading.bottom;
    y += 8.0f * scale;
    if (IsGeneralSettingsPage(vm.settings_page)) {
        y = LayoutSettingsGeneral(l, vm, scale, y, painter);
    } else if (vm.settings_page == 1) {
        const float card_left = l.content.left + pad;
        const float card_right = l.content.right - pad;
        // The master switch has its own card above every search setting.
        y += 30*scale;
        l.search_master_section = D2D1::RectF(card_left, y, card_right, y + 28*scale);
        y += 30*scale;
        l.search_master_row = D2D1::RectF(card_left, y, card_right, y + 88*scale);
        y += 88*scale;
        if (!vm.settings_search_enabled) {
            // Search off: only the master switch; nothing below it applies.
            y += 24.0f * scale;
        } else {
        y += 24*scale;
        l.section[0] = D2D1::RectF(card_left, y, card_right, y + 28*scale);
        y += 30*scale;
        l.global_search_row = D2D1::RectF(card_left, y, card_right, y + 88*scale);
        y += 88*scale;
        l.global_search_hotkey_row = D2D1::RectF(card_left, y, card_right, y + 108*scale);
        l.global_search_hotkey_button = D2D1::RectF(card_left + 54*scale, y + 62*scale, card_right - 16*scale, y + 98*scale);
        y += 108*scale;
        l.global_search_double_ctrl_row = D2D1::RectF(card_left, y, card_right, y + 68*scale);
        y += 68*scale;
        {
            // Same shape as the other three-choice rows: beside the label, or
            // below it when the card is narrow.
            const bool narrow = card_right - card_left < 560*scale;
            const float h = narrow ? 98.0f : 64.0f;
            l.global_search_gap_card = D2D1::RectF(card_left, y, card_right, y + h*scale);
            const float cl = narrow ? card_left + 16*scale : card_right - (282 + 16)*scale;
            const float cr = card_right - 16*scale, w = (cr - cl) / 3;
            for (int i = 0; i < 3; ++i)
                l.global_search_gap[i] = D2D1::RectF(cl + i*w, y + (h - 44)*scale, cl + (i + 1)*w, y + (h - 12)*scale);
            y += h*scale;
        }
        l.global_search_apps_row = D2D1::RectF(card_left, y, card_right, y + 68*scale);
        y += 68*scale;
        l.search_pinyin_row = D2D1::RectF(card_left, y, card_right, y + 68*scale);
        y += 68*scale;
        l.jump_history_row = D2D1::RectF(card_left, y, card_right, y + 68*scale);
        {
            // 清除记录 sits left of the switch, sized to its label.
            float label = 0;
            for (wchar_t c : pulse::l10n::Get(pulse::l10n::StringId::JumpHistoryClear)) label += c >= 0x2E80 ? 14.0f : 7.5f;
            const float right = card_right - 72*scale, w = (label + 32) * scale;
            l.jump_history_clear = D2D1::RectF(right - w, y + 18*scale, right, y + 50*scale);
        }
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
        const float system_top = y;
        if (vm.settings_index_system_known) {
            l.index_system_row = D2D1::RectF(card_left, y, card_right, y + 64.0f * scale);
            y += 60.0f * scale;
            const float chips_left = card_left + 48.0f * scale;
            const float chips_right = card_right - 16.0f * scale;
            float x = chips_left, chip_y = y;
            auto place = [&](float w, float h) {
                w = (std::min)(w, chips_right - chips_left);
                if (x > chips_left && x + w > chips_right) { x = chips_left; chip_y += 28.0f * scale; }
                const D2D1_RECT_F r = D2D1::RectF(x, chip_y + (22.0f * scale - h) * 0.5f,
                                                  x + w, chip_y + (22.0f * scale + h) * 0.5f);
                x += w + 6.0f * scale;
                return r;
            };
            for (int g = 0; g < kSystemGroupRows; ++g) {
                if (!SystemGroupChipVisible(vm, g)) continue;
                l.index_system_chip[g] = place(painter
                    ? painter->MeasureBadgeWidth(pulse::l10n::Get(SystemGroupLabel(g))) : 120.0f * scale,
                    22.0f * scale);
            }
            const float more_w = (painter ? painter->MeasureBadgeWidth(
                pulse::l10n::Get(pulse::l10n::StringId::SettingsSystemCustomize)) : 60.0f * scale) + 16.0f * scale;
            l.index_system_more = place(more_w, 28.0f * scale);
            y = chip_y + 22.0f * scale + 14.0f * scale;
            if (vm.settings_index_system_expanded) {
                for (int g = 0; g < kSystemGroupRows; ++g) {
                    l.index_system_group[g] = D2D1::RectF(card_left + 36.0f * scale, y,
                                                          card_right - 12.0f * scale, y + 52.0f * scale);
                    y += 52.0f * scale;
                }
                y += 8.0f * scale;
            }
        }
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
        if (vm.settings_index_system_known) {
            l.index_system_card = D2D1::RectF(card_left, system_top, card_right, y + 4.0f * scale);
            y += 4.0f * scale;
        } else if (vm.settings_index_excluded_paths.empty()) {
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
        }
    } else if (vm.settings_page == 2) {
        {
            // Intro, then the layer cards placed as LayoutSettingsSysCtx places them.
            const float cl=l.content.left+pad,cr=l.content.right-pad;
            const auto& intro=l10n::Get(l10n::StringId::ContextMenuDesc);
            const float intro_h=painter ? (std::max)(18*scale,painter->MeasureWrappedCaptionHeight(intro,cr-cl)) : 18*scale;
            l.ctx_intro=D2D1::RectF(cl,y,cr,y+intro_h);
            y+=(std::max)(intro_h,32*scale)+14*scale;
            const float gap=10*scale,card_w=(cr-cl-gap)*0.5f;
            l.ctx_layer[0]=D2D1::RectF(cl,y,cl+card_w,y+58*scale);
            l.ctx_layer[1]=D2D1::RectF(cl+card_w+gap,y,cr,y+58*scale);
            y+=58*scale+12*scale;
        }
        l.context_rows.resize(vm.settings_items.size());
        {
            // Columns as LayoutSettingsSysCtx places them: the menus on the
            // left, the switch list, the live preview on wide pages.
            const float left=l.content.left+pad,right=l.content.right-pad,top=y;
            const bool wide=right-left>=960*scale;
            const float nav_w=(wide ? 200 : 176)*scale,side_w=310*scale,gap=12*scale;
            const float list_left=left+nav_w+gap,list_right=wide ? right-side_w-gap : right;
            // Left card: Pulse 菜单 (the three surfaces), then Windows 和其他软件.
            float ny=top+8*scale;
            l.pulse_group[0]=D2D1::RectF(left+6*scale,ny,left+nav_w-6*scale,ny+26*scale);
            ny+=26*scale;
            for(int i=0;i<3;++i) {
                l.pulse_tab[i]=D2D1::RectF(left+6*scale,ny,left+nav_w-6*scale,ny+32*scale);
                ny+=34*scale;
            }
            ny+=12*scale;
            l.pulse_group[1]=D2D1::RectF(left+6*scale,ny,left+nav_w-6*scale,ny+26*scale);
            ny+=26*scale;
            l.context_system_hint=D2D1::RectF(left+6*scale,ny,left+nav_w-6*scale,ny+32*scale);
            ny+=32*scale+8*scale;
            l.pulse_tabs=D2D1::RectF(left,top,left+nav_w,ny);
            // Middle card: head with 恢复默认, the preset chips, the rows.
            l.pulse_head=D2D1::RectF(list_left,top,list_right,top+48*scale);
            const float restore_w=SysCtxHeaderButtonWidth(painter,l10n::Get(l10n::StringId::RestoreDefaults),scale);
            l.pulse_restore=D2D1::RectF(list_right-10*scale-restore_w,top+11*scale,list_right-10*scale,top+11*scale+kSysCtxHeaderButtonDip*scale);
            float ly=l.pulse_head.bottom+8*scale;
            static constexpr l10n::StringId kPresetName[]={l10n::StringId::ContextPresetSlim,l10n::StringId::ContextPresetStandard,
                l10n::StringId::ContextPresetFull,l10n::StringId::ContextPresetCustom};
            const float chip_h=26*scale;
            float cx=list_left+12*scale+SysCtxTextWidth(painter,l10n::Get(l10n::StringId::Preset).c_str(),scale);
            for(int i=0;i<4;++i) {
                const float w=SysCtxTextWidth(painter,l10n::Get(kPresetName[i]),scale)+10*scale;
                if(cx+w>list_right-12*scale && i>0) {cx=list_left+12*scale;ly+=chip_h+6*scale;}
                l.pulse_preset[i]=D2D1::RectF(cx,ly,cx+w,ly+chip_h);
                cx+=w+6*scale;
            }
            ly+=chip_h+8*scale;
            l.pulse_rows_top=D2D1::RectF(list_left,ly,list_right,ly+1);
            ly+=6*scale;
            const auto rows=app::BuiltinMenuOrderedRows(PulseMenuSurface(vm),PulseMenuOrder(vm));
            for(size_t k=0;k<rows.size();++k) {
                const auto& row=rows[k];
                const auto row_rc=D2D1::RectF(list_left+6*scale,ly,list_right-6*scale,ly+kPulseMenuRowDip*scale);
                if(row.fixed!=app::BuiltinFixedRow::None) {
                    l.pulse_fixed[row.fixed==app::BuiltinFixedRow::Open ? 0 : 1]=row_rc;
                } else {
                    const size_t i=vm.settings_builtin_first+static_cast<size_t>(row.item);
                    if(i<l.context_rows.size()) l.context_rows[i]=row_rc;
                }
                ly+=kPulseMenuRowDip*scale+2*scale;
            }
            ly+=8*scale;
            l.pulse_card=D2D1::RectF(list_left,top,list_right,ly);
            float bottom=std::max(ly,l.pulse_tabs.bottom);
            if(wide) {
                // Right card: 实时预览 head, the menu, a note on what it shows.
                const float side_left=right-side_w;
                l.pulse_side_tab=D2D1::RectF(side_left+8*scale,top+8*scale,right-8*scale,top+36*scale);
                const float pv_top=top+56*scale;
                l.pulse_preview=D2D1::RectF(side_left+16*scale,pv_top,right-16*scale,pv_top+PulseMenuPreviewHeight(vm,scale));
                if(PulseMenuOrderCustom(vm,PulseMenuSurface(vm))) {
                    const float order_w=label_btn_w(l10n::Get(l10n::StringId::ContextOrderReset))-8*scale;
                    l.pulse_order_reset=D2D1::RectF(l.pulse_preview.right-order_w,pv_top-4*scale,l.pulse_preview.right,pv_top+20*scale);
                }
                const float note_w=l.pulse_preview.right-l.pulse_preview.left;
                const float note_h=SysCtxWrappedHeight(painter,PulseMenuSideNote(),note_w-48*scale,scale)+24*scale;
                l.pulse_side_note=D2D1::RectF(l.pulse_preview.left,l.pulse_preview.bottom+10*scale,l.pulse_preview.right,
                                              l.pulse_preview.bottom+10*scale+note_h);
                l.pulse_side=D2D1::RectF(side_left,top,right,std::max(l.pulse_side_note.bottom+16*scale,ly));
                bottom=std::max(bottom,l.pulse_side.bottom);
            }
            y=bottom;
        }
        y+=24*scale;
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
        {
            // 备份与迁移: title, description, status, then the buttons; the
            // third only once there is a staged import or a backup.
            const int count = vm.settings_transfer_pending || vm.settings_transfer_backup ? 3 : 2;
            const pulse::l10n::StringId labels[] = {
                pulse::l10n::StringId::SettingsExportButton,
                pulse::l10n::StringId::SettingsImportButton,
                vm.settings_transfer_pending ? pulse::l10n::StringId::SettingsRestartNow
                                             : pulse::l10n::StringId::SettingsRestoreBackup,
            };
            const float transfer_h = (compact_diagnostics ? 108.0f + count * 40.0f : 156.0f) * scale;
            l.transfer_card = D2D1::RectF(card_left, y, card_right, y + transfer_h);
            float left = action_left;
            for (int i = 0; i < 3; ++i) {
                if (i >= count) {
                    l.transfer_action[i] = {};
                } else if (compact_diagnostics) {
                    const float top = y + (108.0f + i * 40.0f) * scale;
                    l.transfer_action[i] = D2D1::RectF(action_left, top, action_right, top + 32.0f * scale);
                } else {
                    const float width = std::min(label_btn_w(pulse::l10n::Get(labels[i])),
                                                 (action_right - action_left - gap * 2.0f) / 3.0f);
                    const float top = y + 108.0f * scale;
                    l.transfer_action[i] = D2D1::RectF(left, top, left + width, top + 32.0f * scale);
                    left += width + gap;
                }
            }
            y += transfer_h + 12.0f * scale;
        }
        {
            // 新手提示: the status-bar hints and the teaching bubbles.
            const float card_top = y;
            y += 44.0f * scale;
            l.hints_row = D2D1::RectF(card_left, y, card_right, y + 64.0f * scale);
            l.hints_reset_row = D2D1::RectF(card_left + 28.0f * scale, y + 64.0f * scale, card_right, y + 128.0f * scale);
            const float rw = label_btn_w(pulse::l10n::Get(pulse::l10n::StringId::HintsResetButton));
            l.hints_reset_button = D2D1::RectF(card_right - 16.0f * scale - rw, l.hints_reset_row.top + 16.0f * scale,
                                               card_right - 16.0f * scale, l.hints_reset_row.top + 48.0f * scale);
            l.tips_card = D2D1::RectF(card_left, card_top, card_right, y + 128.0f * scale);
            y += 128.0f * scale + 12.0f * scale;
        }

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
                const ReleaseNoteView& note = notes[i];
                const ReleaseTagSummary tags = SummarizeReleaseTags(note);
                const float tag_col = ReleaseTagColumn(painter, note, scale);
                for (size_t k = 0; k < note.lines.size(); ++k)
                    body += MeasureReleaseLine(painter, note, k, text_w, tag_col, tags, scale).Total() +
                            kReleaseLineGapDip * scale;
                l.release_body = D2D1::RectF(row_left, ry, row_right, ry + body);
                ry += body + 4.0f * scale;
            }
        }
        l.release_card = D2D1::RectF(card_left, release_top, card_right, ry + 10.0f * scale);
        y = l.release_card.bottom + 24.0f * scale;
    } else if (vm.settings_page == 5 || vm.settings_page == 8) {
        y = LayoutSettingsPacks(l, vm, scale, y, painter);
    } else if (vm.settings_page == 6) {
        y = LayoutSettingsSysCtx(l, vm, scale, y, painter);
    } else if (vm.settings_page == 7) {
        y = LayoutSettingsShortcuts(l, vm, scale, y, painter);
    } else if (vm.settings_page == kSettingsPageNativeExtensions) {
        const float left = (std::min)(l.content.right, l.content.left + pad);
        const float right = (std::max)(left, l.content.right - pad);
        const float intro = painter ? painter->MeasureWrappedCaptionHeight(l10n::Get(l10n::StringId::NativeExtensionSettingsIntro), (std::max)(1.0f, right-left)) : 36.0f*scale;
        const float bottom = (std::max)(l.content.top, l.content.bottom - pad);
        // "‹ 扩展商店": this page is the store's settings view, so a way back leads it.
        const float crumb_w = (painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::StoreTitle)) : 96.0f*scale) + 8.0f*scale;
        l.native_store_crumb = D2D1::RectF(left - 6.0f*scale, l.content.top + 58.0f*scale,
                                           (std::min)(right, left + crumb_w), l.content.top + 84.0f*scale);
        const float top = (std::min)(bottom, l.content.top + 88.0f*scale + intro + 18.0f*scale);
        l.native_settings_content = D2D1::RectF(left, top, right, bottom);
        l.content_h = l.content.bottom - l.content.top;
        return l;
    } else if (vm.settings_page == kSettingsPageAi) {
        y = LayoutSettingsAi(l, vm, scale, y, painter);
    } else if (vm.settings_page == kSettingsPageAssistant) {
        y = LayoutSettingsAssistant(l, vm, scale, y, painter);
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

} // namespace pulse::ui
