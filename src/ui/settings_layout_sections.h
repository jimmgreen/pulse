// Included inside ui_renderer_internal.h's anonymous namespace.
// Default file manager card helpers shared by layout, drawing and tests.
struct IntegrationBadgeInfo { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline int IntegrationSelectedCount(const WindowViewModel& vm) {
    return int(vm.settings_integration_folders)+int(vm.settings_integration_this_pc)+
        int(vm.settings_integration_recycle_bin)+
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
    // Taking over keeps Pulse in the tray on close by itself
    // (AppPrefs::KeepsRunningInBackground); only sign-in is left to ask for.
    if(!vm.settings_launch_on_startup) return l10n::Get(I::IntegrationHintLaunch);
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
    l.recycle_bin_row=item_row(l10n::StringId::SettingsRecycleBinDesc,0);
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
    l.close_tab_double_click_row=row(64);
    l.group[1]=D2D1::RectF(left,l.startup_row[0].top,right,y);
    section(2);
    l.density_card=row(narrow ? 98.0f : 64.0f); segments(l.density_card,l.density_row,3,282);
    l.performance_row=row(64);
    for(auto& list_row : l.list_style_row) list_row=row(64);
    l.folder_sort_card=row(narrow ? 98.0f : 64.0f); segments(l.folder_sort_card,l.folder_sort_row,3,282);
    l.confirm_delete_row=row(64);
    l.group[2]=D2D1::RectF(left,l.density_card.top,right,y);
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

// ---- 预览增强包 page (5) ---------------------------------------------------
// Texts shared by layout (measuring) and drawing. l10n::Pick converts to
// Traditional Chinese at run time, like the Quick Look format catalog.
namespace pack_text {
inline const wchar_t* Title() { return l10n::Pick(L"快速预览", L"Quick Look"); }
inline const wchar_t* Intro() { return l10n::Pick(
    L"按空格键预览文件，在这里查看格式支持并管理扩展。",
    L"Press Space to preview files. Explore format support and manage extensions here."); }
inline const wchar_t* PacksSection() { return l10n::Pick(L"预览增强包", L"Preview packs"); }
inline const wchar_t* FormatsSection() { return l10n::Pick(L"格式", L"Formats"); }
inline std::wstring FormatsSummary(const WindowViewModel& vm) {
    const int packs = int(vm.settings_pack_media_installed) + int(vm.settings_pack_images_installed) +
                      int(vm.settings_pack_raw_installed) + int(vm.settings_pack_archive_installed);
    int codecs = 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) if (vm.settings_preview_codecs & (1u << i)) ++codecs;
    const auto count = std::to_wstring(PreviewFormatCount());
    std::wstring result = l10n::IsChinese()
        ? l10n::Cn(L"共 ") + count + l10n::Cn(L" 种格式 · 增强包 ") + std::to_wstring(packs) + L"/4"
        : count + L" formats · Preview packs " + std::to_wstring(packs) + L"/4";
    if (vm.settings_preview_codecs & kPreviewCodecsDetected)
        result += std::wstring(l10n::Pick(L" · 系统扩展 ", L" · System extensions ")) + std::to_wstring(codecs) + L"/4";
    else result += l10n::Pick(L" · 系统扩展检测中", L" · Checking system extensions");
    return result;
}
inline const wchar_t* FormatsTitle() { return l10n::Pick(L"支持的格式", L"Supported formats"); }
inline const wchar_t* SystemTitle() { return l10n::Pick(L"系统扩展", L"System extensions"); }
inline const wchar_t* Legend(int source) {
    if (source == 0) return l10n::Pick(L"内置", L"Built in");
    if (source == 1) return l10n::Pick(L"预览增强包", L"Preview pack");
    return l10n::Pick(L"增强包或系统扩展", L"Pack or system extension");
}
inline const wchar_t* LegendNote() { return l10n::Pick(
    L"虚线：对应的增强支持尚未启用",
    L"Dashed: enhanced support is not enabled"); }
inline const wchar_t* SystemHint(const WindowViewModel& vm) { return vm.settings_pack_images_installed && vm.settings_pack_images_enabled
    ? l10n::Pick(L"已启用现代图像格式增强包：HEIC / AVIF 可以由增强包解码，无需另装对应的系统扩展。",
                 L"The image pack is enabled: it decodes HEIC / AVIF without the corresponding system extensions.")
    : l10n::Pick(L"HEIC / AVIF 也可通过现代图像格式增强包预览，无需微软账户。",
                 L"The image pack also supports HEIC / AVIF without a Microsoft account."); }

inline const wchar_t* OpenFolder() { return l10n::Pick(L"打开文件夹", L"Open folder"); }
inline const wchar_t* Media() { return l10n::Pick(L"媒体", L"Media"); }
inline const wchar_t* Advanced() { return l10n::Pick(L"高级", L"Advanced"); }
inline const wchar_t* MediaTitle() { return l10n::Pick(L"FFmpeg 媒体增强", L"FFmpeg media"); }
inline const wchar_t* MediaDesc() { return l10n::Pick(
    L"系统缺少解码器的视频也能生成缩略图、显示时长和媒体信息：HEVC、AV1、VP9、ProRes、FLV、RMVB、MPEG-TS 等。",
    L"Thumbnails, playing time and media details for videos Windows has no decoder for: HEVC, AV1, VP9, ProRes, FLV, RMVB, MPEG-TS and more."); }
inline const wchar_t* MediaMeta() { return l10n::Pick(L"LGPL-2.1 · 仅解码构建，不含 GPL 组件", L"LGPL-2.1 · decode-only build, no GPL parts"); }
inline const wchar_t* Images() { return l10n::Pick(L"图像", L"Images"); }
inline const wchar_t* ImagesTitle() { return l10n::Pick(L"现代图像格式", L"Modern image formats"); }
inline const wchar_t* ImagesDesc() { return l10n::Pick(
    L"不依赖微软商店扩展，直接预览 iPhone 照片（HEIC）、AVIF、JPEG XL 和 HDR / EXR 图像，文件夹里也能看到缩略图。",
    L"Preview iPhone photos (HEIC), AVIF, JPEG XL and HDR / EXR pictures without the Microsoft Store extensions, thumbnails included."); }
inline const wchar_t* ImagesMeta() { return l10n::Pick(L"libheif · dav1d · libjxl · OpenEXR · LGPL-3.0 / BSD", L"libheif · dav1d · libjxl · OpenEXR · LGPL-3.0 / BSD"); }
inline const wchar_t* RawTitle() { return l10n::Pick(L"RAW 相机照片", L"RAW camera photos"); }
inline const wchar_t* RawDesc() { return l10n::Pick(
    L"通过 LibRaw 为 CR2、CR3、NEF、ARW、DNG、RAF 等相机原片提供预览和缩略图。",
    L"Use LibRaw for previews and thumbnails of camera originals such as CR2, CR3, NEF, ARW, DNG and RAF."); }
inline const wchar_t* RawMeta() { return L"LibRaw · LGPL-2.1 / CDDL"; }
inline const wchar_t* Archive() { return l10n::Pick(L"压缩包", L"Archives"); }
inline const wchar_t* ArchiveTitle() { return l10n::Pick(L"7-Zip 压缩包增强", L"7-Zip archives"); }
inline const wchar_t* ArchiveDesc() { return l10n::Pick(
    L"通过 7-Zip 扩展压缩包内容列表预览，支持 7z、RAR、ISO、CAB、WIM 等格式。",
    L"Use 7-Zip to preview archive contents, including 7z, RAR, ISO, CAB and WIM."); }
inline const wchar_t* ArchiveMeta() { return L"7-Zip · LGPL-2.1 + unRAR"; }
inline const wchar_t* Install() { return l10n::Pick(L"下载并安装", L"Download and install"); }
inline const wchar_t* Remove() { return l10n::Pick(L"卸载", L"Remove"); }
inline const wchar_t* CustomTitle() { return l10n::Pick(L"使用已有 FFmpeg", L"Use existing FFmpeg"); }
inline const wchar_t* CustomDesc() { return l10n::Pick(
    L"可选来源；关闭后使用 Pulse 管理的增强包",
    L"Optional source; turn off to use the Pulse-managed pack"); }
inline const wchar_t* NoCustom() { return l10n::Pick(L"尚未选择 ffmpeg.exe", L"No ffmpeg.exe chosen"); }
inline const wchar_t* Browse() { return l10n::Pick(L"浏览…", L"Browse…"); }
inline const wchar_t* UseDetected() { return l10n::Pick(L"使用检测到的", L"Use found"); }
inline const wchar_t* RemoveTitle() { return l10n::Pick(L"卸载 Pulse 时删除预览增强包", L"Remove packs when uninstalling Pulse"); }
inline const wchar_t* RemoveDesc() { return l10n::Pick(L"否则保留，重新安装 Pulse 后可直接使用", L"Otherwise they stay and work again after reinstalling Pulse"); }
inline const wchar_t* Note() { return l10n::Pick(
    L"预览增强包在隔离的预览进程中运行：崩溃或超时只影响当前预览，Pulse 会回退到内置的缩略图和文本视图。",
    L"Packs run in the isolated preview process: a crash or timeout only affects that preview, and Pulse falls back to its built-in views."); }
inline std::wstring Bytes(uint64_t bytes) {
    wchar_t text[48]{};
    if (bytes >= 1024ull * 1024 * 1024) swprintf_s(text, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) swprintf_s(text, L"%.0f MB", bytes / (1024.0 * 1024));
    else swprintf_s(text, L"%.0f KB", bytes / 1024.0);
    return text;
}
inline std::wstring Summary(const WindowViewModel& vm) {
    if (!vm.settings_pack_installed) return l10n::Pick(L"尚未安装预览增强包", L"No preview packs installed");
    wchar_t text[128]{};
    swprintf_s(text, l10n::Pick(L"已安装 %u 个 · 占用 %s", L"%u installed · %s"),
               vm.settings_pack_installed, Bytes(vm.settings_pack_bytes).c_str());
    return text;
}
inline std::wstring Found(const std::wstring& path) {
    return std::wstring(l10n::Pick(L"检测到：", L"Found: ")) + path;
}
struct Badge { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline const wchar_t* PrimaryLabel(bool available, bool installed, bool installing) {
    if (installing) return l10n::Pick(L"取消", L"Cancel");
    if (installed) return Remove();
    return available ? Install() : l10n::Pick(L"尚未发布", L"Not published");
}
inline bool PrimaryEnabled(bool available, bool installed, bool installing) {
    return available || installed || installing;
}
inline Badge PackBadge(bool available, bool installed, bool installing, bool enabled, const std::wstring& version) {
    if (installing) return {l10n::Pick(L"正在安装", L"Installing"), fluent::BadgeKind::Accent};
    if (!installed) return {l10n::Pick(available ? L"未安装" : L"尚未发布", available ? L"Not installed" : L"Not published"), fluent::BadgeKind::Neutral};
    if (!enabled) return {l10n::Pick(L"已停用", L"Off"), fluent::BadgeKind::Neutral};
    std::wstring text = l10n::Pick(L"已安装", L"Installed");
    if (!version.empty()) text += L" " + version;
    return {text, fluent::BadgeKind::Success};
}
inline Badge MediaBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_media_available, vm.settings_pack_media_installed,
        vm.settings_pack_installing, true, vm.settings_pack_version);
}
inline const wchar_t* Primary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_media_available, vm.settings_pack_media_installed, vm.settings_pack_installing);
}
inline const wchar_t* SourceTitle() { return l10n::Pick(L"启用媒体增强", L"Enable media enhancement"); }
inline const wchar_t* SourceStatus(const WindowViewModel& vm) {
    if (!vm.settings_pack_use_custom && !vm.settings_pack_media_installed)
        return vm.settings_pack_media_available
            ? l10n::Pick(L"尚无可用媒体来源；可下载增强包或选择已有 FFmpeg", L"No media source available; download a pack or choose existing FFmpeg")
            : l10n::Pick(L"尚无可用媒体来源；可选择已有 FFmpeg", L"No media source available; choose existing FFmpeg");
    if (vm.settings_pack_use_custom && vm.settings_pack_ffmpeg != 2)
        return l10n::Pick(L"当前来源：已有 FFmpeg（路径无效，请重新选择）", L"Source: existing FFmpeg (invalid path; choose again)");
    if (!vm.settings_pack_ffmpeg_enabled)
        return vm.settings_pack_use_custom
            ? l10n::Pick(L"已停用；所选来源：已有 FFmpeg", L"Off; selected source: existing FFmpeg")
            : l10n::Pick(L"已停用；所选来源：Pulse 管理的增强包", L"Off; selected source: Pulse-managed pack");
    if (vm.settings_pack_use_custom)
        return l10n::Pick(L"当前来源：已有 FFmpeg（用户文件，不做完整性校验）", L"Source: existing FFmpeg (user file, not verified)");
    return l10n::Pick(L"当前来源：Pulse 管理的增强包", L"Source: Pulse-managed pack");
}
// "正在下载… 42%" while a pack downloads.
inline std::wstring Downloading(float progress) {
    const int percent = static_cast<int>(progress * 100.0f + 0.5f);
    return std::wstring(l10n::Pick(L"正在下载… ", L"Downloading… ")) +
           std::to_wstring(percent < 0 ? 0 : percent > 100 ? 100 : percent) + L"%";
}
inline std::wstring Downloading(const WindowViewModel& vm) { return Downloading(vm.settings_pack_progress); }
inline Badge ImagesBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_images_available, vm.settings_pack_images_installed,
        vm.settings_pack_images_installing, vm.settings_pack_images_enabled, vm.settings_pack_images_version);
}
inline const wchar_t* ImagesPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_images_available, vm.settings_pack_images_installed, vm.settings_pack_images_installing);
}
inline bool ShowsImagesEnable(const WindowViewModel& vm) { return vm.settings_pack_images_installed; }
inline Badge RawBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_raw_available, vm.settings_pack_raw_installed,
        vm.settings_pack_raw_installing, vm.settings_pack_raw_enabled, vm.settings_pack_raw_version);
}
inline const wchar_t* RawPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_raw_available, vm.settings_pack_raw_installed, vm.settings_pack_raw_installing);
}
inline bool ShowsRawEnable(const WindowViewModel& vm) { return vm.settings_pack_raw_installed; }
inline Badge ArchiveBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_archive_available, vm.settings_pack_archive_installed,
        vm.settings_pack_archive_installing, vm.settings_pack_archive_enabled, vm.settings_pack_archive_version);
}
inline const wchar_t* ArchivePrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_archive_available, vm.settings_pack_archive_installed, vm.settings_pack_archive_installing);
}
inline bool ShowsArchiveEnable(const WindowViewModel& vm) { return vm.settings_pack_archive_installed; }
// Switch and path rows exist once there is something to switch.
inline bool ShowsEnable(const WindowViewModel& vm) { return vm.settings_pack_ffmpeg != 0 || vm.settings_pack_use_custom; }
inline bool ShowsPath(const WindowViewModel&) {
    return true;
}
inline bool ShowsDetect(const WindowViewModel& vm) {
    return !vm.settings_pack_detected_path.empty() &&
        _wcsicmp(vm.settings_pack_detected_path.c_str(), vm.settings_pack_custom_path.c_str()) != 0;
}
} // namespace pack_text

float LayoutSettingsPacks(SettingsLayout& l, const WindowViewModel& vm, float scale,
                          float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h*scale); y = r.bottom; return r; };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 96*scale; };
    auto caption_h = [&](std::wstring_view text, float width) {
        return text.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40*scale, width)) : 40*scale;
    };
    y += caption_h(pack_text::Intro(), right - left) + 10*scale;
    const float font = static_cast<float>(vm.settings_ui_font_scale) / 100.0f;
    l.preview_section = row(28*font);
    const float header_top = y;
    l.preview_header_title = D2D1::RectF(left + 54*scale, y + 12*scale, right - 48*scale, y + (12 + 26*font)*scale);
    l.preview_header_summary = D2D1::RectF(left + 54*scale, l.preview_header_title.bottom, right - 48*scale,
        l.preview_header_title.bottom + caption_h(pack_text::FormatsSummary(vm), right - left - 102*scale));
    y = l.preview_header_summary.bottom + 12*scale;
    l.disclosure[2] = D2D1::RectF(left, header_top, right, y);
    if (vm.settings_expanded & 4u) {
        l.preview_legend = D2D1::RectF(left + 54*scale, y + 4*scale, right - 16*scale, y + 4*scale);
        float legend_x = l.preview_legend.left, legend_y = l.preview_legend.top;
        for (int i = 0; i < 3; ++i) {
            const float width = painter ? painter->MeasureBadgeWidth(pack_text::Legend(i)) : (i == 2 ? 180 : 100)*scale;
            if (legend_x > l.preview_legend.left && legend_x + width > l.preview_legend.right) {
                legend_x = l.preview_legend.left; legend_y += 30*font*scale;
            }
            l.preview_legend_chip[i] = D2D1::RectF(legend_x, legend_y, (std::min)(legend_x + width, l.preview_legend.right), legend_y + 24*font*scale);
            legend_x += width + 10*scale;
        }
        const bool inline_note = l.preview_legend.right - legend_x > 250*font*scale;
        const float note_left = inline_note ? legend_x + 6*scale : l.preview_legend.left;
        const float note_top = inline_note ? legend_y + 2*scale : legend_y + 32*font*scale;
        l.preview_legend_note = D2D1::RectF(note_left, note_top, right - 16*scale,
            note_top + caption_h(pack_text::LegendNote(), right - 16*scale - note_left));
        y = (std::max)(legend_y + 24*font*scale, l.preview_legend_note.bottom) + 14*scale;
        l.preview_legend.bottom = y;
        l.preview_formats = D2D1::RectF(left, y, right, y);
        y += LayoutPreviewFormats(l.preview_formats, scale, nullptr, nullptr, caption_h);
        l.preview_formats.bottom = y;
    }
    l.preview_group = D2D1::RectF(left, header_top, right, y);
    y += 24*scale; l.preview_codec_section = row(28*font);
    const float codecs_top = y;
    const bool stacked_codecs = right - left < 620*font*scale;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const bool button = detected && !has && info.store_id;
        const auto status = !detected ? l10n::Pick(L"检测中", L"Checking") : has
            ? l10n::Pick(L"已安装", L"Installed") : l10n::Pick(L"未安装", L"Not installed");
        const float badge_w = painter ? painter->MeasureBadgeWidth(status) : 110*scale;
        const float bw = button ? button_w(l10n::Pick(L"获取", L"Get")) : 0;
        const float text_right = stacked_codecs ? right - 16*scale : right - 28*scale - badge_w - (button ? bw + 10*scale : 0);
        const float desc_h = caption_h(info.description, text_right - left - 54*scale);
        const float text_h = 14*scale + 26*font*scale + desc_h;
        const float row_h = (std::max)(64*scale, text_h + (stacked_codecs ? 46 : 12)*scale);
        l.preview_codec_row[i] = row(row_h / scale);
        const auto r = l.preview_codec_row[i];
        l.preview_codec_text[i] = D2D1::RectF(left + 54*scale, r.top + 10*scale, text_right, r.top + text_h);
        const float controls_y = stacked_codecs ? r.bottom - 42*scale : (r.top + r.bottom - 32*scale)/2;
        if (button) l.preview_codec_button[i] = D2D1::RectF(right - 16*scale - bw, controls_y, right - 16*scale, controls_y + 32*scale);
        const float badge_right = button ? l.preview_codec_button[i].left - 10*scale : right - 16*scale;
        l.preview_codec_badge[i] = D2D1::RectF(badge_right - badge_w, controls_y + (32 - 24*font)*scale/2,
                                            badge_right, controls_y + (32 + 24*font)*scale/2);
    }
    l.preview_codec_hint = D2D1::RectF(left + 16*scale, y + 12*scale, right - 16*scale,
        y + 12*scale + caption_h(pack_text::SystemHint(vm), right - left - 32*scale));
    y = l.preview_codec_hint.bottom + 14*scale;
    l.preview_codec_group = D2D1::RectF(left, codecs_top, right, y);
    y += 30*scale; l.pack_section = row(32*font);
    // Summary: how many packs, their disk use and where they live.
    const bool compact_pack = right - left < 560*font*scale;
    l.pack_summary = row(compact_pack ? 116.0f : 76.0f);
    const float open_w = button_w(pack_text::OpenFolder());
    l.pack_open = D2D1::RectF(right - 16*scale - open_w, l.pack_summary.bottom - 48*scale,
                              right - 16*scale, l.pack_summary.bottom - 16*scale);

    // One pack card: icon, title + badge (+ switch), description, meta line
    // with the primary button, and the last action's message.
    const float text_left = left + 54*scale, inner_right = right - 16*scale;
    struct CardRects { D2D1_RECT_F *card, *badge, *enable, *primary, *notice; float* desc_h; };
    auto card_layout = [&](const CardRects& out, bool shows_enable, const pack_text::Badge& badge,
                           std::wstring_view desc, std::wstring_view primary, const std::wstring& notice) {
        const float top = y;
        float badge_right = inner_right;
        const float badge_top = top + (compact_pack ? 44*font : 19)*scale;
        if (shows_enable) {
            *out.enable = D2D1::RectF(inner_right - 42*scale, badge_top - 5*scale, inner_right, badge_top + 27*scale);
            badge_right = out.enable->left - 12*scale;
        }
        const float badge_w = painter ? painter->MeasureBadgeWidth(badge.text) : 96*scale;
        *out.badge = D2D1::RectF(badge_right - badge_w, badge_top, badge_right, badge_top + 24*font*scale);
        *out.desc_h = caption_h(desc, inner_right - text_left);
        const float desc_top = compact_pack ? out.badge->bottom + 12*scale : top + 40*scale;
        const float footer = desc_top + *out.desc_h + 12*scale;
        const float primary_w = button_w(primary);
        *out.primary = D2D1::RectF(inner_right - primary_w, footer, inner_right, footer + 32*scale);
        float bottom = footer + 32*scale + 14*scale;
        if (!notice.empty()) {
            const float notice_h = caption_h(notice, inner_right - text_left - 12*scale);
            *out.notice = D2D1::RectF(text_left - 8*scale, bottom - 4*scale, inner_right, bottom + notice_h + 12*scale);
            bottom = out.notice->bottom + 12*scale;
        }
        *out.card = D2D1::RectF(left, top, right, bottom);
        y = bottom;
    };

    // Media: the FFmpeg pack card.
    y += 24*scale; l.pack_media_section = row(28);
    card_layout({&l.pack_card, &l.pack_badge, &l.pack_enable, &l.pack_primary, &l.pack_notice, &l.pack_desc_h},
                false, pack_text::MediaBadge(vm), pack_text::MediaDesc(),
                pack_text::Primary(vm), vm.settings_pack_notice);

    // Optional existing FFmpeg is adjacent to the independently installable media pack.
    y += 12*scale;
    const float source_top = y;
    const bool source_switch = pack_text::ShowsEnable(vm);
    const float source_text_width = right - left - (source_switch ? 88 : 32)*scale;
    l.pack_source_status = row((caption_h(pack_text::SourceStatus(vm), source_text_width) + 47*scale) / scale);
    if (source_switch) {
        const float cy = (l.pack_source_status.top + l.pack_source_status.bottom) * 0.5f;
        l.pack_enable = D2D1::RectF(inner_right - 42*scale, cy - 16*scale, inner_right, cy + 16*scale);
    }
    l.pack_custom_row = row((caption_h(pack_text::CustomDesc(), right - left - 126*scale) + 47*scale) / scale);
    if (pack_text::ShowsPath(vm)) {
        l.pack_path_row = row(pack_text::ShowsDetect(vm) ? 110.0f : 90.0f);
        const float browse_w = button_w(pack_text::Browse());
        const float button_top = l.pack_path_row.bottom - 44*scale;
        l.pack_browse = D2D1::RectF(inner_right - browse_w, button_top, inner_right, button_top + 32*scale);
        if (pack_text::ShowsDetect(vm)) {
            const float detect_w = button_w(pack_text::UseDetected());
            l.pack_detect = D2D1::RectF(l.pack_browse.left - 8*scale - detect_w, button_top,
                                        l.pack_browse.left - 8*scale, button_top + 32*scale);
        }
    }
    l.pack_source_group = D2D1::RectF(left, source_top, right, y);

    // Images: the image pack card.
    y += 24*scale; l.pack_images_section = row(28);
    card_layout({&l.pack_images_card, &l.pack_images_badge, &l.pack_images_enable, &l.pack_images_primary,
                 &l.pack_images_notice, &l.pack_images_desc_h},
                pack_text::ShowsImagesEnable(vm), pack_text::ImagesBadge(vm), pack_text::ImagesDesc(),
                pack_text::ImagesPrimary(vm), vm.settings_pack_images_notice);

    // RAW camera photos card.
    y += 12*scale;
    card_layout({&l.pack_raw_card, &l.pack_raw_badge, &l.pack_raw_enable, &l.pack_raw_primary,
                 &l.pack_raw_notice, &l.pack_raw_desc_h},
                pack_text::ShowsRawEnable(vm), pack_text::RawBadge(vm), pack_text::RawDesc(),
                pack_text::RawPrimary(vm), vm.settings_pack_raw_notice);

    // 7-Zip archive contents card.
    y += 24*scale; l.pack_archive_section = row(28);
    card_layout({&l.pack_archive_card, &l.pack_archive_badge, &l.pack_archive_enable, &l.pack_archive_primary,
                 &l.pack_archive_notice, &l.pack_archive_desc_h},
                pack_text::ShowsArchiveEnable(vm), pack_text::ArchiveBadge(vm), pack_text::ArchiveDesc(),
                pack_text::ArchivePrimary(vm), vm.settings_pack_archive_notice);

    // Uninstall preferences remain separate from the media source.
    y += 24*scale; l.pack_advanced_section = row(28);
    const float group_top = y;
    l.pack_remove_row = row((caption_h(pack_text::RemoveDesc(), right - left - 126*scale) + 47*scale) / scale);
    l.pack_group = D2D1::RectF(left, group_top, right, y);
    y += 12*scale;
    l.pack_note = D2D1::RectF(left, y, right, y + caption_h(pack_text::Note(), right - left - 8*scale) + 4*scale);
    y = l.pack_note.bottom + 24*scale;
    return y;
}
