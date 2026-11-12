#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "store_icons.h"
#include <algorithm>

namespace pulse::ui {

// Settings > 快速预览 (page 5). Geometry: LayoutSettingsPacks; texts:
// pack_text (settings_layout_sections.h); clicks: SettingsPackAction.
void MainRenderer::DrawSettingsPacks(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const float s = scale_;
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brStrokeCard_.get(), 1);
    };
    auto divider_below = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16*s, r.bottom, r.right - r.left - 32*s, 1);
    };
    auto hovered = [&](PackAction action) {
        return IsHovered(vm, H::SettingsPackAction, static_cast<int>(action));
    };
    auto hover_row = [&](D2D1_RECT_F r, PackAction action) {
        if (r.bottom <= r.top || !hovered(action)) return;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), r.left + 4*s, r.top + 2*s,
                        r.right - r.left - 8*s, r.bottom - r.top - 4*s, 4*s);
    };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, PackAction action,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(action);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto toggle = [&](D2D1_RECT_F bounds, bool on, PackAction action) {
        fluent::ControlState state{};
        state.checked = on;
        state.hovered = hovered(action);
        painter_.DrawSwitch(bounds, L"", state);
    };
    auto row_switch = [&](D2D1_RECT_F row) {
        const float cy = (row.top + row.bottom) * 0.5f;
        return D2D1::RectF(row.right - 16*s - 42*s, cy - 16*s, row.right - 16*s, cy + 16*s);
    };
    auto setting = [&](D2D1_RECT_F r, const wchar_t* icon, std::wstring_view title, std::wstring_view desc,
                       StoreIcon vector_icon = StoreIcon::None) {
        const float right = r.right - 72*s;
        if (vector_icon != StoreIcon::None)
            DrawStoreIcon(dc, vector_icon, D2D1::RectF(r.left+16*s,r.top+20*s,r.left+40*s,r.top+44*s), theme.text_secondary);
        else
            DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
    };

    // Format availability and pack controls share the same current settings model.
    const auto source_color = [&](PreviewFormatSource source) {
        if (source == PreviewFormatSource::Pack) {
            auto color = theme.accent;
            if (vm.dark) {
                color.r += (1.0f - color.r) * 0.5f;
                color.g += (1.0f - color.g) * 0.5f;
                color.b += (1.0f - color.b) * 0.5f;
            }
            return color;
        }
        if (source == PreviewFormatSource::PackOrSystem)
            return vm.dark ? HexColor(0xB5C6E5) : HexColor(0x4F5D77);
        return theme.text;
    };
    const auto chip = [&](D2D1_RECT_F bounds, std::wstring_view label, PreviewFormatState support) {
        const auto color = source_color(support.source);
        const auto rounded = D2D1::RoundedRect(bounds, (bounds.bottom - bounds.top)/2, (bounds.bottom - bounds.top)/2);
        MakeBrush(dc, WithAlpha(color, vm.dark ? 0.13f : 0.08f), brFillHover_);
        if (support.available) dc->FillRoundedRectangle(rounded, brFillHover_.get());
        else {
            if (!dashStroke_.get()) {
                ComPtr<ID2D1Factory> factory;
                dc->GetFactory(&factory);
                auto properties = D2D1::StrokeStyleProperties();
                properties.dashStyle = D2D1_DASH_STYLE_DASH;
                factory->CreateStrokeStyle(properties, nullptr, 0, &dashStroke_);
            }
            MakeBrush(dc, WithAlpha(color, 0.65f), brStrokeCard_);
            dc->DrawRoundedRectangle(rounded, brStrokeCard_.get(), s, dashStroke_.get());
        }
        painter_.DrawText(label, bounds, compositor_->SmallFormat(), color, fluent::HorizontalAlignment::Center);
    };
    // 预览方式: hover card or Space only, then the hover delay.
    auto choice_row = [&](D2D1_RECT_F r, const D2D1_RECT_F* choices, int count, int checked,
                          PackAction first, const wchar_t* icon, std::wstring_view title,
                          std::wstring_view desc, const wchar_t* (*label)(int)) {
        if (r.bottom <= r.top) return;
        card(r);
        const bool stacked = choices[0].left < r.left + 100*s;
        const float right = stacked ? r.right - 16*s : choices[0].left - 12*s;
        DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left, choices[0].top,
                                                choices[count - 1].right, choices[count - 1].bottom));
        if (checked >= 0 && checked < count) DrawSettingsSegmentPlate(110000 + static_cast<int>(first), checked, choices[checked]);
        for (int i = 0; i < count; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = choices[i];
            item.text = label(i);
            item.state.checked = i == checked;
            item.suppress_plate = item.state.checked;
            item.state.hovered = hovered(static_cast<PackAction>(static_cast<int>(first) + i));
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                : i == count - 1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    if (vm.settings_page == 8) {
        using I = l10n::StringId;
        settings_store_fields_[0] = lay.store_search;
        settings_store_fields_[1] = lay.sz.folder;
        settings_store_fields_[2] = lay.store_quick.mode;
        settings_store_fields_[3] = lay.store_quick.format;
        settings_store_fields_[4] = lay.store_quick.level;
        settings_store_fields_[5] = lay.sz.more;
        settings_store_fields_[6] = lay.sz.level_track;
        settings_store_clip_ = lay.content;
        if (lay.sz.on) { DrawSevenZipDetail(vm, rect, theme); return; }
        // Pivot: labels on a hairline, the current one in full text with an accent bar under it.
        {
            const int selected=vm.settings_store_browse ? 0 : vm.settings_store_installed ? 1 : 2;
            const float line_left=lay.store_tabs[0].left;
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            for (int i=0;i<3;++i) if (i==0 || lay.store_tabs[i].top>lay.store_tabs[i-1].top)
                FillRect(dc, brStrokeDivider_.get(), line_left, lay.store_tabs[i].bottom-1, lay.content.right-20*s-line_left, 1);
            for (int i=0;i<3;++i) {
                const auto r=lay.store_tabs[i];
                const bool on=selected==i;
                const bool hot=hovered(i==0 ? PackAction::StoreBrowse : i==1 ? PackAction::StoreInstalled : PackAction::StoreManage);
                if (hot && !on) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), r.left, r.top+2*s, r.right-r.left, r.bottom-r.top-8*s, 6*s);
                }
                const bool installed_tab = i == 1;
                const auto label_rect = D2D1::RectF(r.left, r.top, r.right - (installed_tab ? 32*s : 0), r.bottom-4*s);
                painter_.DrawText(store_view::Tab(i), label_rect,
                    on ? compositor_->HeaderFormat() : compositor_->TextFormat(),
                    on || hot ? theme.text : theme.text_secondary, fluent::HorizontalAlignment::Center);
                if (installed_tab) {
                    const bool updates = std::any_of(vm.settings_store_view.items.begin(), vm.settings_store_view.items.end(),
                        [](const auto& item) { return item.settings_package >= 0 && item.action_enabled; });
                    const auto count_color = updates ? theme.accent : theme.text_secondary;
                    const auto badge_rect = D2D1::RectF(r.right-32*s, r.top+8*s, r.right-8*s, r.bottom-10*s);
                    painter_.FillRoundedRect(badge_rect, 9*s, WithAlpha(count_color, .10f));
                    painter_.DrawText(std::to_wstring(vm.settings_store_view.installed.size()), badge_rect,
                        compositor_->SmallFormat(), count_color, fluent::HorizontalAlignment::Center);
                }
                if (on) {
                    MakeBrush(dc, vm.dark ? MixColor(theme.accent, HexColor(0xFFFFFF), 0.22f) : theme.accent, brAccent_);
                    FillRoundedRect(dc, brAccent_.get(), r.left+12*s, r.bottom-3*s, r.right-r.left-24*s, 3*s, 1.5f*s);
                }
            }
        }
        if (!vm.settings_store_browse && vm.settings_store_installed) {
            const auto& view=vm.settings_store_view;
            auto caption=[&](std::wstring_view value,D2D1_RECT_F r,D2D1_COLOR_F color) {
                if (r.bottom<=r.top) return;
                painter_.DrawWrappedCaption(value,D2D1::Point2F(r.left,r.top),r.right-r.left,color);
            };
            auto bar=[&](std::wstring_view value,D2D1_RECT_F r,fluent::InfoBarKind kind) {
                if (r.bottom<=r.top) return;
                const auto color=painter_.StatusToneColor(kind);
                painter_.FillRoundedRect(D2D1::RectF(r.left-12*s,r.top-6*s,r.right+12*s,r.bottom+6*s),6*s,WithAlpha(color,vm.dark?0.12f:0.07f));
                caption(value,r,theme.text_secondary);
            };
            if (lay.store_search.right > lay.store_search.left) {
                painter_.DrawTextFieldFrame(lay.store_search, {}, true);
                DrawStoreIcon(dc, StoreIcon::Search,
                    D2D1::RectF(lay.store_search.left+10*s,lay.store_search.top+9*s,
                        lay.store_search.left+26*s,lay.store_search.top+25*s), theme.text_secondary);
            }
            caption(l10n::Get(I::StoreNoMatch),lay.store_no_match,theme.text_secondary);
            button(lay.store_refresh,l10n::Get(I::Refresh),PackAction::InstalledRefresh,
                fluent::ButtonKind::Standard,store_text::CanRefreshInstalled(view));
            button(lay.store_install_file,l10n::Get(I::StoreInstallFromFile),PackAction::StoreInstallFromFile,
                fluent::ButtonKind::Standard,store_text::CanInstallFromFile(view));
            caption(l10n::Get(view.installed_loading ? I::StoreInstalledLoading : I::StoreInstalledFailed),
                lay.store_state,view.installed_loading ? theme.text_secondary : painter_.StatusToneColor(fluent::InfoBarKind::Error));
            bar(view.installed_error,lay.store_notice,fluent::InfoBarKind::Error);
            if (lay.store_restart.bottom > lay.store_restart.top) {
                auto banner = lay.store_restart;
                if (lay.store_restart_button.bottom > lay.store_restart_button.top && lay.store_restart_button.right > lay.store_restart_button.left) {
                    banner.left = (std::min)(banner.left, lay.store_restart_button.left);
                    banner.top = (std::min)(banner.top, lay.store_restart_button.top);
                    banner.right = (std::max)(banner.right, lay.store_restart_button.right);
                    banner.bottom = (std::max)(banner.bottom, lay.store_restart_button.bottom);
                }
                const auto color = painter_.StatusToneColor(fluent::InfoBarKind::Warning);
                painter_.FillRoundedRect(D2D1::RectF(banner.left-12*s,banner.top-6*s,banner.right+12*s,banner.bottom+6*s),
                    6*s,WithAlpha(color,vm.dark?0.12f:0.07f));
                caption(l10n::Get(I::StoreRestart),lay.store_restart,theme.text_secondary);
                button(lay.store_restart_button,l10n::Get(I::StoreRestartNow),PackAction::StoreRestartNow);
            }
            caption(l10n::Get(I::StoreInstalledEmpty),lay.store_empty,theme.text_secondary);
            for (size_t i=0;i<lay.installed_items.size() && i<view.installed.size();++i) {
                const auto& c=lay.installed_items[i]; const auto& item=view.installed[i];
                if (c.bounds.right <= c.bounds.left || c.bounds.bottom <= c.bounds.top) continue;
                card(c.bounds);
                const auto catalog = std::find_if(view.items.begin(), view.items.end(), [&](const auto& value) {
                    return std::wstring(value.id.begin(), value.id.end()) == item.id;
                });
                const auto icon = catalog == view.items.end() ? (item.id == L"pulse.7zip" ? StoreIcon::Box : StoreIcon::Puzzle) : catalog->icon;
                const auto icon_color = catalog == view.items.end() ? (item.id == L"pulse.7zip" ? HexColor(0x527D70) : theme.accent) : HexColor(catalog->icon_color);
                painter_.FillRoundedRect(c.icon, 11*s, icon_color);
                {
                    const D2D1_GRADIENT_STOP stops[]={{0,MixColor(icon_color,D2D1::ColorF(1,1,1),.17f)},{1,MixColor(icon_color,D2D1::ColorF(0,0,0),.16f)}};
                    ComPtr<ID2D1GradientStopCollection> stops_collection;ComPtr<ID2D1LinearGradientBrush> gradient;
                    if(SUCCEEDED(dc->CreateGradientStopCollection(stops,2,&stops_collection))&&SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({c.icon.left,c.icon.top},{c.icon.right,c.icon.bottom}),stops_collection.get(),&gradient)))
                        dc->FillRoundedRectangle(D2D1::RoundedRect(c.icon,11*s,11*s),gradient.get());
                }
                DrawStoreIcon(dc, icon, D2D1::RectF(c.icon.left+11*s,c.icon.top+11*s,c.icon.right-11*s,c.icon.bottom-11*s), D2D1::ColorF(1,1,1));
                if (c.name.bottom > c.name.top)
                    painter_.DrawWrappedSection(item.name, D2D1::Point2F(c.name.left, c.name.top), c.name.right - c.name.left, theme.text);
                caption(item.id,c.id,theme.text_secondary);
                caption(item.publisher,c.publisher,theme.text_secondary);
                caption(item.version,c.version,theme.text_secondary);
                fluent::BadgeSpec badge{};
                badge.bounds=c.badge; badge.text=store_text::InstalledBadge(item);
                badge.kind=item.enabled ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
                painter_.DrawBadge(badge);
                caption(l10n::Get(I::StoreNativeCode),c.native_code,theme.text_secondary);
                caption(item.status,c.status,theme.text_secondary);
                caption(item.notice,c.notice,theme.text_secondary);
                const auto action=[&](PackAction base) { return static_cast<PackAction>(static_cast<int>(base)+static_cast<int>(i)); };
                const auto switch_bounds = item.enabled ? c.disable : c.enable;
                if (switch_bounds.right > switch_bounds.left) {
                    fluent::ControlState state{};
                    state.checked = item.enabled;
                    state.enabled = (item.enabled ? item.can_disable : item.can_enable) && !view.installed_busy && i < 10000;
                    state.hovered = state.enabled && hovered(action(item.enabled ? PackAction::InstalledDisableBase : PackAction::InstalledEnableBase));
                    painter_.DrawSwitch(switch_bounds, L"", state);
                }
                if (catalog != view.items.end()) {
                    const auto catalog_index = static_cast<int>(catalog - view.items.begin());
                    button(c.settings, l10n::Get(I::Settings),
                        static_cast<PackAction>(static_cast<int>(PackAction::StoreItemSettingsBase) + catalog_index),
                        fluent::ButtonKind::Standard, store_view::HasSettings(view, *catalog) && catalog_index < 10000);
                }
                if (c.more.right > c.more.left) {
                    fluent::ControlState state{};
                    state.enabled = (item.can_uninstall || item.can_rollback) && !view.installed_busy && i < 10000;
                    state.hovered = state.enabled && hovered(action(PackAction::InstalledMoreBase));
                    painter_.DrawButton({c.more, L"", {}, fluent::ButtonKind::Standard, state});
                    DrawStoreIcon(dc, StoreIcon::More,
                        D2D1::RectF(c.more.left+8*s,c.more.top+8*s,c.more.right-8*s,c.more.bottom-8*s),
                        state.enabled ? theme.text_secondary : theme.text_disabled);
                }
            }
            return;
        }
        if (vm.settings_store_browse) {
            DrawSettingsStore(vm, rect, theme);
            return;
        }
    }

    if (vm.settings_page != 8) {
    text(pack_text::TriggerSection(), lay.trigger_section);
    const int trigger = vm.settings_preview_trigger == 1 ? 1 : 0;
    choice_row(lay.trigger_row, lay.trigger_choice, 2, trigger, PackAction::TriggerHover, L"\xE890",
               pack_text::TriggerTitle(), pack_text::TriggerDesc(trigger), &pack_text::TriggerChoice);
    const int delay = vm.settings_hover_delay_ms == 300 ? 0 : vm.settings_hover_delay_ms == 800 ? 2 : 1;
    choice_row(lay.hover_delay_row, lay.hover_delay_choice, 3, delay, PackAction::HoverDelayShort, L"\xE916",
               pack_text::DelayTitle(), pack_text::DelayDesc(), &pack_text::DelayChoice);
    const int card_size = std::clamp(vm.settings_hover_card_size, 0, 3);
    choice_row(lay.hover_size_row, lay.hover_size_choice, 4, card_size, PackAction::HoverSizeSmall, L"\xE740",
               pack_text::SizeTitle(), pack_text::SizeDesc(), &pack_text::SizeChoice);
    const int hold = vm.settings_hold_preview_ms == 300 ? 1 : vm.settings_hold_preview_ms == 500 ? 2
                   : vm.settings_hold_preview_ms == 800 ? 3 : 0;
    choice_row(lay.hold_row, lay.hold_choice, 4, hold, PackAction::HoldOff, L"\xE7C9",
               pack_text::HoldTitle(), pack_text::HoldDesc(), &pack_text::HoldChoice);

    // 预览窗口: the size Space opens it at, Space on media, loop.
    text(pack_text::WindowSection(), lay.window_section);
    choice_row(lay.preview_size_row, lay.preview_size_choice, 5, std::clamp(vm.settings_preview_size, 0, 4),
               PackAction::PreviewSizeFit, L"\xE9A6", pack_text::WindowSizeTitle(), pack_text::WindowSizeDesc(),
               &pack_text::WindowSizeChoice);
    if (lay.preview_remember_row.bottom > lay.preview_remember_row.top) {
        card(lay.preview_remember_row);
        hover_row(lay.preview_remember_row, PackAction::PreviewRememberSize);
        setting(lay.preview_remember_row, L"\xE81C", pack_text::RememberTitle(), pack_text::RememberDesc());
        toggle(row_switch(lay.preview_remember_row), vm.settings_preview_remember, PackAction::PreviewRememberSize);
    }
    choice_row(lay.space_row, lay.space_choice, 2, vm.settings_preview_space_closes ? 0 : 1,
               PackAction::SpaceClose, L"\xE765", pack_text::SpaceTitle(), pack_text::SpaceDesc(),
               &pack_text::SpaceChoice);
    choice_row(lay.loop_row, lay.loop_choice, 3, std::clamp(vm.settings_preview_loop, 0, 2),
               PackAction::LoopAnimations, L"\xE8EE", pack_text::LoopTitle(), pack_text::LoopDesc(),
               &pack_text::LoopChoice);

    text(pack_text::FormatsSection(), lay.preview_section);
    card(lay.preview_group);
    const auto header = lay.disclosure[2];
    if (IsHovered(vm, H::SettingsDisclosure, 2)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), header.left + 2*s, header.top + 2*s,
                        header.right - header.left - 4*s, header.bottom - header.top - 4*s, 6*s);
    }
    DrawIconText(header.left + 16*s, header.top + 20*s, 24*s, 24*s, L"\xE8A5", L"", theme.text_secondary, 0.85f);
    text(pack_text::FormatsTitle(), lay.preview_header_title);
    painter_.DrawWrappedCaption(pack_text::FormatsSummary(vm),
        D2D1::Point2F(lay.preview_header_summary.left, lay.preview_header_summary.top),
        lay.preview_header_summary.right - lay.preview_header_summary.left, theme.text_secondary);
    const bool formats_open = (vm.settings_expanded & 4u) != 0;
    DrawIconText(header.right - 38*s, (header.top + header.bottom)/2 - 9*s, 18*s, 18*s,
                 formats_open ? L"\xE70D" : L"\xE76C", L"", theme.text_secondary, 0.75f);
    if (formats_open) {
        for (int i = 0; i < 3; ++i)
            chip(lay.preview_legend_chip[i], pack_text::Legend(i), {static_cast<PreviewFormatSource>(i), i == 0});
        painter_.DrawWrappedCaption(pack_text::LegendNote(),
            D2D1::Point2F(lay.preview_legend_note.left, lay.preview_legend_note.top),
            lay.preview_legend_note.right - lay.preview_legend_note.left, theme.text_secondary);
        const auto format_divider = [&](float y) {
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), lay.preview_group.left, y,
                     lay.preview_group.right - lay.preview_group.left, 1);
        };
        format_divider(lay.preview_legend.bottom);
        std::vector<PreviewFormatChip> chips;
        std::vector<PreviewFormatRow> rows;
        LayoutPreviewFormats(lay.preview_formats, s, &chips, &rows,
            [&](std::wstring_view value, float width) { return painter_.MeasureWrappedCaptionHeight(value, width); });
        const auto& groups = PreviewFormatGroups();
        for (size_t g = 0; g < rows.size() && g < groups.size(); ++g) {
            text(groups[g].name, rows[g].name);
            const auto& note = rows[g].note;
            if (note.bottom > note.top && !groups[g].note.empty()) {
                painter_.DrawWrappedCaption(groups[g].note, D2D1::Point2F(note.left, note.top),
                                            note.right - note.left, theme.text_secondary);
            }
            if (g + 1 < rows.size()) format_divider(rows[g].bounds.bottom);
        }
        for (const auto& item : chips)
            chip(item.rect, groups[item.group].extensions[item.index], PreviewFormatSupport(vm, item.group, item.index));
    }

    text(pack_text::SystemTitle(), lay.preview_codec_section);
    card(lay.preview_codec_group);
    const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const auto& r = lay.preview_codec_row[i];
        const auto& bounds = lay.preview_codec_text[i];
        const float title_h = 26*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        DrawIconText(r.left + 16*s, r.top + 17*s, 24*s, 24*s,
            i == 1 || i == 2 ? L"\xE714" : L"\xE91B", L"", theme.text_secondary, 0.85f);
        text(info.name, D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + title_h));
        painter_.DrawWrappedCaption(info.description, D2D1::Point2F(bounds.left, bounds.top + title_h),
            bounds.right - bounds.left, theme.text_secondary);
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const auto status = !detected ? l10n::Get(l10n::StringId::Checking).c_str() : has
            ? l10n::Get(l10n::StringId::Installed).c_str() : l10n::Get(l10n::StringId::NotInstalled).c_str();
        fluent::BadgeSpec badge;
        badge.bounds = lay.preview_codec_badge[i]; badge.text = status;
        badge.kind = !detected ? fluent::BadgeKind::Neutral : has ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
        painter_.DrawBadge(badge);
        const auto& get = lay.preview_codec_button[i];
        if (get.right > get.left) {
            fluent::ControlState state{};
            state.hovered = IsHovered(vm, H::SettingsPreviewStore, i);
            painter_.DrawButton({get, l10n::Get(l10n::StringId::Get).c_str(), {}, fluent::ButtonKind::Standard, state});
        }
        divider_below(r);
    }
    painter_.DrawWrappedCaption(pack_text::SystemHint(vm),
        D2D1::Point2F(lay.preview_codec_hint.left, lay.preview_codec_hint.top),
        lay.preview_codec_hint.right - lay.preview_codec_hint.left, theme.text_secondary);
    text(pack_text::PacksSection(), lay.pack_section);
    // The packs live on their own page now: count and size, and the way there.
    {
        const auto& r = lay.pack_summary;
        card(r);
        DrawIconText(r.left + 16*s, r.top + 24*s, 24*s, 24*s, L"\xE7B8", L"", theme.text_secondary, 0.85f);
        const bool compact = r.right - r.left < 560*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        const float right = compact ? r.right - 16*s : lay.pack_manage.left - 12*s;
        text(pack_text::Summary(vm), D2D1::RectF(r.left + 54*s, r.top + 14*s, right, r.top + 38*s));
        text(vm.settings_pack_root, D2D1::RectF(r.left + 54*s, r.top + 38*s, right, r.top + 58*s), true);
        button(lay.pack_manage, pack_text::ManagePacks(), PackAction::ManagePacks);
    }
    return;
    }

    card(lay.pack_native_settings);
    hover_row(lay.pack_native_settings, PackAction::NativeSettings);
    setting(lay.pack_native_settings, nullptr, l10n::Get(l10n::StringId::NativeExtensionSettingsTitle),
        l10n::Get(l10n::StringId::NativeExtensionSettingsIntro), StoreIcon::Gear);
    DrawStoreIcon(dc, StoreIcon::ChevronRight, D2D1::RectF(lay.pack_native_settings.right-38*s,
        lay.pack_native_settings.top+24*s, lay.pack_native_settings.right-18*s,
        lay.pack_native_settings.top+44*s), theme.text_secondary);

    // Summary: count, disk use and location.
    {
        const auto& r = lay.pack_summary;
        card(r);
        DrawStoreIcon(dc, StoreIcon::Box, D2D1::RectF(r.left+16*s,r.top+24*s,r.left+40*s,r.top+48*s), theme.text_secondary);
        const bool compact = r.right - r.left < 560*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        const float right = compact ? r.right - 16*s
            : (lay.pack_update_all.right > lay.pack_update_all.left ? lay.pack_update_all.left : lay.pack_open.left) - 12*s;
        text(pack_text::Summary(vm), D2D1::RectF(r.left + 54*s, r.top + 14*s, right, r.top + 38*s));
        text(vm.settings_pack_root, D2D1::RectF(r.left + 54*s, r.top + 38*s, right, r.top + 58*s), true);
        button(lay.pack_open, pack_text::OpenFolder(), PackAction::OpenFolder);
        if (lay.pack_update_all.right > lay.pack_update_all.left)
            button(lay.pack_update_all, pack_text::UpdateAll(vm.settings_pack_updates), PackAction::UpdateAll,
                   fluent::ButtonKind::Primary);
    }

    // One pack card (geometry from LayoutSettingsPacks' card_layout).
    struct CardSpec {
        D2D1_RECT_F card, badge, enable, primary, notice_box;
        StoreIcon icon;
        std::wstring_view title, desc, meta;
        pack_text::Badge badge_text;
        bool shows_enable, enabled, installed, installing, available;
        float progress;
        std::wstring_view primary_label;
        PackAction enable_action, install_action, remove_action;
        const std::wstring& notice;
        D2D1_RECT_F update;           // 更新到, empty unless a newer release is offered
        std::wstring update_label;
        PackAction update_action;
    };
    auto draw_card = [&](const CardSpec& c) {
        const auto& r = c.card;
        card(r);
        const float text_left = r.left + 54*s, inner_right = r.right - 16*s;
        DrawStoreIcon(dc, c.icon, D2D1::RectF(r.left+16*s,r.top+18*s,r.left+40*s,r.top+42*s), theme.accent);
        const bool stacked = c.badge.top > r.top + 30*s;
        text(c.title, D2D1::RectF(text_left, r.top + 12*s,
            stacked ? inner_right : c.badge.left - 12*s, r.top + 40*s));
        fluent::BadgeSpec spec{};
        spec.bounds = c.badge;
        spec.text = c.badge_text.text;
        spec.kind = c.badge_text.kind;
        painter_.DrawBadge(spec);
        if (c.shows_enable) toggle(c.enable, c.enabled, c.enable_action);
        const float desc_top = stacked ? c.badge.bottom + 12*s : r.top + 40*s;
        painter_.DrawWrappedCaption(c.desc, D2D1::Point2F(text_left, desc_top),
                                    inner_right - text_left, theme.text_secondary);
        const bool updatable = c.update.right > c.update.left;
        const float meta_right = (updatable ? c.update.left : c.primary.left) - 12*s;
        if (c.installing) {
            // Status above a thin bar, both centred on the button's row.
            const float cy = (c.primary.top + c.primary.bottom) * 0.5f;
            text(pack_text::Downloading(c.progress), D2D1::RectF(text_left, cy - 16*s, meta_right, cy + 2*s), true);
            fluent::ProgressSpec bar;
            bar.bounds = D2D1::RectF(text_left, cy + 6*s, (std::min)(meta_right, text_left + 320*s), cy + 10*s);
            bar.value = c.progress;
            painter_.DrawProgressBar(bar);
        } else {
            text(c.meta, D2D1::RectF(text_left, c.primary.top + 6*s, meta_right, c.primary.bottom - 6*s), true);
        }
        const bool installed = c.installed && !c.installing;
        if (c.primary.right > c.primary.left)
            button(c.primary, c.primary_label, installed ? c.remove_action : c.install_action,
                   installed || c.installing || !c.available ? fluent::ButtonKind::Standard : fluent::ButtonKind::Primary,
                   pack_text::PrimaryEnabled(c.available, c.installed, c.installing));
        if (updatable) button(c.update, c.update_label, c.update_action, fluent::ButtonKind::Primary);
        if (c.notice_box.bottom > c.notice_box.top) {
            const auto& n = c.notice_box;
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), n.left, n.top, n.right - n.left, n.bottom - n.top, 6*s);
            MakeBrush(dc, theme.accent, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), n.left, n.top + 6*s, 3*s, n.bottom - n.top - 12*s, 1.5f*s);
            painter_.DrawWrappedCaption(c.notice, D2D1::Point2F(n.left + 12*s, n.top + 6*s),
                                        n.right - n.left - 12*s, theme.text);
        }
    };

    // Media: the FFmpeg pack.
    text(pack_text::Media(), lay.pack_media_section);
    draw_card({lay.pack_card, lay.pack_badge, lay.pack_enable, lay.pack_primary, lay.pack_notice, StoreIcon::Film,
               pack_text::MediaTitle(), pack_text::MediaDesc(), pack_text::MediaMeta(), pack_text::MediaBadge(vm),
               false, vm.settings_pack_ffmpeg_enabled, vm.settings_pack_media_installed,
               vm.settings_pack_installing, vm.settings_pack_media_available, vm.settings_pack_progress, pack_text::Primary(vm),
               PackAction::Enable, PackAction::Install, PackAction::Remove, pack_text::MediaNotice(vm),
               lay.pack_update, pack_text::MediaUpdate(vm), PackAction::Update});

    // Images: the image pack.
    text(pack_text::Images(), lay.pack_images_section);
    draw_card({lay.pack_images_card, lay.pack_images_badge, lay.pack_images_enable, lay.pack_images_primary,
               lay.pack_images_notice, StoreIcon::Image,
               pack_text::ImagesTitle(), pack_text::ImagesDesc(), pack_text::ImagesMeta(), pack_text::ImagesBadge(vm),
               pack_text::ShowsImagesEnable(vm), vm.settings_pack_images_enabled, vm.settings_pack_images_installed,
               vm.settings_pack_images_installing, vm.settings_pack_images_available, vm.settings_pack_images_progress, pack_text::ImagesPrimary(vm),
               PackAction::ImagesEnable, PackAction::ImagesInstall, PackAction::ImagesRemove,
               vm.settings_pack_images_notice, lay.pack_images_update, pack_text::ImagesUpdate(vm), PackAction::ImagesUpdate});

    // RAW camera photos.
    draw_card({lay.pack_raw_card, lay.pack_raw_badge, lay.pack_raw_enable, lay.pack_raw_primary,
               lay.pack_raw_notice, StoreIcon::Camera,
               pack_text::RawTitle(), pack_text::RawDesc(), pack_text::RawMeta(), pack_text::RawBadge(vm),
               pack_text::ShowsRawEnable(vm), vm.settings_pack_raw_enabled, vm.settings_pack_raw_installed,
               vm.settings_pack_raw_installing, vm.settings_pack_raw_available, vm.settings_pack_raw_progress, pack_text::RawPrimary(vm),
               PackAction::RawEnable, PackAction::RawInstall, PackAction::RawRemove,
               vm.settings_pack_raw_notice, lay.pack_raw_update, pack_text::RawUpdate(vm), PackAction::RawUpdate});


    // 搜索: 内容搜索 (index status in the notice box once installed) and OCR.
    text(pack_text::SearchSection(), lay.pack_search_section);
    {
        static const std::wstring kNone;
        const bool ready = pack_text::ContentReady(vm);
        draw_card({lay.pack_content_card, lay.pack_content_badge, lay.pack_content_enable, lay.pack_content_primary,
                   lay.pack_content_notice, StoreIcon::TextSearch,
                   pack_text::ContentTitle(), pack_text::ContentDesc(), pack_text::ContentMeta(), pack_text::ContentBadge(vm),
                   false, false, ready, false, pack_text::ContentAvailable(vm), 0.0f, pack_text::ContentPrimary(vm),
                   PackAction::ContentInstall, PackAction::ContentInstall, PackAction::ContentManage,
                   ready ? vm.settings_content_status : kNone, lay.pack_content_update, std::wstring{}, PackAction::ContentInstall});
        draw_card({lay.pack_ocr_card, lay.pack_ocr_badge, lay.pack_ocr_enable, lay.pack_ocr_primary,
                   lay.pack_ocr_notice, StoreIcon::TextSearch,
                   pack_text::OcrTitle(), pack_text::OcrDesc(), {}, pack_text::OcrBadge(),
                   false, false, false, false, false, 0.0f, pack_text::ComingSoon(),
                   PackAction::ContentInstall, PackAction::ContentInstall, PackAction::ContentInstall,
                   kNone, lay.pack_ocr_update, std::wstring{}, PackAction::ContentInstall});
    }

    // AI: the AI pack; its download shows here (progress bar), not in a toast.
    text(pack_text::AiSection(), lay.pack_ai_section);
    {
        static const std::wstring kNone;
        const PackAction action = pack_text::AiAction(vm);
        draw_card({lay.pack_ai_card, lay.pack_ai_badge, lay.pack_ai_enable, lay.pack_ai_primary,
                   lay.pack_ai_notice, StoreIcon::Sparkle,
                   pack_text::AiTitle(), pack_text::AiDesc(), pack_text::AiMeta(), pack_text::AiBadge(vm),
                   false, false, pack_text::AiUsable(vm), vm.settings_ai_pack_installing,
                   pack_text::AiAvailable(vm), vm.settings_ai_pack_progress, pack_text::AiPrimary(vm),
                   action, action, action, kNone, lay.pack_ai_update, std::wstring{}, action});
    }

    card(lay.pack_source_group);
    const bool source_switch = pack_text::ShowsEnable(vm);
    const float source_right = source_switch ? lay.pack_enable.left - 14*s : lay.pack_source_status.right - 16*s;
    text(pack_text::SourceTitle(), D2D1::RectF(lay.pack_source_status.left + 16*s,
        lay.pack_source_status.top + 10*s, source_right, lay.pack_source_status.top + 34*s));
    painter_.DrawWrappedCaption(pack_text::SourceStatus(vm),
        D2D1::Point2F(lay.pack_source_status.left + 16*s, lay.pack_source_status.top + 35*s),
        source_right - lay.pack_source_status.left - 16*s, theme.text_secondary);
    if (source_switch) toggle(lay.pack_enable, vm.settings_pack_ffmpeg_enabled, PackAction::Enable);
    divider_below(lay.pack_source_status);
    hover_row(lay.pack_custom_row, PackAction::UseCustom);
    setting(lay.pack_custom_row, nullptr, pack_text::CustomTitle(), pack_text::CustomDesc(), StoreIcon::Folder);
    toggle(row_switch(lay.pack_custom_row), vm.settings_pack_use_custom, PackAction::UseCustom);
    divider_below(lay.pack_custom_row);
    if (lay.pack_path_row.bottom > lay.pack_path_row.top) {
        const auto& r = lay.pack_path_row;
        const float right = r.right - 16*s;
        const std::wstring path = vm.settings_pack_custom_path.empty()
            ? std::wstring(pack_text::NoCustom()) : vm.settings_pack_custom_path;
        const bool active = vm.settings_pack_use_custom && vm.settings_pack_ffmpeg == 2;
        if (!pack_text::ShowsDetect(vm)) {
            painter_.DrawText(path, D2D1::RectF(r.left + 54*s, r.top + 14*s, right, r.top + 38*s),
                              compositor_->SmallFormat(), active ? theme.text : theme.text_secondary);
        } else {
            painter_.DrawText(path, D2D1::RectF(r.left + 54*s, r.top + 12*s, right, r.top + 34*s),
                              compositor_->SmallFormat(), active ? theme.text : theme.text_secondary);
            text(pack_text::Found(vm.settings_pack_detected_path),
                 D2D1::RectF(r.left + 54*s, r.top + 34*s, right, r.top + 54*s), true);
        }
        button(lay.pack_detect, pack_text::UseDetected(), PackAction::UseDetected);
        button(lay.pack_browse, pack_text::Browse(), PackAction::Browse);
        divider_below(r);
    }
    text(pack_text::Advanced(), lay.pack_advanced_section);
    card(lay.pack_group);
    if (lay.pack_dl_row.bottom > lay.pack_dl_row.top) {
        // 下载源: title and caption, the drop-down at the right.
        const auto& r = lay.pack_dl_row;
        const float right = lay.pack_dl_choice.left - 14*s;
        DrawStoreIcon(dc, StoreIcon::Download, D2D1::RectF(r.left+16*s,r.top+20*s,r.left+40*s,r.top+44*s), theme.text_secondary);
        text(pack_text::DownloadSourceTitle(), D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(pack_text::DownloadSourceDesc(), D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
        fluent::ButtonSpec control{};
        control.bounds = lay.pack_dl_choice;
        control.text = pack_text::SourceName(vm.settings_pack_download_source);
        control.drop_down = true;
        control.state.hovered = hovered(PackAction::DownloadSource);
        painter_.DrawButton(control);
        divider_below(r);
    }
    hover_row(lay.pack_auto_update_row, PackAction::AutoUpdate);
    setting(lay.pack_auto_update_row, nullptr, pack_text::AutoUpdateTitle(), pack_text::AutoUpdateDesc(), StoreIcon::Refresh);
    toggle(row_switch(lay.pack_auto_update_row), vm.settings_pack_auto_update, PackAction::AutoUpdate);
    divider_below(lay.pack_auto_update_row);
    hover_row(lay.pack_remove_row, PackAction::RemoveOnUninstall);
    setting(lay.pack_remove_row, nullptr, pack_text::RemoveTitle(), pack_text::RemoveDesc(), StoreIcon::Trash);
    toggle(row_switch(lay.pack_remove_row), vm.settings_pack_remove_on_uninstall, PackAction::RemoveOnUninstall);

    if (lay.pack_speed_card.bottom > lay.pack_speed_card.top) {
        // 测速: what it does and its button, then a row per source with a bar
        // (shorter for a slower source) and the result.
        card(lay.pack_speed_card);
        const auto& c = lay.pack_speed_card;
        const float right = lay.pack_speed_button.left - 14*s;
        DrawStoreIcon(dc, StoreIcon::Refresh, D2D1::RectF(c.left+16*s,c.top+20*s,c.left+40*s,c.top+44*s), theme.text_secondary);
        text(pack_text::SpeedTestTitle(), D2D1::RectF(c.left + 54*s, c.top + 10*s, right, c.top + 34*s));
        painter_.DrawWrappedCaption(pack_text::SpeedTestDesc(), D2D1::Point2F(c.left + 54*s, c.top + 35*s),
                                    right - c.left - 54*s, theme.text_secondary);
        button(lay.pack_speed_button, pack_text::SpeedTestButton(vm), PackAction::SpeedTest,
               fluent::ButtonKind::Standard, !pack_text::SpeedTestRunning(vm));
        const auto& list = lay.pack_speed_list;
        painter_.StrokeRoundedRect(D2D1::RectF(list.left + 0.5f, list.top + 0.5f, list.right - 0.5f, list.bottom - 0.5f),
                                   6*s, theme.stroke_card, 1.0f);
        const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
        const D2D1_COLOR_F success = dark ? D2D1::ColorF(0x45D7A4) : D2D1::ColorF(0x107C5A);
        const D2D1_COLOR_F failed = painter_.StatusToneColor(fluent::InfoBarKind::Error);
        for (int i = 0; i < 3; ++i) {
            const auto& r = lay.pack_speed_row[i];
            if (i > 0) {
                MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                FillRect(dc, brStrokeDivider_.get(), r.left + 1, r.top, r.right - r.left - 2, 1);
            }
            const int ms = vm.settings_pack_speed_ms[i];
            const float mid = (r.top + r.bottom) * 0.5f;
            const D2D1_RECT_F status = D2D1::RectF(r.right - 12*s - 120*s, mid - 10*s, r.right - 12*s, mid + 10*s);
            painter_.DrawText(pack_text::SpeedRowStatus(ms), status, compositor_->SmallFormat(),
                              ms >= 0 ? success : ms == -3 ? failed : theme.text_secondary,
                              fluent::HorizontalAlignment::Right);
            const D2D1_RECT_F bar = D2D1::RectF(status.left - 16*s - 90*s, mid - 2*s, status.left - 16*s, mid + 2*s);
            if (bar.left > r.left + 140*s) {
                painter_.FillRoundedRect(bar, 2*s, theme.stroke_divider);
                if (ms >= 0) {
                    // 320 ms about two thirds, 1.5 s under a third, 15 s a sliver.
                    const float share = (std::clamp)(600.0f / (static_cast<float>(ms) + 600.0f), 0.06f, 0.95f);
                    painter_.FillRoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * share,
                                                         bar.bottom), 2*s, success);
                }
            }
            const float name_right = (bar.left > r.left + 140*s ? bar.left : status.left) - 12*s;
            painter_.DrawText(pack_text::SpeedRowName(vm, i), D2D1::RectF(r.left + 12*s, mid - 10*s, name_right, mid + 10*s),
                              compositor_->SmallFormat(), theme.text);
        }
    }

    painter_.DrawWrappedCaption(pack_text::Note(), D2D1::Point2F(lay.pack_note.left + 4*s, lay.pack_note.top),
                                lay.pack_note.right - lay.pack_note.left - 8*s, theme.text_secondary);
}

} // namespace pulse::ui
