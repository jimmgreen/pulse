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

namespace {

// Accent insertion indicator shared by the two sidebar reorders (sections and
// quick-access pins).
void DrawSidebarInsertionLine(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush,
                              const D2D1_RECT_F& sb, float scale, float y) {
    const SidebarMetrics m = MakeSidebarMetrics(scale);
    const float th = 2.5f * scale;
    const float lx0 = sb.left + m.pad + 2.0f * scale;
    const float lx1 = sb.right - m.pad - 2.0f * scale;
    FillRoundedRect(dc, brush, lx0, y - th * 0.5f, lx1 - lx0, th, th * 0.5f);
    dc->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(lx0 + 4.0f * scale, y), 3.0f * scale, 3.0f * scale),
        brush);
}

} // namespace

D2D1_RECT_F MainRenderer::SidebarRect(float w, float h) const {
    float top = title_bar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    const float right = SidebarPeekVisible(w) ? SidebarFullWidth(w) : EffectiveSidebarWidth(w);
    return D2D1::RectF(0.0f, top, right, bottom);
}

void MainRenderer::SidebarGroupBands(const WindowViewModel& vm, float w, float h,
                                     std::vector<SidebarGroupBand>& out) const {
    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, SidebarRect(w, h), scale_, slots, &out);
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
    // Card stack window: top card + two peeking layers + one hidden card
    // that keeps transitions continuous. Independent of the window width.
    (void)window_w;
    return 4;
}
// Hover peek: the expanded sidebar floats over the panes on a raised card.
void MainRenderer::DrawSidebarPeek(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                   const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const float r = theme.radius_flyout * scale_;
    const D2D1_RECT_F card = D2D1::RectF(sb.left + 4.0f * scale_, sb.top - 2.0f * scale_,
                                         sb.right + 6.0f * scale_, sb.bottom + 2.0f * scale_);
    // Cheap layered shadow; no effect graph for a transient overlay.
    for (int i = 3; i >= 1; --i) {
        const float g = static_cast<float>(i) * 3.0f * scale_;
        MakeBrush(dc, D2D1_COLOR_F{0.0f, 0.0f, 0.0f, vm.dark ? 0.12f : 0.045f}, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), card.left - g * 0.5f, card.top + g * 0.4f,
                        card.right - card.left + g, card.bottom - card.top + g * 0.6f, r + g);
    }
    MakeBrush(dc, BlendOver(theme.surface_flyout, theme.bg), brFillSelected_);
    FillRoundedRect(dc, brFillSelected_.get(), card.left, card.top,
                    card.right - card.left, card.bottom - card.top, r);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(card.left + 0.5f, card.top + 0.5f,
        card.right - 0.5f, card.bottom - 0.5f), r, r), brStrokeCard_.get(), 1.0f);
    DrawSidebar(vm, rect, theme);
}

ID2D1Bitmap* MainRenderer::SidebarFolderIcon(const SidebarItem& item, const D2D1_RECT_F& rect) {
    if (!item.shell_folder_icon || item.path.empty()) return nullptr;
    const float size = std::max(rect.right - rect.left, rect.bottom - rect.top);
    icon_cache_.Prefetch(item.path, item.label, true, FILE_ATTRIBUTE_DIRECTORY, size);
    return icon_cache_.CachedBitmapFor(item.path, item.label, true, FILE_ATTRIBUTE_DIRECTORY, size);
}

void MainRenderer::RefreshSidebarFolderIcons(const WindowViewModel& vm) {
    std::vector<std::wstring> paths;
    for (const auto& group : vm.sidebar)
        for (const auto& item : group.items)
            if (item.shell_folder_icon) paths.push_back(item.path);
    icon_cache_.InvalidatePaths(paths);
}

void MainRenderer::DrawSidebar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    D2D1_RECT_F sb = SidebarRect(rect.right, rect.bottom);
    const D2D1_RECT_F sb_clip = sb;
    // Collapse/expand: lay out at the expanded width and reveal it through
    // the easing width, so labels slide rather than re-wrap every frame.
    if (collapse_anim_ && rect.right / scale_ >= kSidebarRailWindowDip)
        sb.right = (std::max)(sb.right, SidebarFullWidth(rect.right));
    const float w = sb.right - sb.left;

    // Partially visible scrolled rows must not paint over the toolbar or status bar.
    dc->PushAxisAlignedClip(sb_clip, D2D1_ANTIALIAS_MODE_ALIASED);

    // The sidebar sits on the shared sheet painted by Render(); the pane cards
    // beside it provide the edge, so it has no fill or divider of its own.

    std::vector<SidebarSlot> slots;
    LayoutSidebar(vm, sb, scale_, slots);
    const bool compact = SidebarRailLayout(w, scale_);
    painter_.BeginFrame(theme, IsHighContrast());

    // Only the most specific matching row is selected (Desktop, not also the
    // C: drive that contains it). That row owns one pill that glides to the
    // newly selected row (ui_motion.h); reorder gestures and high contrast
    // keep the static fill.
    int selected_slot = -1;
    {
        size_t best = 0;
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.kind != SidebarSlot::Item && slot.kind != SidebarSlot::Drive &&
                slot.kind != SidebarSlot::Tag) continue;
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            if (item.tab_row) continue;  // tabs mark themselves (active card)
            if (!PathIsSelfOrChild(item.path, vm.pane.path)) continue;
            if (selected_slot < 0 || item.path.size() > best) {
                selected_slot = static_cast<int>(k);
                best = item.path.size();
            }
        }
    }
    const bool reordering = vm.tag_drag_group >= 0 || vm.sidebar_pin_drag_index >= 0 ||
                            vm.sidebar_group_drag_id >= 0;
    const int pill_slot = !reordering && !IsHighContrast() ? selected_slot : -1;
    const auto isSelectedSlot = [&](const SidebarSlot& slot) {
        return selected_slot >= 0 && &slot == &slots[static_cast<size_t>(selected_slot)];
    };
    // Vertical tabs: the block sits in a recessed well so open tabs read as
    // their own thing, apart from the places below; the active tab's card
    // rises out of it.
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (!vm.sidebar[static_cast<size_t>(g)].tabs_section) continue;
        float top = 1e9f, bottom = -1.0f, left = 1e9f, right = -1.0f;
        for (const auto& slot : slots) {
            if (slot.group != g) continue;
            top = (std::min)(top, slot.rc.top);
            bottom = (std::max)(bottom, slot.rc.bottom);
            left = (std::min)(left, slot.rc.left);
            right = (std::max)(right, slot.rc.right);
        }
        if (bottom < 0.0f) break;
        const float pad = (compact ? 2.0f : 4.0f) * scale_;
        const D2D1_RECT_F well = D2D1::RectF((std::max)(sb.left + 2.0f * scale_, left - pad),
            top - pad, (std::min)(sb.right - 2.0f * scale_, right + pad), bottom + pad);
        const float r = theme.radius_flyout * scale_;
        MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.045f}
                              : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.035f}, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), well.left, well.top,
                        well.right - well.left, well.bottom - well.top, r);
        MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.06f}
                              : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.05f}, brStrokeDivider_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(well.left + 0.5f, well.top + 0.5f,
            well.right - 0.5f, well.bottom - 0.5f), r, r), brStrokeDivider_.get(), 1.0f);
        break;
    }
    if (pill_slot >= 0) {
        const auto& slot = slots[static_cast<size_t>(pill_slot)];
        const auto& item = vm.sidebar[slot.group].items[slot.item];
        const uint64_t context = compact ? 1u : 2u;
        const int64_t key = static_cast<int64_t>(std::hash<std::wstring>{}(item.path)) ^
                            (static_cast<int64_t>(slot.group) << 48);
        const D2D1_RECT_F pill = sidebar_pill_.Update(context, key, slot.rc, motion_frame_,
                                                      motion_now_, 200);
        const float radius = (compact ? theme.radius_control : theme.radius_flyout) * scale_;
        // Same fills the static rows use (Painter::DrawSidebarItem / rail).
        D2D1_COLOR_F fill = theme.fill_selected;
        if (!compact) {
            fill = vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 26.0f / 255.0f}
                           : D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 20.0f / 255.0f};
        }
        MakeBrush(dc, fill, brFillSelected_);
        FillRoundedRect(dc, brFillSelected_.get(), pill.left, pill.top,
                        pill.right - pill.left, pill.bottom - pill.top, radius);
    }
    const auto isPillSlot = [&](const SidebarSlot& slot) {
        return pill_slot >= 0 && &slot == &slots[static_cast<size_t>(pill_slot)];
    };

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
            if (slot.kind == SidebarSlot::Rail) {
                // The section's own rail row: one icon stands in for the whole
                // section, and clicking it folds or unfolds the section.
                const auto& group = vm.sidebar[slot.group];
                if (IsHovered(vm, HitTestResult::SidebarHeader, slot.group)) {
                    MakeBrush(dc, theme.fill_hover, brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                        slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                        theme.radius_control * scale_);
                }
                std::wstring glyph = group.icon_glyph;
                if (glyph.empty() && !group.items.empty())
                    glyph = group.items.front().icon_glyph;
                if (glyph.empty()) glyph = kIconFolder;
                const int rail_svg = FluentSvgIdForGlyph(glyph);
                const float rail_icon = 20.0f * scale_;
                const float rail_pad_x = (slot.rc.right - slot.rc.left - rail_icon) * 0.5f;
                const float rail_pad_y = (slot.rc.bottom - slot.rc.top - rail_icon) * 0.5f;
                const D2D1_RECT_F rail_rc = D2D1::RectF(
                    slot.rc.left + rail_pad_x, slot.rc.top + rail_pad_y,
                    slot.rc.right - rail_pad_x, slot.rc.bottom - rail_pad_y);
                if (IsHighContrast() || rail_svg == 0 || !DrawFluentSvg(rail_svg, rail_rc, 1.0f, nullptr, true)) {
                    const std::wstring fallback = group.header.empty()
                        ? std::wstring(L"?") : group.header.substr(0, 1);
                    DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                        slot.rc.bottom - slot.rc.top, glyph, fallback,
                        theme.text_secondary, 0.92f);
                }
                // Tags read as their own group on the rail: a short divider
                // above the section, and when folded the first tag colors peek
                // out of the icon's corner so the section never looks empty.
                bool tag_section = false;
                for (const auto& it : group.items) {
                    if (it.is_tag) { tag_section = true; break; }
                }
                if (tag_section && slot.run > 0) {
                    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                    FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 10.0f * scale_,
                             slot.rc.top - 1.0f, slot.rc.right - slot.rc.left - 20.0f * scale_, 1.0f);
                }
                if (tag_section && group.collapsed && !IsHighContrast()) {
                    const float mini = 4.0f * scale_;
                    float mx = rail_rc.right + 1.0f * scale_;
                    const float my = rail_rc.bottom - 1.0f * scale_;
                    D2D1_COLOR_F halo = vm.dark ? D2D1_COLOR_F{0.125f, 0.125f, 0.125f, 1.0f}
                                                : D2D1_COLOR_F{0.97f, 0.973f, 0.957f, 1.0f};
                    int shown = 0;
                    for (const auto& it : group.items) {
                        if (!it.is_tag || it.tag_dot.a <= 0.0f) continue;
                        if (++shown > 3) break;
                        MakeBrush(dc, halo, brFillHover_);
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), mini + 1.5f * scale_,
                                                      mini + 1.5f * scale_), brFillHover_.get());
                        MakeBrush(dc, it.tag_dot, brFillHover_);
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), mini, mini),
                                        brFillHover_.get());
                        mx -= mini * 2.0f;
                    }
                }
                // Separator between the section row and its expanded rows.
                if (!group.collapsed) {
                    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                    FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 8.0f * scale_,
                             slot.rc.bottom - 1.0f,
                             slot.rc.right - slot.rc.left - 16.0f * scale_, 1.0f);
                }
                continue;
            }
            if (slot.kind == SidebarSlot::RailMore) {
                // "+N": rows of this section that did not fit on the rail.
                wchar_t label[16]{};
                swprintf_s(label, L"+%d", slot.batch);
                const float cw = 30.0f * scale_;
                const float ch = 20.0f * scale_;
                const float cx = (slot.rc.left + slot.rc.right) * 0.5f;
                const float cy = (slot.rc.top + slot.rc.bottom) * 0.5f;
                const D2D1_RECT_F chip = D2D1::RectF(cx - cw * 0.5f, cy - ch * 0.5f,
                                                     cx + cw * 0.5f, cy + ch * 0.5f);
                MakeBrush(dc, theme.fill_hover, brFillSelected_);
                FillRoundedRect(dc, brFillSelected_.get(), chip.left, chip.top, cw, ch, ch * 0.5f);
                IDWriteTextFormat* fmt = compositor_->SmallFormat();
                const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                MakeBrush(dc, theme.text_secondary, brText_);
                dc->DrawText(label, static_cast<UINT32>(wcslen(label)), fmt, chip, brText_.get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                fmt->SetTextAlignment(oldAlign);
                fmt->SetParagraphAlignment(oldPara);
                continue;
            }
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            const bool pill_row = isPillSlot(slot);
            const bool selected = (!pill_row && isSelectedSlot(slot)) || item.tab_active;
            const bool hovered = !pill_row && IsHovered(vm, HitTestResult::SidebarItem, slot.run);
            if (selected || hovered || slot.run == vm.sidebar_drop_index) {
                MakeBrush(dc, selected ? theme.fill_selected : theme.fill_hover, brFillSelected_);
                FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                    slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                    theme.radius_control * scale_);
            }
            if (item.is_tag) {
                // One colored dot per tag, the same color as the wide sidebar's.
                const float r = 6.0f * scale_;
                const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                      (slot.rc.top + slot.rc.bottom) * 0.5f);
                const D2D1_COLOR_F dot_color = item.tag_dot.a > 0 ? item.tag_dot : theme.accent;
                MakeBrush(dc, dot_color, brFillHover_);
                dc->FillEllipse(D2D1::Ellipse(c, r, r), brFillHover_.get());
                continue;
            }
            const D2D1_COLOR_F iconColor = item.icon_color.a > 0 ? item.icon_color
                                         : item.tag_dot.a > 0 ? item.tag_dot : theme.text;
            const int svg_id = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            const float icon = 20.0f * scale_;
            const float pad_x = (slot.rc.right - slot.rc.left - icon) * 0.5f;
            const float pad_y = (slot.rc.bottom - slot.rc.top - icon) * 0.5f;
            const D2D1_RECT_F icon_rc = D2D1::RectF(slot.rc.left + pad_x, slot.rc.top + pad_y,
                                                    slot.rc.right - pad_x, slot.rc.bottom - pad_y);
            ID2D1Bitmap* shell_icon = SidebarFolderIcon(item, icon_rc);
            if (shell_icon)
                dc->DrawBitmap(shell_icon, &icon_rc, 1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            if (!shell_icon && (IsHighContrast() || svg_id == 0 ||
                !DrawFluentSvg(svg_id, icon_rc, 1.0f, nullptr, true))) {
                DrawIconText(slot.rc.left, slot.rc.top, slot.rc.right - slot.rc.left,
                    slot.rc.bottom - slot.rc.top, item.icon_glyph, item.fallback_text,
                    iconColor, 0.92f);
            }
            if (item.tab_row && item.tab_number > 0 && item.tab_number <= 9) {
                // Rail tab icons carry their Ctrl+N number.
                IDWriteTextFormat* fmt = compositor_->SmallFormat();
                const wchar_t num[2] = {static_cast<wchar_t>(L'0' + item.tab_number), 0};
                const float d = 14.0f * scale_;
                const D2D1_RECT_F b = D2D1::RectF(icon_rc.right - d * 0.45f, icon_rc.bottom - d * 0.55f,
                                                  icon_rc.right + d * 0.55f, icon_rc.bottom + d * 0.45f);
                MakeBrush(dc, item.tab_active ? theme.accent : theme.text_secondary, brAccent_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((b.left + b.right) * 0.5f,
                    (b.top + b.bottom) * 0.5f), d * 0.5f, d * 0.5f), brAccent_.get());
                // Center with DirectWrite alignment on the shared format, then
                // put its alignment back for the other users.
                const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                MakeBrush(dc, HexColor(0xFFFFFF), brText_);
                dc->DrawText(num, 1, fmt, b, brText_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                             DWRITE_MEASURING_MODE_NATURAL);
                fmt->SetTextAlignment(oldAlign);
                fmt->SetParagraphAlignment(oldPara);
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
            header.glyph = vm.sidebar[slot.group].icon_glyph;
            const int header_svg = FluentSvgIdForGlyph(header.glyph);
            header.skip_glyph =
                !IsHighContrast() && header_svg != 0 && EnsureFluentSvg(header_svg, true);
            // The section being dragged stays lit for the whole gesture.
            const bool header_dragging = vm.sidebar_group_drag_id >= 0 &&
                vm.sidebar[slot.group].id == vm.sidebar_group_drag_id;
            // #80: on a navigable section the title is a link and lights on
            // its own; the rest of the row keeps the fold hover.
            const SidebarGroup& group = vm.sidebar[slot.group];
            const bool title_hot = group.navigable && !header_dragging &&
                IsHovered(vm, HitTestResult::SidebarHeader, slot.group, 1);
            header.state.hovered = header_dragging ||
                (!title_hot && IsHovered(vm, HitTestResult::SidebarHeader, slot.group));
            if (title_hot) {
                painter_.FillRoundedRect(
                    painter_.SidebarSectionHeaderTitleRect(slot.rc, group.header,
                                                           !group.icon_glyph.empty()),
                    6.0f * scale_, theme.fill_hover);
            }
            painter_.DrawSidebarSectionHeader(header);
            if (header.skip_glyph) {
                DrawFluentSvg(header_svg, painter_.SidebarSectionHeaderIconRect(slot.rc),
                              1.0f, nullptr, true);
            }
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
            // The empty state (dashed outline + hint) is drawn by DrawTrayDeck.
            // Do not assign a conditional std::wstring temporary to a
            // std::wstring_view: the view would dangle before DrawText runs.
            tray.count_label = countLabel;
            if (vm.tray_deck.total_count != 0) {
                tray.action_text = pulse::l10n::Get(vm.tray_deck.release_move
                    ? pulse::l10n::StringId::TrayReleaseMove
                    : pulse::l10n::StringId::TrayReleaseCopy);
            }
            tray.action_hovered =
                vm.hover_region == static_cast<int>(HitTestResult::TrayRelease);
            tray.item_count = trayCount;
            tray.state.hovered = vm.tray_drop;
            tray.expanded = true;
            painter_.DrawStagingTrayPanel(tray);
            DrawTrayDeck(vm, TrayDeckArea(slot.rc, vm.tray_deck, scale_), theme);
            if (TrayStaleRowH(vm.tray_deck, scale_) > 0.0f && vm.tray_deck.live_count > 0) {
                // Items moved or deleted outside Pulse: amber notice + Find / Remove.
                const D2D1_RECT_F row = TrayStaleRowRect(slot.rc, vm.tray_deck, scale_);
                const D2D1_RECT_F find = TrayStaleButtonRect(slot.rc, vm.tray_deck, scale_, 0);
                std::wstring note = pulse::l10n::Get(pulse::l10n::StringId::TrayStaleFormat);
                const size_t at = note.find(L"{n}");
                if (at != std::wstring::npos)
                    note.replace(at, 3, std::to_wstring(vm.tray_deck.stale_count));
                D2D1_RECT_F nr = row;
                nr.right = find.left - 4.0f * scale_;
                if (IDWriteFactory2* f = compositor_ ? compositor_->DwriteFactory() : nullptr) {
                    auto measure = [&](const std::wstring& s) {
                        return MeasureTextWidth(f, compositor_->SmallFormat(), s);
                    };
                    note = FitEndEllipsis(note, std::max(0.0f, nr.right - nr.left), measure);
                }
                const D2D1_COLOR_F amber = IsHighContrast() ? theme.text : D2D1::ColorF(0xE0A43A);
                painter_.DrawText(note, nr, compositor_->SmallFormat(), amber);
                for (int b = 0; b < 2; ++b) {
                    const D2D1_RECT_F rc = TrayStaleButtonRect(slot.rc, vm.tray_deck, scale_, b);
                    const bool hot = IsHovered(vm, HitTestResult::TrayStale, b);
                    D2D1_COLOR_F fill = theme.fill_hover;
                    fill.a *= hot ? 1.0f : 0.55f;
                    painter_.FillRoundedRect(rc, 6.0f * scale_, fill);
                    painter_.DrawText(pulse::l10n::Get(b == 0 ? pulse::l10n::StringId::TrayStaleFind
                                                            : pulse::l10n::StringId::TrayStaleRemove),
                                      rc, compositor_->SmallFormat(),
                                      hot ? theme.text : theme.text_secondary,
                                      fluent::HorizontalAlignment::Center);
                }
            }
            if (TrayDestRowH(vm.tray_deck, scale_) > 0.0f && vm.tray_deck.live_count > 0) {
                // Recent destinations: click copies the tray there, Shift moves.
                IDWriteFactory2* factory = compositor_ ? compositor_->DwriteFactory() : nullptr;
                for (int d = 0; d < static_cast<int>(vm.tray_deck.dests.size()); ++d) {
                    const TrayDestView& dest = vm.tray_deck.dests[static_cast<size_t>(d)];
                    const D2D1_RECT_F rc = TrayDestChipRect(slot.rc, vm.tray_deck, scale_, d);
                    const bool hot = !dest.missing && IsHovered(vm, HitTestResult::TrayDest, d);
                    D2D1_COLOR_F fill = theme.fill_hover;
                    fill.a *= hot ? 1.0f : 0.55f;
                    painter_.FillRoundedRect(rc, 6.0f * scale_, fill);
                    D2D1_COLOR_F fg = hot ? theme.text : theme.text_secondary;
                    if (dest.missing) fg.a *= 0.45f;
                    DrawIconText(rc.left + 4.0f * scale_, rc.top, 18.0f * scale_, rc.bottom - rc.top,
                                 L"\xE8B7", L"\u25A1", fg, 0.62f);
                    D2D1_RECT_F tr = rc;
                    tr.left += 24.0f * scale_;
                    tr.right -= 6.0f * scale_;
                    std::wstring label = dest.label;
                    if (factory && compositor_->SmallFormat()) {
                        auto measure = [&](const std::wstring& s) {
                            return MeasureTextWidth(factory, compositor_->SmallFormat(), s);
                        };
                        label = FitEndEllipsis(label, std::max(0.0f, tr.right - tr.left), measure);
                    }
                    painter_.DrawText(label, tr, compositor_->SmallFormat(), fg);
                }
            }
            continue;
        }
        if (slot.kind == SidebarSlot::TrayRelease) continue;
        if (slot.group < 0 || slot.item < 0) continue;

        const auto& item = vm.sidebar[slot.group].items[slot.item];
        fluent::ControlState state;
        state.selected = isSelectedSlot(slot);
        state.hovered = IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run);
        // Rows of the section being reordered stay lit like its header, and the
        // dragged pin row reads as held.
        if (vm.sidebar_group_drag_id >= 0 &&
            vm.sidebar[slot.group].id == vm.sidebar_group_drag_id) state.hovered = true;
        if (vm.sidebar_pin_drag_index >= 0 && slot.run == vm.sidebar_pin_drag_index)
            state.hovered = true;
        if (vm.tag_drag_group >= 0) state.hovered = false; // run indices shift mid-drag
        if (item.tab_row && item.tab_active) {
            // Active tab: raised card with an accent bar, like the top strip's
            // active tab plate.
            state.selected = false;
            const float r = theme.radius_control * scale_;
            MakeBrush(dc, vm.dark ? D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 0.10f} : HexColor(0xFFFFFF), brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), slot.rc.left, slot.rc.top,
                slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top, r);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(slot.rc.left + 0.5f, slot.rc.top + 0.5f,
                slot.rc.right - 0.5f, slot.rc.bottom - 0.5f), r, r), brStrokeCard_.get(), 1.0f);
            const float barH = (slot.rc.bottom - slot.rc.top) - 16.0f * scale_;
            MakeBrush(dc, item.tag_dot.a > 0.0f ? item.tag_dot : theme.accent, brAccent_);
            FillRoundedRect(dc, brAccent_.get(), slot.rc.left + 1.0f * scale_,
                slot.rc.top + 8.0f * scale_, 3.0f * scale_, barH, 1.5f * scale_);
        } else if (item.tab_row) {
            state.selected = false;
        }
        if (item.tab_row && item.flash > 0.0f) {
            MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.34f : 0.26f) * item.flash), brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), slot.rc.left, slot.rc.top,
                slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top, theme.radius_control * scale_);
        }
        if (isPillSlot(slot)) {
            // The gliding pill already painted this row's background.
            state.selected = false;
            state.hovered = false;
            state.pressed = false;
        }
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
            drive.skip_glyph = !IsHighContrast() && drive_svg != 0 && EnsureFluentSvg(drive_svg, true);
            painter_.DrawDriveSidebarItem(drive);
            if (drive.skip_glyph) {
                DrawFluentSvg(drive_svg, painter_.DriveSidebarItemIconRect(slot.rc),
                              1.0f, nullptr, true);
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
            // Resolve only cached artwork here; Shell lookups and conversion stay off the UI thread.
            const auto icon_rect = painter_.SidebarItemIconRect(slot.rc, item.status_dot);
            const float folder_size = std::min(icon_rect.right - icon_rect.left, icon_rect.bottom - icon_rect.top);
            const float folder_top = (icon_rect.top + icon_rect.bottom - folder_size) * 0.5f;
            const auto folder_rect = D2D1::RectF(icon_rect.left, folder_top,
                icon_rect.left + folder_size, folder_top + folder_size);
            ID2D1Bitmap* folder_icon = SidebarFolderIcon(item, folder_rect);
            row.skip_glyph = folder_icon || (!IsHighContrast() && row_svg != 0 && EnsureFluentSvg(row_svg, true));
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
            if (folder_icon) {
                dc->DrawBitmap(folder_icon, &folder_rect, 1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else if (row.skip_glyph) {
                DrawFluentSvg(row_svg, painter_.SidebarItemIconRect(slot.rc, item.status_dot),
                              1.0f, nullptr, true);
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
            if (unpin && item.tab_row) {
                // Tab rows: close button, shown on hover and on the active tab.
                const bool close_hot = IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
                if (state.hovered || item.tab_active) {
                    if (close_hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), unpin_rc.left, unpin_rc.top,
                            unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                            4.0f * scale_);
                    }
                    DrawIconText(unpin_rc.left, unpin_rc.top,
                        unpin_rc.right - unpin_rc.left, unpin_rc.bottom - unpin_rc.top,
                        kIconCloseSmall, L"x", close_hot ? theme.text : theme.text_secondary, 0.62f);
                }
            } else if (unpin) {
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

    // Insertion lines for the two reorders: the dragged section and the dragged
    // quick-access pin share one indicator.
    if (vm.sidebar_group_gap_line_y > 0.0f || vm.sidebar_pin_gap_line_y > 0.0f) {
        MakeBrush(dc, theme.accent, brAccent_);
        if (vm.sidebar_group_gap_line_y > 0.0f)
            DrawSidebarInsertionLine(dc, brAccent_.get(), sb, scale_,
                                     vm.sidebar_group_gap_line_y);
        if (vm.sidebar_pin_gap_line_y > 0.0f)
            DrawSidebarInsertionLine(dc, brAccent_.get(), sb, scale_,
                                     vm.sidebar_pin_gap_line_y);
    }

    painter_.DrawScrollbar(SidebarScrollbarSpec(vm, sb, scale_));
    dc->PopAxisAlignedClip();
}

void MainRenderer::DrawTrayDeck(const WindowViewModel& vm, const D2D1_RECT_F& panel_rc,
                                const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    const TrayDeckView& deck = vm.tray_deck;
    const bool hc = IsHighContrast();

    if (tray_formats_scale_ != scale_ || !tray_name_format_.get() || !tray_folder_format_.get()) {
        tray_name_format_.reset();
        tray_folder_format_.reset();
        typography::CreateTextFormat(factory,
            {typography::FontRole::Text, 13.0f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            &tray_name_format_);
        typography::CreateTextFormat(factory,
            {typography::FontRole::Text, 11.0f * scale_, DWRITE_FONT_WEIGHT_NORMAL},
            &tray_folder_format_);
        for (IDWriteTextFormat* f : {tray_name_format_.get(), tray_folder_format_.get()})
            if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        tray_formats_scale_ = scale_;
    }
    IDWriteTextFormat* nameFmt = tray_name_format_.get() ? tray_name_format_.get()
                                                         : compositor_->TextFormat();
    IDWriteTextFormat* subFmt = compositor_->SmallFormat();
    IDWriteTextFormat* folderFmt = tray_folder_format_.get() ? tray_folder_format_.get()
                                                             : compositor_->SmallFormat();
    ComPtr<ID2D1Factory> d2dFactory;
    dc->GetFactory(&d2dFactory);

    // Local brushes only: member brushes are reused by later drawing code.
    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(theme.text, &br);
    if (!br.get()) return;
    auto color = [&](const D2D1_COLOR_F& c, float alpha = 1.0f) -> ID2D1SolidColorBrush* {
        D2D1_COLOR_F v = c;
        v.a *= alpha;
        br->SetColor(v);
        return br.get();
    };
    auto rgba = [](uint32_t rgb, float a) {
        return D2D1::ColorF(rgb, a);
    };

    // -------------------------------------------------------------------
    // Empty state: dashed outline + hint, fading in as exiting cards leave.
    // -------------------------------------------------------------------
    float ghost_alpha = 0.0f;
    for (const auto& c : deck.cards)
        if (c.ghost) ghost_alpha = std::max(ghost_alpha, std::clamp(c.opacity, 0.0f, 1.0f));
    if (deck.live_count == 0) {
        const float a = 1.0f - ghost_alpha;
        if (a > 0.01f) {
            const D2D1_RECT_F box = D2D1::RectF(panel_rc.left + 10.0f * scale_,
                panel_rc.top + 36.0f * scale_, panel_rc.right - 10.0f * scale_,
                panel_rc.bottom - 10.0f * scale_);
            if (box.bottom - box.top > 24.0f * scale_) {
                ComPtr<ID2D1StrokeStyle> dash;
                if (d2dFactory.get()) {
                    const float dashes[] = {4.0f, 3.0f};
                    d2dFactory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                        dashes, 2, &dash);
                }
                const D2D1_COLOR_F edge = vm.tray_drop ? theme.accent
                    : (vm.dark ? rgba(0x8CA0BE, 0.35f) : rgba(0x506482, 0.30f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.75f,
                    box.top + 0.75f, box.right - 0.75f, box.bottom - 0.75f),
                    12.0f * scale_, 12.0f * scale_), color(edge, a), 1.5f * scale_, dash.get());
                const float mid = (box.top + box.bottom) * 0.5f;
                D2D1_COLOR_F t1 = theme.text_secondary; t1.a *= a;
                D2D1_COLOR_F t2 = theme.text_secondary; t2.a *= 0.75f * a;
                painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::StagingTrayEmpty),
                    D2D1::RectF(box.left + 8.0f * scale_, mid - 20.0f * scale_,
                                box.right - 8.0f * scale_, mid),
                    compositor_->TextFormat(), t1, fluent::HorizontalAlignment::Center);
                painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::StagingTrayHelper),
                    D2D1::RectF(box.left + 8.0f * scale_, mid + 1.0f * scale_,
                                box.right - 8.0f * scale_, mid + 19.0f * scale_),
                    compositor_->SmallFormat(), t2, fluent::HorizontalAlignment::Center);
            }
        }
    }
    if (deck.cards.empty() && deck.puffs.empty()) return;

    const TrayStackGeom g = TrayStackGeometry(panel_rc, scale_, deck.thumb_dip);
    const float cw = g.card.right - g.card.left;
    const float ch = g.card.bottom - g.card.top;
    const float pad = 10.0f * scale_;
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);

    const D2D1_COLOR_F cardFill = hc ? theme.surface_flyout
                                : vm.dark ? rgba(0x1D2635, 1.0f) : rgba(0xFFFFFF, 1.0f);
    const D2D1_COLOR_F cardEdge = hc ? theme.text
                                : vm.dark ? rgba(0xFFFFFF, 0.07f) : rgba(0x000000, 0.06f);
    const D2D1_COLOR_F nameColor = vm.dark ? rgba(0xEEF3F9, 1.0f) : rgba(0x1B1F24, 1.0f);
    const D2D1_COLOR_F subColor = vm.dark ? rgba(0x9FB0C4, 1.0f) : rgba(0x5B6675, 1.0f);
    const D2D1_COLOR_F folderColor = vm.dark ? rgba(0x71829A, 1.0f) : rgba(0x8A95A3, 1.0f);
    const std::wstring moveLabel = pulse::l10n::Get(pulse::l10n::StringId::TrayIntentMove);
    const std::wstring copyLabel = pulse::l10n::Get(pulse::l10n::StringId::TrayIntentCopy);

    auto draw_card = [&](const TrayCardView& card, bool is_top) {
        const TrayCardPose pose = TrayCardPoseOf(g, card, deck.spread);
        if (pose.opacity <= 0.005f) return;
        dc->SetTransform(pose.m * old);
        const bool layered = pose.opacity < 0.995f;
        if (layered) {
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pose.opacity),
                nullptr);
        }
        const D2D1_RECT_F rc = g.card;
        // Soft shadow: a few widening translucent layers below the card.
        if (!hc) {
            const float base = vm.dark ? 0.11f : 0.05f;
            for (int k = 3; k >= 1; --k) {
                const float grow = static_cast<float>(k) * 1.6f * scale_;
                const float drop = (2.0f + 1.5f * static_cast<float>(k)) * scale_;
                FillRoundedRect(dc, color(rgba(0x000000, base * (1.0f - 0.22f * (k - 1)))),
                    rc.left - grow, rc.top - grow + drop, cw + grow * 2.0f, ch + grow * 2.0f,
                    g.radius + grow);
            }
        }
        FillRoundedRect(dc, color(cardFill), rc.left, rc.top, cw, ch, g.radius);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f,
            rc.right - 0.5f, rc.bottom - 0.5f), g.radius, g.radius), color(cardEdge), 1.0f);

        // Thumbnail tile.
        const float tr = 9.0f * scale_;
        const D2D1_RECT_F thumb = D2D1::RectF(rc.left + pad, (rc.top + rc.bottom - g.thumb) * 0.5f,
            rc.left + pad + g.thumb, (rc.top + rc.bottom + g.thumb) * 0.5f);
        const std::wstring chip = TypeChipLabel(card.name, card.is_dir);
        const uint32_t tint = card.is_dir ? 0xF5B942u : TypeChipRgb(chip);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (d2dFactory.get())
            d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(thumb, tr, tr), &clip);
        if (clip.get()) {
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
        }
        FillRoundedRect(dc, color(rgba(tint, vm.dark ? 0.16f : 0.12f)), thumb.left, thumb.top,
                        g.thumb, g.thumb, tr);
        const bool drewThumbnail = !card.missing && !card.is_dir &&
            thumbnail_cache_.Draw(dc, thumb, card.path, card.attrs,
                static_cast<uint32_t>(std::clamp(std::lround(g.thumb), 32l, 256l)),
                0, 0, 0, 1.0f) == PreviewDrawResult::Bitmap;
        if (!drewThumbnail) {
            const float icon = std::round(g.thumb * 0.72f);
            if (ID2D1Bitmap* bitmap = icon_cache_.BitmapFor(card.missing ? L"" : card.path,
                    card.name, card.is_dir, card.attrs, icon)) {
                const float ix = (thumb.left + thumb.right - icon) * 0.5f;
                const float iy = (thumb.top + thumb.bottom - icon) * 0.5f;
                const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + icon, iy + icon);
                dc->DrawBitmap(bitmap, &dest, card.missing ? 0.45f : 1.0f,
                               D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
        }
        if (clip.get()) dc->PopLayer();

        // Text block: name (+ cut marker), size · type, folder.
        const float tx = thumb.right + pad;
        const float reserve = is_top ? 20.0f * scale_ * std::clamp(card.hover, 0.0f, 1.0f) : 0.0f;
        const float tw = std::max(0.0f, rc.right - pad - reserve - tx);
        const float lineName = 18.0f * scale_, lineSub = 16.0f * scale_, lineDir = 15.0f * scale_;
        float ty = (rc.top + rc.bottom) * 0.5f - (lineName + lineSub + lineDir) * 0.5f;
        const float nameW = tw;
        DrawTextEndEllipsis(dc, factory, nameFmt,
            color(card.missing ? theme.text_disabled : nameColor), card.name, tx, ty, nameW, lineName);
        // Intent chip (bottom-right): what release does with this card's
        // batch. Orange = move, neutral = copy; clickable on the top card.
        const D2D1_RECT_F chipRc = TrayIntentRect(g);
        if (card.batch >= 0) {
            const std::wstring& chipLabel = card.cut ? moveLabel : copyLabel;
            const bool chipHot = is_top &&
                vm.hover_region == static_cast<int>(HitTestResult::TrayIntent);
            const float bw = chipRc.right - chipRc.left, bh = chipRc.bottom - chipRc.top;
            const D2D1_COLOR_F chipFill = card.cut
                ? (vm.dark ? rgba(0xFFAA3C, chipHot ? 0.30f : 0.18f)
                           : rgba(0xD67800, chipHot ? 0.22f : 0.12f))
                : (vm.dark ? rgba(0xFFFFFF, chipHot ? 0.16f : 0.08f)
                           : rgba(0x000000, chipHot ? 0.10f : 0.05f));
            FillRoundedRect(dc, color(chipFill), chipRc.left, chipRc.top, bw, bh, bh * 0.5f);
            if (is_top && card.hover > 0.01f && !hc) {
                D2D1_COLOR_F edge = card.cut ? (vm.dark ? rgba(0xFFB454, 0.55f) : rgba(0xB25E00, 0.45f))
                                             : theme.text_secondary;
                edge.a *= 0.6f * std::clamp(card.hover, 0.0f, 1.0f);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(chipRc.left + 0.5f,
                    chipRc.top + 0.5f, chipRc.right - 0.5f, chipRc.bottom - 0.5f),
                    bh * 0.5f, bh * 0.5f), color(edge), 1.0f);
            }
            ComPtr<IDWriteTextLayout> bl;
            if (factory && SUCCEEDED(factory->CreateTextLayout(chipLabel.c_str(),
                    static_cast<UINT32>(chipLabel.size()), folderFmt, bw, bh, &bl)) && bl.get()) {
                bl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                bl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                bl->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, static_cast<UINT32>(chipLabel.size())});
                dc->DrawTextLayout(D2D1::Point2F(chipRc.left, chipRc.top), bl.get(),
                    color(card.cut ? (vm.dark ? rgba(0xFFB454, 1.0f) : rgba(0xB25E00, 1.0f))
                                   : theme.text_secondary));
            }
        }
        ty += lineName;
        std::wstring sub;
        if (!card.is_dir && card.size > 0)
            sub = pulse::format::ByteSize(card.size, true) + L" \xB7 ";
        sub += FormatListType(card.name, card.is_dir);
        DrawTextEndEllipsis(dc, factory, subFmt, color(subColor), sub, tx, ty, tw, lineSub);
        ty += lineSub;
        if (!card.folder.empty() && factory) {
            auto measure = [&](const std::wstring& s) { return MeasureTextWidth(factory, folderFmt, s); };
            // The last line shares its row with the intent chip.
            const float dirW = card.batch >= 0
                ? std::max(0.0f, std::min(tw, chipRc.left - 6.0f * scale_ - tx)) : tw;
            const auto [head, last] = MiddleEllipsisPath(card.folder, dirW, measure);
            DrawTextEndEllipsis(dc, factory, folderFmt, color(folderColor), head + last,
                                tx, ty, dirW, lineDir);
        }

        // Peeking layers read as "further away": dim them.
        if (pose.dim > 0.001f && !hc) {
            FillRoundedRect(dc, color(rgba(0x000000, pose.dim * (vm.dark ? 0.22f : 0.035f))),
                            rc.left, rc.top, cw, ch, g.radius);
        }

        // Close badge on the hovered top card.
        if (is_top && card.hover > 0.01f) {
            const float h = std::clamp(card.hover, 0.0f, 1.0f);
            const D2D1_POINT_2F c = TrayCloseCentre(g);
            const float r = 10.0f * scale_;
            const bool hot = vm.hover_region == static_cast<int>(HitTestResult::TrayItemRemove);
            const D2D1_COLOR_F fill = hot ? theme.danger
                : (vm.dark ? rgba(0xFFFFFF, 0.09f) : rgba(0x000000, 0.06f));
            dc->FillEllipse(D2D1::Ellipse(c, r, r), color(fill, h));
            D2D1_COLOR_F glyph = hot ? HexColor(0xFFFFFF) : theme.text_secondary;
            glyph.a *= h;
            DrawIconText(c.x - r, c.y - r, r * 2.0f, r * 2.0f, kIconCloseSmall, L"x", glyph, 0.55f);
        }
        if (layered) dc->PopLayer();
        dc->SetTransform(old);
    };

    if (deck.comparing) {
        // Two staged files side by side: where / modified / size / content.
        const TrayCompareGeom cg = TrayCompareGeometry(panel_rc, scale_, deck.thumb_dip);
        const TrayCompareView& cv = deck.compare;
        dc->FillRoundedRectangle(D2D1::RoundedRect(cg.box, g.radius, g.radius), color(cardFill));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(cg.box, g.radius, g.radius), color(cardEdge),
                                 1.0f);
        auto fit = [&](const std::wstring& t, const D2D1_RECT_F& rc, IDWriteTextFormat* f) {
            if (!factory) return t;
            return FitEndEllipsis(t, std::max(0.0f, rc.right - rc.left),
                [&](const std::wstring& x) { return MeasureTextWidth(factory, f, x); });
        };
        using TrayStr = pulse::l10n::StringId;
        const TrayStr labels[4] = {TrayStr::TrayCmpLocation, TrayStr::TrayCmpModified, TrayStr::TrayCmpSize,
                               TrayStr::TrayCmpContent};
        for (int row = 1; row <= 4; ++row) {
            const D2D1_RECT_F rc = TrayCompareCell(cg, row, -1);
            painter_.DrawText(fit(pulse::l10n::Get(labels[row - 1]), rc, subFmt), rc, subFmt,
                              folderColor);
        }
        const std::wstring newer = L" \x2191"; // later mtime (2 s tolerance)
        for (int c = 0; c < 2; ++c) {
            D2D1_RECT_F rc = TrayCompareCell(cg, 0, c);
            painter_.DrawText(fit(cv.name[c], rc, nameFmt), rc, nameFmt, nameColor);
            rc = TrayCompareCell(cg, 1, c);
            painter_.DrawText(fit(cv.where[c], rc, subFmt), rc, subFmt, subColor);
            rc = TrayCompareCell(cg, 2, c);
            const bool is_newer = cv.newer == c;
            painter_.DrawText(fit(is_newer ? cv.time[c] + newer : cv.time[c], rc, subFmt), rc,
                              subFmt, is_newer ? theme.accent
                                               : (cv.newer >= 0 ? folderColor : subColor));
            rc = TrayCompareCell(cg, 3, c);
            painter_.DrawText(fit(cv.size[c], rc, subFmt), rc, subFmt,
                              cv.size_differs ? nameColor : subColor);
        }
        const D2D1_RECT_F crc = TrayCompareCell(cg, 4, 2);
        std::wstring ctext;
        D2D1_COLOR_F ccol = subColor;
        switch (cv.content) {
        case 0: {
            ctext = pulse::l10n::Get(TrayStr::TrayCmpCheck);
            ccol = theme.accent;
            if (vm.hover_region == static_cast<int>(HitTestResult::TrayCompare) &&
                vm.hover_control_index == 1) {
                painter_.FillRoundedRect(crc, 6.0f * scale_, theme.fill_hover);
            }
            break;
        }
        case 1: ctext = pulse::l10n::Get(TrayStr::TrayCmpChecking); break;
        case 2: ctext = pulse::l10n::Get(TrayStr::TrayCmpSame); ccol = theme.accent; break;
        case 3: {
            if (cv.text == 1) {
                ctext = pulse::l10n::Get(TrayStr::TrayCmpViewDiff);
                ccol = theme.accent;
                if (vm.hover_region == static_cast<int>(HitTestResult::TrayCompare) &&
                    vm.hover_control_index == 2)
                    painter_.FillRoundedRect(crc, 6.0f * scale_, theme.fill_hover);
            } else {
                ctext = pulse::l10n::Get(cv.text == 0 ? TrayStr::TrayCmpBinary
                                                      : TrayStr::TrayCmpDifferent);
                ccol = theme.danger;
            }
            break;
        }
        default: ctext = pulse::l10n::Get(TrayStr::TrayCmpFailed); break;
        }
        D2D1_RECT_F ctr = crc;
        ctr.left += 4.0f * scale_;
        painter_.DrawText(fit(ctext, ctr, subFmt), ctr, subFmt, ccol);
    } else {
        const std::vector<int> order = TrayCardPaintOrder(deck);
        for (const int idx : order) {
            const TrayCardView& card = deck.cards[static_cast<size_t>(idx)];
            draw_card(card, !card.ghost && idx == 0);
        }
    }

    // Dismiss smoke: radial puffs drifting out of the × badge.
    if (!deck.puffs.empty() && !hc) {
        const D2D1_POINT_2F anchor = TrayCloseCentre(g);
        const D2D1_COLOR_F inner = vm.dark ? rgba(0xFFFFFF, 1.0f) : rgba(0xE3E8EF, 1.0f);
        const D2D1_COLOR_F outer = vm.dark ? rgba(0xD8DEE8, 1.0f) : rgba(0xC3CCD8, 1.0f);
        D2D1_GRADIENT_STOP stops[] = {
            {0.0f, inner}, {0.55f, outer},
            {0.72f, D2D1::ColorF(outer.r, outer.g, outer.b, 0.0f)},
            {1.0f, D2D1::ColorF(outer.r, outer.g, outer.b, 0.0f)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> puff;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &coll)) && coll.get())
            dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                anchor, D2D1::Point2F(), 1.0f, 1.0f), coll.get(), &puff);
        if (puff.get()) {
            for (const TrayPuffView& p : deck.puffs) {
                if (p.t <= 0.0f || p.t >= 1.0f) continue;
                const float e = 1.0f - std::pow(1.0f - p.t, 3.0f);
                const float alpha = p.t < 0.7f ? 0.95f - 0.15f * (p.t / 0.7f)
                                               : 0.8f * (1.0f - (p.t - 0.7f) / 0.3f);
                const float radius = 9.0f * scale_ * (0.2f + (p.size - 0.2f) * e);
                const D2D1_POINT_2F c = D2D1::Point2F(
                    anchor.x + std::cos(p.angle) * p.dist * e * scale_,
                    anchor.y + (std::sin(p.angle) * p.dist - 6.0f) * e * scale_);
                puff->SetCenter(c);
                puff->SetGradientOriginOffset(D2D1::Point2F(-radius * 0.3f, -radius * 0.3f));
                puff->SetRadiusX(radius);
                puff->SetRadiusY(radius);
                puff->SetOpacity(std::clamp(alpha, 0.0f, 1.0f));
                dc->FillEllipse(D2D1::Ellipse(c, radius, radius), puff.get());
            }
        }
    }

    // Footer: pager (‹ n / N ›) + totals left, 清空 right. Skip while only
    // exiting ghosts remain (tray already cleared).
    if (deck.live_count > 0) {
        const std::wstring index_text = TrayIndexText(deck);
        const TrayFooterGeom f = TrayFooterGeometry(panel_rc, scale_,
            deck.comparing ? 1 : deck.total_count,
            MeasureTextWidth(factory, compositor_->SmallFormat(), index_text), deck.can_compare);
        if (f.pager) {
            auto pager_button = [&](const D2D1_RECT_F& rc, const wchar_t* glyph,
                                    const wchar_t* fallback, HitTestResult::Region region) {
                const bool hot = vm.hover_region == static_cast<int>(region);
                if (hot) {
                    D2D1_COLOR_F hf = theme.fill_hover;
                    painter_.FillRoundedRect(rc, 6.0f * scale_, hf);
                }
                DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                             glyph, fallback, hot ? theme.text : theme.text_secondary, 0.62f);
            };
            pager_button(f.prev, L"\xE76B", L"\u2039", HitTestResult::TrayPrev);
            pager_button(f.next, kIconChevronRight, L"\u203A", HitTestResult::TrayNext);
            painter_.DrawText(index_text, f.index, compositor_->SmallFormat(),
                              theme.text_secondary, fluent::HorizontalAlignment::Center);
        }
        wchar_t footer[96]{};
        const std::wstring size_text = pulse::format::ByteSize(deck.total_size, true);
        if (deck.batch_count > 1)
            swprintf_s(footer,
                pulse::l10n::Get(pulse::l10n::StringId::TotalSizeBatchesFormat).c_str(),
                size_text.c_str(), deck.batch_count);
        else
            swprintf_s(footer,
                pulse::l10n::Get(pulse::l10n::StringId::TotalSizeFormat).c_str(),
                size_text.c_str());
        std::wstring totals = f.pager ? std::wstring(L"\xB7 ") + footer : std::wstring(footer);
        if (factory) {
            auto measure = [&](const std::wstring& s) {
                return MeasureTextWidth(factory, compositor_->SmallFormat(), s);
            };
            const float avail = std::max(0.0f, f.totals.right - f.totals.left);
            // Next to the pager the sentence rarely fits: fall back to the bare
            // size ("1.6 MB") before cutting anything with an ellipsis.
            if (f.pager && measure(totals) > avail) totals = size_text;
            // A lone "…" next to the pager says nothing: leave the slot empty.
            if (f.pager && measure(totals) > avail) totals.clear();
            totals = FitEndEllipsis(totals, avail, measure);
        }
        painter_.DrawText(totals, f.totals, compositor_->SmallFormat(), theme.text_secondary);
        if (deck.can_compare) {
            const bool cmp_hot = vm.hover_region == static_cast<int>(HitTestResult::TrayCompare) &&
                                 vm.hover_control_index == 0;
            const float cr = (f.compare.bottom - f.compare.top) * 0.5f;
            if (deck.comparing) {
                D2D1_COLOR_F on = theme.accent;
                on.a *= cmp_hot ? 0.28f : 0.18f;
                painter_.FillRoundedRect(f.compare, cr, on);
            } else if (cmp_hot) {
                painter_.FillRoundedRect(f.compare, cr, theme.fill_hover);
            }
            painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::TrayCompare), f.compare,
                              compositor_->SmallFormat(),
                              deck.comparing || cmp_hot ? theme.accent : theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        }
        const bool clear_hovered =
            vm.hover_region == static_cast<int>(HitTestResult::TrayClear);
        const float clear_w = 72.0f * scale_;
        const D2D1_RECT_F clear_rc = D2D1::RectF(panel_rc.right - 10.0f * scale_ - clear_w,
            f.row.top, panel_rc.right - 10.0f * scale_, f.row.bottom);
        const float clear_r = (clear_rc.bottom - clear_rc.top) * 0.5f;
        if (clear_hovered) {
            D2D1_COLOR_F hot_fill = theme.danger;
            hot_fill.a *= 0.12f;
            painter_.FillRoundedRect(clear_rc, clear_r, hot_fill);
        }
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::ClearAll), clear_rc,
                          compositor_->SmallFormat(), theme.danger,
                          fluent::HorizontalAlignment::Center);
    }
}
float MainRenderer::SidebarMaxScroll(const WindowViewModel& vm, float window_w,
                                     float window_h) const {
    const D2D1_RECT_F sb = SidebarRect(window_w, window_h);
    const auto bar = SidebarScrollbarSpec(vm, sb, scale_);
    return bar.enabled ? std::max(0.0f, bar.content_extent - bar.viewport_extent) : 0.0f;
}

bool MainRenderer::SidebarScrollbarGeometry(const WindowViewModel& vm, float window_w,
                                            float window_h, D2D1_RECT_F& track,
                                            D2D1_RECT_F& thumb, float& max_scroll) const {
    const auto bar = SidebarScrollbarSpec(vm, SidebarRect(window_w, window_h), scale_);
    track = bar.viewport;
    thumb = fluent::ScrollbarThumbRect(bar, scale_);
    max_scroll = bar.enabled ? std::max(0.0f, bar.content_extent - bar.viewport_extent) : 0.0f;
    return max_scroll > 0.0f && thumb.bottom > thumb.top;
}

} // namespace pulse::ui
