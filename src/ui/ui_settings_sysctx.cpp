#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "typography.h"

#include <algorithm>
#include <cmath>

// Settings > 系统右键菜单 (page 6). Geometry and texts: settings_sysctx_layout.h;
// data: app::SystemCtxModel.
namespace pulse::ui {

namespace {

bool IsDark(const Theme& theme) {
    return theme.bg.r * 0.30f + theme.bg.g * 0.59f + theme.bg.b * 0.11f < 0.5f;
}

} // namespace

IDWriteTextFormat* MainRenderer::SysCtxTitleFormat() {
    const float s = scale_;
    if (sysctx_title_scale_ != s || !sysctx_title_format_.get()) {
        sysctx_title_format_.reset();
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Text, 13.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &sysctx_title_format_);
        if (sysctx_title_format_.get()) sysctx_title_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        sysctx_stat_format_.reset();
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Text, 22.0f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &sysctx_stat_format_);
        if (sysctx_stat_format_.get()) sysctx_stat_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        sysctx_title_scale_ = s;
    }
    return sysctx_title_format_.get() ? sysctx_title_format_.get() : compositor_->TextFormat();
}

void MainRenderer::DrawCtxLayerCards(const D2D1_RECT_F& pulse, const D2D1_RECT_F& system, bool system_current,
                                     bool other_hovered, const Theme& theme) {
    const float s = scale_;
    const bool dark = IsDark(theme);
    const D2D1_COLOR_F subtle = WithAlpha(theme.text, dark ? 0.06f : 0.05f);
    IDWriteTextFormat* title_format = SysCtxTitleFormat();
    auto layer = [&](const D2D1_RECT_F& r, const wchar_t* glyph, const wchar_t* title, const wchar_t* caption,
                     const wchar_t* tag, bool current, bool hover) {
        if (r.right <= r.left) return;
        painter_.FillRoundedRect(r, 10 * s, current ? WithAlpha(theme.accent, 0.14f) : hover ? theme.fill_input_hover
                                                                                             : theme.fill_input);
        painter_.StrokeRoundedRect(r, 10 * s, current ? WithAlpha(theme.accent, 0.55f) : theme.stroke_card, 1);
        const float mid = (r.top + r.bottom) * 0.5f;
        const auto box = D2D1::RectF(r.left + 14 * s, mid - 15 * s, r.left + 44 * s, mid + 15 * s);
        painter_.FillRoundedRect(box, 8 * s, subtle);
        DrawIconText(box.left + 7 * s, box.top + 7 * s, 16 * s, 16 * s, glyph, L"", theme.text, 0.8f);
        const float tag_w = CellTextWidth(tag, true) + 8 * s;
        const float text_right = r.right - 14 * s - tag_w;
        painter_.DrawText(title, D2D1::RectF(box.right + 12 * s, mid - 19 * s, text_right, mid + 1 * s),
                          title_format, theme.text);
        painter_.DrawText(caption, D2D1::RectF(box.right + 12 * s, mid + 1 * s, text_right, mid + 19 * s),
                          compositor_->SmallFormat(), theme.text_secondary);
        painter_.DrawText(tag, D2D1::RectF(text_right, r.top, r.right - 14 * s, r.bottom), compositor_->SmallFormat(),
                          theme.text_secondary, fluent::HorizontalAlignment::Right);
    };
    // The current page's card is accented; the other one is the way there.
    layer(pulse, L"\xE8FD", sysctx_text::LocalTitle(), sysctx_text::LocalCaption(),
          system_current ? sysctx_text::LocalTag() : sysctx_text::Current(), !system_current,
          system_current && other_hovered);
    layer(system, L"\xE7F4", sysctx_text::SystemTitle(), sysctx_text::SystemCaption(),
          system_current ? sysctx_text::Current() : sysctx_text::LocalTag(), system_current,
          !system_current && other_hovered);
}

void MainRenderer::DrawSettingsSysCtx(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const SysCtxLayout& o = l.sysctx;
    const SysCtxView* v = vm.settings_sysctx.get();
    const float s = scale_;
    auto* dc = compositor_->Dc();
    const bool dark = IsDark(theme);
    const D2D1_COLOR_F amber = dark ? D2D1::ColorF(0xE8B04A) : D2D1::ColorF(0xB7791F);
    const D2D1_COLOR_F red = dark ? D2D1::ColorF(0xE6716B) : D2D1::ColorF(0xC2413A);
    const D2D1_COLOR_F green = dark ? D2D1::ColorF(0x5CC489) : D2D1::ColorF(0x2E8B57);
    const D2D1_COLOR_F subtle = WithAlpha(theme.text, dark ? 0.06f : 0.05f);
    const bool ready = v && v->state == SysCtxView::State::Ready;
    IDWriteTextFormat* title_format = SysCtxTitleFormat();
    IDWriteTextFormat* stat_format = sysctx_stat_format_.get() ? sysctx_stat_format_.get() : title_format;
    auto hovered = [&](int index) { return IsHovered(vm, H::SettingsSysCtx, index); };
    auto button = [&](const D2D1_RECT_F& r, std::wstring_view text, std::wstring_view glyph, int hit, bool enabled,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(hit);
        // The stock button glyph is 20 px beside 14 px text; on this page's
        // text buttons it is drawn text-sized in the same slot instead.
        const bool small_glyph = !glyph.empty() && !text.empty() && kind == fluent::ButtonKind::Standard;
        fluent::ButtonSpec spec{r, text, glyph, kind, state};
        spec.skip_glyph = small_glyph;
        painter_.DrawButton(spec);
        if (small_glyph) {
            const float mid = (r.top + r.bottom) * 0.5f;
            DrawIconText(r.left + 8 * s, mid - 10 * s, 20 * s, 20 * s, std::wstring(glyph), L"",
                         enabled ? theme.text : theme.text_disabled, 0.72f);
        }
    };
    auto card = [&](const D2D1_RECT_F& r, float radius = 10.0f) {
        painter_.FillRoundedRect(r, radius * s, theme.fill_input);
        painter_.StrokeRoundedRect(r, radius * s, theme.stroke_card, 1);
    };
    auto icon_box = [&](const D2D1_RECT_F& box, const std::wstring& path, const wchar_t* glyph, float opacity,
                        bool plate = true) {
        if (plate) painter_.FillRoundedRect(box, 7 * s, subtle);
        const float pad = (box.right - box.left) * 0.15f;
        const auto inner = D2D1::RectF(box.left + pad, box.top + pad, box.right - pad, box.bottom - pad);
        bool drew = false;
        if (!path.empty()) {
            const size_t slash = path.find_last_of(L"\\/");
            drew = icon_cache_.Draw(dc, inner, path, slash == std::wstring::npos ? path : path.substr(slash + 1), false,
                                    FILE_ATTRIBUTE_NORMAL, opacity);
        }
        if (!drew)
            DrawIconText(inner.left, inner.top, inner.right - inner.left, inner.bottom - inner.top, glyph, L"",
                         WithAlpha(theme.text_secondary, opacity), 0.8f);
    };
    auto divider = [&](float x0, float x1, float y0) {
        painter_.FillRoundedRect(D2D1::RectF(x0, y0, x1, y0 + 1), 0, theme.stroke_divider);
    };

    // ---- header -------------------------------------------------------------
    painter_.DrawWrappedCaption(sysctx_text::Intro(), D2D1::Point2F(o.intro.left, o.intro.top + 6 * s),
                                o.intro.right - o.intro.left, theme.text_secondary);
    // Page commands: quiet caption-sized buttons, a fill only under the pointer.
    auto head_button = [&](const D2D1_RECT_F& r, std::wstring_view text, const wchar_t* glyph, int hit, bool enabled) {
        if (r.right <= r.left) return;
        if (enabled && hovered(hit)) painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
        const D2D1_COLOR_F color = enabled ? theme.text_secondary : theme.text_disabled;
        const float mid = (r.top + r.bottom) * 0.5f;
        DrawIconText(r.left + 6 * s, mid - 8 * s, 16 * s, 16 * s, glyph, L"", color, 0.62f);
        painter_.DrawText(text, D2D1::RectF(r.left + 24 * s, r.top, r.right - 4 * s, r.bottom), compositor_->SmallFormat(),
                          color);
    };
    head_button(o.undo, sysctx_text::Undo(), L"\xE7A7", kSysCtxUndo, ready && v->undo_steps > 0);
    head_button(o.log, sysctx_text::Log(), L"\xE81C", kSysCtxLog, true);
    head_button(o.restore, sysctx_text::RestoreAll(), L"\xE777", kSysCtxRestoreAll, ready && v->undo_steps > 0);

    // ---- layer cards ----------------------------------------------------------
    DrawCtxLayerCards(o.local_card, o.system_card, true, hovered(kSysCtxLocalLayer), theme);
    if (!v) return;

    // ---- result of the last batch --------------------------------------------
    if (o.notice.right > o.notice.left) {
        const D2D1_COLOR_F tone = v->message_error ? red : green;
        painter_.FillRoundedRect(o.notice, 8 * s, WithAlpha(tone, 0.10f));
        painter_.StrokeRoundedRect(o.notice, 8 * s, WithAlpha(tone, 0.35f), 1);
        DrawIconText(o.notice.left + 12 * s, o.notice.top + 12 * s, 16 * s, 16 * s,
                     v->message_error ? L"\xE783" : L"\xE73E", L"", tone, 0.8f);
        painter_.DrawText(v->message, D2D1::RectF(o.notice.left + 38 * s, o.notice.top, o.notice.right - 12 * s,
                                                  o.notice.bottom),
                          compositor_->TextFormat(), theme.text);
    }

    // ---- details (side panel or under the selected row) -----------------------
    auto draw_detail = [&](const SysCtxRowView& row) {
        const SysCtxDetailLayout& d = o.detail;
        if (d.bounds.bottom <= d.bounds.top) return;
        const bool inline_head = !o.wide;  // the row above already shows icon and name
        if (!inline_head) {
            const float hm = d.head.top + 16 * s;
            icon_box(D2D1::RectF(d.head.left, hm - 14 * s, d.head.left + 28 * s, hm + 14 * s), row.icon_path, row.glyph,
                     1.0f, false);
            painter_.DrawText(row.title, D2D1::RectF(d.head.left + 38 * s, d.head.top, d.head.right, d.head.top + 30 * s),
                              title_format, row.missing ? red : theme.text);
        }
        painter_.DrawText(row.kind_label,
                          D2D1::RectF(d.head.left, inline_head ? d.head.top : d.head.top + 30 * s, d.head.right, d.head.bottom),
                          compositor_->SmallFormat(), theme.text_secondary);
        const std::wstring values[4] = {row.registry, row.file, row.publisher, sysctx_text::Permission(row)};
        for (size_t i = 0; i < 4; ++i) {
            const auto& r = d.kv[i];
            painter_.DrawText(sysctx_text::Key(static_cast<int>(i)), D2D1::RectF(r.left, r.top, r.left + 64 * s, r.top + 18 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
            const D2D1_COLOR_F tone = i == 3 ? (row.machine ? amber : green) : (i == 1 && row.missing ? red : theme.text);
            painter_.DrawWrappedCaption(values[i].empty() ? L"—" : values[i], D2D1::Point2F(r.left + 74 * s, r.top),
                                        r.right - r.left - 74 * s, tone);
        }
        divider(d.switch_row.left, d.switch_row.right, d.switch_row.top);
        const float sm = (d.switch_row.top + d.switch_row.bottom) * 0.5f;
        painter_.DrawText(sysctx_text::EnabledTitle(), D2D1::RectF(d.switch_row.left, sm - 18 * s, d.switch_row.right - 60 * s, sm + 1 * s),
                          compositor_->TextFormat(), theme.text);
        painter_.DrawText(sysctx_text::EnabledCaption(), D2D1::RectF(d.switch_row.left, sm + 1 * s, d.switch_row.right - 60 * s, sm + 18 * s),
                          compositor_->SmallFormat(), theme.text_secondary);
        if (d.toggle.right > d.toggle.left) {
            fluent::ControlState state{};
            state.checked = row.on;
            state.enabled = ready;
            state.hovered = ready && hovered(kSysCtxDetailSwitch);
            painter_.DrawSwitch(d.toggle, L"", state);
        } else {
            const wchar_t* why = row.locked ? (row.system_owned ? sysctx_text::SystemOwned() : sysctx_text::Protected())
                                            : sysctx_text::ReadOnly();
            const float w = CellTextWidth(why, true);
            painter_.DrawText(why, D2D1::RectF(d.switch_row.right - w - 2 * s, d.switch_row.top, d.switch_row.right + 2 * s,
                                               d.switch_row.bottom),
                              compositor_->SmallFormat(), theme.text_secondary);
            DrawIconText(d.switch_row.right - w - 22 * s, sm - 8 * s, 16 * s, 16 * s, L"\xE72E", L"", theme.text_secondary, 0.7f);
        }
        if (d.pulse_row.bottom > d.pulse_row.top) {
            divider(d.pulse_row.left, d.pulse_row.right, d.pulse_row.top);
            const float pm = (d.pulse_row.top + d.pulse_row.bottom) * 0.5f;
            painter_.DrawText(sysctx_text::PulseShownTitle(),
                              D2D1::RectF(d.pulse_row.left, pm - 18 * s, d.pulse_row.right - 60 * s, pm + 1 * s),
                              compositor_->TextFormat(), theme.text);
            painter_.DrawText(sysctx_text::PulseShownCaption(),
                              D2D1::RectF(d.pulse_row.left, pm + 1 * s, d.pulse_row.right - 60 * s, pm + 18 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
            fluent::ControlState state{};
            state.checked = row.pulse == 1;
            state.hovered = hovered(kSysCtxDetailPulse);
            painter_.DrawSwitch(d.pulse_toggle, L"", state);
        }
        painter_.FillRoundedRect(d.method, 7 * s, subtle);
        painter_.DrawWrappedCaption(row.method, D2D1::Point2F(d.method.left + 10 * s, d.method.top + 8 * s),
                                    d.method.right - d.method.left - 20 * s, theme.text_secondary);
        button(d.copy, sysctx_text::CopyKey(), L"\xE8C8", kSysCtxDetailCopy, row.registry_copyable);
        button(d.folder, sysctx_text::OpenFolder(), L"\xE838", kSysCtxDetailFolder, row.registry_copyable);
        button(d.remove, row.deleting ? sysctx_text::KeepLeftover() : sysctx_text::DeleteLeftover(),
               row.deleting ? L"\xE7A7" : L"\xE74D", kSysCtxDetailDelete, ready);
    };

    // ---- scene list ------------------------------------------------------------
    if (o.scenes_card.right > o.scenes_card.left) {
        card(o.scenes_card);
        for (size_t g = 0; g < o.groups.size(); ++g) {
            const auto& r = o.groups[g];
            if (r.bottom <= r.top) continue;
            painter_.DrawText(sysctx_text::Group(static_cast<int>(g)), D2D1::RectF(r.left + 10 * s, r.top + 6 * s, r.right, r.bottom),
                              compositor_->SmallFormat(), theme.text_secondary);
        }
        // 概览 row above the groups (the prototype's home scene).
        if (o.overview_row.right > o.overview_row.left) {
            const auto& r = o.overview_row;
            if (v->overview.shown) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_selected);
                painter_.FillRoundedRect(D2D1::RectF(r.left, r.top + 8 * s, r.left + 3 * s, r.bottom - 8 * s), 1.5f * s, theme.accent);
            } else if (hovered(kSysCtxOverview)) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            }
            const float mid = (r.top + r.bottom) * 0.5f;
            DrawIconText(r.left + 10 * s, mid - 8 * s, 16 * s, 16 * s, L"\xE80F", L"", theme.text_secondary, 0.75f);
            painter_.DrawText(sysctx_text::Overview(), D2D1::RectF(r.left + 36 * s, r.top, r.right - 10 * s, r.bottom),
                              compositor_->TextFormat(), theme.text);
        }
        for (size_t i = 0; i < o.scenes.size() && i < v->scenes.size(); ++i) {
            const auto& r = o.scenes[i];
            const auto& scene = v->scenes[i];
            const bool active = !v->overview.shown && static_cast<int>(i) == v->scene;
            if (active) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_selected);
                painter_.FillRoundedRect(D2D1::RectF(r.left, r.top + 8 * s, r.left + 3 * s, r.bottom - 8 * s), 1.5f * s, theme.accent);
            } else if (hovered(kSysCtxSceneBase + static_cast<int>(i))) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            }
            const float mid = (r.top + r.bottom) * 0.5f;
            DrawIconText(r.left + 10 * s, mid - 8 * s, 16 * s, 16 * s, scene.glyph, L"", theme.text_secondary, 0.75f);
            // "31 · 3": the switched-off count in amber, like the prototype.
            const std::wstring off = scene.disabled > 0 ? L" · " + std::to_wstring(scene.disabled) : std::wstring();
            const std::wstring count = std::to_wstring(scene.count);
            float cr = r.right - 10 * s;
            if (scene.pending) {
                painter_.FillRoundedRect(D2D1::RectF(cr - 6 * s, mid - 3 * s, cr, mid + 3 * s), 3 * s, theme.accent);
                cr -= 10 * s;
            }
            if (!off.empty()) {
                const float w = CellTextWidth(off, true);
                painter_.DrawText(off, D2D1::RectF(cr - w - 2 * s, r.top, cr + 2 * s, r.bottom), compositor_->SmallFormat(),
                                  amber, fluent::HorizontalAlignment::Right);
                cr -= w;
            }
            const float cw = CellTextWidth(count, true);
            painter_.DrawText(count, D2D1::RectF(cr - cw - 2 * s, r.top, cr + 2 * s, r.bottom), compositor_->SmallFormat(),
                              theme.text_secondary, fluent::HorizontalAlignment::Right);
            painter_.DrawText(scene.label, D2D1::RectF(r.left + 36 * s, r.top, cr - cw - 6 * s, r.bottom),
                              compositor_->TextFormat(), theme.text);
        }
    }

    // ---- entry list ---------------------------------------------------------------
    if (o.list_card.right > o.list_card.left) {
        card(o.list_card);
        const auto& h = o.list_head;
        const std::wstring head_title = v->overview.shown ? std::wstring(sysctx_text::Overview()) : v->scene_title;
        const std::wstring head_keys = v->overview.shown ? sysctx_text::OverviewKey(*v) : v->scene_keys;
        const float title_w = CellTextWidth(head_title, false) * 1.08f + 4 * s;
        painter_.DrawText(head_title, D2D1::RectF(h.left + 14 * s, h.top, h.left + 14 * s + title_w, h.bottom),
                          title_format, theme.text);
        const float keys_right = (o.clean.right > o.clean.left ? o.clean.left : o.refresh.left) - 8 * s;
        painter_.DrawText(head_keys, D2D1::RectF(h.left + 24 * s + title_w, h.top, keys_right, h.bottom),
                          compositor_->SmallFormat(), theme.text_secondary);
        button(o.clean, sysctx_text::Clean(v->leftovers), L"\xE74D", kSysCtxClean, ready);
        {
            fluent::ControlState state{};
            state.enabled = ready;
            state.hovered = ready && hovered(kSysCtxRefresh);
            painter_.DrawButton({o.refresh, {}, L"\xE72C", fluent::ButtonKind::Transparent, state, true});
        }
        divider(h.left, h.right, h.bottom);
        auto chip = [&](const D2D1_RECT_F& r, const std::wstring& text, bool on, int hit, float radius) {
            const bool hover = hovered(hit);
            painter_.FillRoundedRect(r, radius, on ? WithAlpha(theme.accent, 0.16f) : hover ? theme.fill_hover
                                                                                            : WithAlpha(theme.text, 0.0f));
            painter_.StrokeRoundedRect(r, radius, on ? WithAlpha(theme.accent, 0.55f) : theme.stroke_card, 1);
            painter_.DrawText(text, r, compositor_->SmallFormat(), on ? theme.text : theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        };
        for (size_t f = 0; f < o.filters.size(); ++f)
            chip(o.filters[f], sysctx_text::Chip(*v, static_cast<int>(f)), static_cast<int>(f) == v->filter,
                 kSysCtxFilterBase + static_cast<int>(f), 13 * s);
        if (!o.subs.empty()) {
            const auto& first = o.subs.front();
            painter_.DrawText(sysctx_text::SubLabel(v->scene == 11),
                              D2D1::RectF(h.left + 12 * s, first.top, first.left, first.bottom), compositor_->SmallFormat(),
                              theme.text_secondary);
            for (size_t i = 0; i < o.subs.size() && i < v->subs.size(); ++i)
                chip(o.subs[i], v->subs[i], static_cast<int>(i) == v->sub, kSysCtxSubBase + static_cast<int>(i), 6 * s);
        }
        divider(h.left, h.right, o.rows_top.top);

        if (v->overview.shown) {
            // ---- 概览 cards ----
            const SysCtxOverview& ov = v->overview;
            const SysCtxOverviewLayout& g = o.overview;
            const D2D1_COLOR_F tile = D2D1::ColorF(0, 0, 0, dark ? 0.18f : 0.05f);
            const D2D1_COLOR_F violet = dark ? D2D1::ColorF(0xC9B8FF) : D2D1::ColorF(0x6B4FBB);
            const D2D1_COLOR_F salmon = dark ? D2D1::ColorF(0xFFB4AC) : red;
            auto ov_card = [&](const D2D1_RECT_F& r, const wchar_t* title, const wchar_t* caption) {
                if (r.right <= r.left) return;
                painter_.FillRoundedRect(r, 10 * s, subtle);
                painter_.StrokeRoundedRect(r, 10 * s, theme.stroke_card, 1);
                if (!title) return;
                painter_.DrawText(title, D2D1::RectF(r.left + 14 * s, r.top + 12 * s, r.right - 14 * s, r.top + 34 * s),
                                  title_format, theme.text);
                painter_.DrawWrappedCaption(caption, D2D1::Point2F(r.left + 14 * s, r.top + 36 * s), r.right - r.left - 28 * s,
                                            theme.text_secondary);
            };
            auto badge_at = [&](float& right, float mid, std::wstring_view text, fluent::BadgeKind kind) {
                const float w = painter_.MeasureBadgeWidth(text);
                fluent::BadgeSpec spec;
                spec.bounds = D2D1::RectF(right - w, mid - 10 * s, right, mid + 10 * s);
                spec.text = text;
                spec.kind = kind;
                painter_.DrawBadge(spec);
                right -= w + 6 * s;
            };

            // Stats.
            ov_card(g.stats_card, nullptr, nullptr);
            const int values[4] = {v->disabled, ov.marks, ov.slow, ov.changes};
            const D2D1_COLOR_F tones[4] = {theme.text, violet, salmon, theme.text};
            for (size_t i = 0; i < g.stats.size(); ++i) {
                const auto& r = g.stats[i];
                painter_.FillRoundedRect(r, 8 * s, tile);
                painter_.DrawText(std::to_wstring(values[i]), D2D1::RectF(r.left + 12 * s, r.top + 6 * s, r.right - 8 * s, r.top + 36 * s),
                                  stat_format, tones[i]);
                painter_.DrawText(sysctx_text::Stat(static_cast<int>(i)),
                                  D2D1::RectF(r.left + 12 * s, r.top + 36 * s, r.right - 8 * s, r.bottom - 4 * s),
                                  compositor_->SmallFormat(), theme.text_secondary);
            }

            // 慢扩展排行.
            ov_card(g.slow_card, sysctx_text::SlowTitle(), sysctx_text::SlowCaption());
            if (g.slow_empty.bottom > g.slow_empty.top)
                painter_.DrawText(sysctx_text::SlowEmpty(), g.slow_empty, compositor_->SmallFormat(), theme.text_secondary);
            const uint32_t max_ms = ov.slow_rows.empty() ? 1u : (std::max)(1u, ov.slow_rows.front().ms);
            for (size_t i = 0; i < g.slow_rows.size() && i < ov.slow_rows.size(); ++i) {
                const auto& r = g.slow_rows[i];
                const SysCtxSlowRow& row = ov.slow_rows[i];
                if (i > 0) divider(r.left, r.right, r.top);
                const float mid = (r.top + r.bottom) * 0.5f;
                icon_box(D2D1::RectF(r.left, mid - 14 * s, r.left + 28 * s, mid + 14 * s), row.icon_path, row.glyph,
                         row.on ? 1.0f : 0.5f);
                // Right to left: buttons or the state badge, the time, the bar; the name takes the rest.
                float right = r.right;
                const auto& toggle = g.slow_toggle[i];
                const auto& skip = g.slow_skip[i];
                if (toggle.right > toggle.left) {
                    button(toggle, sysctx_text::TurnOff(), {}, kSysCtxSlowToggleBase + static_cast<int>(i), ready);
                    right = toggle.left - 6 * s;
                }
                if (skip.right > skip.left) {
                    button(skip, sysctx_text::PulseSkip(), {}, kSysCtxSlowSkipBase + static_cast<int>(i), true);
                    right = skip.left - 6 * s;
                }
                if (!row.on)
                    badge_at(right, mid, row.pending ? sysctx_text::Queued() : sysctx_text::Off(),
                             row.pending ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral);
                if (row.pulse_skipped) badge_at(right, mid, sysctx_text::PulseSkipped(), fluent::BadgeKind::Neutral);
                const float value_w = 56 * s;
                painter_.DrawText(sysctx_text::Ms(row.ms), D2D1::RectF(right - value_w, r.top, right, r.bottom),
                                  compositor_->SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Right);
                right -= value_w + 10 * s;
                const float text_left = r.left + 38 * s;
                const float bar_w = std::clamp((right - text_left) * 0.3f, 50 * s, 120 * s);
                const auto track = D2D1::RectF(right - bar_w, mid - 3 * s, right, mid + 3 * s);
                painter_.FillRoundedRect(track, 3 * s, WithAlpha(theme.text, dark ? 0.07f : 0.08f));
                const float fill_w = bar_w * static_cast<float>(row.ms) / static_cast<float>(max_ms);
                if (fill_w > 0.5f) {
                    // #e8b04a → #e6716b, like the prototype's .bar i.
                    const auto fill = D2D1::RectF(track.left, track.top, track.left + fill_w, track.bottom);
                    const D2D1_GRADIENT_STOP stops_data[2] = {{0.0f, D2D1::ColorF(0xE8B04A)}, {1.0f, D2D1::ColorF(0xE6716B)}};
                    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
                    dc->CreateGradientStopCollection(stops_data, 2, &stops);
                    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
                    if (stops)
                        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(fill.left, 0),
                                                                                          D2D1::Point2F(fill.right, 0)),
                                                      stops.Get(), &brush);
                    if (brush) dc->FillRoundedRectangle(D2D1::RoundedRect(fill, 3 * s, 3 * s), brush.Get());
                    else painter_.FillRoundedRect(fill, 3 * s, amber);
                }
                right -= bar_w + 10 * s;
                const float text_right = (std::max)(text_left + 40 * s, right);
                painter_.DrawText(row.title, D2D1::RectF(text_left, r.top + 4 * s, text_right, mid + 1 * s), title_format,
                                  row.on ? theme.text : theme.text_secondary);
                painter_.DrawText(row.publisher, D2D1::RectF(text_left, mid, text_right, r.bottom - 3 * s),
                                  compositor_->SmallFormat(), theme.text_secondary);
            }

            // 来自其他工具的禁用标记.
            ov_card(g.marks_card, sysctx_text::MarksTitle(), sysctx_text::MarksCaption());
            if (g.marks_empty.bottom > g.marks_empty.top)
                painter_.DrawText(sysctx_text::MarksEmpty(), g.marks_empty, compositor_->SmallFormat(), theme.text_secondary);
            for (size_t i = 0; i < g.mark_rows.size() && i < ov.mark_rows.size() && i < g.mark_restore.size(); ++i) {
                const auto& r = g.mark_rows[i];
                const SysCtxMarkRow& row = ov.mark_rows[i];
                if (i > 0) divider(r.left, r.right, r.top);
                const float mid = (r.top + r.bottom) * 0.5f;
                icon_box(D2D1::RectF(r.left, mid - 14 * s, r.left + 28 * s, mid + 14 * s), row.icon_path, row.glyph, 1.0f);
                const auto& restore = g.mark_restore[i];
                button(restore, sysctx_text::Restore(), {}, kSysCtxMarkRestoreBase + static_cast<int>(i), ready);
                const float text_right = restore.left - 10 * s;
                painter_.DrawText(row.title, D2D1::RectF(r.left + 38 * s, r.top + 5 * s, text_right, mid + 1 * s),
                                  compositor_->TextFormat(), theme.text);
                painter_.DrawText(row.registry, D2D1::RectF(r.left + 38 * s, mid, text_right, r.bottom - 4 * s),
                                  compositor_->SmallFormat(), theme.text_secondary);
            }

            // 最近的改动.
            ov_card(g.recent_card, sysctx_text::RecentTitle(), sysctx_text::RecentCaption());
            if (g.recent_empty.bottom > g.recent_empty.top)
                painter_.DrawText(sysctx_text::RecentEmpty(), g.recent_empty, compositor_->SmallFormat(), theme.text_secondary);
            for (size_t i = 0; i < g.recent_rows.size() && i < ov.recent.size(); ++i) {
                const auto& r = g.recent_rows[i];
                const SysCtxChangeRow& row = ov.recent[i];
                if (i > 0) divider(r.left, r.right, r.top);
                const float mid = (r.top + r.bottom) * 0.5f;
                const std::wstring title = row.undone ? sysctx_text::UndonePrefix() + row.title : row.title;
                painter_.DrawText(title, D2D1::RectF(r.left, r.top + 3 * s, r.right, mid + 1 * s), compositor_->TextFormat(),
                                  row.undone ? theme.text_secondary : theme.text);
                painter_.DrawText(row.caption, D2D1::RectF(r.left, mid, r.right, r.bottom - 3 * s), compositor_->SmallFormat(),
                                  theme.text_secondary);
            }
        }

        if (o.empty.bottom > o.empty.top) {
            const std::wstring text = v->state == SysCtxView::State::Loading && v->total == 0
                                          ? std::wstring(sysctx_text::Loading())
                                          : std::wstring(sysctx_text::Empty(v->filter != kSysCtxFilterAll));
            painter_.DrawText(text, o.empty, compositor_->TextFormat(), theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        }
        const float rh = kSysCtxRowDip * s;
        for (size_t i = 0; i < o.rows.size() && i < v->rows.size(); ++i) {
            const auto r = o.rows[i];
            if (r.bottom < l.content.top || r.top > l.content.bottom) continue;  // off screen
            const SysCtxRowView& row = v->rows[i];
            const float mid = r.top + rh * 0.5f;
            if (row.selected) {
                painter_.FillRoundedRect(r, 8 * s, theme.fill_selected);
                painter_.StrokeRoundedRect(r, 8 * s, theme.stroke_card, 1);
            } else if (hovered(kSysCtxSelectBase + static_cast<int>(i)) || hovered(kSysCtxRowBase + static_cast<int>(i)) ||
                       hovered(kSysCtxMoreBase + static_cast<int>(i)) || hovered(kSysCtxRemoveBase + static_cast<int>(i))) {
                painter_.FillRoundedRect(r, 8 * s, theme.fill_hover);
            }
            if (row.pending)
                painter_.FillRoundedRect(D2D1::RectF(r.left + 2 * s, r.top + 12 * s, r.left + 5 * s, r.top + rh - 12 * s),
                                         1.5f * s, amber);
            const float dim = row.on ? 1.0f : 0.5f;
            icon_box(D2D1::RectF(r.left + 10 * s, mid - 14 * s, r.left + 38 * s, mid + 14 * s), row.icon_path, row.glyph, dim);

            // Right side: "…", then the switch or why there is none, then badges.
            {
                fluent::ControlState state{};
                state.hovered = hovered(kSysCtxMoreBase + static_cast<int>(i));
                painter_.DrawButton({o.more[i], {}, L"\xE712", fluent::ButtonKind::Transparent, state, true});
            }
            float badge_right = o.switches[i].left - 10 * s;
            if (row.deletable && i < o.removes.size()) {
                // Leftover: 删除 (red) in the switch's place; 撤回 once queued.
                const D2D1_RECT_F& b = o.removes[i];
                fluent::ControlState state{};
                state.enabled = ready;
                state.hovered = ready && hovered(kSysCtxRemoveBase + static_cast<int>(i));
                fluent::ButtonSpec spec{b, row.deleting ? sysctx_text::RowKeep() : sysctx_text::RowDelete(),
                                        row.deleting ? L"\xE7A7" : L"\xE74D",
                                        row.deleting ? fluent::ButtonKind::Standard : fluent::ButtonKind::Danger, state};
                spec.skip_glyph = true;
                painter_.DrawButton(spec);
                const float bm = (b.top + b.bottom) * 0.5f;
                DrawIconText(b.left + 8 * s, bm - 10 * s, 20 * s, 20 * s, row.deleting ? L"\xE7A7" : L"\xE74D", L"",
                             !ready ? theme.text_disabled : row.deleting ? theme.text : D2D1::ColorF(D2D1::ColorF::White),
                             0.62f);
                badge_right = b.left - 10 * s;
            } else if (SysCtxSwitchable(row)) {
                fluent::ControlState state{};
                state.checked = row.on;
                state.enabled = ready;
                state.hovered = ready && hovered(kSysCtxRowBase + static_cast<int>(i));
                painter_.DrawSwitch(o.switches[i], L"", state);
            } else {
                const wchar_t* note = row.locked ? (row.system_owned ? sysctx_text::SystemOwned() : sysctx_text::Protected())
                                                 : sysctx_text::ReadOnly();
                const float w = CellTextWidth(note, true);
                const float note_right = o.switches[i].right;
                painter_.DrawText(note, D2D1::RectF(note_right - w - 2 * s, r.top, note_right + 2 * s, r.top + rh),
                                  compositor_->SmallFormat(), theme.text_secondary);
                DrawIconText(note_right - w - 20 * s, mid - 8 * s, 16 * s, 16 * s, row.locked ? L"\xE72E" : L"\xE946", L"",
                             theme.text_secondary, 0.7f);
                badge_right = note_right - w - 30 * s;
            }
            const float text_left = r.left + 50 * s;
            auto badge = [&](std::wstring_view text, fluent::BadgeKind kind) {
                const float w = painter_.MeasureBadgeWidth(text);
                if (badge_right - w < text_left + 120 * s) return;  // the title keeps priority
                fluent::BadgeSpec spec;
                spec.bounds = D2D1::RectF(badge_right - w, mid - 10 * s, badge_right, mid + 10 * s);
                spec.text = text;
                spec.kind = kind;
                painter_.DrawBadge(spec);
                badge_right -= w + 6 * s;
            };
            if (row.machine) badge(sysctx_text::Admin(), fluent::BadgeKind::Warning);
            else if (SysCtxSwitchable(row)) badge(sysctx_text::NoAdmin(), fluent::BadgeKind::Success);
            // Queued state first: it must not be the badge that runs out of room.
            if (row.deleting) badge(sysctx_text::WillDelete(), fluent::BadgeKind::Danger);
            else if (row.pending) badge(sysctx_text::Queued(), fluent::BadgeKind::Accent);
            if (row.extended) badge(sysctx_text::Shift(), fluent::BadgeKind::Keycap);
            if (row.place != kSysCtxZoneDefault) badge(sysctx_text::Zone(row.place), fluent::BadgeKind::Neutral);
            if (row.default_hidden) badge(sysctx_text::DefaultHidden(), fluent::BadgeKind::Neutral);
            if (row.pulse == 2) badge(sysctx_text::PulseHidden(), fluent::BadgeKind::Neutral);
            if (row.missing && !row.deleting) badge(sysctx_text::Missing(), fluent::BadgeKind::Danger);

            const float text_right = (std::max)(text_left + 40 * s, badge_right - 6 * s);
            const D2D1_COLOR_F title_color = row.deleting ? theme.text_secondary : row.missing ? red
                                           : row.on ? theme.text : theme.text_secondary;
            painter_.DrawText(row.title, D2D1::RectF(text_left, r.top + 7 * s, text_right, mid + 2 * s),
                              compositor_->TextFormat(), title_color);
            if (row.deleting) {
                // Struck through until 应用更改 removes it.
                const float tw = (std::min)(CellTextWidth(row.title, false), text_right - text_left);
                const float ty = r.top + 7 * s + (mid + 2 * s - r.top - 7 * s) * 0.5f;
                painter_.FillRoundedRect(D2D1::RectF(text_left, ty - 0.5f * s, text_left + tw, ty + 0.5f * s), 0,
                                         theme.text_secondary);
            }
            painter_.DrawText(row.subtitle, D2D1::RectF(text_left, mid + 1 * s, text_right, r.top + rh - 6 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
            if (!o.wide && static_cast<int>(i) == v->selected) {
                divider(r.left + 46 * s, r.right - 12 * s, r.top + rh);
                draw_detail(row);
            }
        }
    }

    // ---- side panel: live preview / details ------------------------------------------
    if (o.side_card.right > o.side_card.left) {
        card(o.side_card);
        auto tab = [&](const D2D1_RECT_F& r, const wchar_t* text, bool on, int hit) {
            if (on) painter_.FillRoundedRect(r, 6 * s, theme.fill_selected);
            else if (hovered(hit)) painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            painter_.DrawText(text, r, compositor_->TextFormat(), on ? theme.text : theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
        };
        tab(o.tab_preview, sysctx_text::TabPreview(), v->side_tab == 0, kSysCtxTabPreview);
        tab(o.tab_detail, sysctx_text::TabDetail(), v->side_tab == 1, kSysCtxTabDetail);
        divider(o.side_card.left, o.side_card.right, o.tab_preview.bottom + 8 * s);
        const auto& b = o.side_body;
        if (v->side_tab == 1) {
            if (v->selected >= 0 && v->selected < static_cast<int>(v->rows.size())) draw_detail(v->rows[static_cast<size_t>(v->selected)]);
            else painter_.DrawWrappedCaption(sysctx_text::NoSelection(), D2D1::Point2F(b.left, b.top + 4 * s),
                                             b.right - b.left, theme.text_secondary);
        } else if (v->overview.shown) {
            // 概览: no scene chosen for the preview yet (the prototype's hint + infobox).
            const float w = b.right - b.left;
            painter_.DrawWrappedCaption(sysctx_text::OverviewSideHint(), D2D1::Point2F(b.left, b.top + 4 * s), w,
                                        theme.text_secondary);
            const float note_top = b.top + 4 * s + painter_.MeasureWrappedCaptionHeight(sysctx_text::OverviewSideHint(), w) + 14 * s;
            const float note_h = painter_.MeasureWrappedCaptionHeight(sysctx_text::OverviewSideNote(), w - 48 * s) + 24 * s;
            const auto note = D2D1::RectF(b.left, note_top, b.right, note_top + note_h);
            painter_.FillRoundedRect(note, 8 * s, subtle);
            painter_.StrokeRoundedRect(note, 8 * s, theme.stroke_card, 1);
            DrawIconText(note.left + 12 * s, note.top + 12 * s, 16 * s, 16 * s, L"\xE946", L"", theme.text_secondary, 0.8f);
            painter_.DrawWrappedCaption(sysctx_text::OverviewSideNote(), D2D1::Point2F(note.left + 36 * s, note.top + 12 * s),
                                        w - 48 * s, theme.text_secondary);
        } else {
            const SysCtxPreviewLayout pl = LayoutSysCtxPreview(*v, b, s);
            const bool live = pl.dragging && v->drag_item >= 0 && v->drag_item < static_cast<int>(v->preview.size());
            const SysCtxPreviewItem* held = live ? &v->preview[static_cast<size_t>(v->drag_item)] : nullptr;
            // Head: the scene; while dragging, what the drop can do.
            const std::wstring head = held ? std::wstring(held->movable ? sysctx_text::DragHintVerb() : sysctx_text::DragHintHandler())
                                           : sysctx_text::PreviewHead(*v);
            painter_.DrawText(head, pl.head, compositor_->SmallFormat(), held ? theme.accent : theme.text_secondary);

            auto grip = [&](float x, float cy, const D2D1_COLOR_F& color) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
                dc->CreateSolidColorBrush(color, &br);
                if (!br) return;
                for (int col = 0; col < 2; ++col)
                    for (int k = -1; k <= 1; ++k)
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + col * 3.5f * s, cy + k * 4.5f * s), 1.1f * s, 1.1f * s), br.Get());
            };
            auto insertion = [&](float x0, float x1, float y0) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
                dc->CreateSolidColorBrush(theme.accent, &br);
                if (!br) return;
                dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y0), br.Get(), 2 * s);
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inner;
                dc->CreateSolidColorBrush(theme.surface_flyout, &inner);
                if (inner) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0, y0), 3.5f * s, 3.5f * s), inner.Get());
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x0, y0), 3 * s, 3 * s), br.Get(), 1.5f * s);
            };
            // One menu item; `shelf`: drawn dimmed on the 已隐藏 shelf.
            auto draw_item = [&](const SysCtxPreviewItem& p, const D2D1_RECT_F& r, bool shelf) {
                if (p.change == 1) painter_.FillRoundedRect(r, 5 * s, WithAlpha(red, 0.12f));
                else if (p.change == 2) painter_.FillRoundedRect(r, 5 * s, WithAlpha(green, 0.12f));
                else if (p.moved) painter_.FillRoundedRect(r, 5 * s, WithAlpha(theme.accent, 0.12f));
                if (p.selected) painter_.StrokeRoundedRect(r, 5 * s, WithAlpha(theme.accent, 0.7f), 1);
                const float mid = (r.top + r.bottom) * 0.5f;
                const bool dim = shelf && p.change != 1;
                icon_box(D2D1::RectF(r.left + 12 * s, mid - 9 * s, r.left + 30 * s, mid + 9 * s), p.icon_path, p.glyph,
                         dim || p.change == 1 ? 0.5f : 1.0f, false);
                const wchar_t* tag = p.change == 1 ? sysctx_text::WillHide()
                                   : p.change == 2 ? sysctx_text::WillShow()
                                   : p.moved       ? sysctx_text::Moved(p.moved - 1)
                                   : !shelf && p.zone != kSysCtxZoneDefault ? sysctx_text::Zone(p.zone) : L"";
                const D2D1_COLOR_F tag_color = p.change == 1 ? red : p.change == 2 ? green : p.moved ? theme.accent : theme.text_secondary;
                const float tag_w = *tag ? CellTextWidth(tag, true) + 10 * s : 0.0f;
                std::wstring title = p.title;
                if (p.extended) title += L"  \x21E7";
                painter_.DrawText(title, D2D1::RectF(r.left + 38 * s, r.top, r.right - 8 * s - tag_w, r.bottom),
                                  compositor_->TextFormat(), dim || p.change == 1 ? theme.text_secondary : theme.text);
                if (*tag)
                    painter_.DrawText(tag, D2D1::RectF(r.right - 8 * s - tag_w, r.top, r.right - 8 * s, r.bottom),
                                      compositor_->SmallFormat(), tag_color, fluent::HorizontalAlignment::Right);
            };

            // Menu surface.
            painter_.FillRoundedRect(D2D1::RectF(pl.menu.left + 1 * s, pl.menu.top + 2 * s, pl.menu.right + 1 * s, pl.menu.bottom + 3 * s),
                                     8 * s, D2D1::ColorF(0, 0, 0, 0.06f));
            painter_.FillRoundedRect(pl.menu, 8 * s, theme.surface_flyout);
            painter_.StrokeRoundedRect(pl.menu, 8 * s, theme.stroke_card, 1);
            for (int z = 0; z < 3; ++z) {
                const auto& r = pl.zone_label[static_cast<size_t>(z)];
                if (r.bottom <= r.top) continue;
                const bool target = pl.drop_zone == z;
                const wchar_t* label = sysctx_text::Zone(z);
                const float w = CellTextWidth(label, true);
                painter_.DrawText(label, D2D1::RectF(r.left + 8 * s, r.top, r.left + 12 * s + w, r.bottom), compositor_->SmallFormat(),
                                  target ? theme.accent : theme.text_secondary);
                const float ly = (r.top + r.bottom) * 0.5f;
                painter_.FillRoundedRect(D2D1::RectF(r.left + 18 * s + w, ly, r.right - 8 * s, ly + 1),
                                         0, target ? WithAlpha(theme.accent, 0.6f) : theme.stroke_divider);
            }
            // 已隐藏 shelf.
            const int shelf_count = static_cast<int>(std::count_if(v->preview.begin(), v->preview.end(),
                                                                   [](const SysCtxPreviewItem& p) { return p.hidden; }));
            const bool shelf_target = pl.drop_zone == kSysCtxZoneHidden;
            painter_.DrawText(sysctx_text::ShelfHead(shelf_count), pl.shelf_head, compositor_->SmallFormat(),
                              shelf_target ? theme.accent : theme.text_secondary);
            painter_.FillRoundedRect(pl.shelf, 8 * s, shelf_target ? WithAlpha(theme.accent, 0.10f) : subtle);
            painter_.StrokeRoundedRect(pl.shelf, 8 * s, shelf_target ? WithAlpha(theme.accent, 0.6f) : theme.stroke_card, 1);
            if (shelf_count == 0 && !(shelf_target && pl.gap.bottom > pl.gap.top))
                painter_.DrawText(sysctx_text::ShelfEmpty(), pl.shelf, compositor_->SmallFormat(), theme.text_secondary,
                                  fluent::HorizontalAlignment::Center);

            for (size_t i = 0; i < pl.items.size() && i < v->preview.size(); ++i) {
                const auto& r = pl.items[i];
                if (r.bottom <= r.top) continue;
                const auto& p = v->preview[i];
                const bool hover = !pl.dragging && hovered(kSysCtxPreviewBase + static_cast<int>(i));
                if (hover) painter_.FillRoundedRect(r, 5 * s, theme.fill_hover);
                draw_item(p, r, p.hidden);
                if (hover && p.draggable) grip(r.left + 3 * s, (r.top + r.bottom) * 0.5f, theme.text_secondary);
            }
            if (pl.more_menu_count > 0)
                painter_.DrawText(sysctx_text::More(pl.more_menu_count),
                                  D2D1::RectF(pl.more_menu.left + 38 * s, pl.more_menu.top, pl.more_menu.right, pl.more_menu.bottom),
                                  compositor_->SmallFormat(), theme.text_secondary);
            if (pl.more_shelf_count > 0)
                painter_.DrawText(sysctx_text::More(pl.more_shelf_count),
                                  D2D1::RectF(pl.more_shelf.left + 38 * s, pl.more_shelf.top, pl.more_shelf.right, pl.more_shelf.bottom),
                                  compositor_->SmallFormat(), theme.text_secondary);
            if (held) {
                // The landing place, then the item riding on the pointer.
                insertion(pl.gap.left + 2 * s, pl.gap.right - 2 * s, (pl.gap.top + pl.gap.bottom) * 0.5f);
                const auto& g = pl.ghost;
                painter_.FillRoundedRect(D2D1::RectF(g.left - 1 * s, g.top + 3 * s, g.right + 1 * s, g.bottom + 6 * s), 7 * s,
                                         D2D1::ColorF(0.16f, 0.2f, 0.35f, 0.08f));
                painter_.FillRoundedRect(D2D1::RectF(g.left, g.top + 1 * s, g.right, g.bottom + 3 * s), 6 * s,
                                         D2D1::ColorF(0.16f, 0.2f, 0.35f, 0.08f));
                painter_.FillRoundedRect(g, 6 * s, theme.surface_flyout);
                painter_.StrokeRoundedRect(g, 6 * s, theme.stroke_card, 1);
                draw_item(*held, g, false);
                grip(g.left + 3 * s, (g.top + g.bottom) * 0.5f, theme.text_secondary);
            }

            // Legend.
            const float ly = pl.legend.top + 2 * s;
            float lx = b.left;
            auto legend = [&](const D2D1_COLOR_F& c, const wchar_t* text) {
                painter_.FillRoundedRect(D2D1::RectF(lx, ly + 5 * s, lx + 8 * s, ly + 13 * s), 2 * s, c);
                const float w = CellTextWidth(text, true);
                painter_.DrawText(text, D2D1::RectF(lx + 13 * s, ly, lx + 15 * s + w, ly + 18 * s), compositor_->SmallFormat(),
                                  theme.text_secondary);
                lx += w + 28 * s;
            };
            legend(WithAlpha(red, 0.7f), sysctx_text::WillHide());
            legend(WithAlpha(green, 0.7f), sysctx_text::WillShow());
            legend(WithAlpha(theme.accent, 0.7f), sysctx_text::WillMove());
            painter_.DrawText(sysctx_text::PreviewNote(), D2D1::RectF(b.left, ly + 20 * s, b.right, ly + 38 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
        }
    }

    // ---- floating pending bar ------------------------------------------------------------
    if (o.pending_bar.right > o.pending_bar.left) {
        const auto& r = o.pending_bar;
        painter_.FillRoundedRect(D2D1::RectF(r.left - 2 * s, r.top + 2 * s, r.right + 2 * s, r.bottom + 6 * s), 14 * s,
                                 D2D1::ColorF(0, 0.18f));
        painter_.FillRoundedRect(r, 12 * s, theme.surface_flyout);
        painter_.StrokeRoundedRect(r, 12 * s, theme.stroke_card, 1);
        const float mid = (r.top + r.bottom) * 0.5f;
        const auto box = D2D1::RectF(r.left + 14 * s, mid - 15 * s, r.left + 44 * s, mid + 15 * s);
        painter_.FillRoundedRect(box, 8 * s, WithAlpha(amber, 0.16f));
        const bool applying = v->state == SysCtxView::State::Applying;
        DrawIconText(box.left + 7 * s, box.top + 7 * s, 16 * s, 16 * s, applying ? L"\xE895" : L"\xEA18", L"", amber, 0.8f);
        const float text_right = o.discard.right > o.discard.left ? o.discard.left - 12 * s : r.right - 14 * s;
        if (applying) {
            painter_.DrawText(sysctx_text::Applying(), D2D1::RectF(box.right + 12 * s, r.top, text_right, r.bottom),
                              compositor_->TextFormat(), theme.text);
        } else {
            painter_.DrawText(sysctx_text::PendingTitle(v->pending), D2D1::RectF(box.right + 12 * s, mid - 19 * s, text_right, mid + 1 * s),
                              title_format, theme.text);
            painter_.DrawText(sysctx_text::PendingCaption(*v), D2D1::RectF(box.right + 12 * s, mid + 1 * s, text_right, mid + 19 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
        }
        button(o.discard, sysctx_text::Discard(), {}, kSysCtxDiscard, ready);
        button(o.apply, sysctx_text::Apply(), L"\xEA18", kSysCtxApply, ready, fluent::ButtonKind::Primary);
    }
}

// Top of preview item `item` at rest (grab offset when a press arms a drag).
float MainRenderer::SysCtxPreviewItemTop(const WindowViewModel& vm, const D2D1_RECT_F& rect, int item) {
    const SysCtxView* v = vm.settings_sysctx.get();
    if (!v || vm.settings_page != 6 || v->side_tab != 0) return NAN;
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    if (l.sysctx.side_body.bottom <= l.sysctx.side_body.top) return NAN;
    const auto pl = LayoutSysCtxPreview(*v, l.sysctx.side_body, scale_, false);
    if (item < 0 || item >= static_cast<int>(pl.items.size())) return NAN;
    const auto& r = pl.items[static_cast<size_t>(item)];
    return r.bottom > r.top ? r.top : NAN;
}

// Where the live drag would land if released at (x, y); kSysCtxZoneNone when
// the pointer left the side panel (the drop is cancelled, as on the Pulse menu page).
int MainRenderer::SysCtxDropZone(const WindowViewModel& vm, const D2D1_RECT_F& rect, float x, float y) {
    const SysCtxView* v = vm.settings_sysctx.get();
    if (!v || vm.settings_page != 6) return kSysCtxZoneNone;
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const auto& card = l.sysctx.side_card;
    const float slack = 40 * scale_;
    if (card.bottom <= card.top || x < card.left - slack || x > card.right + slack || y < card.top - slack ||
        y > card.bottom + slack)
        return kSysCtxZoneNone;
    return LayoutSysCtxPreview(*v, l.sysctx.side_body, scale_).drop_zone;
}

} // namespace pulse::ui
