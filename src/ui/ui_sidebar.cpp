// ui_sidebar.cpp — Sidebar and staging-tray deck.
#include "ui_renderer.h"
#ifdef PULSE_STARTUP_TRACE_DIAGNOSTICS
#include "../app/startup_trace.h"
#define PULSE_SIDEBAR_MARK(label) app::startup_trace::Mark(label)
#else
#define PULSE_SIDEBAR_MARK(label) ((void)0)
#endif
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "command_icons.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../app/keymap.h"
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

// Sidebar motion (animation phase 7, pulse_motion_v2): the selection pill
// stretches toward the new row - its leading edge on a stiff spring, the
// trailing edge on a softer one - a hover plate glides under the pointer, and
// an item's expand chevron turns. The state lives here, per renderer; frames
// come from the renderer's frame pump (RequestMotionFrame), so idle stays
// idle: nothing ticks once everything has settled.
constexpr motion::SpringSpec kPillLead{900.0f, 60.0f};

constexpr float kPillMaxStretch = 1.9f;  // times the row height
constexpr float kHoverFadeInMs = 90.0f, kHoverFadeOutMs = 120.0f;

struct ChevronTurn {
    bool expanded = false;
    uint64_t at = 0;
};

struct SidebarMotion {
    motion::SpringRectMotion lead, hover;
    uint64_t hover_context = 0;
    float hover_alpha = 0.0f;
    D2D1_RECT_F hover_rect{}, hover_target{};
    int64_t hover_key = 0;
    std::wstring workspace_preview_path;
    uint64_t workspace_preview_start = 0;
    D2D1_RECT_F workspace_preview_anchor{};
    float hover_radius = 0.0f;
    uint64_t hover_last = 0;
    std::unordered_map<std::wstring, ChevronTurn> chevrons;
    // Quick-access reorder: per-row make-room offsets (px) keyed by run, the
    // held row's lift start and the last frame time for the follow filter.
    std::unordered_map<int, float> pin_offsets;
    int pin_run = -1;
    // Count roll + gulp per row path (motion v2 "dropbin"). epoch counts the
    // expanded sidebar's frames so only a change seen frame-to-frame plays.
    struct CountRoll {
        std::wstring last, prev;
        uint64_t start = 0;
        uint64_t epoch = 0;
        bool down = false;
    };
    std::unordered_map<std::wstring, CountRoll> counts;
    uint64_t count_epoch = 0;
    // Star flight (motion v2 "star"): launch point, start, and the 已加星标
    // row as last painted (valid when star_dest_epoch == count_epoch).
    D2D1_POINT_2F star_from{};
    uint64_t star_start = 0;
    bool star_landed = false;
    D2D1_RECT_F star_dest{};
    uint64_t star_dest_epoch = 0;
    uint64_t pin_lift_start = 0;
    uint64_t pin_last = 0;
    bool animations = true;
    uint64_t animations_checked = 0;
};

SidebarMotion& SidebarMotionFor(const void* renderer) {
    static std::unordered_map<const void*, SidebarMotion> motions;
    return motions[renderer];
}

// The pill spans both springs: leading edge from the stiff one, trailing edge
// from the soft one, cut to kPillMaxStretch rows and a touch thinner while
// stretched.
D2D1_RECT_F StretchPill(const D2D1_RECT_F& lead, const D2D1_RECT_F& trail, float scale) {
    D2D1_RECT_F pill = D2D1::RectF((std::min)(lead.left, trail.left), (std::min)(lead.top, trail.top),
                                   (std::max)(lead.right, trail.right), (std::max)(lead.bottom, trail.bottom));
    const float row_h = (std::max)(1.0f, lead.bottom - lead.top);
    const float max_h = row_h * kPillMaxStretch;
    if (pill.bottom - pill.top > max_h) {
        if (lead.top >= trail.top) pill.top = pill.bottom - max_h;  // moving down: the bottom leads
        else pill.bottom = pill.top + max_h;
    }
    const float stretch = (std::clamp)((pill.bottom - pill.top - row_h) / row_h, 0.0f, 1.0f);
    pill.left += 2.0f * scale * stretch;
    pill.right -= 2.0f * scale * stretch;
    return pill;
}

float ChevronAngle(SidebarMotion& m, const std::wstring& key, bool expanded, uint64_t now, bool& moving) {
    auto [it, fresh] = m.chevrons.try_emplace(key);
    ChevronTurn& turn = it->second;
    if (fresh) {
        turn.expanded = expanded;
        turn.at = 0;
    } else if (turn.expanded != expanded) {
        turn.expanded = expanded;
        turn.at = m.animations ? now : 0;
    }
    float p = 1.0f;
    if (turn.at) {
        const float t = static_cast<float>(now - turn.at);
        p = motion::SpringProgress(motion::spring::Snappy, t);
        if (t < static_cast<float>(motion::SpringSettleMs(motion::spring::Snappy))) moving = true;
        else turn.at = 0;
    }
    return 90.0f * (expanded ? p : 1.0f - p);
}

} // namespace

bool SidebarSelectionMovingForTest(const MainRenderer* renderer) {
    const auto& m = SidebarMotionFor(renderer);
    return m.lead.Active(motion::NowMs());
}
D2D1_RECT_F SidebarHoverPositionForTest(const MainRenderer* renderer) {
    return SidebarMotionFor(renderer).hover_rect;
}

D2D1_RECT_F MainRenderer::SidebarRect(float w, float h) const {
    float top = title_bar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    const float right = SidebarPeekVisible(w) ? SidebarFullWidth(w) : SidebarOwnWidth(w);
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
    // Motion v2 "rail": while the sidebar folds, the icon column stays solid
    // and everything right of it (labels, counts) fades - late on the way
    // out to full width, early on the way into the rail.
    bool label_layer = false;
    if (collapse_anim_ && rect.right / scale_ >= kSidebarRailWindowDip) {
        const float open = std::clamp((1.0f - collapse_value_ - 0.35f) / 0.65f, 0.0f, 1.0f);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(0, 0, 0, 1.0f)},
            {0.5f, D2D1::ColorF(0, 0, 0, 1.0f)},
            {1.0f, D2D1::ColorF(0, 0, 0, open * open)},
        };
        ComPtr<ID2D1GradientStopCollection> stop_set;
        ComPtr<ID2D1LinearGradientBrush> fade;
        const float x0 = sb.left + 38.0f * scale_;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &stop_set)) && stop_set.get() &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0 - 8.0f * scale_, 0.0f),
                                                    D2D1::Point2F(x0 + 8.0f * scale_, 0.0f)),
                stop_set.get(), &fade)) && fade.get()) {
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(sb_clip, nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f, fade.get());
            dc->PushLayer(lp, nullptr);
            label_layer = true;
        }
    }

    // The sidebar sits on the shared sheet painted by Render(); the pane cards
    // beside it provide the edge, so it has no fill or divider of its own.

    std::vector<SidebarSlot> slots;
    PULSE_SIDEBAR_MARK("sidebar_layout_begin");
    LayoutSidebar(vm, sb, scale_, slots);
    PULSE_SIDEBAR_MARK("sidebar_layout_end");
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
    const bool reordering = vm.sidebar_pin_drag_index >= 0 || vm.sidebar_group_drag_id >= 0;
    const int pill_slot = !reordering && !IsHighContrast() ? selected_slot : -1;
    SidebarMotion& sm = SidebarMotionFor(this);
    if (motion_now_ - sm.animations_checked > 2000 || !sm.animations_checked) {
        // The Windows animation switch, re-read every couple of seconds.
        sm.animations_checked = motion_now_ ? motion_now_ : 1;
        sm.animations = motion::SystemAnimationsEnabled();
        sm.lead.SetAnimationsEnabled(sm.animations);
        sm.hover.SetAnimationsEnabled(sm.animations);
    }
    bool sidebar_moving = false;
    // One hover plate glides between rows (the menus' plate); reorders and
    // high contrast keep each row's own static hover.
    const bool plate_live = !reordering && !IsHighContrast();
    int hover_slot = -1;
    if (plate_live) {
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.kind != SidebarSlot::Item && slot.kind != SidebarSlot::Drive &&
                slot.kind != SidebarSlot::Tag) continue;
            if (slot.group < 0 || slot.item < 0) continue;
            if (static_cast<int>(k) == pill_slot) continue;
            const auto& item = vm.sidebar[slot.group].items[slot.item];
            if (item.tab_row && item.tab_active) continue;  // the raised card is its own hover
            if (IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
                IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
                IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run)) {
                hover_slot = static_cast<int>(k);
                break;
            }
        }
    }
    {
        const uint64_t context = compact ? 1u : 2u;
        const float dt = static_cast<float>((std::min<uint64_t>)(motion_now_ - sm.hover_last, 64));
        sm.hover_last = motion_now_;
        if (hover_slot >= 0) {
            const auto& slot = slots[static_cast<size_t>(hover_slot)];
            // Coming from nothing, the plate appears at the row (no glide
            // from wherever it was last).
            if (sm.hover_alpha <= 0.0f) ++sm.hover_context;
            sm.hover_key = slot.run;
            sm.hover_target = slot.rc;
            sm.hover_rect = sm.hover.Update(context * 0x10000u + sm.hover_context, slot.run, slot.rc,
                                            motion_frame_, motion_now_, motion::spring::Plate);
            sm.hover_radius = (compact ? theme.radius_control : theme.radius_flyout) * scale_;
            sm.hover_alpha = sm.animations ? (std::min)(1.0f, sm.hover_alpha + dt / kHoverFadeInMs) : 1.0f;
        } else {
            // Fading out where it was; kept in step so a pointer coming back
            // mid-fade glides on from there.
            sm.hover_alpha = sm.animations && plate_live ? (std::max)(0.0f, sm.hover_alpha - dt / kHoverFadeOutMs) : 0.0f;
            if (sm.hover_alpha > 0.0f)
                sm.hover_rect = sm.hover.Update(context * 0x10000u + sm.hover_context, sm.hover_key, sm.hover_target,
                                                motion_frame_, motion_now_, motion::spring::Plate);
            else
                sm.hover.Hide();
        }
        if (sm.hover.Active(motion_now_) || (sm.hover_alpha > 0.0f && sm.hover_alpha < 1.0f)) sidebar_moving = true;
    }
    const auto isSelectedSlot = [&](const SidebarSlot& slot) {
        if (slot.group >= 0 && slot.item >= 0) {
            const auto& entry = vm.sidebar[slot.group].items[slot.item];
            if (entry.workspace_index >= 0) return entry.workspace_active;
        }
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
    if (sm.hover_alpha > 0.0f) {
        D2D1_COLOR_F fill = theme.fill_hover;
        fill.a *= sm.hover_alpha;
        painter_.FillRoundedRect(sm.hover_rect, sm.hover_radius, fill);
    }
    if (pill_slot >= 0) {
        const auto& slot = slots[static_cast<size_t>(pill_slot)];
        const auto& item = vm.sidebar[slot.group].items[slot.item];
        const uint64_t context = compact ? 1u : 2u;
        const int64_t key = static_cast<int64_t>(std::hash<std::wstring>{}(item.path)) ^
                            (static_cast<int64_t>(slot.group) << 48);
        const D2D1_RECT_F lead = sm.lead.Update(context, key, slot.rc, motion_frame_, motion_now_, kPillLead);
        // One continuously retargeted spring keeps the transition without a second
        // stretching tail competing with hover during rapid tab changes.
        const D2D1_RECT_F trail = lead;
        const D2D1_RECT_F pill = StretchPill(lead, trail, scale_);
        if (sm.lead.Active(motion_now_)) sidebar_moving = true;
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
    if (pill_slot < 0) {
        sm.lead.Hide();
    }
    const auto isPillSlot = [&](const SidebarSlot& slot) {
        return pill_slot >= 0 && &slot == &slots[static_cast<size_t>(pill_slot)];
    };
    // Every return below asks for the next frame while something moves.
    struct FrameRequest {
        const MainRenderer* self;
        const bool& moving;
        ~FrameRequest() {
            if (moving) self->RequestMotionFrame();
        }
    } frame_request{this, sidebar_moving};

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
                if (group.badge_count > 0) {
                    // 网络位置: devices found on this network that the user has
                    // not looked at yet.
                    IDWriteTextFormat* fmt = compositor_->SmallFormat();
                    wchar_t num[4]{};
                    if (group.badge_count > 9) wcscpy_s(num, L"9+");
                    else num[0] = static_cast<wchar_t>(L'0' + group.badge_count);
                    const float d = 14.0f * scale_;
                    const float badge_w = group.badge_count > 9 ? d * 1.45f : d;
                    const D2D1_RECT_F b = D2D1::RectF(rail_rc.right - badge_w * 0.4f, rail_rc.top - d * 0.45f,
                                                      rail_rc.right + badge_w * 0.6f, rail_rc.top + d * 0.55f);
                    MakeBrush(dc, theme.accent, brAccent_);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(b, d * 0.5f, d * 0.5f), brAccent_.get());
                    const DWRITE_TEXT_ALIGNMENT oldAlign = fmt->GetTextAlignment();
                    const DWRITE_PARAGRAPH_ALIGNMENT oldPara = fmt->GetParagraphAlignment();
                    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    MakeBrush(dc, theme.accent_text, brText_);
                    dc->DrawText(num, static_cast<UINT32>(wcslen(num)), fmt, b, brText_.get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                    fmt->SetTextAlignment(oldAlign);
                    fmt->SetParagraphAlignment(oldPara);
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
                if (IsHighContrast() || !compositor_->DrawLumaText(label, fmt, chip, theme.text_secondary,
                                                                   theme.bg, DWRITE_TEXT_ALIGNMENT_CENTER))
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
            const bool hovered = !pill_row && !plate_live && IsHovered(vm, HitTestResult::SidebarItem, slot.run);
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
            if (IsHighContrast() || svg_id == 0 ||
                !DrawFluentSvg(svg_id, icon_rc, 1.0f, nullptr, true)) {
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
                if (IsHighContrast() || !compositor_->DrawLumaText(std::wstring_view(num, 1), fmt, b,
                                                                   HexColor(0xFFFFFF), theme.bg,
                                                                   DWRITE_TEXT_ALIGNMENT_CENTER))
                    dc->DrawText(num, 1, fmt, b, brText_.get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                 DWRITE_MEASURING_MODE_NATURAL);
                fmt->SetTextAlignment(oldAlign);
                fmt->SetParagraphAlignment(oldPara);
            }
        }
        if (label_layer) dc->PopLayer();
        dc->PopAxisAlignedClip();
        return;
    }


    // Sidebar row reorder (pulse_motion_v2 qareorder; quick-access rows, tabs
    // and tags): the held row lifts and rides the cursor while the rows it
    // passes slide aside to open its slot.
    // It paints last so the rows sliding under it never cover the card.
    std::vector<size_t> paint_order(slots.size());
    for (size_t k = 0; k < slots.size(); ++k) paint_order[k] = k;
    int pin_slot = -1;
    float pin_pitch = 0.0f;
    const bool pin_live = vm.sidebar_pin_drag_index >= 0 && sm.animations && !IsHighContrast();
    if (pin_live) {
        for (size_t k = 0; k < slots.size(); ++k) {
            const auto& slot = slots[k];
            if (slot.group < 0 || slot.item < 0 || slot.kind == SidebarSlot::Header) continue;
            if (slot.run != vm.sidebar_pin_drag_index) continue;
            pin_slot = static_cast<int>(k);
            break;
        }
    }
    if (pin_slot >= 0) {
        const auto& held = slots[static_cast<size_t>(pin_slot)];
        pin_pitch = held.rc.bottom - held.rc.top;
        if (static_cast<size_t>(pin_slot) + 1 < slots.size() &&
            slots[static_cast<size_t>(pin_slot) + 1].group == held.group &&
            slots[static_cast<size_t>(pin_slot) + 1].rc.top > held.rc.top)
            pin_pitch = slots[static_cast<size_t>(pin_slot) + 1].rc.top - held.rc.top;
        else if (pin_slot > 0 && slots[static_cast<size_t>(pin_slot) - 1].group == held.group &&
                 slots[static_cast<size_t>(pin_slot) - 1].rc.top < held.rc.top)
            pin_pitch = held.rc.top - slots[static_cast<size_t>(pin_slot) - 1].rc.top;
        paint_order.erase(paint_order.begin() + pin_slot);
        paint_order.push_back(static_cast<size_t>(pin_slot));
        if (sm.pin_run != held.run) {
            sm.pin_run = held.run;
            sm.pin_lift_start = motion_now_;
            sm.pin_offsets.clear();
            sm.pin_last = motion_now_;
        }
        const float held_c = (held.rc.top + held.rc.bottom) * 0.5f;
        const float gap = vm.sidebar_pin_gap_line_y;
        const float dt = static_cast<float>(motion_now_ > sm.pin_last ? motion_now_ - sm.pin_last : 0);
        sm.pin_last = motion_now_;
        const float follow = 1.0f - std::exp(-(std::min)(dt, 64.0f) / 55.0f);
        bool sliding = false;
        for (const auto& slot : slots) {
            if (slot.group != held.group || slot.item < 0 || slot.kind == SidebarSlot::Header ||
                slot.run == held.run) continue;
            const float c = (slot.rc.top + slot.rc.bottom) * 0.5f;
            float target = 0.0f;
            if (gap > 0.0f) {
                if (c > held_c && c < gap) target = -pin_pitch;
                else if (c < held_c && c > gap) target = pin_pitch;
            }
            float& cur = sm.pin_offsets[slot.run];
            cur += (target - cur) * follow;
            if (std::fabs(target - cur) < 0.4f) cur = target;
            else sliding = true;
        }
        const uint64_t lift_ms = motion_now_ - sm.pin_lift_start;
        if (sliding || lift_ms < static_cast<uint64_t>(motion::SpringSettleMs(motion::spring::Pop)))
            RequestMotionFrame();
    } else if (sm.pin_run >= 0) {
        sm.pin_run = -1;
        sm.pin_offsets.clear();
    }
    // Per-row transform for the reorder; restores on every exit of the body.
    struct RowXform {
        ID2D1DeviceContext* dc = nullptr;
        D2D1_MATRIX_3X2_F saved{};
        bool active = false;
        void Apply(ID2D1DeviceContext* d, const D2D1_MATRIX_3X2_F& m) {
            dc = d;
            dc->GetTransform(&saved);
            dc->SetTransform(m * saved);
            active = true;
        }
        ~RowXform() { if (active) dc->SetTransform(saved); }
    };

    if (!IsHighContrast() && !reordering && !collapse_anim_) {
        std::vector<std::pair<int, D2D1_RECT_F>> icons;
        icons.reserve(slots.size());
        for (const auto& slot : slots) {
            if (slot.group < 0) continue;
            if (slot.kind == SidebarSlot::Header) {
                icons.emplace_back(FluentSvgIdForGlyph(vm.sidebar[slot.group].icon_glyph),
                                   painter_.SidebarSectionHeaderIconRect(slot.rc));
            } else if (slot.item >= 0) {
                const auto& item = vm.sidebar[slot.group].items[slot.item];
                if (item.is_tag || item.tab_row) continue;
                if (slot.kind == SidebarSlot::Drive)
                    icons.emplace_back(FluentSvgIdForGlyph(item.icon_glyph), painter_.DriveSidebarItemIconRect(slot.rc));
                else if (slot.kind == SidebarSlot::Item)
                    icons.emplace_back(FluentSvgIdForGlyph(item.icon_glyph), painter_.SidebarItemIconRect(slot.rc, item.status_dot));
            }
        }
        PrepareFluentSvgBitmaps(icons);
    }

    ++sm.count_epoch;
    for (const size_t paint_k : paint_order) {
        const auto& slot = slots[paint_k];
        RowXform row_xform;
        if (pin_slot >= 0 && slot.group == slots[static_cast<size_t>(pin_slot)].group &&
            slot.item >= 0 && slot.kind != SidebarSlot::Header) {
            if (static_cast<int>(paint_k) == pin_slot) {
                const float t = static_cast<float>(motion_now_ - sm.pin_lift_start);
                const float lift = motion::SpringProgress(motion::spring::Pop, t);
                const float s_ = 1.0f + 0.03f * lift;
                const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                      (slot.rc.top + slot.rc.bottom) * 0.5f);
                row_xform.Apply(dc, D2D1::Matrix3x2F::Scale(s_, s_, c) *
                                    D2D1::Matrix3x2F::Translation(0.0f, vm.sidebar_pin_drag_dy));
            } else if (auto it = sm.pin_offsets.find(slot.run);
                       it != sm.pin_offsets.end() && it->second != 0.0f) {
                row_xform.Apply(dc, D2D1::Matrix3x2F::Translation(0.0f, it->second));
            }
        }
        if (slot.kind == SidebarSlot::Header) {
            fluent::SidebarSectionHeaderSpec header;
            header.bounds = slot.rc;
            std::wstring workspace_header;
            header.text = vm.sidebar[slot.group].header;
            if (vm.sidebar[slot.group].add_action == SidebarAddAction::SaveWorkspace) {
                int count = 0;
                for (const auto& entry : vm.sidebar[slot.group].items)
                    if (entry.workspace_index >= 0) ++count;
                workspace_header = vm.sidebar[slot.group].header + L"  " + std::to_wstring(count);
                header.text = workspace_header;
            }
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
            // Shelf redesign: strip / drop / shelf.
            DrawTrayShelf(vm, slot.rc, theme);
            continue;
        }
        if (slot.kind == SidebarSlot::TrayRelease) continue;
        if (slot.group < 0 || slot.item < 0) continue;

        const auto& item = vm.sidebar[slot.group].items[slot.item];
        if (item.workspace_empty && !compact) {
            painter_.StrokeRoundedRect(slot.rc, 8.0f * scale_, theme.stroke_card);
            painter_.DrawWrappedCaption(item.label,
                D2D1::Point2F(slot.rc.left + 12.0f * scale_, slot.rc.top + 10.0f * scale_),
                (std::max)(1.0f, slot.rc.right - slot.rc.left - 24.0f * scale_), theme.text_secondary);
            const auto button = WorkspaceActionRect(slot.rc, scale_, 4);
            const bool hot = IsHovered(vm, HitTestResult::SidebarWorkspaceAction, slot.run, 4);
            painter_.FillRoundedRect(button, 5.0f * scale_, hot ? theme.fill_hover : WithAlpha(theme.accent, 0.08f));
            DrawIconText(button.left + 6.0f * scale_, button.top, 24.0f * scale_, button.bottom - button.top,
                         kIconAdd, L"+", theme.accent, 0.72f);
            painter_.DrawText(item.detail,
                D2D1::RectF(button.left + 30.0f * scale_, button.top, button.right - 6.0f * scale_, button.bottom),
                compositor_->SmallFormat(), theme.accent, fluent::HorizontalAlignment::Center);
            continue;
        }
        if (item.workspace_child_number > 0 && !compact) {
            const bool held = vm.sidebar_pin_drag_index == slot.run;
            const bool hot = held || IsHovered(vm, HitTestResult::SidebarItem, slot.run);
            if (hot) painter_.FillRoundedRect(slot.rc, 5.0f * scale_, theme.fill_hover);
            const float left = slot.rc.left + (46.0f - static_cast<float>(item.indent) * 16.0f) * scale_;
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            dc->DrawLine(D2D1::Point2F(left - 9.0f * scale_, slot.rc.top - 2.0f * scale_),
                         D2D1::Point2F(left - 9.0f * scale_, slot.rc.bottom + 2.0f * scale_), brStrokeDivider_.get(), scale_);
            const auto badge = D2D1::RectF(left, slot.rc.top + 5.0f * scale_, left + 16.0f * scale_, slot.rc.top + 21.0f * scale_);
            painter_.FillRoundedRect(badge, 3.0f * scale_, WithAlpha(theme.accent, 0.12f));
            painter_.DrawText(std::to_wstring(item.workspace_child_number), badge, compositor_->SmallFormat(), theme.accent, fluent::HorizontalAlignment::Center);
            painter_.DrawText(item.label, D2D1::RectF(left + 22.0f * scale_, slot.rc.top, (std::max)(left + 22.0f * scale_, slot.rc.right - 28.0f * scale_), slot.rc.bottom), compositor_->SmallFormat(), theme.text_secondary);
            const wchar_t* glyph = L"\xE80A";
            switch (item.workspace_child_view) {
            case ViewMode::Details: glyph = L"\xE14C"; break;
            case ViewMode::List: glyph = L"\xE8FD"; break;
            case ViewMode::Tiles: glyph = L"\xECA5"; break;
            case ViewMode::Content: glyph = L"\xE8A9"; break;
            default: break;
            }
            if (hot) glyph = L"\xE784";
            DrawIconText(slot.rc.right - 26.0f * scale_, slot.rc.top, 22.0f * scale_, slot.rc.bottom - slot.rc.top,
                         glyph, L"", theme.text_secondary, 0.62f);
            continue;
        }
        // Motion v2 "dropbin": a row whose count changes (files dropped on the
        // 回收站, items tagged) gulps - a quick squeeze that springs back past
        // rest - under an accent wash, while its count rolls to the new value.
        float count_roll = 1.0f;
        bool count_down = false;
        const std::wstring* count_prev = nullptr;
        if (item.path == L"pulse:starred") {
            sm.star_dest = slot.rc;
            sm.star_dest_epoch = sm.count_epoch;
        }
        if (!item.path.empty() && slot.kind != SidebarSlot::Drive && !item.tab_row) {
            std::wstring now_count;
            if (item.path == L"pulse:recycle") now_count = item.detail;
            else if (item.show_count || item.count > 0) now_count = std::to_wstring((std::min)(item.count, 99));
            auto [found, fresh] = sm.counts.try_emplace(item.path);
            SidebarMotion::CountRoll& cr = found->second;
            const bool seen_last_frame = !fresh && cr.epoch + 1 == sm.count_epoch;
            if (seen_last_frame && sm.animations && cr.last != now_count &&
                !now_count.empty() && !cr.last.empty()) {
                cr.prev = cr.last;
                cr.start = motion_now_ ? motion_now_ : 1;
                cr.down = wcstol(now_count.c_str(), nullptr, 10) < wcstol(cr.last.c_str(), nullptr, 10);
            }
            cr.last = std::move(now_count);
            cr.epoch = sm.count_epoch;
            if (cr.start) {
                constexpr motion::SpringSpec kGulp{420.0f, 20.0f};  // HTML "bouncy"
                constexpr float kSqueezeMs = 90.0f;
                const float t = static_cast<float>(motion_now_ - cr.start);
                if (t >= 760.0f) {
                    cr.start = 0;
                } else {
                    count_roll = t >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))
                        ? 1.0f : motion::SpringProgress(motion::spring::Smooth, t);
                    count_down = cr.down;
                    count_prev = &cr.prev;
                    const float squeeze = t < kSqueezeMs
                        ? motion::EaseOutCubic(t / kSqueezeMs)
                        : 1.0f - motion::SpringProgress(kGulp, t - kSqueezeMs);
                    const float g = 1.0f - 0.07f * squeeze;
                    if (!row_xform.active) {
                        const D2D1_POINT_2F c = D2D1::Point2F((slot.rc.left + slot.rc.right) * 0.5f,
                                                              (slot.rc.top + slot.rc.bottom) * 0.5f);
                        row_xform.Apply(dc, D2D1::Matrix3x2F::Scale(g, g, c));
                    }
                    const float wash = 1.0f - t / 760.0f;
                    MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.30f : 0.22f) * wash * wash), brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), slot.rc.left, slot.rc.top,
                        slot.rc.right - slot.rc.left, slot.rc.bottom - slot.rc.top,
                        theme.radius_control * scale_);
                    RequestMotionFrame();
                }
            }
        }
        fluent::ControlState state;
        state.selected = isSelectedSlot(slot);
        state.hovered = IsHovered(vm, HitTestResult::SidebarItem, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemAction, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run) ||
            IsHovered(vm, HitTestResult::SidebarBarSwitch, slot.run);
        // Rows of the section being reordered stay lit like its header, and the
        // dragged pin row reads as held.
        if (vm.sidebar_group_drag_id >= 0 &&
            vm.sidebar[slot.group].id == vm.sidebar_group_drag_id) state.hovered = true;
        if (vm.sidebar_pin_drag_index >= 0 && slot.run == vm.sidebar_pin_drag_index)
            state.hovered = true;
        const bool row_hot = state.hovered;
        // The gliding plate paints the hover under ordinary rows.
        if (plate_live && !(item.tab_row && item.tab_active)) state.hovered = false;
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
            drive.free_bytes = item.free_bytes;
            drive.total_bytes = item.total_bytes;
            drive.state = state;
            drive.drop_target = slot.run == vm.sidebar_drop_index;
            drive.icon_color = item.icon_color;
            const int drive_svg = FluentSvgIdForGlyph(item.icon_glyph);
            drive.skip_glyph = !IsHighContrast() && drive_svg != 0 && EnsureFluentSvg(drive_svg, true);
            painter_.DrawDriveSidebarItem(drive);
            if (drive.skip_glyph) {
                DrawFluentSvg(drive_svg, painter_.DriveSidebarItemIconRect(slot.rc),
                              1.0f, nullptr, true);
            }
        } else {
            const bool heldPin = static_cast<int>(paint_k) == pin_slot;
            if (heldPin) {
                // Raised while dragging: a clearly deeper shadow + brighter card
                // than a plain selected row, so the lift reads at a glance.
                const float r = theme.radius_control * scale_;
                ComPtr<ID2D1CommandList> card;
                dc->CreateCommandList(&card);
                if (card.get()) {
                    ComPtr<ID2D1Image> prev;
                    dc->GetTarget(&prev);
                    // Record untransformed: DrawImage below applies the current
                    // transform (the held pin's lift/follow), so recording
                    // through it as well would move the card twice as far.
                    D2D1_MATRIX_3X2_F record_saved{};
                    dc->GetTransform(&record_saved);
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    dc->SetTarget(card.get());
                    MakeBrush(dc, vm.dark ? HexColor(0x303030) : HexColor(0xFFFFFF), brFillSelected_);
                    FillRoundedRect(dc, brFillSelected_.get(),
                        slot.rc.left + 1.0f, slot.rc.top + 1.0f,
                        slot.rc.right - slot.rc.left - 2.0f,
                        slot.rc.bottom - slot.rc.top - 2.0f, r);
                    dc->SetTarget(prev.get());
                    dc->SetTransform(record_saved);
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
            if (item.divider_above) {
                // 收藏夹's separator: in the gap above the row, inset like the row.
                MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
                FillRect(dc, brStrokeDivider_.get(), slot.rc.left + 10.0f * scale_,
                         std::round(slot.rc.top - 1.0f * scale_),
                         (std::max)(0.0f, slot.rc.right - slot.rc.left - 20.0f * scale_), 1.0f);
            }
            fluent::SidebarItemSpec row;
            row.bounds = slot.rc;
            row.text = item.label;
            if (item.path == L"pulse:recycle" || item.workspace_index >= 0 || item.workspace_empty) row.detail = item.detail;
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
            if (count_prev) {
                row.count_roll = count_roll;
                row.count_prev = *count_prev;
                row.count_roll_down = count_down;
            }
            // The label is the folder's 别名 (places show it): the mark follows it.
            row.alias_mark = !item.path.empty() && !item.is_tag && !item.editing &&
                             !app::AliasStore::Instance().Empty() && app::AliasStore::Instance().ShowMark() &&
                             app::AliasStore::Instance().Shown(item.path) == item.label;
            const int row_svg = item.is_tag ? 0 : FluentSvgIdForGlyph(item.icon_glyph);
            PULSE_SIDEBAR_MARK("sidebar_row_svg_begin");
            row.skip_glyph = !IsHighContrast() && row_svg != 0 && EnsureFluentSvg(row_svg, true);
            PULSE_SIDEBAR_MARK("sidebar_row_svg_end");
            // 网盘 roots: the program's registered icon, as Explorer's navigation pane.
            const D2D1_RECT_F row_icon_rc = painter_.SidebarItemIconRect(slot.rc, item.status_dot);
            ID2D1Bitmap* row_shell_icon = item.icon_path.empty() || IsHighContrast() ? nullptr
                : icon_cache_.ExactBitmapFor(item.icon_path,
                      (std::max)(row_icon_rc.right - row_icon_rc.left, row_icon_rc.bottom - row_icon_rc.top));
            if (row_shell_icon) row.skip_glyph = true;
            const bool unpin = item.workspace_index < 0 && SidebarItemHasUnpin(item);
            const auto unpin_rc = WorkspaceUnpinRect(slot.rc, scale_);
            const auto expand_rc = SidebarExpandRect(item.workspace_index >= 0 ? D2D1::RectF(slot.rc.left, slot.rc.top, slot.rc.right, slot.rc.top + 44.0f * scale_) : slot.rc, scale_);
            if (unpin)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - unpin_rc.left) - 6.0f * scale_);
            else if (item.expandable)
                row.trailing_reserve = (std::max)(0.0f,
                    (slot.rc.right - (item.bar_switch ? SidebarBarSwitchRect(slot.rc, scale_) : expand_rc).left) -
                    4.0f * scale_);
            if (item.workspace_index >= 0) {
                row.bounds.bottom = row.bounds.top + 44.0f * scale_;
                row.trailing_reserve = 78.0f * scale_;
                row.skip_glyph = true;
                row.suppress_text = true;
                row.badge_text = {};
            }
            PULSE_SIDEBAR_MARK("sidebar_row_text_begin");
            painter_.DrawSidebarItem(row);
            PULSE_SIDEBAR_MARK("sidebar_row_text_end");
            if (item.workspace_index >= 0) {
                const bool workspace_hot = row_hot || IsHovered(vm, HitTestResult::SidebarWorkspaceAction, slot.run);
                const float text_right = (std::max)(slot.rc.left + 46.0f * scale_, slot.rc.right - (workspace_hot || !item.badge.empty() ? 80.0f : 30.0f) * scale_);
                MakeBrush(dc, theme.text, brText_);
                if (!item.editing) DrawTextEndEllipsis(compositor_, compositor_->TextFormat(), brText_.get(), item.label,
                    slot.rc.left + 46.0f * scale_, slot.rc.top + 3.0f * scale_, text_right - slot.rc.left - 46.0f * scale_, 21.0f * scale_);
                MakeBrush(dc, theme.text_secondary, brText_);
                DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(), brText_.get(), item.detail,
                    slot.rc.left + 46.0f * scale_, slot.rc.top + 24.0f * scale_, text_right - slot.rc.left - 46.0f * scale_, 18.0f * scale_);
                if (!workspace_hot && !item.badge.empty()) {
                    auto badge = WorkspaceActionRect(slot.rc, scale_, 0);
                    badge.right = WorkspaceActionRect(slot.rc, scale_, 1).right;
                    badge.top += 3.0f * scale_; badge.bottom -= 3.0f * scale_;
                    painter_.FillRoundedRect(badge, 5.0f * scale_, WithAlpha(theme.accent, 0.16f));
                    painter_.DrawText(item.badge, badge, compositor_->SmallFormat(), theme.accent, fluent::HorizontalAlignment::Center);
                }
                if (item.workspace_dirty) {
                    const auto drift = WorkspaceDriftRect(slot.rc, scale_);
                    const auto warning = vm.dark ? D2D1::ColorF(0xF5C26B) : D2D1::ColorF(0x985A00);
                    const auto background = vm.dark ? WithAlpha(D2D1::ColorF(0xF5B450),0.12f)
                                                       : WithAlpha(D2D1::ColorF(0xE8A028),0.14f);
                    painter_.FillRoundedRect(drift,7.0f*scale_,IsHighContrast() ? theme.surface_card : background);
                    const float label_left=drift.left+9.0f*scale_;
                    const float label_right=WorkspaceActionRect(slot.rc,scale_,2).left-6.0f*scale_;
                    MakeBrush(dc,theme.text,brText_);
                    DrawTextEndEllipsis(compositor_,compositor_->SmallFormat(),brText_.get(),
                        l10n::Get(l10n::StringId::WorkspaceLayoutChanged),label_left,drift.top,
                        (std::max)(1.0f,label_right-label_left),drift.bottom-drift.top);
                    for (int action = 2; action < 4; ++action) {
                        const auto button = WorkspaceActionRect(slot.rc, scale_, action);
                        if (IsHovered(vm,HitTestResult::SidebarWorkspaceAction,slot.run,action))
                            painter_.FillRoundedRect(button,5.0f*scale_,theme.fill_hover);
                        painter_.DrawText(l10n::Get(action == 2 ? l10n::StringId::WorkspaceUpdate : l10n::StringId::WorkspaceRestore),
                            button,compositor_->SmallFormat(),IsHighContrast() ? theme.text : warning,fluent::HorizontalAlignment::Center);
                    }
                }
                const auto rgb = item.workspace_color_rgb;
                const auto ink = D2D1::ColorF(rgb);
                const auto icon = D2D1::RectF(slot.rc.left + 6.0f * scale_, slot.rc.top + 8.0f * scale_,
                                             slot.rc.left + 34.0f * scale_, slot.rc.top + 36.0f * scale_);
                MakeBrush(dc, WithAlpha(ink, 0.16f), brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(icon, 6.0f * scale_, 6.0f * scale_), brFillHover_.get());
                MakeBrush(dc, ink, brText_);
                const auto inner = D2D1::RectF(icon.left + 6.0f * scale_, icon.top + 6.0f * scale_,
                                              icon.right - 6.0f * scale_, icon.bottom - 6.0f * scale_);
                for (size_t pane = 0; pane < (std::min)(item.workspace_panes.size(), size_t{4}); ++pane) {
                    auto tile = WorkspacePreviewPaneRect(inner, item.workspace_layout, static_cast<int>(pane));
                    tile.left += 0.8f * scale_; tile.top += 0.8f * scale_;
                    tile.right -= 0.8f * scale_; tile.bottom -= 0.8f * scale_;
                    dc->FillRoundedRectangle(D2D1::RoundedRect(tile, scale_, scale_), brText_.get());
                }
                if (row_hot || IsHovered(vm, HitTestResult::SidebarWorkspaceAction, slot.run)) {
                    for (int action = 0; action < 2; ++action) {
                        const auto button = WorkspaceActionRect(slot.rc, scale_, action);
                        DrawIconText(button.left, button.top, button.right - button.left, button.bottom - button.top,
                                     action == 0 ? L"\xE8A7" : L"\xE712", action == 0 ? L"+" : L"...",
                                     theme.text_secondary, 0.7f);
                    }
                }
            }
            if (row_shell_icon) {
                dc->DrawBitmap(row_shell_icon, &row_icon_rc, 1.0f,
                               D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else if (row.skip_glyph && item.workspace_index < 0) {
                DrawFluentSvg(row_svg, row_icon_rc, 1.0f, nullptr, true);
            }
            if (item.editing) {
                // In-place tag rename: give the hosted edit a real TextField
                // frame. Geometry must match TagRenameCell in app_main.cpp.
                D2D1_RECT_F cell = slot.rc;
                cell.left += (item.workspace_index >= 0 ? 46.0f : 34.0f) * scale_;
                cell.right -= (item.workspace_index >= 0 ? 28.0f : 38.0f) * scale_;
                cell.top += 2.0f * scale_;
                cell.bottom = item.workspace_index >= 0 ? cell.top + 24.0f * scale_ : cell.bottom - 2.0f * scale_;
                if (cell.right > cell.left && cell.bottom > cell.top) {
                    fluent::ControlState field;
                    field.focused = true;
                    painter_.DrawTextFieldFrame(cell, field);
                }
            }
            if (unpin && item.tab_row) {
                // Tab rows: close button, shown on hover and on the active tab.
                const bool close_hot = IsHovered(vm, HitTestResult::SidebarItemAction, slot.run);
                if (row_hot || item.tab_active) {
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
                if (item.bar_switch && row_hot) {
                    // 收藏夹 root: 改成横向收藏栏 (the same switch as Ctrl+Shift+B).
                    const D2D1_RECT_F sw = SidebarBarSwitchRect(slot.rc, scale_);
                    const bool switch_hot = IsHovered(vm, HitTestResult::SidebarBarSwitch, slot.run);
                    if (switch_hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), sw.left, sw.top, sw.right - sw.left,
                                        sw.bottom - sw.top, 4.0f * scale_);
                    }
                    DrawIconText(sw.left, sw.top, sw.right - sw.left, sw.bottom - sw.top,
                                 command_icons::kGlyphTopBar, L"", switch_hot ? theme.accent : theme.text_secondary,
                                 0.68f);
                }
                const bool expand_hot = IsHovered(vm, HitTestResult::SidebarItemExpand, slot.run);
                if (expand_hot) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        4.0f * scale_);
                }
                // The chevron turns (Snappy) instead of swapping glyphs.
                const float angle = IsHighContrast() ? (item.expanded ? 90.0f : 0.0f)
                    : ChevronAngle(sm, std::to_wstring(slot.group) + L"|" + item.path, item.expanded,
                                   motion_now_, sidebar_moving);
                if (angle <= 0.01f || angle >= 89.99f) {
                    DrawIconText(expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        item.expanded ? kIconChevronDown : kIconChevronRight,
                        item.expanded ? L"v" : L">", theme.text_secondary, 0.68f);
                } else {
                    D2D1_MATRIX_3X2_F saved{};
                    dc->GetTransform(&saved);
                    const D2D1_POINT_2F center = D2D1::Point2F((expand_rc.left + expand_rc.right) * 0.5f,
                                                               (expand_rc.top + expand_rc.bottom) * 0.5f);
                    dc->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center) * saved);
                    DrawIconText(expand_rc.left, expand_rc.top,
                        expand_rc.right - expand_rc.left, expand_rc.bottom - expand_rc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.68f);
                    dc->SetTransform(saved);
                }
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
        if (vm.sidebar_pin_gap_line_y > 0.0f && pin_slot < 0)
            DrawSidebarInsertionLine(dc, brAccent_.get(), sb, scale_,
                                     vm.sidebar_pin_gap_line_y);
    }

    const SidebarSlot* preview_slot = nullptr;
    if (!compact) {
        for (const auto& candidate : slots) {
            if (candidate.group < 0 || candidate.item < 0) continue;
            const auto& entry = vm.sidebar[candidate.group].items[candidate.item];
            if (entry.workspace_index >= 0 && IsHovered(vm, HitTestResult::SidebarItem, candidate.run)) {
                preview_slot = &candidate;
                break;
            }
        }
    }
    if (!preview_slot) {
        sm.workspace_preview_path.clear();
        sm.workspace_preview_start = 0;
    } else {
        const auto& entry = vm.sidebar[preview_slot->group].items[preview_slot->item];
        if (sm.workspace_preview_path != entry.path) {
            sm.workspace_preview_path = entry.path;
            sm.workspace_preview_start = motion_now_;
        }
        if (motion_now_ - sm.workspace_preview_start < 500) {
            RequestMotionFrame();
        }
        sm.workspace_preview_anchor = D2D1::RectF(sb.right, preview_slot->rc.top, sb.right, sb.bottom);
    }


    painter_.DrawScrollbar(SidebarScrollbarSpec(vm, sb, scale_));
    if (label_layer) dc->PopLayer();
    dc->PopAxisAlignedClip();
}

void MainRenderer::DrawWorkspacePreview(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    const auto& sm = SidebarMotionFor(this);
    if (sm.workspace_preview_path.empty() || motion_now_ - sm.workspace_preview_start < 500) return;
    const SidebarItem* entry = nullptr;
    for (const auto& group : vm.sidebar) for (const auto& item : group.items)
        if (item.workspace_index >= 0 && item.path == sm.workspace_preview_path) entry = &item;
    if (!entry) return;
    const float left = sm.workspace_preview_anchor.left + 8.0f * scale_;
    const float right = (std::min)(rect.right - 8.0f * scale_, left + 280.0f * scale_);
    if (right - left < 120.0f * scale_) return;
    const float height = 210.0f * scale_;
    const float top = (std::max)(rect.top + 8.0f * scale_, (std::min)(sm.workspace_preview_anchor.top, rect.bottom - height - 8.0f * scale_));
    const auto card = D2D1::RectF(left, top, right, top + height);
    painter_.FillRoundedRect(card, 8.0f * scale_, BlendOver(theme.surface_flyout, theme.bg));
    painter_.StrokeRoundedRect(card, 8.0f * scale_, theme.stroke_card);
    painter_.DrawText(entry->label, D2D1::RectF(left + 12.0f * scale_, top + 8.0f * scale_, right - 12.0f * scale_, top + 32.0f * scale_), compositor_->TextFormat(), theme.text);
    painter_.DrawText(pulse::path::FriendlyPathText(entry->workspace_root), D2D1::RectF(left + 12.0f * scale_, top + 32.0f * scale_, right - 12.0f * scale_, top + 54.0f * scale_), compositor_->SmallFormat(), theme.text_secondary);
    const auto inner = D2D1::RectF(left + 12.0f * scale_, top + 62.0f * scale_, right - 12.0f * scale_, top + 172.0f * scale_);
    for (size_t i = 0; i < (std::min)(entry->workspace_panes.size(), size_t{4}); ++i) {
        auto cell = WorkspacePreviewPaneRect(inner, entry->workspace_layout, static_cast<int>(i));
        cell.right -= 3.0f * scale_; cell.bottom -= 3.0f * scale_;
        painter_.FillRoundedRect(cell, 4.0f * scale_, WithAlpha(theme.accent, 0.10f));
        auto name = entry->workspace_panes[i];
        while (name.size() > 3 && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
        const auto slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < name.size()) name.erase(0, slash + 1);
        const auto label = std::to_wstring(i + 1) + L"  " + name;
        const auto title = D2D1::RectF(cell.left + 6.0f * scale_, cell.top + 4.0f * scale_, cell.right - 6.0f * scale_, cell.top + 22.0f * scale_);
        painter_.DrawText(label, title, compositor_->SmallFormat(), theme.text);
        for (int line = 0; line < 3; ++line) {
            const float y = cell.top + (30.0f + line * 12.0f) * scale_;
            if (y + 3.0f * scale_ >= cell.bottom - 5.0f * scale_) break;
            painter_.FillRoundedRect(D2D1::RectF(cell.left + 8.0f * scale_, y,
                cell.right - (line == 2 ? 28.0f : 12.0f) * scale_, y + 3.0f * scale_),
                1.5f * scale_, WithAlpha(theme.text_secondary, 0.20f));
        }
    }
    wchar_t count[64]{};
    swprintf_s(count, l10n::Get(l10n::StringId::WorkspacePanes).c_str(), static_cast<int>(entry->workspace_panes.size()));
    const app::KeyChord workspace_key{app::kKeyCtrl | app::kKeyAlt,
        static_cast<uint16_t>('1' + entry->workspace_index)};
    const bool has_shortcut = entry->workspace_index >= 0 && entry->workspace_index < 9 &&
        app::WorkspaceShortcutIndex(app::ActiveKeymap(), workspace_key) == entry->workspace_index;
    const auto footer = std::wstring(count) + (has_shortcut ? L"    " + app::FormatChord(workspace_key) : L"");
    painter_.DrawText(footer, D2D1::RectF(left + 12.0f * scale_, top + 180.0f * scale_, right - 12.0f * scale_, top + 202.0f * scale_), compositor_->SmallFormat(), theme.text_secondary);
}

void MainRenderer::LaunchStarFlight(float x, float y) {
    if (!motion::SystemAnimationsEnabled()) return;
    SidebarMotion& sm = SidebarMotionFor(this);
    sm.star_from = D2D1::Point2F(x, y);
    sm.star_start = motion::NowMs();
    sm.star_landed = false;
    RequestMotionFrame();
}

// Motion v2 "star": the star arcs from where it was clicked into the sidebar's
// 已加星标 row with a short spark trail, bursts there, and the row gulps.
void MainRenderer::DrawStarFlight(const Theme& theme) {
    SidebarMotion& sm = SidebarMotionFor(this);
    if (!sm.star_start || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    constexpr float kFlightMs = 560.0f;
    constexpr float kBurstMs = 360.0f;
    const uint64_t now = motion::NowMs();
    const float t = static_cast<float>(now - sm.star_start);
    if (t >= kFlightMs + kBurstMs) {
        sm.star_start = 0;
        return;
    }
    D2D1_POINT_2F to = D2D1::Point2F(28.0f * scale_, 96.0f * scale_);
    if (sm.star_dest_epoch == sm.count_epoch && sm.star_dest.right > sm.star_dest.left)
        to = D2D1::Point2F(sm.star_dest.left + 22.0f * scale_,
                           (sm.star_dest.top + sm.star_dest.bottom) * 0.5f);
    const D2D1_POINT_2F from = sm.star_from;
    const float dist = std::hypot(to.x - from.x, to.y - from.y);
    // Arc over the content, its apex kept inside the window (16 dip down):
    // the curve's peak is (from + 2*ctrl + to) / 4.
    const float apex_floor = (4.0f * 16.0f * scale_ - from.y - to.y) * 0.5f;
    const D2D1_POINT_2F ctrl = D2D1::Point2F((from.x + to.x) * 0.5f,
        (std::max)(apex_floor, (std::min)(from.y, to.y) - (std::max)(60.0f * scale_, dist * 0.22f)));
    auto at = [&](float e) {
        const float v = 1.0f - e;
        return D2D1::Point2F(v * v * from.x + 2.0f * v * e * ctrl.x + e * e * to.x,
                             v * v * from.y + 2.0f * v * e * ctrl.y + e * e * to.y);
    };
    auto ease = [](float u) {
        u = std::clamp(u, 0.0f, 1.0f);
        return u < 0.5f ? 4.0f * u * u * u : 1.0f - std::pow(-2.0f * u + 2.0f, 3.0f) * 0.5f;
    };
    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    if (t < kFlightMs) {
        const float e = ease(t / kFlightMs);
        // Spark trail: a few dots sampled behind the star, smaller and fainter.
        for (int k = 1; k <= 5; ++k) {
            const float ek = e - 0.045f * static_cast<float>(k);
            if (ek <= 0.0f) break;
            const D2D1_POINT_2F q = at(ek);
            const float fade = 1.0f - static_cast<float>(k) / 6.0f;
            MakeBrush(dc, WithAlpha(theme.accent, 0.55f * fade), brAccent_);
            const float r = (2.4f * fade + 0.6f) * scale_;
            dc->FillEllipse(D2D1::Ellipse(q, r, r), brAccent_.get());
        }
        const D2D1_POINT_2F p = at(e);
        const float size = 18.0f * scale_ * (1.0f + 0.45f * std::sin(3.14159265f * e) - 0.25f * e);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(300.0f * e, p) * saved);
        DrawIconText(p.x - size * 0.5f, p.y - size * 0.5f, size, size, L"\xE735", L"*",
                     theme.accent, 1.0f);
        dc->SetTransform(saved);
    } else {
        if (!sm.star_landed) {
            sm.star_landed = true;
            // The row takes the star in: replay its gulp now that it lands.
            auto found = sm.counts.find(L"pulse:starred");
            if (found != sm.counts.end()) {
                if (found->second.prev.empty()) found->second.prev = found->second.last;
                found->second.start = now;
            }
        }
        const float b = std::clamp((t - kFlightMs) / kBurstMs, 0.0f, 1.0f);
        const float out = 1.0f - (1.0f - b) * (1.0f - b) * (1.0f - b);
        for (int k = 0; k < 8; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / 8.0f + 0.3f;
            const float rr = (5.0f + 15.0f * out) * scale_;
            const D2D1_POINT_2F q = D2D1::Point2F(to.x + std::cos(a) * rr, to.y + std::sin(a) * rr);
            MakeBrush(dc, WithAlpha(theme.accent, 0.8f * (1.0f - b)), brAccent_);
            const float r = 2.2f * (1.0f - b * 0.7f) * scale_;
            dc->FillEllipse(D2D1::Ellipse(q, r, r), brAccent_.get());
        }
        MakeBrush(dc, WithAlpha(theme.accent, 0.45f * (1.0f - b)), brAccent_);
        const float ring = (6.0f + 14.0f * out) * scale_;
        dc->DrawEllipse(D2D1::Ellipse(to, ring, ring), brAccent_.get(), 1.4f * scale_);
    }
    RequestMotionFrame();
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
        DrawTextEndEllipsis(compositor_, nameFmt,
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
                compositor_->DrawTextLayout(D2D1::Point2F(chipRc.left, chipRc.top), bl.get(),
                    color(card.cut ? (vm.dark ? rgba(0xFFB454, 1.0f) : rgba(0xB25E00, 1.0f))
                                   : theme.text_secondary));
            }
        }
        ty += lineName;
        std::wstring sub;
        if (!card.is_dir && card.size > 0)
            sub = pulse::format::ByteSize(card.size, true) + L" \xB7 ";
        sub += FormatListType(card.name, card.is_dir);
        DrawTextEndEllipsis(compositor_, subFmt, color(subColor), sub, tx, ty, tw, lineSub);
        ty += lineSub;
        if (!card.folder.empty() && factory) {
            auto measure = [&](const std::wstring& s) { return MeasureTextWidth(factory, folderFmt, s); };
            // The last line shares its row with the intent chip.
            const float dirW = card.batch >= 0
                ? std::max(0.0f, std::min(tw, chipRc.left - 6.0f * scale_ - tx)) : tw;
            const auto [head, last] = MiddleEllipsisPath(card.folder, dirW, measure);
            DrawTextEndEllipsis(compositor_, folderFmt, color(folderColor), head + last,
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
