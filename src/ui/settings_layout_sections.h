// Included inside ui_renderer_internal.h's anonymous namespace.
// Default file manager card helpers shared by layout, drawing and tests.
struct IntegrationBadgeInfo { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline int IntegrationSelectedCount(const WindowViewModel& vm) {
    return int(vm.settings_integration_folders)+int(vm.settings_integration_this_pc)+
        int(vm.settings_integration_win_e)+int(vm.settings_integration_experimental);
}
inline IntegrationBadgeInfo MakeIntegrationBadge(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(vm.settings_integration_state==3) return {l10n::Get(I::IntegrationFailed),fluent::BadgeKind::Danger};
    if(vm.settings_integration_state==2) return {l10n::Get(I::IntegrationPartial),fluent::BadgeKind::Warning};
    if(!vm.settings_integration_enabled) return {l10n::Get(I::IntegrationOff),fluent::BadgeKind::Neutral};
    const int count=IntegrationSelectedCount(vm);
    if(count==0) return {l10n::Get(I::IntegrationNone),fluent::BadgeKind::Warning};
    wchar_t value[128]{};
    swprintf_s(value,l10n::Get(I::IntegrationOnCount).c_str(),count);
    return {value,fluent::BadgeKind::Success};
}
// Problems get an InfoBar with their own actions; healthy states need none.
inline bool IntegrationHasProblem(const WindowViewModel& vm) {
    return vm.settings_integration_state==2 || vm.settings_integration_state==3;
}
inline std::wstring IntegrationProblemMessage(const WindowViewModel& vm) {
    if(vm.settings_integration_state==3)
        return vm.settings_integration_summary.empty() ? l10n::Get(l10n::StringId::IntegrationFailed) : vm.settings_integration_summary;
    return vm.settings_integration_state==2 ? l10n::Get(l10n::StringId::IntegrationDriftDesc) : std::wstring{};
}
// One short next step, naming only the settings that are actually missing.
inline std::wstring IntegrationHint(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(!vm.settings_integration_enabled) return l10n::Get(I::IntegrationInactiveHint);
    if(IntegrationSelectedCount(vm)==0) return l10n::Get(I::IntegrationNoneHint);
    if(!vm.settings_integration_experimental) return {};
    if(!vm.settings_launch_on_startup && !vm.settings_keep_running) return l10n::Get(I::IntegrationRunningHint);
    if(!vm.settings_launch_on_startup) return l10n::Get(I::IntegrationHintLaunch);
    if(!vm.settings_keep_running) return l10n::Get(I::IntegrationHintKeep);
    return {};
}
inline constexpr l10n::StringId kIntegrationChips[]={l10n::StringId::IntegrationExperimentalTag,
    l10n::StringId::IntegrationFactRunning,l10n::StringId::IntegrationFactShift,l10n::StringId::IntegrationFactWin11};

float LayoutSettingsGeneral(SettingsLayout& l, const WindowViewModel& vm, float scale,
                            float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    const bool narrow = right - left < 560*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y+h*scale); y=r.bottom; return r; };
    auto section = [&](int i) { y+=24*scale; l.section[i]=row(28); };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left+16*scale : r.right-(width+16)*scale,
            r.bottom-44*scale, r.right-16*scale, r.bottom-12*scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c=choice(r,width); const float w=(c.right-c.left)/count;
        for(int i=0;i<count;++i) output[i]=D2D1::RectF(c.left+i*w,c.top,c.left+(i+1)*w,c.bottom);
    };
    section(0);
    l.theme_row=row(narrow ? 142.0f : 112.0f);
    const float tw=(std::min)(96*scale,(right-left-32*scale)/3);
    const float tile_left=narrow ? left+16*scale : right-16*scale-3*tw;
    for(int i=0;i<3;++i) l.theme_tile[i]=D2D1::RectF(tile_left+i*tw+4*scale,
        l.theme_row.bottom-90*scale,tile_left+(i+1)*tw-4*scale,l.theme_row.bottom-12*scale);
    l.accent_card=row(96);
    const float picker=kBloomPickerDip*scale;
    l.accent_picker=D2D1::RectF(right-16*scale-picker,l.accent_card.top+(96*scale-picker)/2,
        right-16*scale,l.accent_card.top+(96*scale+picker)/2);
    // The tiles reuse SettingsEffect's existing hit regions and controller.
    // A single tile selector avoids duplicate controls for the same setting.
    const int effect_columns = right-left < 440*scale ? 2 : 4;
    const int effect_rows = kWindowEffectCount/effect_columns;
    const float effect_top = 66.0f;
    l.effect_card=row(effect_top+82.0f*effect_rows+12.0f);
    l.effect_choice = {};
    const float effect_gap=8*scale;
    const float effect_width=(right-left-32*scale-effect_gap*(effect_columns-1))/effect_columns;
    for(int i=0;i<kWindowEffectCount;++i) {
        const float tile_x=left+16*scale+(i%effect_columns)*(effect_width+effect_gap);
        const float tile_y=l.effect_card.top+effect_top*scale+(i/effect_columns)*82*scale;
        l.effect_row[i]=D2D1::RectF(tile_x,tile_y,tile_x+effect_width,tile_y+74*scale);
    }
    l.language_card=row(narrow ? 98.0f : 64.0f); l.language_choice=choice(l.language_card,176);
    l.text_render_card=row(narrow ? 98.0f : 64.0f); segments(l.text_render_card,l.text_render_row,3,282);
    l.ui_font_size_card=row(narrow ? 98.0f : 64.0f); segments(l.ui_font_size_card,l.ui_font_size_row,4,340);
    l.group[0]=D2D1::RectF(left,l.theme_row.top,right,y);
    // Default file manager: a titled card of its own, independent of startup and background running.
    // Every choice stays visible, so users see what the master switch hands to Pulse.
    y+=24*scale; l.integration_section=row(28);
    const float integration_top=y;
    auto caption_height=[&](std::wstring_view value,float width) {
        return value.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(value,(std::max)(40*scale,width)) : 42*scale;
    };
    {
        const auto badge=MakeIntegrationBadge(vm);
        const float badge_w=painter ? painter->MeasureBadgeWidth(badge.text) : 96*scale;
        const float switch_left=right-60*scale;
        float text_right=switch_left-24*scale-badge_w;
        const bool stacked=text_right-(left+54*scale)<220*scale;
        if(stacked) text_right=switch_left-12*scale;
        const float desc_h=caption_height(l10n::Get(l10n::StringId::IntegrationMasterDesc),text_right-left-54*scale);
        const float body=35*scale+desc_h+(stacked ? 30*scale : 0.0f)+14*scale;
        l.default_manager_row=row((std::max)(64.0f,body/scale));
        const float top=l.default_manager_row.top;
        l.integration_badge=stacked
            ? D2D1::RectF(left+54*scale,top+35*scale+desc_h+6*scale,left+54*scale+badge_w,top+35*scale+desc_h+28*scale)
            : D2D1::RectF(text_right+12*scale,top+21*scale,text_right+12*scale+badge_w,top+43*scale);
        l.integration_text_right=text_right;
    }
    if(IntegrationHasProblem(vm)) {
        const bool failed=vm.settings_integration_state==3;
        const float bar_left=left+16*scale, bar_right=right-16*scale;
        const float msg_h=caption_height(IntegrationProblemMessage(vm),bar_right-bar_left-60*scale);
        const auto retry_text=l10n::Get(failed ? l10n::StringId::IntegrationRetry : l10n::StringId::IntegrationReapply);
        const float retry_w=!vm.settings_integration_can_retry ? 0.0f
            : painter ? painter->MeasureButtonWidth(retry_text) : 96*scale;
        const float restore_w=!vm.settings_integration_can_restore ? 0.0f
            : painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::IntegrationRestore)) : 200*scale;
        const float button_left=bar_left+52*scale, button_right=bar_right-12*scale;
        const bool stack=retry_w>0 && restore_w>0 && button_left+retry_w+8*scale+restore_w>button_right;
        const float buttons_h=(retry_w>0 || restore_w>0) ? (stack ? 72*scale : 32*scale) : 0.0f;
        y+=4*scale;
        l.integration_bar=D2D1::RectF(bar_left,y,bar_right,y+32*scale+msg_h+(buttons_h>0 ? 10*scale+buttons_h : 0.0f)+12*scale);
        y=l.integration_bar.bottom+12*scale;
        const float button_top=l.integration_bar.top+32*scale+msg_h+10*scale;
        if(retry_w>0) l.integration_retry=D2D1::RectF(button_left,button_top,(std::min)(button_left+retry_w,button_right),button_top+32*scale);
        if(restore_w>0) {
            const float x=retry_w>0 && !stack ? l.integration_retry.right+8*scale : button_left;
            const float top=retry_w>0 && stack ? button_top+40*scale : button_top;
            l.integration_restore=D2D1::RectF(x,top,(std::min)(x+restore_w,button_right),top+32*scale);
        }
    }
    l.integration_list_head=row(32);
    const float item_text_left=left+112*scale, item_text_right=right-16*scale;
    auto item_row=[&](l10n::StringId desc,float extra) {
        const float h=caption_height(l10n::Get(desc),item_text_right-item_text_left);
        return row((std::max)(52.0f,(31*scale+h+extra+12*scale)/scale));
    };
    l.startup_row[2]=item_row(l10n::StringId::IntegrationFoldersDesc,0);
    l.this_pc_row=item_row(l10n::StringId::SettingsThisPcDesc,0);
    l.win_e_row=item_row(l10n::StringId::SettingsWinEDesc,0);
    {
        // Experimental caveats as compact chips that wrap with the window width.
        float x=item_text_left, chip_y=0.0f;
        for(int i=0;i<4;++i) {
            const float w=(std::min)(item_text_right-item_text_left,
                painter ? painter->MeasureBadgeWidth(l10n::Get(kIntegrationChips[i])) : 120*scale);
            if(x>item_text_left && x+w>item_text_right) { x=item_text_left; chip_y+=28*scale; }
            l.integration_chip[i]=D2D1::RectF(x,chip_y,x+w,chip_y+22*scale);
            x+=w+6*scale;
        }
        const float desc_h=caption_height(l10n::Get(l10n::StringId::IntegrationExperimentalDesc),item_text_right-item_text_left);
        l.explorer_windows_row=item_row(l10n::StringId::IntegrationExperimentalDesc,8*scale+chip_y+22*scale);
        const float chip_top=l.explorer_windows_row.top+31*scale+desc_h+8*scale;
        for(auto& chip:l.integration_chip) { chip.top+=chip_top; chip.bottom+=chip_top; }
    }
    const auto hint=IntegrationHint(vm);
    if(!hint.empty()) l.integration_hint=row((4*scale+caption_height(hint,item_text_right-(left+78*scale))+14*scale)/scale);
    else y+=6*scale;
    l.integration_card=D2D1::RectF(left,integration_top,right,y);
    section(1);
    l.startup_row[0]=row(64); l.start_in_tray_row=row(64); l.startup_row[1]=row(64);
    l.notify_icon_card=row(narrow ? 98.0f : 64.0f); segments(l.notify_icon_card,l.notify_icon_row,3,282);
    l.home_folder_card=row(narrow ? 98.0f : 64.0f);
    {
        const float rw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ThisPc)) : 80*scale;
        const float cw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::SettingsHomeFolderPick)) : 120*scale;
        l.home_folder_reset=D2D1::RectF(right-16*scale-rw,y-44*scale,right-16*scale,y-12*scale);
        l.home_folder_choose=D2D1::RectF(l.home_folder_reset.left-8*scale-cw,y-44*scale,l.home_folder_reset.left-8*scale,y-12*scale);
    }
    l.startup_open_card=row(narrow ? 98.0f : 64.0f); segments(l.startup_open_card,l.startup_open_row,2,282);
    l.new_tab_open_card=row(narrow ? 98.0f : 64.0f); segments(l.new_tab_open_card,l.new_tab_open_row,2,282);
    l.close_last_tab_row=row(64);
    l.group[1]=D2D1::RectF(left,l.startup_row[0].top,right,y);
    section(2);
    l.density_card=row(narrow ? 98.0f : 64.0f); segments(l.density_card,l.density_row,3,282);
    l.performance_row=row(64);
    for(auto& list_row : l.list_style_row) list_row=row(64);
    l.folder_sort_card=row(narrow ? 98.0f : 64.0f); segments(l.folder_sort_card,l.folder_sort_row,3,282);
    l.confirm_delete_row=row(64);
    l.group[2]=D2D1::RectF(left,l.density_card.top,right,y);
    // Quick Look: read-only supported formats card, bit 2 of settings_expanded.
    y+=24*scale; l.preview_section=row(28);
    l.disclosure[2]=row(64);
    if(vm.settings_expanded & 4u) {
        l.preview_formats=D2D1::RectF(left,y,right,y);
        y+=LayoutPreviewFormats(l.preview_formats,scale,nullptr,nullptr);
        l.preview_formats.bottom=y;
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Pick(L"获取", L"Get")) : 72*scale;
        for(int i=0;i<kPreviewCodecCount;++i) {
            l.preview_codec_row[i]=row(60);
            const bool detected=(vm.settings_preview_codecs & kPreviewCodecsDetected)!=0;
            if(detected && !(vm.settings_preview_codecs & (1u<<i)) && PreviewCodec(i).store_id)
                l.preview_codec_button[i]=D2D1::RectF(right-16*scale-bw,l.preview_codec_row[i].top+14*scale,
                    right-16*scale,l.preview_codec_row[i].top+46*scale);
        }
    }
    l.preview_group=D2D1::RectF(left,l.disclosure[2].top,right,y);
    y+=18*scale;
    l.disclosure[0]=row(64);
    if(vm.settings_expanded & 1u) {
        y+=10*scale;
        l.wallpaper_card=row(124);
        l.wallpaper_preview=D2D1::RectF(left+16*scale,l.wallpaper_card.top+12*scale,left+112*scale,l.wallpaper_card.top+68*scale);
        const float cw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::Clear)) : 80*scale;
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ChooseImage)) : 120*scale;
        l.wallpaper_clear=D2D1::RectF(right-16*scale-cw,y-44*scale,right-16*scale,y-12*scale);
        l.wallpaper_choose=D2D1::RectF(l.wallpaper_clear.left-8*scale-bw,y-44*scale,l.wallpaper_clear.left-8*scale,y-12*scale);
        y+=8*scale; l.wallpaper_look_card=row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_look_card,l.wallpaper_look_row,3,282);
        y+=8*scale; l.wallpaper_blur_card=row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_blur_card,l.wallpaper_blur_row,3,282);
        y+=8*scale; l.tray_icon_card=row(narrow ? 98.0f : 64.0f); segments(l.tray_icon_card,l.tray_icon_row,3,282);
        y+=8*scale; l.shell_tags_row=row(64);
        y+=8*scale; l.hidden_files_row=row(64);
        y+=8*scale; l.protected_files_row=row(64);
        y+=8*scale; l.pinned_names_row=row(64);
        y+=8*scale; l.multi_instance_row=row(64);
        y+=8*scale; l.vertical_tabs_row=row(64);
        y+=8*scale; l.hints_row=row(64);
        y+=8*scale; l.hints_reset_row=row(64);
        {
            const float rw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::HintsResetButton)) : 80*scale;
            l.hints_reset_button=D2D1::RectF(right-16*scale-rw,l.hints_reset_row.top+16*scale,right-16*scale,l.hints_reset_row.top+48*scale);
        }
        y+=8*scale; l.blank_click_row=row(narrow ? 98.0f : 64.0f); segments(l.blank_click_row,l.blank_click_choice,3,282);
        y+=8*scale; l.change_tracking_row=row(64);
        l.change_days_row=row(narrow ? 98.0f : 64.0f); segments(l.change_days_row,l.change_days,3,282);
    }
    y+=12*scale; l.footer=row(28); y+=16*scale;
    return y;
}

float LayoutSettingsContent(SettingsLayout& l, const WindowViewModel& vm, float scale,
                           float y, const fluent::Painter* painter) {
    const float left=l.content.left+20*scale,right=l.content.right-20*scale;
    const bool narrow=right-left<560*scale;
    auto row=[&](float h) { auto r=D2D1::RectF(left,y,right,y+h*scale);y=r.bottom;return r; };
    y+=14*scale; l.section[1]=row(28);
    l.content_header=row(72);
    l.content_types=row(narrow ? 236.0f : 160.0f);
    if(vm.settings_content_folders.empty()) l.content_empty=row(84);
    else if (!vm.settings_content_instant) {
        auto r=row(52);
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(vm.settings_content_paused ? l10n::StringId::ContentIndexResume : l10n::StringId::ContentIndexPause)) : 150*scale;
        l.content_pause=D2D1::RectF(left+16*scale,r.top+10*scale,left+16*scale+bw,r.bottom-10*scale);
    }
    l.content_options=row(64);
    if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) l.content_rebuild=row(64);
    l.group[1]=D2D1::RectF(left,l.content_header.top,right,y);
    y+=8*scale; l.footer=row(40);
    y+=24*scale;
    return y;
}
