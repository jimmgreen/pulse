// ui_settings_view.cpp — Settings page layout and painting.
#include "ui_renderer.h"
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

void MainRenderer::DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    D2D1_COLOR_F nav_bg = theme.tab_bg;
    if (vm.backdrop_enabled) nav_bg.a = vm.dark ? 0.62f : 0.70f;
    MakeBrush(dc, nav_bg, brFillHover_);
    FillRect(dc, brFillHover_.get(), lay.nav.left, lay.nav.top,
             lay.nav.right - lay.nav.left, lay.nav.bottom - lay.nav.top);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.right - 1.0f, lay.nav.top, 1.0f,
             lay.nav.bottom - lay.nav.top);

    static constexpr pulse::l10n::StringId kNav[] = {
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsSearchIndex,
        pulse::l10n::StringId::SettingsContextMenu,
        pulse::l10n::StringId::SettingsAboutDiagnostics,
    };
    static constexpr const wchar_t* kNavIcon[] = {
        kIconHome, kIconSearch, kIconSettings, kIconInfo
    };
    for (int i = 0; i < 4; ++i) {
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
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), pulse::l10n::Get(kNav[i]),
                     rc.left + 36.0f * scale_, rc.top, rc.right - rc.left - 44.0f * scale_,
                     rc.bottom - rc.top);
    }

    dc->PushAxisAlignedClip(lay.content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float pad = 20.0f * scale_;
    const float origin = lay.content_origin;
    const float switch_w = 42.0f * scale_;
    const float switch_h = 32.0f * scale_;

    const auto page_title_id = vm.settings_page == 0
        ? pulse::l10n::StringId::SettingsGeneral
        : vm.settings_page == 1 ? pulse::l10n::StringId::SettingsSearchIndex
        : vm.settings_page == 2 ? pulse::l10n::StringId::SettingsContextMenu
                                : pulse::l10n::StringId::SettingsAboutDiagnostics;
    MakeBrush(dc, theme.text, brText_);
    DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), pulse::l10n::Get(page_title_id),
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
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsAppearance),
                     lay.content.left + pad, origin + pad + 44.0f * scale_,
                     200.0f * scale_, 22.0f * scale_);

        draw_card(lay.accent_card);
        const float accent_text_w = (std::max)(40.0f * scale_,
            lay.accent_picker.left - lay.accent_card.left - 32.0f * scale_);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsThemeColor),
                     lay.accent_card.left + 16.0f * scale_, lay.accent_card.top + 16.0f * scale_,
                     accent_text_w, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsThemeColorDesc),
                     lay.accent_card.left + 16.0f * scale_, lay.accent_card.top + 42.0f * scale_,
                     accent_text_w, 36.0f * scale_);
        if (vm.settings_bloom) {
            vm.settings_bloom->SetDisk(lay.accent_picker);
            vm.settings_bloom->Draw(dc, theme);
        }

        draw_card(lay.effect_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsWindowEffect),
                     lay.effect_card.left + 16.0f * scale_, lay.effect_card.top + 10.0f * scale_,
                     lay.effect_card.right - lay.effect_card.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsWindowEffectDesc),
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
            static constexpr pulse::l10n::StringId labels[] = {
                pulse::l10n::StringId::EffectNone, pulse::l10n::StringId::EffectAcrylic,
                pulse::l10n::StringId::EffectMica, pulse::l10n::StringId::EffectMicaAlt,
            };
            painter_.DrawRadioButton(D2D1::RectF(row.left + 16.0f * scale_, row.top,
                                                 row.right - 16.0f * scale_, row.bottom),
                                     pulse::l10n::Get(labels[i]), st);
        }

        draw_card(lay.density_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsRowHeight),
                     lay.density_card.left + 16.0f * scale_, lay.density_card.top + 10.0f * scale_,
                     lay.density_card.right - lay.density_card.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsRowHeightDesc),
                     lay.density_card.left + 16.0f * scale_, lay.density_card.top + 32.0f * scale_,
                     lay.density_card.right - lay.density_card.left - 32.0f * scale_, 18.0f * scale_);
        static constexpr pulse::l10n::StringId kDensityLabels[] = {
            pulse::l10n::StringId::DensityCompact,
            pulse::l10n::StringId::DensityStandard,
            pulse::l10n::StringId::DensityComfortable,
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
                                     pulse::l10n::Get(kDensityLabels[i]), st);
        }

        draw_card(lay.tray_icon_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsTrayIcon),
                     lay.tray_icon_card.left + 16.0f * scale_, lay.tray_icon_card.top + 10.0f * scale_,
                     lay.tray_icon_card.right - lay.tray_icon_card.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsTrayIconDesc),
                     lay.tray_icon_card.left + 16.0f * scale_, lay.tray_icon_card.top + 32.0f * scale_,
                     lay.tray_icon_card.right - lay.tray_icon_card.left - 32.0f * scale_, 18.0f * scale_);
        static constexpr pulse::l10n::StringId kTrayIconLabels[] = {
            pulse::l10n::StringId::TrayIconSmall,
            pulse::l10n::StringId::TrayIconStandard,
            pulse::l10n::StringId::TrayIconLarge,
        };
        static constexpr int kTrayIconDips[] = { 40, 48, 56 };
        for (int i = 0; i < 3; ++i) {
            const auto& row = lay.tray_icon_row[i];
            if (IsHovered(vm, HitTestResult::SettingsTrayIcon, i)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            fluent::ControlState st{};
            st.checked = vm.settings_tray_icon == kTrayIconDips[i];
            st.hovered = IsHovered(vm, HitTestResult::SettingsTrayIcon, i);
            painter_.DrawRadioButton(D2D1::RectF(row.left + 16.0f * scale_, row.top,
                                                 row.right - 16.0f * scale_, row.bottom),
                                     pulse::l10n::Get(kTrayIconLabels[i]), st);
        }

        draw_card(lay.language_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsLanguage),
                     lay.language_card.left + 16.0f * scale_,
                     lay.language_card.top + 10.0f * scale_,
                     lay.language_card.right - lay.language_card.left - 32.0f * scale_,
                     22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsLanguageDesc),
                     lay.language_card.left + 16.0f * scale_,
                     lay.language_card.top + 32.0f * scale_,
                     lay.language_card.right - lay.language_card.left - 32.0f * scale_,
                     22.0f * scale_);
        static constexpr pulse::l10n::StringId kLanguageLabels[] = {
            pulse::l10n::StringId::LanguageSystem,
            pulse::l10n::StringId::LanguageZhCN,
            pulse::l10n::StringId::LanguageEnUS,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = lay.language_segment[i];
            segment.text = pulse::l10n::Get(kLanguageLabels[i]);
            segment.position = i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = vm.settings_language == i;
            segment.state.hovered = IsHovered(vm, HitTestResult::SettingsLanguage, i);
            painter_.DrawSegmentedItem(segment);
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

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsStartupShutdown),
                     lay.content.left + pad, lay.startup_row[0].top - 30.0f * scale_,
                     200.0f * scale_, 22.0f * scale_);
        const D2D1_RECT_F startup_card = D2D1::RectF(lay.startup_row[0].left, lay.startup_row[0].top,
                                                     lay.startup_row[2].right, lay.startup_row[2].bottom);
        draw_card(startup_card);
        auto draw_row = [&](const D2D1_RECT_F& row, const std::wstring& title,
                            const std::wstring& desc,
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
        draw_row(lay.startup_row[0],
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsLaunch),
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsLaunchDesc),
                 vm.settings_launch_on_startup, 1);
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), startup_card.left + 16.0f * scale_,
                 lay.startup_row[0].bottom, startup_card.right - startup_card.left - 32.0f * scale_, 1.0f);
        draw_row(lay.startup_row[1],
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsKeepRunning),
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsKeepRunningDesc),
                 vm.settings_keep_running, 2);
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), startup_card.left + 16.0f * scale_,
                 lay.startup_row[1].bottom, startup_card.right - startup_card.left - 32.0f * scale_, 1.0f);
        draw_row(lay.startup_row[2],
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsOpenFolders),
                 pulse::l10n::Get(pulse::l10n::StringId::SettingsOpenFoldersDesc),
                 vm.settings_open_folders, 3);
    } else if (vm.settings_page == 1) {
        fluent::InfoBarSpec info;
        info.bounds = lay.index_info;
        info.title = pulse::l10n::Get(vm.settings_index_service
            ? pulse::l10n::StringId::SettingsFullIndex
            : pulse::l10n::StringId::SettingsUserIndex);
        info.message = vm.settings_index_service
            ? pulse::l10n::Get(pulse::l10n::StringId::SettingsFullIndexDesc)
            : pulse::l10n::Get(pulse::l10n::StringId::SettingsUserIndexDesc);
        info.kind = vm.settings_index_error.empty() ? fluent::InfoBarKind::Informational
                                                     : fluent::InfoBarKind::Error;
        if (!vm.settings_index_error.empty()) info.message = vm.settings_index_error;
        info.show_close = false;
        painter_.DrawInfoBar(info);

        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };
        draw_card(lay.index_status);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::IndexStatus),
                     lay.index_status.left + 16.0f * scale_, lay.index_status.top + 12.0f * scale_,
                     lay.index_status.right - lay.index_status.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_index_status,
                     lay.index_status.left + 16.0f * scale_, lay.index_status.top + 40.0f * scale_,
                     lay.index_status.right - lay.index_status.left - 32.0f * scale_, 24.0f * scale_);

        draw_card(lay.index_path);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::IndexLocation),
                     lay.index_path.left + 16.0f * scale_, lay.index_path.top + 8.0f * scale_,
                     100.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_index_path,
                     lay.index_path.left + 16.0f * scale_, lay.index_path.top + 32.0f * scale_,
                     lay.index_action[0].top > lay.index_path.top + 60.0f * scale_
                         ? lay.index_path.right - lay.index_path.left - 32.0f * scale_
                         : lay.index_action[2].left - lay.index_path.left - 28.0f * scale_,
                     20.0f * scale_);
        const std::wstring actions[] = {
            pulse::l10n::Get(pulse::l10n::StringId::Rebuild),
            pulse::l10n::Get(pulse::l10n::StringId::OpenLocation),
            pulse::l10n::Get(vm.settings_index_service
                ? pulse::l10n::StringId::ChangeLocation
                : pulse::l10n::StringId::InstallService),
        };
        for (int i = 0; i < 3; ++i) {
            fluent::ControlState st{};
            st.enabled = i == 2 || vm.settings_index_service;
            st.hovered = st.enabled && IsHovered(vm, HitTestResult::SettingsIndexAction, i);
            painter_.DrawButton({ lay.index_action[i], actions[i], {},
                                  i == 2 ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  st });
        }

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        const float header_y = lay.index_volume_rows.empty()
            ? lay.index_path.bottom + 18.0f * scale_
            : lay.index_volume_rows.front().top - 30.0f * scale_;
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::LocalDrives),
                     lay.content.left + pad, header_y, 200.0f * scale_, 22.0f * scale_);
        for (size_t i = 0; i < vm.settings_index_volumes.size() && i < lay.index_volume_rows.size(); ++i) {
            const auto& volume = vm.settings_index_volumes[i];
            const auto& row = lay.index_volume_rows[i];
            if (IsHovered(vm, HitTestResult::SettingsIndexVolume, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                row.right - row.left, row.bottom - row.top, 6.0f * scale_);
            }
            fluent::ControlState check{};
            check.checked = volume.checked;
            check.enabled = volume.enabled && !volume.pending;
            check.hovered = IsHovered(vm, HitTestResult::SettingsIndexVolume, static_cast<int>(i));
            painter_.DrawCheckBox(D2D1::RectF(row.left + 12.0f * scale_, row.top,
                                              row.left + 44.0f * scale_, row.bottom), L"", check);
            const float badge_h = 22.0f * scale_;
            const float badge_w = volume.state.empty() ? 0.0f
                : (std::min)(painter_.MeasureBadgeWidth(volume.state), 148.0f * scale_);
            const float text_w = (std::max)(40.0f * scale_,
                row.right - row.left - 60.0f * scale_ - (badge_w > 0 ? badge_w + 16.0f * scale_ : 0));
            MakeBrush(dc, check.enabled ? theme.text : theme.text_disabled, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), volume.title,
                         row.left + 48.0f * scale_, row.top + 6.0f * scale_,
                         text_w, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), volume.detail,
                         row.left + 48.0f * scale_, row.top + 30.0f * scale_,
                         text_w, 18.0f * scale_);
            if (badge_w > 0.0f) {
                const float badge_x = row.right - 12.0f * scale_ - badge_w;
                const float badge_y = row.top + ((row.bottom - row.top) - badge_h) * 0.5f;
                painter_.DrawBadge({ D2D1::RectF(badge_x, badge_y,
                                                 badge_x + badge_w, badge_y + badge_h),
                                     volume.state, IndexVolumeBadgeKind(volume.state) });
            }
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), row.left + 12.0f * scale_, row.bottom - 1.0f,
                     row.right - row.left - 24.0f * scale_, 1.0f);
        }

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::Exclusions),
                     lay.content.left + pad, lay.index_exclude_action.top + 5.0f * scale_,
                     160.0f * scale_, 22.0f * scale_);
        fluent::ControlState add_exclude{};
        add_exclude.enabled = vm.settings_index_service;
        add_exclude.hovered = add_exclude.enabled &&
            IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 0);
        painter_.DrawButton({ lay.index_exclude_action,
                              pulse::l10n::Get(pulse::l10n::StringId::AddFolder), {},
                              fluent::ButtonKind::Primary, add_exclude });
        if (vm.settings_index_excluded_paths.empty() &&
            lay.index_exclude_empty.bottom > lay.index_exclude_empty.top) {
            draw_card(lay.index_exclude_empty);
            const float art_top = lay.index_exclude_empty.top + 8.0f * scale_;
            const float art_bottom = lay.index_exclude_empty.bottom - 48.0f * scale_;
            const D2D1_RECT_F art = D2D1::RectF(lay.index_exclude_empty.left + 16.0f * scale_,
                                                art_top,
                                                lay.index_exclude_empty.right - 16.0f * scale_,
                                                art_bottom);
            const float svg_opacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
            if (!DrawExcludeEmptySvg(art, svg_opacity)) {
                fluent::EmptyStateSpec fallback;
                fallback.bounds = art;
                fallback.glyph = L"\xE738";
                fallback.title = pulse::l10n::Get(pulse::l10n::StringId::NoExcludedFolders);
                painter_.DrawEmptyState(fallback);
            } else {
                const D2D1_RECT_F caption = D2D1::RectF(
                    lay.index_exclude_empty.left + 16.0f * scale_,
                    art_bottom + 6.0f * scale_,
                    lay.index_exclude_empty.right - 16.0f * scale_,
                    lay.index_exclude_empty.bottom - 12.0f * scale_);
                painter_.DrawText(
                    pulse::l10n::Get(pulse::l10n::StringId::NoExcludedFoldersDesc),
                    caption, compositor_->SmallFormat(), theme.text_secondary,
                    fluent::HorizontalAlignment::Center);
            }
        }
        for (size_t i = 0; i < vm.settings_index_excluded_paths.size() &&
                           i < lay.index_exclude_rows.size() &&
                           i < lay.index_exclude_remove.size(); ++i) {
            const auto& row = lay.index_exclude_rows[i];
            const auto& remove_rc = lay.index_exclude_remove[i];
            if (IsHovered(vm, HitTestResult::SettingsIndexExcludeRemove, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                row.right - row.left, row.bottom - row.top, 6.0f * scale_);
            }
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(),
                         vm.settings_index_excluded_paths[i],
                         row.left + 16.0f * scale_, row.top + 17.0f * scale_,
                         std::max(40.0f * scale_, remove_rc.left - 8.0f * scale_ -
                                  (row.left + 16.0f * scale_)), 22.0f * scale_);
            fluent::ControlState remove{};
            remove.enabled = vm.settings_index_service;
            remove.hovered = remove.enabled &&
                IsHovered(vm, HitTestResult::SettingsIndexExcludeRemove, static_cast<int>(i));
            painter_.DrawButton({ remove_rc,
                                  pulse::l10n::Get(pulse::l10n::StringId::Remove), {},
                                  fluent::ButtonKind::Standard, remove });
        }

        const float network_header_y = lay.network_action[0].top + 5.0f * scale_;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::ServerFolders),
                     lay.content.left + pad, network_header_y, 160.0f * scale_, 22.0f * scale_);
        const std::wstring network_actions[] = {
            pulse::l10n::Get(pulse::l10n::StringId::AddFolder),
            pulse::l10n::Get(pulse::l10n::StringId::Rescan),
        };
        for (int i = 0; i < 2; ++i) {
            fluent::ControlState state{};
            state.enabled = i == 0 || !vm.settings_network_roots.empty();
            state.hovered = state.enabled && IsHovered(vm, HitTestResult::SettingsNetworkAction, i);
            painter_.DrawButton({ lay.network_action[i], network_actions[i], {},
                                  i == 0 ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  state });
        }
        if (vm.settings_network_roots.empty()) {
            const float empty_y = lay.network_action[0].bottom + 10.0f * scale_;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::NoServerFoldersDesc),
                         lay.content.left + pad, empty_y,
                         lay.content.right - lay.content.left - pad * 2, 22.0f * scale_);
        }
        for (size_t i = 0; i < vm.settings_network_roots.size() && i < lay.network_rows.size(); ++i) {
            const auto& network = vm.settings_network_roots[i];
            const auto& row = lay.network_rows[i];
            draw_card(row);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), network.path,
                         row.left + 16.0f * scale_, row.top + 8.0f * scale_,
                         (std::max)(40.0f * scale_,
                                    (i < lay.network_remove.size()
                                         ? lay.network_remove[i].left - 8.0f * scale_
                                         : row.right - 12.0f * scale_) -
                                        (row.left + 16.0f * scale_)),
                         22.0f * scale_);
            MakeBrush(dc, network.online ? theme.text_secondary : theme.text_disabled,
                      brTextSecondary_);
            const std::wstring detail = network.detail.empty() ? network.state
                                                               : network.state + L" · " + network.detail;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), detail,
                         row.left + 16.0f * scale_, row.top + 34.0f * scale_,
                         (std::max)(40.0f * scale_,
                                    (i < lay.network_remove.size()
                                         ? lay.network_remove[i].left - 8.0f * scale_
                                         : row.right - 12.0f * scale_) -
                                        (row.left + 16.0f * scale_)),
                         18.0f * scale_);
            fluent::ControlState remove{};
            remove.enabled = true;
            remove.hovered = remove.enabled &&
                IsHovered(vm, HitTestResult::SettingsNetworkRemove, static_cast<int>(i));
            painter_.DrawButton({ lay.network_remove[i],
                                  pulse::l10n::Get(pulse::l10n::StringId::Remove), {},
                                  fluent::ButtonKind::Standard, remove });
        }
    } else if (vm.settings_page == 2) {
        float y = origin + pad + 44.0f * scale_;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::ContextMenuDesc),
                     lay.content.left + pad, y, lay.content.right - lay.content.left - pad * 2,
                     22.0f * scale_);
        y += 30.0f * scale_;
        static constexpr pulse::l10n::StringId kGroupTitle[] = {
            pulse::l10n::StringId::ContextSoftware,
            pulse::l10n::StringId::ContextOpenWith,
            pulse::l10n::StringId::ContextShare,
            pulse::l10n::StringId::ContextSystem,
            pulse::l10n::StringId::ContextPrint,
        };
        static constexpr pulse::l10n::StringId kGroupDesc[] = {
            pulse::l10n::StringId::ContextSoftwareDesc,
            pulse::l10n::StringId::ContextOpenWithDesc,
            pulse::l10n::StringId::ContextShareDesc,
            pulse::l10n::StringId::ContextSystemDesc,
            pulse::l10n::StringId::ContextPrintDesc,
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
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(kGroupTitle[g]),
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
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(kGroupDesc[g]),
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
                              pulse::l10n::Get(pulse::l10n::StringId::RestoreDefaults), {},
                              fluent::ButtonKind::Standard, restore });
    } else {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };

        draw_card(lay.about_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::AboutPulse),
                     lay.about_card.left + 16.0f * scale_, lay.about_card.top + 12.0f * scale_,
                     lay.about_card.right - lay.about_card.left - 32.0f * scale_,
                     24.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_version,
                     lay.about_card.left + 16.0f * scale_, lay.about_card.top + 42.0f * scale_,
                     lay.about_card.right - lay.about_card.left - 32.0f * scale_,
                     20.0f * scale_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_build_id,
                     lay.about_card.left + 16.0f * scale_, lay.about_card.top + 66.0f * scale_,
                     lay.about_card.right - lay.about_card.left - 32.0f * scale_,
                     20.0f * scale_);

        draw_card(lay.diagnostics_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::Diagnostics),
                     lay.diagnostics_card.left + 16.0f * scale_,
                     lay.diagnostics_card.top + 12.0f * scale_,
                     lay.diagnostics_card.right - lay.diagnostics_card.left - 32.0f * scale_,
                     22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        const std::wstring diagnostics_status = vm.settings_diagnostics_exporting
            ? pulse::l10n::Get(pulse::l10n::StringId::DiagnosticsExporting)
            : pulse::l10n::Get(pulse::l10n::StringId::DiagnosticsDesc);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), diagnostics_status,
                     lay.diagnostics_card.left + 16.0f * scale_,
                     lay.diagnostics_card.top + 40.0f * scale_,
                     lay.diagnostics_card.right - lay.diagnostics_card.left - 32.0f * scale_,
                     42.0f * scale_);
        static constexpr pulse::l10n::StringId kDiagnosticsActions[] = {
            pulse::l10n::StringId::OpenDiagnostics,
            pulse::l10n::StringId::ClearDiagnostics,
            pulse::l10n::StringId::ExportDiagnostics,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::ControlState state{};
            state.enabled = !vm.settings_diagnostics_exporting;
            state.hovered = state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDiagnosticsAction, i);
            painter_.DrawButton({lay.diagnostics_action[i],
                pulse::l10n::Get(kDiagnosticsActions[i]), {},
                fluent::ButtonKind::Standard, state});
        }

        draw_card(lay.update_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::Update),
                     lay.update_card.left + 16.0f * scale_, lay.update_card.top + 12.0f * scale_,
                     lay.update_card.right - lay.update_card.left - 32.0f * scale_,
                     22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     vm.settings_update_status,
                     lay.update_card.left + 16.0f * scale_, lay.update_card.top + 40.0f * scale_,
                     lay.update_card.right - lay.update_card.left - 32.0f * scale_,
                     38.0f * scale_);
        if (vm.settings_update_available && !vm.settings_update_hash.empty()) {
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         vm.settings_update_hash,
                         lay.update_card.left + 16.0f * scale_,
                         lay.update_card.top + 82.0f * scale_,
                         lay.update_card.right - lay.update_card.left - 32.0f * scale_,
                         34.0f * scale_);
        }
        fluent::ControlState check{};
        check.enabled = vm.settings_update_enabled && !vm.settings_update_checking;
        check.hovered = check.enabled && IsHovered(vm, HitTestResult::SettingsUpdateAction, 0);
        painter_.DrawButton({lay.update_action[0],
            pulse::l10n::Get(pulse::l10n::StringId::CheckForUpdates), {},
            fluent::ButtonKind::Standard, check});
        if (vm.settings_update_available) {
            fluent::ControlState download{};
            download.enabled = true;
            download.hovered = IsHovered(vm, HitTestResult::SettingsUpdateAction, 1);
            painter_.DrawButton({lay.update_action[1],
                pulse::l10n::Get(pulse::l10n::StringId::DownloadUpdate), {},
                fluent::ButtonKind::Primary, download});
        }
        if (!vm.settings_index_error.empty()) {
            MakeBrush(dc, theme.danger, brDanger_);
            DrawTextRect(dc, compositor_->SmallFormat(), brDanger_.get(),
                         vm.settings_index_error,
                         lay.content.left + pad, lay.update_card.bottom + 8.0f * scale_,
                         lay.content.right - lay.content.left - pad * 2, 36.0f * scale_);
        }
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
                                                  status_height_, &painter_);
    const float view = (std::max)(0.0f, lay.content.bottom - lay.content.top);
    return (std::max)(0.0f, lay.content_h - view);
}

} // namespace pulse::ui
