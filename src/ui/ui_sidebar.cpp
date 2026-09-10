// ui_sidebar.cpp — Sidebar and staging-tray deck.
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

int MainRenderer::TrayDeckCapacity(float window_w) const {
    // Mirror TrayFanGeometry's available width: tray panel rect is the
    // sidebar minus 8px padding on each side, and the deck lane reserves
    // 24px of horizontal padding. Neighbors may overlap by at most 50%, so
    // every card beyond the first needs icon/2 of room.
    const float icon = tray_icon_dip_ * scale_;
    const float avail = EffectiveSidebarWidth(window_w) - 40.0f * scale_ - icon;
    return std::max(1, static_cast<int>(std::floor(avail / (icon * 0.5f))) + 1);
}
void MainRenderer::DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const float w = sb.right - sb.left;

    // Partially visible scrolled rows must not paint over the toolbar or status bar.
    dc->PushAxisAlignedClip(sb, D2D1_ANTIALIAS_MODE_ALIASED);

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
            const int svg_id = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            const float icon = 20.0f * scale_;
            const float pad_x = (slot.rc.right - slot.rc.left - icon) * 0.5f;
            const float pad_y = (slot.rc.bottom - slot.rc.top - icon) * 0.5f;
            const D2D1_RECT_F icon_rc = D2D1::RectF(slot.rc.left + pad_x, slot.rc.top + pad_y,
                                                    slot.rc.right - pad_x, slot.rc.bottom - pad_y);
            if (IsHighContrast() || svg_id == 0 ||
                !DrawFluentSvg(svg_id, icon_rc, 1.0f)) {
                DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                    slot.rc.bottom - slot.rc.top, item.icon_glyph, item.fallback_text,
                    iconColor, 0.92f);
            }
        }
        dc->PopAxisAlignedClip();
        return;
    }

    wchar_t countLabel[24]{};
    const int trayCount = TrayTotalCount(vm);
    if (trayCount > 0) swprintf_s(countLabel,
        pulse::l10n::Get(pulse::l10n::StringId::ItemsCountFormat).c_str(), trayCount);

    for (const auto& slot : slots) {
        if (slot.kind == SidebarSlot::Header) {
            fluent::SidebarSectionHeaderSpec header;
            header.bounds = slot.rc;
            header.text = vm.sidebar[slot.group].header;
            header.expanded = !vm.sidebar[slot.group].collapsed;
            header.state.hovered = IsHovered(vm, HitTestResult::SidebarHeader, slot.group);
            painter_.DrawSidebarSectionHeader(header);
            if (vm.sidebar[slot.group].add_action != SidebarAddAction::None) {
                DrawIconText(slot.rc.right - 52.0f * scale_, slot.rc.top,
                    24.0f * scale_, slot.rc.bottom - slot.rc.top,
                    kIconAdd, L"+", theme.text_secondary, 0.72f);
            }
            continue;
        }
        if (slot.kind == SidebarSlot::TrayPanel) {
            fluent::StagingTrayPanelSpec tray;
            tray.bounds = slot.rc;
            tray.title = pulse::l10n::Get(pulse::l10n::StringId::StagingTray);
            // Do not assign a conditional std::wstring temporary to a
            // std::wstring_view: the view would dangle before DrawText runs.
            if (vm.tray_deck.cards.empty()) {
                tray.helper = pulse::l10n::Get(pulse::l10n::StringId::StagingTrayHelper);
            }
            tray.count_label = countLabel;
            if (vm.tray_deck.total_count != 0) {
                tray.action_text = pulse::l10n::Get(pulse::l10n::StringId::Release);
            }
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
            IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run);
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
            const int drive_svg = FluentSvgIdForGlyph(item.icon_glyph);
            drive.skip_glyph = !IsHighContrast() && drive_svg != 0 && EnsureFluentSvg(drive_svg);
            painter_.DrawDriveSidebarItem(drive);
            if (drive.skip_glyph) {
                DrawFluentSvg(drive_svg, painter_.DriveSidebarItemIconRect(slot.rc), 1.0f);
            }
        } else {
            const bool draggedTag =
                slot.group == vm.tag_drag_group && slot.item == vm.tag_drag_item;
            if (draggedTag) {
                // Insertion indicator under the lifted card: accent line with a
                // leading dot at the tentative gap's top edge.
                if (vm.tag_gap_line_y > 0.0f) {
                    const float th = 2.5f * scale_;
                    const float lx0 = slot.rc.left + 2.0f * scale_;
                    const float lx1 = slot.rc.right - 2.0f * scale_;
                    MakeBrush(dc, theme.accent, brAccent_);
                    FillRoundedRect(dc, brAccent_.get(), lx0,
                        vm.tag_gap_line_y - th * 0.5f, lx1 - lx0, th, th * 0.5f);
                    dc->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(lx0 + 4.0f * scale_, vm.tag_gap_line_y),
                        3.0f * scale_, 3.0f * scale_), brAccent_.get());
                }
                // Raised while dragging: a clearly deeper shadow + brighter card
                // than a plain selected row, so the lift reads at a glance.
                const float r = theme.radius_control * scale_;
                ComPtr<ID2D1CommandList> card;
                dc->CreateCommandList(&card);
                if (card.get()) {
                    ComPtr<ID2D1Image> prev;
                    dc->GetTarget(&prev);
                    dc->SetTarget(card.get());
                    MakeBrush(dc, vm.dark ? HexColor(0x303030) : HexColor(0xFFFFFF), brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(),
                        slot.rc.left + 1.0f, slot.rc.top + 1.0f,
                        slot.rc.right - slot.rc.left - 2.0f,
                        slot.rc.bottom - slot.rc.top - 2.0f, r);
                    dc->SetTarget(prev.get());
                    card->Close();
                    ComPtr<ID2D1Effect> shadow;
                    if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &shadow)) && shadow.get()) {
                        shadow->SetInput(0, card.get());
                        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 8.0f * scale_);
                        shadow->SetValue(D2D1_SHADOW_PROP_COLOR,
                            D2D1::Vector4F(0.0f, 0.0f, 0.0f, vm.dark ? 0.44f : 0.28f));
                        dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 3.0f * scale_),
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
            if (item.path == L"pulse:recycle") row.detail = item.detail;
            row.glyph = item.icon_glyph;
            row.badge_text = item.badge;
            row.badge_color = item.badge_color;
            row.custom_badge_color = !item.badge.empty() && item.badge_color.a > 0.0f;
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
            const int row_svg = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            row.skip_glyph = !IsHighContrast() && row_svg != 0 && EnsureFluentSvg(row_svg);
            const bool unpin = SidebarItemHasUnpin(item);
            const auto unpin_rc = WorkspaceUnpinRect(slot.rc, scale_);
            const auto expand_rc = SidebarExpandRect(slot.rc, scale_);
            if (unpin)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - unpin_rc.left) - 6.0f * scale_);
            else if (item.expandable)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - expand_rc.left) - 4.0f * scale_);
            painter_.DrawSidebarItem(row);
            if (row.skip_glyph) {
                DrawFluentSvg(row_svg, painter_.SidebarItemIconRect(slot.rc, item.status_dot),
                              1.0f);
            }
            if (item.editing) {
                // In-place tag rename: give the hosted edit a real TextField
                // frame. Geometry must match TagRenameCell in app_main.cpp.
                D2D1_RECT_F cell = slot.rc;
                cell.left += 34.0f * scale_;
                cell.right -= 38.0f * scale_;
                cell.top += 2.0f * scale_;
                cell.bottom -= 2.0f * scale_;
                if (cell.right > cell.left && cell.bottom > cell.top) {
                    fluent::ControlState field;
                    field.focused = true;
                    painter_.DrawTextFieldFrame(cell, field);
                }
            }
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
            } else if (item.expandable) {
                const bool expand_hot = IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run);
                if (expand_hot) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        4.0f * scale_);
                }
                DrawIconText(expand_rc.left, expand_rc.top,
                    expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                    item.expanded ? kIconChevronDown : kIconChevronRight,
                    item.expanded ? L"v" : L">", theme.text_secondary, 0.68f);
            }
        }
    }

    const float max_scroll = SidebarMaxScroll(vm, rect.right, rect.bottom);
    if (max_scroll > 0.0f) {
        const SidebarMetrics metrics = MakeSidebarMetrics(scale_);
        float tray_height = ExpandedTrayHeight(vm, metrics);
        tray_height = std::min(tray_height,
            std::max(120.0f * scale_, (sb.bottom - sb.top) * 0.52f));
        const float bottom = sb.bottom - metrics.pad - tray_height - metrics.pad;
        const float viewport_extent = std::max(0.0f, bottom - sb.top);
        fluent::ScrollbarSpec bar;
        bar.viewport = D2D1::RectF(sb.right - 8.0f * scale_, sb.top,
                                   sb.right - 2.0f * scale_, bottom);
        bar.offset = std::clamp(vm.sidebar_scroll, 0.0f, max_scroll);
        bar.viewport_extent = viewport_extent;
        bar.content_extent = viewport_extent + max_scroll;
        bar.expand_progress = 1.0f;
        painter_.DrawScrollbar(bar);
    }
    dc->PopAxisAlignedClip();
}

void MainRenderer::DrawTrayDeck(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc,
                                const Theme& theme) {
    if (vm.tray_deck.cards.empty() || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    int layout_count = vm.tray_deck.live_count;
    if (layout_count == 0) {
        // Keep the cleared fan in place instead of collapsing to a single icon.
        for (const auto& card : vm.tray_deck.cards)
            layout_count = std::max(layout_count, card.exit_layout_count);
    }
    const TrayFanGeom g = TrayFanGeometry(panel_rc, layout_count,
                                          vm.tray_deck.open, scale_, tray_icon_dip_);

    // Ghosts (exiting) underneath; live icons top-to-bottom so lower cards
    // overlap the ones above; the hovered entry always draws last.
    const std::vector<int> order = TrayCardPaintOrder(vm.tray_deck, g, scale_);

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
        ID2D1Bitmap* bitmap = drewThumbnail ? nullptr
            : icon_cache_.BitmapFor(card.missing ? L"" : card.path,
                                   card.name, card.is_dir, card.attrs, g.icon);
        if (!drewThumbnail && bitmap) {
            // Soft drop shadow straight off the bitmap silhouette, no card.
            // Effects retain their inputs, so pointer keys stay valid until eviction.
            auto found = tray_shadows_.find(bitmap);
            if (found == tray_shadows_.end()) {
                if (tray_shadows_.size() >= 32) tray_shadows_.clear();
                ComPtr<ID2D1Effect> created;
                if (SUCCEEDED(dc->CreateEffect(kShadowEffectClsid, &created)) && created.get()) {
                    created->SetInput(0, bitmap);
                    created->SetValue(D2D1_PROPERTY_CACHED, TRUE);
                    found = tray_shadows_.emplace(bitmap, std::move(created)).first;
                }
            }
            if (found != tray_shadows_.end()) {
                ID2D1Effect* shadow = found->second.get();
                shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION,
                                 (hovered ? 5.0f : 3.0f) * scale_);
                shadow->SetValue(D2D1_SHADOW_PROP_COLOR,
                    D2D1::Vector4F(0.0f, 0.0f, 0.0f, (vm.dark ? 0.55f : 0.30f) * opacity));
                dc->DrawImage(shadow, D2D1::Point2F(0.0f, 2.0f * scale_),
                              D2D1_INTERPOLATION_MODE_LINEAR);
            }
            dc->DrawBitmap(bitmap, &dest, opacity,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
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
            swprintf_s(footer,
                pulse::l10n::Get(pulse::l10n::StringId::TotalSizeBatchesFormat).c_str(),
                size_text.c_str(), vm.tray_deck.batch_count);
        else
            swprintf_s(footer,
                pulse::l10n::Get(pulse::l10n::StringId::TotalSizeFormat).c_str(),
                size_text.c_str());
        const float footer_h = 22.0f * scale_;
        const float footerTop = panel_rc.bottom - 8.0f * scale_ - footer_h;
        painter_.DrawText(footer,
                          D2D1::RectF(panel_rc.left + 10.0f * scale_, footerTop,
                          panel_rc.right - 84.0f * scale_, footerTop + footer_h),
                          compositor_->SmallFormat(), theme.text_secondary);
        const bool clear_hovered =
            vm.hover_region == static_cast<int>(HitTestResult::TrayClear);
        const float clear_w = 72.0f * scale_;
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
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::ClearAll), clear_rc,
                          compositor_->SmallFormat(),
                          theme.danger,
                          fluent::HorizontalAlignment::Center);
    }

    // Name text: a single staged item always shows name + size/type; with
    // several cards, only the hovered icon gets a small pill label.
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
        const float tw = std::min(MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
                                                    fmt, card.name),
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
float MainRenderer::SidebarMaxScroll(const WindowViewModel& vm, float window_w,
                                     float window_h) const {
    const D2D1_RECT_F sb = SidebarRect(window_w, window_h);
    if (sb.right - sb.left <= 60.0f * scale_) return 0.0f;
    const SidebarMetrics metrics = MakeSidebarMetrics(scale_);
    float tray_height = ExpandedTrayHeight(vm, metrics);
    tray_height = std::min(tray_height,
        std::max(120.0f * scale_, (sb.bottom - sb.top) * 0.52f));
    const float available = std::max(0.0f,
        sb.bottom - metrics.pad - tray_height - metrics.pad - sb.top);
    return std::max(0.0f, SidebarContentHeight(vm, metrics) - available);
}

} // namespace pulse::ui
