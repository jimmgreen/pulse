#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"

namespace pulse::ui {
void MainRenderer::DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc=compositor_->Dc();
    const auto lay=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    auto text=[&](const std::wstring& value,D2D1_RECT_F r,bool is_small=false) {
        painter_.DrawText(value,r,is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),is_small ? theme.text_secondary : theme.text);
    };
    auto draw_card=[&](D2D1_RECT_F r) {
        if(r.bottom<=r.top) return;
        MakeBrush(dc,theme.fill_input,brFillInput_); MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brStrokeCard_.get(),1);
    };
    auto divider=[&](D2D1_RECT_F r) {
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        FillRect(dc,brStrokeDivider_.get(),r.left+16*scale_,r.bottom,r.right-r.left-32*scale_,1);
    };
    auto label=[&](D2D1_RECT_F r,const std::wstring& title,const std::wstring& desc,const wchar_t* icon,float text_right=0.0f,float description_height=21.0f) {
        const float right=text_right>0 ? text_right : r.right-72*scale_;
        DrawIconText(r.left+16*scale_,r.top+17*scale_,24*scale_,24*scale_,icon,L"",theme.text_secondary,0.85f);
        text(title,D2D1::RectF(r.left+54*scale_,r.top+10*scale_,right,r.top+34*scale_));
        if(!desc.empty()) {
            const auto bounds=D2D1::RectF(r.left+54*scale_,r.top+35*scale_,right,r.top+(35+description_height)*scale_);
            if(description_height<=21.0f) text(desc,bounds,true);
            else {
                ComPtr<IDWriteTextLayout> description;
                if(SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(desc.c_str(),static_cast<UINT32>(desc.size()),
                    compositor_->SmallFormat(),bounds.right-bounds.left,bounds.bottom-bounds.top,&description))) {
                    description->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    description->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                    dc->DrawTextLayout(D2D1::Point2F(bounds.left,bounds.top),description.get(),brTextSecondary_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int hit) {
        if(IsHovered(vm,H::SettingsToggle,hit)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon,0.0f,hit==15 ? 43.0f : 21.0f);
        fluent::ControlState state{}; state.checked=on; state.hovered=IsHovered(vm,H::SettingsToggle,hit);
        painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
    };
    auto button=[&](D2D1_RECT_F r,const std::wstring& value,H::Region region,int index,bool primary=false,bool enabled=true,const wchar_t* glyph=L"") {
        fluent::ControlState st{}; st.enabled=enabled; st.hovered=enabled && IsHovered(vm,region,index);
        painter_.DrawButton({r,value,glyph,primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,st});
    };
    auto disclosure=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,int id,bool own_card) {
        if(own_card) draw_card(r);
        if(IsHovered(vm,H::SettingsDisclosure,id)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon);
        DrawIconText(r.right-38*scale_,r.top+22*scale_,18*scale_,18*scale_,
            (vm.settings_expanded&(1u<<id)) ? L"\xE70D" : L"\xE76C",L"",theme.text_secondary,0.75f);
    };
    auto section=[&](int i,I title) { text(l10n::Get(title),lay.section[i]); };
    auto segmented=[&](D2D1_RECT_F card,D2D1_RECT_F const* choices,const I* labels,const int* values,int current,H::Region hit,I title,I desc) {
        const bool stacked=choices[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),L"\xE8A4",stacked ? card.right-16*scale_ : choices[0].left-12*scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[2].right,choices[2].bottom));
        for(int i=0;i<3;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(labels[i]);
            item.state.checked=current==values[i];item.state.hovered=IsHovered(vm,hit,i);
            item.shared_track=true; item.position=i==0 ? fluent::SegmentPosition::First : i==2 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    if(vm.settings_page==0) {
        section(0,I::SettingsAppearance);section(1,I::SettingsStartupShutdown);section(2,I::SettingsFileList);
        for(const auto& group:lay.group) draw_card(group);
        const bool small_theme=lay.theme_tile[0].left<lay.theme_row.left+100*scale_;
        label(lay.theme_row,l10n::Get(I::SettingsTheme),small_theme ? L"" : l10n::Get(I::SettingsThemeDesc),L"\xE790",
            small_theme ? lay.theme_row.right-16*scale_ : lay.theme_tile[0].left-12*scale_);
        const I theme_names[]={I::SettingsThemeLight,I::SettingsThemeDark,I::SettingsThemeSystem};
        const int theme_values[]={1,2,0};
        for(int i=0;i<3;++i) {
            const auto r=lay.theme_tile[i];auto preview=r;preview.bottom-=22*scale_;
            const bool selected=vm.settings_theme==theme_values[i];
            MakeBrush(dc,i==0 ? D2D1::ColorF(0xe7ecf2) : D2D1::ColorF(0x20262e),brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(preview,5*scale_,5*scale_),brFillHover_.get());
            MakeBrush(dc,i==0 ? D2D1::ColorF(0xffffff) : D2D1::ColorF(0x39424d),brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+8*scale_,preview.right-preview.left-14*scale_,8*scale_,2*scale_);
            FillRoundedRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+21*scale_,14*scale_,preview.bottom-preview.top-28*scale_,2*scale_);
            if(i==2) {MakeBrush(dc,D2D1::ColorF(0xe7ecf2),brFillHover_); FillRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+8*scale_,(preview.right-preview.left)/2-7*scale_,preview.bottom-preview.top-15*scale_);}
            MakeBrush(dc,selected ? theme.accent : IsHovered(vm,H::SettingsTheme,theme_values[i]) ? theme.text_secondary : theme.stroke_card,brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(preview,5*scale_,5*scale_),brStrokeCard_.get(),selected ? 2*scale_ : 1);
            if(selected) DrawIconText(preview.right-23*scale_,preview.top+3*scale_,20*scale_,20*scale_,L"\xE73E",L"",theme.accent,0.7f);
            painter_.DrawText(l10n::Get(theme_names[i]),D2D1::RectF(r.left,preview.bottom+2*scale_,r.right,r.bottom),compositor_->SmallFormat(),theme.text,fluent::HorizontalAlignment::Center);
        }
        divider(lay.theme_row);
        label(lay.accent_card,l10n::Get(I::SettingsThemeColor),l10n::Get(I::SettingsThemeColorDesc),L"\xE790",lay.accent_picker.left-16*scale_);
        if(vm.settings_bloom) {vm.settings_bloom->SetDisk(lay.accent_picker);vm.settings_bloom->Draw(dc,theme);}
        divider(lay.accent_card);
        const I effects[]={I::EffectNone,I::EffectAcrylic,I::EffectMica,I::EffectMicaAlt};
        const I languages[]={I::LanguageSystem,I::LanguageZhCN,I::LanguageEnUS};
        const auto dropdown=[&](D2D1_RECT_F r,D2D1_RECT_F c,I title,I desc,const std::wstring& value,int id) {
            label(r,l10n::Get(title),l10n::Get(desc),id==0 ? L"\xE790" : L"\xE8C1",c.left<r.left+100*scale_ ? r.right-16*scale_ : c.left-12*scale_);
            fluent::ButtonSpec control{};
            control.bounds=c; control.text=value; control.drop_down=true;
            control.state.hovered=IsHovered(vm,H::SettingsDropdown,id);
            painter_.DrawButton(control);
        };
        dropdown(lay.effect_card,lay.effect_choice,I::SettingsWindowEffect,compat::ModernWindows() ? I::SettingsWindowEffectDesc : I::EffectUnavailable,l10n::Get(effects[static_cast<int>(compat::ModernWindows() ? vm.window_effect : WindowEffect::None)]),0);
        divider(lay.effect_card);
        dropdown(lay.language_card,lay.language_choice,I::SettingsLanguage,I::SettingsLanguageDesc,l10n::Get(languages[vm.settings_language]),1);
        toggle(lay.startup_row[0],I::SettingsLaunch,I::SettingsLaunchDesc,L"\xE7E8",vm.settings_launch_on_startup,1);divider(lay.startup_row[0]);
        toggle(lay.startup_row[1],I::SettingsKeepRunning,I::SettingsKeepRunningDesc,L"\xE737",vm.settings_keep_running,2);
        const I density[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy};const int heights[]={28,34,40};
        segmented(lay.density_card,lay.density_row,density,heights,vm.settings_row_height,H::SettingsDensity,I::SettingsRowHeight,I::SettingsRowHeightDesc);divider(lay.density_card);
        toggle(lay.performance_row,I::SettingsShowPerformance,I::SettingsShowPerformanceDesc,L"\xE946",vm.settings_show_performance,4);divider(lay.performance_row);
        toggle(lay.list_style_row[0],I::ListSmartDate,I::ListSmartDateDesc,L"\xE787",vm.settings_list_smart_date,17);divider(lay.list_style_row[0]);
        toggle(lay.list_style_row[1],I::ListZebraRows,I::ListZebraRowsDesc,L"\xE8FD",vm.settings_list_zebra_rows,18);divider(lay.list_style_row[1]);
        toggle(lay.list_style_row[2],I::ListSizeBar,I::ListSizeBarDesc,L"\xE9D2",vm.settings_list_size_bar,19);
        disclosure(lay.disclosure[0],I::SettingsAdvanced,I::SettingsAdvancedDesc,L"\xE713",0,true);
        if(vm.settings_expanded & 1u) {
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
        const bool compact_wallpaper = lay.wallpaper_choose.top >
            lay.wallpaper_card.top + 64.0f * scale_;
        const float text_right = compact_wallpaper
            ? lay.wallpaper_card.right - 16.0f * scale_
            : lay.wallpaper_choose.left - 12.0f * scale_;
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaper),
                     text_left, lay.wallpaper_card.top + 18.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 22.0f * scale_);
        const std::wstring wallpaper_desc = vm.background_image.empty()
            ? pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaperDesc)
            : FileNameOf(vm.background_image);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), wallpaper_desc,
                     text_left, lay.wallpaper_card.top + 42.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 18.0f * scale_);
        fluent::ControlState choose{};
        choose.hovered = IsHovered(vm, HitTestResult::SettingsWallpaper, 0);
        painter_.DrawButton({ lay.wallpaper_choose,
                              pulse::l10n::Get(pulse::l10n::StringId::ChooseImage), {},
                              fluent::ButtonKind::Standard, choose });
        fluent::ControlState clear{};
        clear.enabled = !vm.background_image.empty();
        clear.hovered = clear.enabled && IsHovered(vm, HitTestResult::SettingsWallpaper, 1);
        painter_.DrawButton({ lay.wallpaper_clear,
                              pulse::l10n::Get(pulse::l10n::StringId::Clear), {},
                              fluent::ButtonKind::Standard, clear });


            const I sizes[]={I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};const int icons[]={40,48,56};
            draw_card(lay.tray_icon_card);segmented(lay.tray_icon_card,lay.tray_icon_row,sizes,icons,vm.settings_tray_icon,H::SettingsTrayIcon,I::SettingsTrayIcon,I::SettingsTrayIconDesc);
            draw_card(lay.startup_row[2]);toggle(lay.startup_row[2],I::SettingsOpenFolders,I::SettingsOpenFoldersDesc,L"\xE8B7",vm.settings_open_folders,3);
            draw_card(lay.hidden_files_row);toggle(lay.hidden_files_row,I::SettingsShowHidden,I::SettingsShowHiddenDesc,L"\xE890",vm.settings_show_hidden_files,5);
            // Hidden + system entries: File Explorer keeps these behind a second option.
            draw_card(lay.protected_files_row);toggle(lay.protected_files_row,I::SettingsShowProtected,I::SettingsShowProtectedDesc,L"\xE72E",vm.settings_show_protected_os_files,16);
            draw_card(lay.pinned_names_row);toggle(lay.pinned_names_row,I::PinnedNames,I::PinnedNamesDesc,L"\xE718",vm.show_pinned_tab_names,6);
            draw_card(lay.blank_click_row);toggle(lay.blank_click_row,I::SettingsBlankClickBack,I::SettingsBlankClickBackDesc,L"\xE72B",vm.settings_blank_click_go_back,7);
            draw_card(lay.change_tracking_row);toggle(lay.change_tracking_row,I::SettingsChangeTracking,I::SettingsChangeTrackingDesc,L"\xE823",vm.settings_change_tracking,8);
            const I days[]={I::ChangeToday,I::ChangeLast3Days,I::ChangeLast7Days};const int day_values[]={1,3,7};
            draw_card(lay.change_days_row);segmented(lay.change_days_row,lay.change_days,days,day_values,vm.settings_change_days,H::SettingsChangeDays,I::SettingsChangeDays,I::SettingsChangeTrackingDesc);
        }
        text(l10n::Get(I::SettingsImmediate),lay.footer,true);
    } else {
        section(0,I::SearchModeName);section(1,I::SearchModeContent);
        draw_card(lay.group[0]);draw_card(lay.group[1]);
        toggle(lay.global_search_row,I::GlobalSearch,I::GlobalSearchDesc,L"\xE721",vm.settings_global_search_enabled,15);divider(lay.global_search_row);
        label(lay.global_search_hotkey_row,l10n::Get(I::GlobalSearchHotkey),
            vm.settings_global_search_error.empty() ? l10n::Get(I::GlobalSearchHotkeyDesc) : vm.settings_global_search_error,
            L"\xE765",lay.global_search_hotkey_row.right-16*scale_);
        button(lay.global_search_hotkey_button,vm.settings_global_search_capturing ? l10n::Get(I::GlobalSearchRecording) : vm.settings_global_search_hotkey,H::SettingsGlobalSearchHotkey,0);
        divider(lay.global_search_hotkey_row);
        toggle(lay.search_pinyin_row,I::SearchPinyin,I::SearchPinyinDesc,L"\xE721",vm.settings_search_pinyin,9);divider(lay.search_pinyin_row);
        label(lay.filename_status,l10n::Get(I::SettingsFilenameIndex),vm.settings_index_status,L"\xE8A5",lay.filename_status.right-16*scale_);divider(lay.filename_status);
        disclosure(lay.disclosure[1],I::SettingsMaintenance,I::SettingsMaintenanceDesc,L"\xE713",1,false);
        label(lay.content_header,l10n::Get(I::SettingsContentFolders),vm.settings_content_summary,L"\xE8B7",
            lay.content_header.right-16*scale_);
        divider(lay.content_header);
        label(lay.content_types,l10n::Get(I::SettingsContentTypes),l10n::Get(I::SettingsContentTypesDesc),L"\xE8A5",
            lay.content_types.right-16*scale_,(lay.content_types.bottom-lay.content_types.top)/scale_-45);
        divider(lay.content_types);
        if(vm.settings_content_folders.empty()) {
            label(lay.content_empty,l10n::Get(I::ContentIndexEmpty),l10n::Get(I::SettingsContentEmptyHelp),L"\xE8B7",lay.content_empty.right-16*scale_);
        }
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) button(lay.content_pause,l10n::Get(vm.settings_content_paused ? I::ContentIndexResume : I::ContentIndexPause),H::SettingsContentAction,1);
        label(lay.content_options,l10n::Get(I::SettingsContentOptions),l10n::Get(I::SettingsContentOptionsDesc),L"\xE8A5");
        DrawIconText(lay.content_options.right-38*scale_,lay.content_options.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        text(l10n::Get(vm.settings_content_instant ? I::ContentInstantReady : I::SettingsContentHelp),lay.footer,true);
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) {
        divider(lay.content_options);label(lay.content_rebuild,l10n::Get(I::ContentIndexRebuild),l10n::Get(I::SettingsContentRebuildHelp),L"\xE72C");
        DrawIconText(lay.content_rebuild.right-38*scale_,lay.content_rebuild.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        }
    }
}
} // namespace pulse::ui
