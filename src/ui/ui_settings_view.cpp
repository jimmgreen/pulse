// ui_settings_view.cpp — Settings page layout and painting.
#include "../common/windows_compat.h"
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

namespace {
// The bar over the content's right edge; drawing and dragging share it.
fluent::ScrollbarSpec SettingsScrollbar(const SettingsLayout& lay, float scale) {
    fluent::ScrollbarSpec bar;
    bar.viewport = D2D1::RectF(lay.content.right - 10.0f * scale, lay.content.top,
                               lay.content.right - 2.0f * scale, lay.content.bottom);
    bar.viewport_extent = lay.content.bottom - lay.content.top;
    bar.content_extent = lay.content_h;
    bar.expand_progress = 1.0f;
    return bar;
}
} // namespace

void MainRenderer::DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    D2D1_COLOR_F nav_bg = theme.tab_bg;
    if (vm.backdrop_enabled) {
        // Follows 侧边栏与标题栏: as before at 10, solid at 0, gone at 100.
        nav_bg.a = vm.dark ? 0.62f : 0.70f;
        const int v = std::clamp(vm.frame_transparency, 0, 100);
        nav_bg.a = v <= 10 ? nav_bg.a + (1.0f - nav_bg.a) * static_cast<float>(10 - v) / 10.0f
                           : nav_bg.a * static_cast<float>(100 - v) / 90.0f;
    }
    MakeBrush(dc, nav_bg, brFillHover_);
    FillRect(dc, brFillHover_.get(), lay.nav.left, lay.nav.top,
             lay.nav.right - lay.nav.left, lay.nav.bottom - lay.nav.top);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.right - 1.0f, lay.nav.top, 1.0f,
             lay.nav.bottom - lay.nav.top);

    const bool compact_nav = lay.nav.right - lay.nav.left < 100*scale_;
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.left+16*scale_, lay.nav_row[3].top-12*scale_,
        lay.nav.right-lay.nav.left-32*scale_, 1);
    // A low window lists 关于 with the other rows; the version line gives way.
    if (!compact_nav && lay.nav_row[3].bottom + 4*scale_ <= lay.nav.bottom-40*scale_) painter_.DrawText(L"Pulse " + vm.settings_version,
        D2D1::RectF(lay.nav.left+20*scale_,lay.nav.bottom-40*scale_,lay.nav.right-12*scale_,lay.nav.bottom-16*scale_),
        compositor_->SmallFormat(),theme.text_secondary);

    static constexpr pulse::l10n::StringId kNav[] = {
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsSearchIndex,
        pulse::l10n::StringId::SettingsContextMenu,
        pulse::l10n::StringId::SettingsAboutDiagnostics,
        pulse::l10n::StringId::SettingsDuplicates,
        pulse::l10n::StringId::SettingsGeneral,  // 5-8: see nav_label
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsFileList,
        pulse::l10n::StringId::SettingsPageTabs,
        pulse::l10n::StringId::SettingsPageSystem,
        pulse::l10n::StringId::SettingsPageFileOps,
    };
    static_assert(std::size(kNav) == kSettingsNavCount);
    static constexpr const wchar_t* kNavIcon[] = {
        // 右键菜单 (2): the list glyph of its Pulse 右键菜单 card, not the gear.
        L"\xE771", kIconSearch, L"\xE8FD", kIconInfo, kIconCopy, L"\xE890",  // Quick Look
        L"\xE71D",  // 系统右键菜单
        L"\xE765",  // 快捷键 (keyboard)
        L"\xE7B8",  // 增强包 (package)
        L"\xED25",  // 文件列表
        L"\xE737",  // 标签页与窗口
        L"\xE7E8",  // 启动与集成
        L"\xE8DE",  // 文件操作
    };
    static_assert(std::size(kNavIcon) == kSettingsNavCount);
    // Page 5 (快速预览) has no string-table entry; see pack_text. Page 2 is
    // the single 右键菜单 entry; page 6 shares its row (layer cards on both pages).
    auto nav_label = [](int page) -> std::wstring {
        if (page == 2 || page == 6) return std::wstring(sysctx_text::PageTitle());
        if (page == 7) return std::wstring(shortcuts_text::Title());
        if (page == 8) return std::wstring(pack_text::PacksSection());
        if (page == 0) return pulse::l10n::Get(pulse::l10n::StringId::SettingsAppearance);
        return page == 5 ? std::wstring(pack_text::Title()) : pulse::l10n::Get(kNav[page]);
    };
    {
        // Group headings when the sidebar is tall enough, thin separators otherwise.
        static constexpr pulse::l10n::StringId kGroups[] = {
            pulse::l10n::StringId::SettingsNavPersonal, pulse::l10n::StringId::SettingsNavBrowse,
            pulse::l10n::StringId::SettingsNavSystem, pulse::l10n::StringId::SettingsNavFeatures,
            pulse::l10n::StringId::SettingsNavTools,
        };
        static_assert(std::size(kGroups) == kSettingsNavGroups);
        for (int g = 0; g < kSettingsNavGroups; ++g) {
            const auto& head = lay.nav_group_label[g];
            if (head.bottom > head.top)
                painter_.DrawText(pulse::l10n::Get(kGroups[g]), head, compositor_->SmallFormat(), theme.text_secondary);
            const auto& sep = lay.nav_separator[g];
            if (sep.right > sep.left) FillRect(dc, brStrokeDivider_.get(), sep.left, sep.top, sep.right - sep.left, 1);
        }
    }
    // Sub-pages keep their parent selected: 系统右键菜单 (6) under 右键菜单,
    // AI 与 MCP (kSettingsPageAi) under 增强包.
    const int nav_page = vm.settings_page == 6 ? 2 : vm.settings_page == kSettingsPageAi ? 8 : vm.settings_page;
    // Hover plate and selection pill glide between rows (ui_motion.h); the
    // accent bar travels with the pill.
    {
        const uint64_t now = motion_now_;
        int hovered_row = -1;
        for (int i = 0; i < kSettingsNavCount; ++i)
            if (i != nav_page && IsHovered(vm, HitTestResult::SettingsNav, i)) hovered_row = i;
        if (hovered_row >= 0) {
            const D2D1_RECT_F rc = settings_nav_hover_.Update(
                1, hovered_row, lay.nav_row[hovered_row], motion_frame_, now, 120);
            MakeBrush(dc, theme.fill_hover, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * scale_);
        }
        if (nav_page >= 0 && nav_page < kSettingsNavCount) {
            const D2D1_RECT_F rc = settings_nav_pill_.Update(
                1, nav_page, lay.nav_row[nav_page], motion_frame_, now, 200);
            MakeBrush(dc, theme.fill_selected, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * scale_);
            MakeBrush(dc, theme.accent, brFillSelected_);
            const float bar_h = (std::min)(20.0f * scale_, (rc.bottom - rc.top) * 0.45f);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, (rc.top + rc.bottom - bar_h) * 0.5f, 3*scale_, bar_h, 1.5f*scale_);
        }
    }
    for (int i = 0; i < kSettingsNavCount; ++i) {
        if (i == 6) continue;  // no row: reached through the 右键菜单 layer cards
        const bool active = nav_page == i;
        const auto& rc = lay.nav_row[i];
        DrawIconText(rc.left + (compact_nav ? 13.0f : 12.0f) * scale_, rc.top, 22.0f * scale_, rc.bottom - rc.top,
                     kNavIcon[i], L"*", active ? theme.accent : theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        if (!compact_nav) DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), nav_label(i),
                     rc.left + 42.0f * scale_, rc.top, rc.right - rc.left - 48.0f * scale_,
                     rc.bottom - rc.top);
        // 快捷键: a red count of the keys that may not work (taken / IME);
        // 增强包: an accent count of the packs with a newer release.
        if ((i == 7 && vm.settings_shortcut_issues > 0) || (i == 8 && vm.settings_pack_updates > 0)) {
            const std::wstring count = std::to_wstring(i == 7 ? vm.settings_shortcut_issues : vm.settings_pack_updates);
            const float bw = (std::max)(18.0f * scale_, painter_.MeasureBadgeWidth(count) - 10.0f * scale_);
            const float mid = (rc.top + rc.bottom) * 0.5f;
            const D2D1_RECT_F badge = compact_nav
                ? D2D1::RectF(rc.left + 26.0f * scale_, rc.top + 4.0f * scale_, rc.left + 26.0f * scale_ + bw, rc.top + 20.0f * scale_)
                : D2D1::RectF(rc.right - 10.0f * scale_ - bw, mid - 9.0f * scale_, rc.right - 10.0f * scale_, mid + 9.0f * scale_);
            painter_.FillRoundedRect(badge, (badge.bottom - badge.top) * 0.5f, i == 8 ? theme.accent
                                     : vm.dark ? D2D1::ColorF(0xE6716B) : D2D1::ColorF(0xC2413A));
            painter_.DrawText(count, badge, compositor_->SmallFormat(), D2D1::ColorF(0xFFFFFF),
                              fluent::HorizontalAlignment::Center);
        }
    }

    dc->PushAxisAlignedClip(lay.content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float pad = 20.0f * scale_;
    const float origin = lay.content_origin;
    const float switch_w = 42.0f * scale_;
    const float switch_h = 32.0f * scale_;

    const auto page_title_id = vm.settings_page == 0 ? pulse::l10n::StringId::SettingsAppearance
        : vm.settings_page >= 9 ? kNav[(std::min)(vm.settings_page, kSettingsNavCount - 1)]
        : vm.settings_page == 1 ? pulse::l10n::StringId::SettingsSearchIndex
        : vm.settings_page == 2 ? pulse::l10n::StringId::SettingsContextMenu
        : vm.settings_page == 4 ? pulse::l10n::StringId::SettingsDuplicates
                                : pulse::l10n::StringId::SettingsAboutDiagnostics;
    MakeBrush(dc, theme.text, brText_);
    const std::wstring title = vm.settings_page == 2 || vm.settings_page == 6 ? std::wstring(sysctx_text::PageTitle())
        : vm.settings_page == 5 ? std::wstring(pack_text::Title())
        : vm.settings_page == 7 ? std::wstring(shortcuts_text::Title())
        : vm.settings_page == 8 ? std::wstring(pack_text::PacksSection())
        : vm.settings_page == kSettingsPageAi ? std::wstring(ai_text::Title()) : l10n::Get(page_title_id);
    ComPtr<IDWriteTextLayout> title_layout;
    compositor_->DwriteFactory()->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()),
        compositor_->HeaderFormat(), lay.content.right-lay.content.left-pad*2, 40*scale_, &title_layout);
    if (title_layout.get()) {
        title_layout->SetFontSize(26*scale_, {0,static_cast<UINT32>(title.size())});
        title_layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0,static_cast<UINT32>(title.size())});
        compositor_->DrawTextLayout(D2D1::Point2F(lay.content.left+pad,origin+pad),title_layout.get(),brText_.get());
    }
    if (vm.settings_page == 5 || vm.settings_page == 8 || vm.settings_page == kSettingsPageAi)
        painter_.DrawWrappedCaption(vm.settings_page == kSettingsPageAi ? ai_text::Intro()
                                    : vm.settings_page == 8 ? pack_text::PacksIntro() : pack_text::Intro(),
            D2D1::Point2F(lay.content.left+pad, origin+60*scale_),
            lay.content.right-lay.content.left-2*pad, theme.text_secondary);
    if (IsGeneralSettingsPage(vm.settings_page) || vm.settings_page == 1)
        painter_.DrawText(l10n::Get(vm.settings_page == 0 ? l10n::StringId::SettingsAppearanceIntro
            : vm.settings_page == 9 ? l10n::StringId::SettingsFileListIntro
            : vm.settings_page == 10 ? l10n::StringId::SettingsTabsIntro
            : vm.settings_page == 11 ? l10n::StringId::SettingsSystemIntro
            : vm.settings_page == 12 ? l10n::StringId::SettingsFileOpsIntro : l10n::StringId::SettingsSearchIntro),
            D2D1::RectF(lay.content.left+pad,origin+60*scale_,lay.content.right-pad,origin+84*scale_),
            compositor_->SmallFormat(),theme.text_secondary);


    if (IsGeneralSettingsPage(vm.settings_page)) {
        DrawSettingsCore(vm, rect, theme);
    } else if (vm.settings_page == 1) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };
        DrawSettingsCore(vm, rect, theme);
        if (vm.settings_expanded & 2u) {
        fluent::InfoBarSpec info;
        info.bounds = lay.index_info;
        info.title = pulse::l10n::Get((vm.settings_index_service || vm.settings_index_installed)
            ? pulse::l10n::StringId::SettingsFullIndex
            : pulse::l10n::StringId::SettingsUserIndex);
        info.message = vm.settings_index_service
            ? pulse::l10n::Get(pulse::l10n::StringId::SettingsFullIndexDesc)
            : pulse::l10n::Get(vm.settings_index_installed
                ? pulse::l10n::StringId::SettingsServiceWaiting
                : pulse::l10n::StringId::SettingsUserIndexDesc);
        info.kind = vm.settings_index_error.empty() ? fluent::InfoBarKind::Informational
                                                     : fluent::InfoBarKind::Error;
        if (!vm.settings_index_error.empty()) info.message = vm.settings_index_error;
        else if (vm.settings_index_migrating)
            info.message = pulse::l10n::Get(pulse::l10n::StringId::IndexMigratingShort);
        info.show_close = false;
        painter_.DrawInfoBar(info);


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
            st.enabled = (i == 2 || vm.settings_index_service) && (!vm.settings_index_migrating || i == 1);
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
                                     volume.state, IndexVolumeBadgeKind(volume.raw_state.empty() ? volume.state : volume.raw_state) });
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
        if (vm.settings_index_system_known &&
            lay.index_system_card.bottom > lay.index_system_card.top) {
            draw_card(lay.index_system_card);
            const auto& row = lay.index_system_row;
            const bool service = vm.settings_index_service;
            const bool on = vm.settings_index_exclude_system;
            if (service && IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 1)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top + 4.0f * scale_,
                                row.right - row.left - 8.0f * scale_, 52.0f * scale_, 6.0f * scale_);
            }
            {
                // Outlined shield, matching the 16-DIP row icons.
                Microsoft::WRL::ComPtr<ID2D1Factory> factory;
                dc->GetFactory(&factory);
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> shield;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (factory && SUCCEEDED(factory->CreatePathGeometry(&shield)) &&
                    SUCCEEDED(shield->Open(&sink))) {
                    const float u = scale_;
                    const float ox = row.left + 16.0f * scale_, oy = row.top + 15.0f * scale_;
                    const auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * u, oy + y * u); };
                    sink->BeginFigure(P(8.0f, 1.8f), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(P(13.0f, 3.6f));
                    sink->AddLine(P(13.0f, 7.6f));
                    sink->AddBezier(D2D1::BezierSegment(P(13.0f, 10.7f), P(10.9f, 13.0f), P(8.0f, 14.2f)));
                    sink->AddBezier(D2D1::BezierSegment(P(5.1f, 13.0f), P(3.0f, 10.7f), P(3.0f, 7.6f)));
                    sink->AddLine(P(3.0f, 3.6f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->BeginFigure(P(5.8f, 8.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(P(7.4f, 9.6f));
                    sink->AddLine(P(10.4f, 6.6f));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        MakeBrush(dc, on ? theme.accent : theme.text_secondary, brAccent_);
                        dc->DrawGeometry(shield.Get(), brAccent_.get(), 1.2f * scale_);
                    }
                }
            }
            const float text_left = row.left + 48.0f * scale_;
            const float text_w = (std::max)(40.0f * scale_, row.right - text_left - 32.0f * scale_ - switch_w);
            MakeBrush(dc, service ? theme.text : theme.text_disabled, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsSystemFolders),
                         text_left, row.top + 10.0f * scale_, text_w, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsSystemFoldersDesc),
                         text_left, row.top + 33.0f * scale_, text_w, 18.0f * scale_);
            fluent::ControlState sw{};
            sw.checked = on;
            sw.enabled = service;
            sw.hovered = service && IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 1);
            painter_.DrawSwitch(D2D1::RectF(row.right - 16.0f * scale_ - switch_w,
                                            row.top + 30.0f * scale_ - switch_h * 0.5f,
                                            row.right - 16.0f * scale_,
                                            row.top + 30.0f * scale_ + switch_h * 0.5f),
                                L"", sw);
            for (int g = 0; g < kSystemGroupRows; ++g) {
                const auto& chip = lay.index_system_chip[g];
                if (chip.right <= chip.left) continue;
                painter_.DrawBadge({ chip, pulse::l10n::Get(SystemGroupLabel(g)),
                                     on ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral });
            }
            {
                const auto& more = lay.index_system_more;
                if (IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 2)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), more.left, more.top,
                                    more.right - more.left, more.bottom - more.top, 4.0f * scale_);
                }
                MakeBrush(dc, theme.accent, brAccent_);
                DrawTextRect(dc, compositor_->SmallFormat(), brAccent_.get(),
                             pulse::l10n::Get(pulse::l10n::StringId::SettingsSystemCustomize),
                             more.left + 6.0f * scale_, more.top + 5.0f * scale_,
                             more.right - more.left - 20.0f * scale_, 18.0f * scale_);
                const float cx = more.right - 10.0f * scale_;
                const float cy = (more.top + more.bottom) * 0.5f;
                const float d = vm.settings_index_system_expanded ? -2.5f * scale_ : 2.5f * scale_;
                dc->DrawLine(D2D1::Point2F(cx - 3.5f * scale_, cy - d * 0.6f), D2D1::Point2F(cx, cy + d * 0.8f),
                             brAccent_.get(), 1.3f * scale_);
                dc->DrawLine(D2D1::Point2F(cx, cy + d * 0.8f), D2D1::Point2F(cx + 3.5f * scale_, cy - d * 0.6f),
                             brAccent_.get(), 1.3f * scale_);
            }
            for (int g = 0; g < kSystemGroupRows && vm.settings_index_system_expanded; ++g) {
                const auto& group = lay.index_system_group[g];
                if (group.bottom <= group.top) continue;
                const bool locked = g >= kSystemGroupConfigurable;
                const bool group_enabled = service && on && !locked;
                MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                FillRect(dc, brStrokeDivider_.get(), group.left, group.top, group.right - group.left, 1.0f);
                if (group_enabled && IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 10 + g)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), group.left, group.top + 2.0f * scale_,
                                    group.right - group.left, group.bottom - group.top - 4.0f * scale_,
                                    6.0f * scale_);
                }
                fluent::ControlState check{};
                check.checked = on && SystemGroupEnabled(vm, g);
                check.enabled = group_enabled;
                check.hovered = group_enabled && IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 10 + g);
                painter_.DrawCheckBox(D2D1::RectF(group.left + 4.0f * scale_, group.top,
                                                  group.left + 36.0f * scale_, group.bottom), L"", check);
                const float label_left = group.left + 44.0f * scale_;
                const auto label = pulse::l10n::Get(SystemGroupLabel(g));
                MakeBrush(dc, locked || !on ? theme.text_secondary : theme.text, brText_);
                DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), label,
                             label_left, group.top + 6.0f * scale_,
                             group.right - label_left - 8.0f * scale_, 22.0f * scale_);
                if (locked) {
                    const auto always = pulse::l10n::Get(pulse::l10n::StringId::SettingsSystemAlways);
                    const float bw = painter_.MeasureBadgeWidth(always);
                    const float bx = group.right - 8.0f * scale_ - bw;
                    painter_.DrawBadge({ D2D1::RectF(bx, group.top + 15.0f * scale_, bx + bw,
                                                     group.top + 37.0f * scale_),
                                         always, fluent::BadgeKind::Neutral });
                }
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), SystemGroupDetail(g),
                             label_left, group.top + 29.0f * scale_,
                             group.right - label_left - 8.0f * scale_, 18.0f * scale_);
            }
            if (!lay.index_exclude_rows.empty()) {
                MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                FillRect(dc, brStrokeDivider_.get(), lay.index_system_card.left + 12.0f * scale_,
                         lay.index_exclude_rows.front().top,
                         lay.index_system_card.right - lay.index_system_card.left - 24.0f * scale_, 1.0f);
            }
        }
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
        }
    } else if (vm.settings_page == 2) {
        DrawSettingsContext(vm,rect,theme);
    } else if (vm.settings_page == 3) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };

        draw_card(lay.about_card);
        {
            const D2D1_RECT_F& card = lay.about_card;
            const float x0 = card.left + 16.0f * scale_;
            const float inner_w = card.right - card.left - 32.0f * scale_;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutPulse),
                         x0, card.top + 12.0f * scale_, inner_w, 24.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutTagline),
                         x0, card.top + 38.0f * scale_, inner_w, 20.0f * scale_);
            const auto& rows = vm.settings_about_rows;
            const bool two_columns = AboutTwoColumns(card.right - card.left, scale_);
            const size_t lines = AboutRowLines(vm, card.right - card.left, scale_);
            const float col_w = two_columns ? inner_w * 0.5f : inner_w;
            const float label_w = (std::min)(96.0f * scale_, col_w * 0.4f);
            const float value_w = (std::max)(0.0f, col_w - label_w - 12.0f * scale_);
            IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
            const auto measure = [&](const std::wstring& t) {
                return MeasureTextWidth(compositor_->DwriteFactory(), small_fmt, t);
            };
            MakeBrush(dc, theme.text, brText_);
            for (size_t i = 0; lines > 0 && i < rows.size(); ++i) {
                const float col = two_columns ? static_cast<float>(i / lines) : 0.0f;
                const float row = static_cast<float>(two_columns ? i % lines : i);
                const float rx = x0 + col * col_w;
                const float ry = card.top + 66.0f * scale_ + row * kAboutRowDip * scale_;
                DrawTextRect(dc, small_fmt, brTextSecondary_.get(), rows[i].first,
                             rx, ry, label_w, 22.0f * scale_);
                DrawTextRect(dc, small_fmt, brText_.get(), FitEndEllipsis(rows[i].second, value_w, measure),
                             rx + label_w, ry, value_w, 22.0f * scale_);
            }
            static constexpr pulse::l10n::StringId kAboutActions[] = {
                pulse::l10n::StringId::AboutCopyInfo,
                pulse::l10n::StringId::AboutHomepage,
            };
            for (int i = 0; i < 2; ++i) {
                fluent::ControlState state{};
                state.enabled = true;
                state.hovered = IsHovered(vm, HitTestResult::SettingsAboutAction, i);
                // "Copy info" confirms in place (check + "Copied") instead of a toast.
                const bool copied = copy_feedback_.Elapsed(
                    static_cast<int>(HitTestResult::SettingsAboutAction), i, GetTickCount64()) >= 0;
                painter_.DrawButton({lay.about_action[i],
                                     pulse::l10n::Get(copied ? pulse::l10n::StringId::CopiedShort
                                                             : kAboutActions[i]),
                                     copied ? std::wstring_view(L"\xE73E") : std::wstring_view{},
                                     fluent::ButtonKind::Standard, state});
            }
        }

        draw_card(lay.apps_card);
        {
            const D2D1_RECT_F& card = lay.apps_card;
            const float x0 = card.left + 16.0f * scale_;
            const float inner_w = card.right - card.left - 32.0f * scale_;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutMoreApps),
                         x0, card.top + 12.0f * scale_, inner_w, 24.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutMoreAppsDesc),
                         x0, card.top + 38.0f * scale_, inner_w, 20.0f * scale_);
            struct RecommendedApp {
                const wchar_t* glyph;
                const wchar_t* name;
                pulse::l10n::StringId description;
            };
            static constexpr RecommendedApp kApps[] = {
                {L"\uE8A5", L"LumenPDF", pulse::l10n::StringId::AboutLumenPdfDesc},
                {L"\uE722", L"LumaShot", pulse::l10n::StringId::AboutLumaShotDesc},
            };
            IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
            const auto measure = [&](const std::wstring& t) {
                return MeasureTextWidth(compositor_->DwriteFactory(), small_fmt, t);
            };
            for (int i = 0; i < 2; ++i) {
                const D2D1_RECT_F& row = lay.apps_row[i];
                if (!VisibleInContent(row, lay.content)) continue;
                if (IsHovered(vm, HitTestResult::SettingsAboutAction, 3 + i)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                    row.right - row.left, row.bottom - row.top, 4.0f * scale_);
                }
                painter_.DrawGlyph(kApps[i].glyph,
                                   D2D1::RectF(row.left + 8.0f * scale_, row.top,
                                               row.left + 40.0f * scale_, row.bottom),
                                   theme.accent);
                const float tx = row.left + 48.0f * scale_;
                const float tw = (std::max)(0.0f, row.right - 44.0f * scale_ - tx);
                DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), kApps[i].name,
                             tx, row.top + 6.0f * scale_, tw, 22.0f * scale_);
                DrawTextRect(dc, small_fmt, brTextSecondary_.get(),
                             FitEndEllipsis(pulse::l10n::Get(kApps[i].description), tw, measure),
                             tx, row.top + 28.0f * scale_, tw, 20.0f * scale_);
                painter_.DrawGlyph(L"\uE8A7",
                                   D2D1::RectF(row.right - 36.0f * scale_, row.top,
                                               row.right - 12.0f * scale_, row.bottom),
                                   theme.text_secondary);
            }
        }

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
        {
            const D2D1_RECT_F& row = lay.diagnostics_perf;
            if (IsHovered(vm, HitTestResult::SettingsToggle, 4)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsShowPerformance),
                         row.left + 16.0f * scale_, row.top + 8.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsShowPerformanceDesc),
                         row.left + 16.0f * scale_, row.top + 30.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 18.0f * scale_);
            fluent::ControlState st{};
            st.checked = vm.settings_show_performance;
            st.hovered = IsHovered(vm, HitTestResult::SettingsToggle, 4);
            painter_.DrawSwitch(D2D1::RectF(row.right - 16.0f * scale_ - switch_w,
                                            row.top + (56.0f * scale_ - switch_h) * 0.5f,
                                            row.right - 16.0f * scale_,
                                            row.top + (56.0f * scale_ + switch_h) * 0.5f),
                                L"", st);
        }
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

        if (lay.transfer_card.bottom > lay.transfer_card.top) {
            // 备份与迁移: export / import settings, restore the pre-import backup.
            const D2D1_RECT_F& card = lay.transfer_card;
            const float text_w = card.right - card.left - 32.0f * scale_;
            draw_card(card);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsTransfer),
                         card.left + 16.0f * scale_, card.top + 12.0f * scale_, text_w, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsTransferDesc),
                         card.left + 16.0f * scale_, card.top + 40.0f * scale_, text_w, 40.0f * scale_);
            std::wstring status;
            if (vm.settings_transfer_busy) {
                status = pulse::l10n::Get(pulse::l10n::StringId::SettingsTransferBusy);
            } else if (vm.settings_transfer_pending) {
                status = pulse::l10n::Get(pulse::l10n::StringId::SettingsTransferPending);
            } else if (vm.settings_transfer_backup) {
                status = pulse::l10n::Get(pulse::l10n::StringId::SettingsTransferBackupFrom);
                const size_t at = status.find(L"{time}");
                if (at != std::wstring::npos) status.replace(at, 6, vm.settings_transfer_backup_time);
            }
            if (!status.empty()) {
                MakeBrush(dc, vm.settings_transfer_pending ? theme.text : theme.text_secondary,
                          brTextSecondary_);
                DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), status,
                             card.left + 16.0f * scale_, card.top + 80.0f * scale_, text_w, 20.0f * scale_);
            }
            static constexpr pulse::l10n::StringId kTransferActions[] = {
                pulse::l10n::StringId::SettingsExportButton,
                pulse::l10n::StringId::SettingsImportButton,
                pulse::l10n::StringId::SettingsRestoreBackup,
            };
            for (int i = 0; i < 3; ++i) {
                const D2D1_RECT_F& button = lay.transfer_action[i];
                if (button.right <= button.left) continue;
                fluent::ControlState state{};
                state.enabled = !vm.settings_transfer_busy;
                state.hovered = state.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDiagnosticsAction, 3 + i);
                const bool restart = i == 2 && vm.settings_transfer_pending;
                painter_.DrawButton({button,
                    pulse::l10n::Get(restart ? pulse::l10n::StringId::SettingsRestartNow
                                             : kTransferActions[i]), {},
                    restart ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard, state});
            }
        }

        if (lay.tips_card.bottom > lay.tips_card.top) {
            // 新手提示: the hint switch and, beneath it, 重新显示.
            draw_card(lay.tips_card);
            const D2D1_RECT_F& card = lay.tips_card;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsSecTips),
                         card.left + 16.0f * scale_, card.top + 12.0f * scale_,
                         card.right - card.left - 32.0f * scale_, 22.0f * scale_);
            auto tip_row = [&](const D2D1_RECT_F& row, const wchar_t* icon, const std::wstring& title,
                               const std::wstring& desc, float text_right) {
                DrawIconText(row.left + 16.0f * scale_, row.top, 24.0f * scale_, row.bottom - row.top,
                             icon, L"", theme.text_secondary, 0.85f);
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), title,
                             row.left + 52.0f * scale_, row.top + 11.0f * scale_, text_right - row.left - 52.0f * scale_, 22.0f * scale_);
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), desc,
                             row.left + 52.0f * scale_, row.top + 33.0f * scale_, text_right - row.left - 52.0f * scale_, 20.0f * scale_);
            };
            const D2D1_RECT_F& hints = lay.hints_row;
            FillRect(dc, brStrokeDivider_.get(), card.left + 16.0f * scale_, hints.top, card.right - card.left - 32.0f * scale_, 1);
            if (IsHovered(vm, HitTestResult::SettingsToggle, 24)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), hints.left + 4.0f * scale_, hints.top + 2.0f * scale_,
                                hints.right - hints.left - 8.0f * scale_, hints.bottom - hints.top - 4.0f * scale_, 4.0f * scale_);
            }
            const float sw_right = hints.right - 16.0f * scale_;
            tip_row(hints, L"\xE82F", pulse::l10n::Get(pulse::l10n::StringId::SettingsHints),
                    pulse::l10n::Get(pulse::l10n::StringId::SettingsHintsDesc), sw_right - switch_w - 12.0f * scale_);
            fluent::ControlState st{};
            st.checked = vm.settings_show_hints;
            st.hovered = IsHovered(vm, HitTestResult::SettingsToggle, 24);
            const float mid = (hints.top + hints.bottom) * 0.5f;
            painter_.DrawSwitch(D2D1::RectF(sw_right - switch_w, mid - switch_h * 0.5f, sw_right, mid + switch_h * 0.5f), L"", st);
            const D2D1_RECT_F& reset = lay.hints_reset_row;
            {
                // The dependent row's band, clipped to the card's rounded corners.
                ComPtr<ID2D1Factory> factory; dc->GetFactory(&factory);
                ComPtr<ID2D1RoundedRectangleGeometry> clip;
                if (factory.get()) factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(card.left + 1.0f, card.top + 1.0f, card.right - 1.0f, card.bottom - 1.0f),
                                      7.0f * scale_, 7.0f * scale_), &clip);
                if (clip.get()) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
                MakeBrush(dc, WithAlpha(theme.text, vm.dark ? 0.035f : 0.024f), brFillHover_);
                dc->FillRectangle(D2D1::RectF(card.left, reset.top, card.right, card.bottom), brFillHover_.get());
                if (clip.get()) dc->PopLayer();
            }
            MakeBrush(dc, WithAlpha(theme.text_secondary, 0.32f), brTextSecondary_);
            FillRoundedRect(dc, brTextSecondary_.get(), card.left + 29.0f * scale_, reset.top + 12.0f * scale_,
                            1.5f * scale_, reset.bottom - reset.top - 24.0f * scale_, 0.75f * scale_);
            tip_row(reset, L"\xE72C", pulse::l10n::Get(pulse::l10n::StringId::SettingsHintsReset),
                    pulse::l10n::Get(vm.settings_tips_seen ? pulse::l10n::StringId::SettingsHintsResetDesc
                                                           : pulse::l10n::StringId::HintsResetDone),
                    lay.hints_reset_button.left - 8.0f * scale_);
            fluent::ControlState bs{};
            bs.enabled = vm.settings_tips_seen;
            bs.hovered = bs.enabled && IsHovered(vm, HitTestResult::SettingsToggle, 25);
            painter_.DrawButton({lay.hints_reset_button, pulse::l10n::Get(pulse::l10n::StringId::HintsResetButton), {},
                fluent::ButtonKind::Standard, bs});
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
                     vm.settings_version,
                     lay.update_card.left + 16.0f * scale_, lay.update_card.top + 40.0f * scale_,
                     lay.update_card.right - lay.update_card.left - 32.0f * scale_,
                     22.0f * scale_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     vm.settings_update_status,
                     lay.update_card.left + 16.0f * scale_, lay.update_card.top + 66.0f * scale_,
                     lay.update_card.right - lay.update_card.left - 32.0f * scale_,
                     38.0f * scale_);
        fluent::ControlState check{};
        check.enabled = vm.settings_update_enabled && !vm.settings_update_checking &&
            !vm.settings_update_downloading && !vm.settings_update_installing;
        check.hovered = check.enabled && IsHovered(vm, HitTestResult::SettingsUpdateAction, 0);
        painter_.DrawButton({lay.update_action[0],
            pulse::l10n::Get(pulse::l10n::StringId::CheckForUpdates), {},
            fluent::ButtonKind::Standard, check});
        if (vm.settings_update_available) {
            fluent::ControlState download{};
            download.enabled = !vm.settings_update_installing;
            download.hovered = IsHovered(vm, HitTestResult::SettingsUpdateAction, 1);
            painter_.DrawButton({lay.update_action[1],
                pulse::l10n::Get(vm.settings_update_downloading ? pulse::l10n::StringId::Cancel :
                    pulse::l10n::StringId::DownloadUpdate), {},
                fluent::ButtonKind::Primary, download});
        }
        if (lay.update_auto_row.bottom > lay.update_auto_row.top) {
            // Same row as the diagnostics card's performance switch.
            const D2D1_RECT_F& row = lay.update_auto_row;
            const bool hovered = IsHovered(vm, HitTestResult::SettingsToggle, 31);
            if (hovered) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsAutoUpdate),
                         row.left + 16.0f * scale_, row.top + 8.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::SettingsAutoUpdateDesc),
                         row.left + 16.0f * scale_, row.top + 30.0f * scale_,
                         row.right - row.left - 80.0f * scale_, 18.0f * scale_);
            fluent::ControlState st{};
            st.checked = vm.settings_update_auto;
            st.hovered = hovered;
            painter_.DrawSwitch(D2D1::RectF(row.right - 16.0f * scale_ - switch_w,
                                            row.top + (56.0f * scale_ - switch_h) * 0.5f,
                                            row.right - 16.0f * scale_,
                                            row.top + (56.0f * scale_ + switch_h) * 0.5f),
                                L"", st);
        }
        if (!vm.settings_index_error.empty()) {
            MakeBrush(dc, theme.danger, brDanger_);
            DrawTextRect(dc, compositor_->SmallFormat(), brDanger_.get(),
                         vm.settings_index_error,
                         lay.content.left + pad, lay.update_card.bottom + 8.0f * scale_,
                         lay.content.right - lay.content.left - pad * 2, 36.0f * scale_);
        }

        draw_card(lay.release_card);
        {
            const D2D1_RECT_F& card = lay.release_card;
            const float x0 = card.left + 16.0f * scale_;
            const float head_w = (std::max)(0.0f, lay.release_all.left - 12.0f * scale_ - x0);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::ReleaseNotes),
                         x0, card.top + 12.0f * scale_, head_w, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::ReleaseNotesDesc),
                         x0, card.top + 38.0f * scale_, head_w, 22.0f * scale_);
            fluent::ControlState all{};
            all.enabled = true;
            all.hovered = IsHovered(vm, HitTestResult::SettingsAboutAction, 2);
            painter_.DrawButton({lay.release_all, pulse::l10n::Get(pulse::l10n::StringId::ReleaseAll),
                                 {}, fluent::ButtonKind::Standard, all});
            const auto* notes = vm.settings_release_notes;
            IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
            const auto measure_small = [&](const std::wstring& t) {
                return MeasureTextWidth(compositor_->DwriteFactory(), small_fmt, t);
            };
            for (size_t i = 0; notes && i < notes->size() && i < lay.release_rows.size(); ++i) {
                const auto& note = (*notes)[i];
                const D2D1_RECT_F& row = lay.release_rows[i];
                const bool expanded = static_cast<int>(i) == vm.settings_release_expanded;
                if (VisibleInContent(row, lay.content)) {
                    if (IsHovered(vm, HitTestResult::SettingsReleaseNote, static_cast<int>(i))) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                        row.right - row.left, row.bottom - row.top, 4.0f * scale_);
                    }
                    painter_.DrawGlyph(expanded ? L"\uE70D" : L"\uE76C",
                                       D2D1::RectF(row.left + 6.0f * scale_, row.top,
                                                   row.left + 26.0f * scale_, row.bottom),
                                       theme.text_secondary);
                    const float vx = row.left + kReleaseTextInsetDip * scale_;
                    const std::wstring version_title = L"Pulse " + note.version;
                    const float title_w = MeasureTextWidth(compositor_->DwriteFactory(),
                                                           compositor_->TextFormat(), version_title);
                    MakeBrush(dc, theme.text, brText_);
                    DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), version_title,
                                 vx, row.top + 7.0f * scale_, title_w + 4.0f * scale_, 22.0f * scale_);
                    float after = vx + title_w + 12.0f * scale_;
                    if (note.current) {
                        const std::wstring badge = pulse::l10n::Get(pulse::l10n::StringId::ReleaseCurrent);
                        const float bw = painter_.MeasureBadgeWidth(badge);
                        fluent::BadgeSpec spec{};
                        spec.bounds = D2D1::RectF(after, row.top + 8.0f * scale_, after + bw,
                                                  row.top + 28.0f * scale_);
                        spec.text = badge;
                        spec.kind = fluent::BadgeKind::Accent;
                        painter_.DrawBadge(spec);
                        after += bw + 12.0f * scale_;
                    }
                    const float preview_w = row.right - 12.0f * scale_ - after;
                    if (!expanded && !note.lines.empty() && preview_w > 60.0f * scale_) {
                        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                        DrawTextRect(dc, small_fmt, brTextSecondary_.get(),
                                     FitEndEllipsis(note.lines.front(), preview_w, measure_small),
                                     after, row.top + 8.0f * scale_, preview_w, 20.0f * scale_);
                    }
                }
                if (!expanded || lay.release_body.bottom <= lay.release_body.top) continue;
                const D2D1_RECT_F& body = lay.release_body;
                const float text_x = body.left + kReleaseTextInsetDip * scale_;
                const float text_w = ReleaseTextWidth(body.right - body.left, scale_);
                float ly = body.top + 4.0f * scale_;
                MakeBrush(dc, theme.accent, brAccent_);
                const ReleaseTagSummary tags = SummarizeReleaseTags(note);
                const float tag_col = ReleaseTagColumn(&painter_, note, scale_);
                const float badge_h = kReleaseBadgeHeightDip * scale_;
                const auto draw_badge = [&](ReleaseTag tag, const std::wstring& label, float x, float y) {
                    const float bw = painter_.MeasureBadgeWidth(label);
                    fluent::BadgeSpec spec{};
                    spec.bounds = D2D1::RectF(x, y, x + bw, y + badge_h);
                    spec.text = label;
                    StyleReleaseBadge(spec, tag, vm.dark, painter_.HighContrast());
                    painter_.DrawBadge(spec);
                    return bw;
                };
                for (size_t k = 0; k < note.lines.size(); ++k) {
                    if (ly > lay.content.bottom) break;
                    const ReleaseLineBox box = MeasureReleaseLine(&painter_, note, k, text_w, tag_col, tags, scale_);
                    const float h = box.Total();
                    if (ly + h >= lay.content.top) {
                        const float ty = ly + box.gap_above;
                        if (ReleaseLineHeading(note, k)) {
                            painter_.DrawWrappedSection(note.lines[k], D2D1::Point2F(text_x, ty), text_w,
                                                        theme.text);
                        } else if (tag_col > 0.0f && ReleaseLineTagged(note, k)) {
                            // Tag in a column of its own; the item wraps beside it.
                            draw_badge(note.tags[k], note.tag_labels[k], text_x, ty + 1.0f * scale_);
                            painter_.DrawWrappedCaption(note.lines[k], D2D1::Point2F(text_x + tag_col, ty),
                                                        (std::max)(40.0f * scale_, text_w - tag_col), theme.text);
                        } else {
                            if (k < note.bullets.size() && note.bullets[k]) {
                                const float r = 2.5f * scale_;
                                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(text_x - 12.0f * scale_,
                                                                            ty + 9.0f * scale_), r, r),
                                                brAccent_.get());
                            }
                            painter_.DrawWrappedCaption(note.lines[k], D2D1::Point2F(text_x, ty), text_w,
                                                        theme.text);
                        }
                        if (box.legend_h > 0.0f) {
                            // Under the summary: 新功能 10 · 新设置 8 · 改进 19 · 修复 8.
                            float lx = text_x;
                            const float legend_y = ty + box.text_h + 6.0f * scale_;
                            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                            for (int t = 1; t < kReleaseTagCount; ++t) {
                                if (!tags.count[t] || !tags.label[t]) continue;
                                lx += draw_badge(static_cast<ReleaseTag>(t), *tags.label[t], lx, legend_y) +
                                      5.0f * scale_;
                                const std::wstring count = std::to_wstring(tags.count[t]);
                                const float count_w = measure_small(count);
                                DrawTextRect(dc, small_fmt, brTextSecondary_.get(), count, lx,
                                             legend_y - 1.0f * scale_, count_w + 4.0f * scale_, badge_h);
                                lx += count_w + 16.0f * scale_;
                            }
                        }
                    }
                    ly += h + kReleaseLineGapDip * scale_;
                }
            }
        }
    } else if (vm.settings_page == 5 || vm.settings_page == 8) {
        DrawSettingsPacks(vm, rect, theme);
    } else if (vm.settings_page == 6) {
        DrawSettingsSysCtx(vm, rect, theme);
    } else if (vm.settings_page == 7) {
        DrawSettingsShortcuts(vm, rect, theme);
    } else if (vm.settings_page == kSettingsPageAi) {
        DrawSettingsAi(vm, rect, theme);
    } else if (vm.settings_page == 4) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            if (card.right <= card.left || card.bottom <= card.top) return;
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };
        draw_card(lay.duplicate_options);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.dup_hint,
                     lay.content.left + pad, origin + pad + 44.0f * scale_,
                     lay.content.right - lay.content.left - pad * 2, 28.0f * scale_);
        static constexpr pulse::l10n::StringId kScope[] = {
            pulse::l10n::StringId::DupScopeFolder,
            pulse::l10n::StringId::DupScopeDrive,
            pulse::l10n::StringId::DupScopeAll,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = lay.dup_scope[i];
            if (i == 0) painter_.DrawSegmentedTrack(D2D1::RectF(segment.bounds.left,
                segment.bounds.top, lay.dup_scope[2].right, lay.dup_scope[2].bottom));
            segment.bounds = D2D1::RectF(segment.bounds.left + 3 * scale_, segment.bounds.top + 3 * scale_,
                segment.bounds.right - 3 * scale_, segment.bounds.bottom - 3 * scale_);
            segment.shared_track = true;
            segment.text = pulse::l10n::Get(kScope[i]);
            segment.position = i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = vm.dup_scope == i;
            segment.state.enabled = !vm.dup_scanning;
            segment.state.hovered = segment.state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDupScope, i);
            painter_.DrawSegmentedItem(segment);
        }
        if (vm.dup_scope == 0) {
            MakeBrush(dc, theme.text, brText_);
            const std::wstring folder = vm.dup_folder.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::DupFolderPlaceholder)
                : vm.dup_folder;
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), folder,
                         lay.content.left + pad + 16.0f * scale_, lay.dup_browse.top,
                         lay.dup_browse.left - lay.content.left - pad - 28.0f * scale_,
                         32.0f * scale_);
            fluent::ControlState browse{};
            browse.enabled = !vm.dup_scanning;
            browse.hovered = browse.enabled && IsHovered(vm, HitTestResult::SettingsDupBrowse);
            painter_.DrawButton({lay.dup_browse,
                pulse::l10n::Get(pulse::l10n::StringId::DupBrowse), {},
                fluent::ButtonKind::Standard, browse});
        } else if (vm.dup_scope == 1) {
            for (size_t i = 0; i < vm.dup_drives.size() && i < lay.dup_drives.size(); ++i) {
                fluent::ControlState chip{};
                chip.checked = vm.dup_drives[i].selected;
                chip.selected = vm.dup_drives[i].selected;
                chip.enabled = !vm.dup_scanning;
                chip.hovered = chip.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupDrive, static_cast<int>(i));
                painter_.DrawButton({lay.dup_drives[i], vm.dup_drives[i].label, {},
                    fluent::ButtonKind::Toggle, chip});
            }
        }
        static constexpr pulse::l10n::StringId kMin[] = {
            pulse::l10n::StringId::DupMinSize1KB,
            pulse::l10n::StringId::DupMinSize1MB,
            pulse::l10n::StringId::DupMinSize10MB,
        };
        {
            const float label_top = lay.dup_min_size[0].top - 40.0f * scale_;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::DupMinSize),
                         lay.content.left + pad + 16*scale_, label_top,
                         lay.content.right - lay.content.left - pad * 2, 20.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::DupMinSizeDesc),
                         lay.content.left + pad + 16*scale_, label_top + 20.0f * scale_,
                         lay.content.right - lay.content.left - pad * 2, 18.0f * scale_);
        }
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = lay.dup_min_size[i];
            if (i == 0) painter_.DrawSegmentedTrack(D2D1::RectF(segment.bounds.left,
                segment.bounds.top, lay.dup_min_size[2].right, lay.dup_min_size[2].bottom));
            segment.bounds = D2D1::RectF(segment.bounds.left + 3 * scale_, segment.bounds.top + 3 * scale_,
                segment.bounds.right - 3 * scale_, segment.bounds.bottom - 3 * scale_);
            segment.shared_track = true;
            segment.text = pulse::l10n::Get(kMin[i]);
            segment.position = i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = vm.dup_min_size == i;
            segment.state.enabled = !vm.dup_scanning;
            segment.state.hovered = segment.state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDupMinSize, i);
            painter_.DrawSegmentedItem(segment);
        }
        fluent::ControlState scan{};
        scan.enabled = vm.dup_can_scan && !vm.dup_scanning;
        scan.hovered = scan.enabled && IsHovered(vm, HitTestResult::SettingsDupScan);
        painter_.DrawButton({lay.dup_scan, pulse::l10n::Get(pulse::l10n::StringId::DupScan), {},
            fluent::ButtonKind::Primary, scan});
        fluent::ControlState cancel{};
        cancel.enabled = vm.dup_scanning;
        cancel.hovered = cancel.enabled && IsHovered(vm, HitTestResult::SettingsDupCancel);
        painter_.DrawButton({lay.dup_cancel, pulse::l10n::Get(pulse::l10n::StringId::Cancel), {},
            fluent::ButtonKind::Standard, cancel});
        if (vm.dup_show_progress) {
            draw_card(lay.dup_progress);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), vm.dup_status,
                         lay.dup_progress.left + 16.0f * scale_, lay.dup_progress.top + 10.0f * scale_,
                         lay.dup_progress.right - lay.dup_progress.left - 32.0f * scale_,
                         18.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.dup_speed,
                         lay.dup_progress.left + 16.0f * scale_, lay.dup_progress.top + 28.0f * scale_,
                         lay.dup_progress.right - lay.dup_progress.left - 32.0f * scale_,
                         16.0f * scale_);
            fluent::ProgressSpec bar;
            bar.bounds = D2D1::RectF(lay.dup_progress.left + 16.0f * scale_,
                                     lay.dup_progress.bottom - 18.0f * scale_,
                                     lay.dup_progress.right - 16.0f * scale_,
                                     lay.dup_progress.bottom - 10.0f * scale_);
            bar.indeterminate = vm.dup_progress_indeterminate;
            bar.value = vm.dup_progress_value;
            bar.animation_progress = vm.dup_animation;
            painter_.DrawProgressBar(bar);
        }
        if (!vm.dup_empty.empty()) {
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.dup_empty,
                         lay.content.left + pad, lay.dup_progress.bottom > lay.dup_progress.top
                             ? lay.dup_progress.bottom + 8.0f * scale_
                             : lay.dup_scan.bottom + 12.0f * scale_,
                         lay.content.right - lay.content.left - pad * 2, 28.0f * scale_);
        }
        for (size_t g = 0; g < vm.dup_groups.size() && g < lay.dup_group_cards.size(); ++g) {
            const auto& card = lay.dup_group_cards[g];
            if (!VisibleInContent(card, lay.content)) continue;
            draw_card(card);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), vm.dup_groups[g].title,
                         card.left + 16.0f * scale_, card.top + 12.0f * scale_,
                         card.right - card.left - 32.0f * scale_, 24.0f * scale_);
            const float file_h = 32.0f * scale_;
            const float inner = 16.0f * scale_;
            float fy = card.top + 48.0f * scale_;
            for (size_t f = 0; f < vm.dup_groups[g].files.size(); ++f) {
                if (fy + file_h <= lay.content.top) {
                    fy += file_h;
                    continue;
                }
                if (fy >= lay.content.bottom) break;
                const auto& file = vm.dup_groups[g].files[f];
                const D2D1_RECT_F keep_rc = D2D1::RectF(card.left + inner, fy,
                    card.left + inner + 88.0f * scale_, fy + file_h);
                const D2D1_RECT_F open_rc = D2D1::RectF(card.left + inner + 92.0f * scale_, fy,
                    card.right - inner, fy + file_h);
                fluent::ControlState radio{};
                radio.checked = file.keep;
                radio.enabled = !vm.dup_scanning;
                radio.hovered = radio.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupKeep, static_cast<int>(g),
                              static_cast<int>(f));
                painter_.DrawRadioButton(keep_rc,
                    pulse::l10n::Get(pulse::l10n::StringId::DupKeep), radio);
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), file.name,
                             open_rc.left, open_rc.top, open_rc.right - open_rc.left,
                             16.0f * scale_);
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), file.detail,
                             open_rc.left, open_rc.top + 14.0f * scale_,
                             open_rc.right - open_rc.left, 14.0f * scale_);
                fy += file_h;
            }
            if (g < lay.dup_group_delete.size() &&
                VisibleInContent(lay.dup_group_delete[g], lay.content)) {
                fluent::ControlState del{};
                del.enabled = !vm.dup_scanning;
                del.hovered = del.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupGroupDelete, static_cast<int>(g));
                painter_.DrawButton({lay.dup_group_delete[g],
                    pulse::l10n::Get(pulse::l10n::StringId::DupDeleteExtras), {},
                    fluent::ButtonKind::Standard, del});
            }
        }
        if (vm.dup_show_delete_all) {
            fluent::ControlState all{};
            all.enabled = !vm.dup_scanning;
            all.hovered = all.enabled && IsHovered(vm, HitTestResult::SettingsDupDeleteAll);
            painter_.DrawButton({lay.dup_delete_all, vm.dup_delete_all, {},
                fluent::ButtonKind::Primary, all});
        }
    }
    dc->PopAxisAlignedClip();

    const float view = lay.content.bottom - lay.content.top;
    if (lay.content_h > view + 1.0f) {
        fluent::ScrollbarSpec bar = SettingsScrollbar(lay, scale_);
        bar.offset = vm.settings_scroll;
        painter_.DrawScrollbar(bar);
    }
}

D2D1_RECT_F MainRenderer::SettingsDropdownBounds(const WindowViewModel& vm, int index, float window_w, float window_h) const {
    const auto l=MakeSettingsLayout(vm,D2D1::RectF(0,0,window_w,window_h),scale_,title_bar_height_,status_height_,&painter_);
    return index==0 ? l.effect_choice : index==1 ? l.language_choice : l.ui_font_family_choice;
}

float MainRenderer::SettingsDestinationOffset(const WindowViewModel& vm, int setting_id, float window_w, float window_h) const {
    const auto l=MakeSettingsLayout(vm,D2D1::RectF(0,0,window_w,window_h),scale_,title_bar_height_,status_height_,&painter_);
    using I=l10n::StringId;
    D2D1_RECT_F target{};
    switch(static_cast<I>(setting_id)) {
    case I::SettingsTheme: target=l.theme_row;break;
    case I::SettingsThemeColor: target=l.accent_card;break;
    case I::SettingsWindowEffect: target=l.effect_card;break;
    case I::MenuAcrylic: target=l.menu_acrylic_row;break;
    case I::SettingsLanguage: target=l.language_card;break;
    case I::SettingsIntegration: target=l.integration_section;break;
    case I::SettingsDefaultManager: target=l.default_manager_row;break;
    case I::SettingsLaunch: target=l.startup_row[0];break;
    case I::SettingsStartInTray: target=l.start_in_tray_row;break;
    case I::SettingsKeepRunning: target=l.startup_row[1];break;
    case I::SettingsNotifyIcon: target=l.notify_icon_card;break;
    case I::SettingsHomeFolder: target=l.home_folder_card;break;
    case I::SettingsStartupOpen: target=l.startup_open_card;break;
    case I::SettingsNewTabOpen: target=l.new_tab_open_card;break;
    case I::SettingsExternalOpen: target=l.external_open_card;break;
    case I::SettingsCloseLastTab: target=l.close_last_tab_row;break;
    case I::SettingsCloseTabDoubleClick: target=l.close_tab_double_click_row;break;
    case I::SettingsConfirmDelete: target=l.confirm_delete_row;break;
    case I::SettingsOpenFolders: target=l.integration_cover[0];break;
    case I::SettingsRowHeight: target=l.density_card;break;
    case I::SettingsShowPerformance: target=l.diagnostics_perf;break;
    case I::AccentCustomTitle: target=l.accent_custom_head;break;
    case I::ListSmartDate: target=l.list_style_row[0];break;
    case I::ListZebraRows: target=l.list_style_row[1];break;
    case I::ListSizeBar: target=l.list_style_row[2];break;
        case I::ListTagNameColor: target=l.list_style_row[3];break;
    case I::ListSelectionOutline: target=l.list_style_row[4];break;
    case I::ListThumbnailBadges: target=l.list_style_row[5];break;
    case I::SettingsClickOpen: target=l.click_open_card;break;
    case I::SettingsItemCheckboxes: target=l.item_checkbox_row;break;
    case I::ListMtimeHint: target=l.mtime_style_card;break;
    case I::ListMtimeRange: target=vm.settings_mtime.style ? l.mtime_range_card : l.mtime_style_card;break;
    case I::ListMtimePalette: target=vm.settings_mtime.style ? l.mtime_palette_card : l.mtime_style_card;break;
    case I::SyncOneDrive: target=l.sync_row[0];break;
    case I::SyncSvn: target=l.sync_row[1];break;
    case I::SettingsFolderSort: target=l.folder_sort_card;break;
    case I::SettingsTextRender: target=l.text_render_card;break;
    case I::SettingsUiFontSize: target=l.ui_font_size_card;break;
    case I::SettingsUiFontFamily: target=l.ui_font_family_card;break;
    case I::SettingsWallpaper: target=l.wallpaper_card;break;
    case I::SettingsWallpaperLook: target=l.wallpaper_look_card;break;
    case I::SettingsWallpaperBlur: target=l.wallpaper_blur_card;break;
    case I::SettingsFrameLook: target=l.frame_look_card;break;
    case I::SettingsTrayIcon: target=l.tray_icon_card;break;
    case I::SettingsShowHidden: target=l.hidden_files_row;break;
    case I::SettingsShowProtected: target=l.protected_files_row;break;
    case I::PinnedNames: target=l.pinned_names_row;break;
    case I::AliasMark: target=l.alias_mark_row;break;
    case I::SettingsTreeSticky: target=l.tree_sticky_row;break;
    case I::ThumbCache: target=l.thumb_cache_row;break;
    case I::SettingsVerticalTabs: target=l.vertical_tabs_row;break;
    case I::TrayHideWhenEmpty: target=l.tray_hide_row;break;
    case I::SettingsLanDiscovery: target=l.lan_discovery_row;break;
    case I::SettingsLanCache: target=l.lan_cache_card;break;
    case I::SettingsNetworkRecent: target=l.network_recent_row;break;
    case I::NetworkShowHiddenShares: target=l.hidden_shares_row;break;
    case I::SettingsClearNetwork: target=l.network_clear_row;break;
    case I::SettingsHints: target=l.hints_row;break;
    case I::SettingsHintsReset: target=l.hints_reset_row;break;
    case I::SettingsTransfer: target=l.transfer_card;break;
    case I::SettingsBlankClickBack: target=l.blank_click_row;break;
    case I::SettingsWinE: target=l.integration_cover[2];break;
    case I::SettingsThisPc: target=l.integration_cover[1];break;
    case I::SettingsRecycleBin: target=l.recycle_bin_row;break;
    case I::SettingsExplorerWindows: target=l.explorer_windows_row;break;
    case I::SettingsShellTags: target=l.shell_tags_row;break;
    case I::SettingsChangeTracking: target=l.change_tracking_row;break;
    case I::SearchSwitch: target=l.search_master_row;break;
    case I::GlobalSearch: target=l.global_search_row;break;
    case I::GlobalSearchHotkey: target=l.global_search_hotkey_row;break;
    case I::GlobalSearchDoubleCtrl: target=l.global_search_double_ctrl_row;break;
    case I::GlobalSearchDoubleCtrlGap: target=l.global_search_gap_card;break;
    case I::GlobalSearchApps: target=l.global_search_apps_row;break;
    case I::DialogJumpBar: target=l.dialog_jump_row;break;
    case I::DialogJumpAuto: target=l.dialog_jump_auto_row;break;
    case I::SearchPinyin: target=l.search_pinyin_row;break;
    case I::JumpHistory: target=l.jump_history_row;break;
    case I::ContentIndexManage: target=l.content_header;break;
    case I::IndexLocation: target=l.index_path;break;
    case I::LocalDrives: if(!l.index_volume_rows.empty()) target=l.index_volume_rows.front();break;
    case I::Exclusions: target=l.index_exclude_action;break;
    case I::ServerFolders: target=l.network_action[0];break;
    case I::ReleaseNotes: target=l.release_card;break;
    case I::SettingsAutoUpdate: target=l.update_auto_row;break;
    default: return 0;
    }
    return (std::max)(0.0f,target.top-l.content_origin-20*scale_);
}

float MainRenderer::SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const {
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, window_w, window_h);
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    const float view = (std::max)(0.0f, lay.content.bottom - lay.content.top);
    return (std::max)(0.0f, lay.content_h - view);
}

bool MainRenderer::SettingsScrollbarGeometry(const WindowViewModel& vm, float window_w, float window_h,
                                             D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const {
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, window_w, window_h);
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    const float view = (std::max)(0.0f, lay.content.bottom - lay.content.top);
    max_scroll = (std::max)(0.0f, lay.content_h - view);
    if (lay.content_h <= view + 1.0f) return false;   // DrawSettings draws no bar
    fluent::ScrollbarSpec bar = SettingsScrollbar(lay, scale_);
    bar.offset = vm.settings_scroll;
    track = bar.viewport;
    thumb = fluent::ScrollbarThumbRect(bar, scale_);
    return max_scroll > 0.0f && thumb.bottom > thumb.top;
}

} // namespace pulse::ui
