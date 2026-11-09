#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "typography.h"

#include <algorithm>
#include <cmath>

// Settings > 快捷键 (page 7). Geometry and texts: settings_shortcuts_layout.h;
// data: app::ShortcutSettings.
namespace pulse::ui {

namespace {

bool ShortcutsDark(const Theme& theme) {
    return theme.bg.r * 0.30f + theme.bg.g * 0.59f + theme.bg.b * 0.11f < 0.5f;
}

} // namespace

void MainRenderer::DrawSettingsShortcuts(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    const ShortcutsView* v = vm.settings_shortcuts.get();
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const ShortcutsLayout& o = l.shortcuts;
    const float s = scale_;
    const bool dark = ShortcutsDark(theme);
    const D2D1_COLOR_F amber = dark ? D2D1::ColorF(0xE8B04A) : D2D1::ColorF(0xB7791F);
    const D2D1_COLOR_F red = dark ? D2D1::ColorF(0xE6716B) : D2D1::ColorF(0xC2413A);
    const D2D1_COLOR_F green = dark ? D2D1::ColorF(0x5CC489) : D2D1::ColorF(0x2E8B57);
    const D2D1_COLOR_F subtle = WithAlpha(theme.text, dark ? 0.06f : 0.05f);
    IDWriteTextFormat* title_format = SysCtxTitleFormat();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    auto level_color = [&](uint8_t level) { return level >= 3 ? red : level == 2 ? amber : green; };
    auto hot = [&](int index) { return IsHovered(vm, H::SettingsShortcuts, index); };
    auto text_width = [&](std::wstring_view text, IDWriteTextFormat* format) {
        if (text.empty() || !format) return 0.0f;
        ComPtr<IDWriteTextLayout> measure;
        compositor_->DwriteFactory()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 4000.0f,
                                                       100.0f, &measure);
        DWRITE_TEXT_METRICS metrics{};
        if (!measure.get() || FAILED(measure->GetMetrics(&metrics))) return 0.0f;
        return metrics.widthIncludingTrailingWhitespace;
    };
    auto push_button = [&](const D2D1_RECT_F& r, std::wstring_view text, std::wstring_view glyph, int hit, bool enabled,
                           fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool checked = false) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hot(hit);
        state.checked = checked;
        // A pressed tool (按键查找 listening, a popover open) reads as a toggle.
        if (checked && kind == fluent::ButtonKind::Standard) kind = fluent::ButtonKind::Toggle;
        const bool small_glyph = !glyph.empty() && !text.empty();
        fluent::ButtonSpec spec{r, text, glyph, kind, state};
        spec.icon_only = text.empty();
        spec.skip_glyph = small_glyph;
        painter_.DrawButton(spec);
        if (small_glyph) {
            const float mid = (r.top + r.bottom) * 0.5f;
            const D2D1_COLOR_F color = !enabled ? theme.text_disabled
                : kind == fluent::ButtonKind::Primary || (kind == fluent::ButtonKind::Toggle && checked) ? theme.accent_text
                                                                                                       : theme.text;
            DrawIconText(r.left + 8 * s, mid - 10 * s, 20 * s, 20 * s, std::wstring(glyph), L"", color, 0.72f);
        }
    };
    // One chord as keycaps from x, vertically centred on mid; returns the right edge.
    auto draw_caps = [&](const std::vector<std::wstring>& caps, float x, float mid, uint8_t level, bool muted) {
        const float h = kScCapDip * s;
        const D2D1_COLOR_F face = dark ? WithAlpha(D2D1::ColorF(0xFFFFFF), 0.07f) : D2D1::ColorF(0xFFFFFF);
        const D2D1_COLOR_F edge = level >= 2 ? WithAlpha(level_color(level), 0.75f) : theme.stroke_card;
        const D2D1_COLOR_F ink = muted ? theme.text_secondary : level >= 2 ? level_color(level) : theme.text;
        for (size_t i = 0; i < caps.size(); ++i) {
            const float w = ShortcutCapWidth(&painter_, caps[i], s);
            const auto r = D2D1::RectF(x, mid - h * 0.5f, x + w, mid + h * 0.5f);
            painter_.FillRoundedRect(r, 4 * s, face);
            painter_.StrokeRoundedRect(r, 4 * s, edge, 1);
            // A slightly heavier bottom edge reads as a key.
            painter_.FillRoundedRect(D2D1::RectF(r.left + 2 * s, r.bottom - 1.5f * s, r.right - 2 * s, r.bottom), 0,
                                     WithAlpha(edge, 0.6f));
            painter_.DrawText(caps[i], D2D1::RectF(r.left + 2 * s, r.top, r.right - 2 * s, r.bottom), small_fmt, ink,
                              fluent::HorizontalAlignment::Center);
            x += w + 3 * s;
        }
        return x - 3 * s;
    };

    // ---- header ---------------------------------------------------------------
    painter_.DrawWrappedCaption(shortcuts_text::Lead(), D2D1::Point2F(o.lead.left, o.lead.top), o.lead.right - o.lead.left,
                                theme.text_secondary);
    if (o.search.right > o.search.left && v) {
        fluent::TextFieldSpec field{};
        field.bounds = o.search;
        field.leading_glyph = kIconSearch;
        field.compact_leading_glyph = true;
        const bool show_caps = !v->find_caps.empty() && !v->finding;
        field.text = show_caps ? std::wstring_view() : std::wstring_view(v->query);
        field.placeholder = v->finding ? shortcuts_text::FindWaiting() : shortcuts_text::SearchHint();
        field.state.focused = v->search_focused || v->finding;
        field.state.hovered = hot(kScSearch);
        field.trailing_width = o.search_clear.right > o.search_clear.left ? 22.0f : 0.0f;
        painter_.DrawTextField(field);
        if (!v->search_focused) shortcut_caret_focus_ = false;
        const float text_left = o.search.left + 10 * s + 18 * s + 6 * s;
        if (show_caps) {
            draw_caps(v->find_caps, text_left, (o.search.top + o.search.bottom) * 0.5f, 0, false);
        } else if (v->search_focused && [&] {
            // Blink like a native caret, solid again after each edit.
            if (!shortcut_caret_focus_ || v->query != shortcut_caret_query_) {
                shortcut_caret_query_ = v->query;
                shortcut_caret_epoch_ = GetTickCount();
            }
            shortcut_caret_focus_ = true;
            const UINT blink = GetCaretBlinkTime();
            if (blink == 0 || blink == INFINITE) return true;
            const DWORD since = GetTickCount() - shortcut_caret_epoch_;
            caret_due_ms_ = std::max<UINT>(1, blink - since % blink);
            return (since / blink) % 2u == 0u;
        }()) {
            const float caret_x = text_left + text_width(v->query, compositor_->TextFormat()) + 1 * s;
            if (caret_x < o.search.right - 30 * s)
                painter_.FillRoundedRect(D2D1::RectF(caret_x, o.search.top + 8 * s, caret_x + 1 * s, o.search.bottom - 8 * s), 0,
                                         theme.text);
        }
        if (o.search_clear.right > o.search_clear.left) {
            if (hot(kScSearchClear)) painter_.FillRoundedRect(o.search_clear, 4 * s, theme.fill_hover);
            const float mid_x = (o.search_clear.left + o.search_clear.right) * 0.5f;
            const float mid_y = (o.search_clear.top + o.search_clear.bottom) * 0.5f;
            DrawIconText(mid_x - 8 * s, mid_y - 8 * s, 16 * s, 16 * s, kIconClose, L"x", theme.text_secondary, 0.55f);
        }
    }
    push_button(o.find, shortcuts_text::Find(), L"\xE7C8", kScFind, v != nullptr, fluent::ButtonKind::Standard,
                v && v->finding);
    push_button(o.tester, shortcuts_text::Tester(), L"\xE765", kScTester, v != nullptr, fluent::ButtonKind::Standard,
                v && v->pop.kind == ShortcutPopView::Tester);
    push_button(o.more, L"", L"\xE712", kScMore, v != nullptr, fluent::ButtonKind::Standard,
                v && v->pop.kind == ShortcutPopView::More);
    if (!v) return;

    // ---- conflict strip ---------------------------------------------------------
    {
        const int issues = v->red + v->amber;
        const D2D1_COLOR_F tone = issues ? (v->red ? red : amber) : v->probing ? theme.text_secondary : green;
        painter_.FillRoundedRect(o.strip, 8 * s, WithAlpha(tone, dark ? 0.12f : 0.08f));
        painter_.StrokeRoundedRect(o.strip, 8 * s, WithAlpha(tone, 0.30f), 1);
        const float head_mid = o.strip.top + 22 * s;
        DrawIconText(o.strip.left + 10 * s, head_mid - 10 * s, 20 * s, 20 * s,
                     issues ? L"\xE7BA" : v->probing ? L"\xE895" : L"\xE73E", L"!", tone, 0.72f);
        const std::wstring title = issues ? shortcuts_text::StripTitle(issues)
                                          : std::wstring(v->probing ? shortcuts_text::Probing() : shortcuts_text::AllGood());
        painter_.DrawText(title, D2D1::RectF(o.strip_text.left, o.strip.top, o.strip_text.right + 40 * s, o.strip.top + 44 * s),
                          title_format, theme.text);
        for (size_t i = 0; i < o.chips.size() && i < v->chips.size(); ++i) {
            const auto& r = o.chips[i];
            const D2D1_COLOR_F c = level_color(v->chips[i].level);
            const bool over = hot(kScChipBase + static_cast<int>(i));
            painter_.FillRoundedRect(r, (r.bottom - r.top) * 0.5f, WithAlpha(c, over ? 0.22f : 0.13f));
            painter_.DrawText(v->chips[i].text, D2D1::RectF(r.left + 7 * s, r.top, r.right - 7 * s, r.bottom), small_fmt,
                              dark ? theme.text : c, fluent::HorizontalAlignment::Center);
        }
        // 重新检测: a quiet caption button.
        const auto& r = o.recheck;
        const bool enabled = !v->probing;
        if (enabled && hot(kScRecheck)) painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
        const D2D1_COLOR_F color = enabled ? theme.text_secondary : theme.text_disabled;
        const float mid = (r.top + r.bottom) * 0.5f;
        DrawIconText(r.left + 6 * s, mid - 8 * s, 16 * s, 16 * s, L"\xE72C", L"", color, 0.62f);
        painter_.DrawText(shortcuts_text::Recheck(), D2D1::RectF(r.left + 24 * s, r.top, r.right - 4 * s, r.bottom), small_fmt,
                          color);
    }

    // ---- filter bar ---------------------------------------------------------------
    {
        const auto track = D2D1::RectF(o.seg[0].left - 3 * s, o.seg[0].top - 3 * s, o.seg[2].right + 3 * s, o.seg[2].bottom + 3 * s);
        painter_.FillRoundedRect(track, 8 * s, subtle);
        for (int i = 0; i < 3; ++i) {
            const auto& r = o.seg[i];
            const bool on = v->filter == i;
            if (on) {
                painter_.FillRoundedRect(r, 6 * s, dark ? WithAlpha(D2D1::ColorF(0xFFFFFF), 0.10f) : D2D1::ColorF(0xFFFFFF));
                painter_.StrokeRoundedRect(r, 6 * s, theme.stroke_card, 1);
            } else if (hot(kScFilterBase + i)) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            }
            D2D1_COLOR_F ink = on ? theme.text : theme.text_secondary;
            if (i == 1 && v->counts[1] > 0) ink = v->red ? red : amber;
            painter_.DrawText(shortcuts_text::FilterChip(*v, i), r, small_fmt, ink, fluent::HorizontalAlignment::Center);
        }
        push_button(o.reset_all, shortcuts_text::ResetAll(), L"\xE777", kScResetAll, v->any_modified);
    }

    // ---- cards ----------------------------------------------------------------------
    if (o.empty.right > o.empty.left)
        painter_.DrawText(shortcuts_text::Empty(), o.empty, compositor_->TextFormat(), theme.text_secondary,
                          fluent::HorizontalAlignment::Center);
    for (const auto& card : o.cards) {
        if (card.rect.bottom < l.content.top || card.rect.top > l.content.bottom) continue;
        const auto& group = v->groups[static_cast<size_t>(card.group)];
        painter_.FillRoundedRect(card.rect, 10 * s, theme.fill_input);
        painter_.StrokeRoundedRect(card.rect, 10 * s, theme.stroke_card, 1);
        const float title_w = text_width(group.title, title_format);
        painter_.DrawText(group.title, D2D1::RectF(card.head.left + 16 * s, card.head.top, card.head.right - 16 * s, card.head.bottom),
                          title_format, theme.text);
        if (group.issues > 0) {
            const std::wstring n = std::to_wstring(group.issues);
            const float bx = card.head.left + 16 * s + title_w + 8 * s;
            const float mid = (card.head.top + card.head.bottom) * 0.5f;
            const auto badge = D2D1::RectF(bx, mid - 9 * s, bx + (std::max)(18 * s, SysCtxTextWidth(&painter_, n, s) - 8 * s), mid + 9 * s);
            painter_.FillRoundedRect(badge, 9 * s, level_color(group.level));
            painter_.DrawText(n, badge, small_fmt, D2D1::ColorF(0xFFFFFF), fluent::HorizontalAlignment::Center);
        }
        painter_.FillRoundedRect(D2D1::RectF(card.head.left + 12 * s, card.head.bottom - 1, card.head.right - 12 * s, card.head.bottom),
                                 0, theme.stroke_divider);
        for (const auto& rl : card.rows) {
            const auto& row = group.rows[static_cast<size_t>(rl.row)];
            const auto& r = rl.rect;
            const float mid = (r.top + r.bottom) * 0.5f;
            bool row_hot = !row.fixed && hot(kScRowBase + row.action);
            for (size_t k = 0; k < rl.keys.size() && !row_hot; ++k)
                row_hot = hot(kScKeyBase + row.action * 4 + static_cast<int>(k));
            const bool add_hot = hot(kScKeyBase + row.action * 4 + 3);
            row_hot = row_hot || add_hot;
            const bool editing = v->pop.action == row.action &&
                (v->pop.kind == ShortcutPopView::Record || v->pop.kind == ShortcutPopView::Range || v->pop.kind == ShortcutPopView::Info);
            if (row.level >= 2) {
                const D2D1_COLOR_F c = level_color(row.level);
                painter_.FillRoundedRect(r, 6 * s, WithAlpha(c, dark ? 0.14f : 0.08f));
                painter_.FillRoundedRect(D2D1::RectF(r.left, r.top + 7 * s, r.left + 3 * s, r.bottom - 7 * s), 1.5f * s, c);
            } else if (row.found || editing) {
                painter_.FillRoundedRect(r, 6 * s, WithAlpha(theme.accent, dark ? 0.18f : 0.10f));
            } else if (row_hot) {
                painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            }
            // Name, then the 已修改 dot and the 全局 tag right after it.
            const float name_left = r.left + 12 * s;
            const float name_w = (std::min)(text_width(row.name, compositor_->TextFormat()), (std::max)(0.0f, rl.name_right - name_left));
            painter_.DrawText(row.name, D2D1::RectF(name_left, r.top, (std::max)(name_left, rl.name_right), r.bottom),
                              compositor_->TextFormat(), row.fixed ? theme.text_secondary : theme.text);
            float tx = name_left + name_w + 6 * s;
            if (row.modified && tx + 6 * s < rl.name_right) {
                painter_.FillRoundedRect(D2D1::RectF(tx, mid - 3 * s, tx + 6 * s, mid + 3 * s), 3 * s, theme.accent);
                tx += 12 * s;
            }
            auto tag = [&](std::wstring_view text) {
                const float w = SysCtxTextWidth(&painter_, text, s) - 2 * s;
                if (tx + w > rl.name_right) return;
                const auto t = D2D1::RectF(tx, mid - 9 * s, tx + w, mid + 9 * s);
                painter_.FillRoundedRect(t, 9 * s, subtle);
                painter_.DrawText(text, t, small_fmt, theme.text_secondary, fluent::HorizontalAlignment::Center);
                tx += w + 4 * s;
            };
            if (row.global) tag(shortcuts_text::GlobalTag());
            if (row.global_off) tag(shortcuts_text::GlobalOff());
            // Keys, joined by "/".
            if (row.range) {
                if (!rl.keys.empty()) {
                    const auto& k = rl.keys.front();
                    if (hot(kScKeyBase + row.action * 4)) painter_.FillRoundedRect(k, 6 * s, theme.fill_hover);
                    const uint8_t level = row.bindings.empty() ? 0 : row.bindings.front().level;
                    draw_caps(ShortcutRangeCaps(row), k.left + 4 * s, mid, level, false);
                }
            } else {
                for (size_t k = 0; k < rl.keys.size() && k < row.bindings.size(); ++k) {
                    const auto& kr = rl.keys[k];
                    if (!row.fixed && hot(kScKeyBase + row.action * 4 + static_cast<int>(k)))
                        painter_.FillRoundedRect(kr, 6 * s, theme.fill_hover);
                    draw_caps(row.bindings[k].caps, kr.left + 4 * s, mid, row.bindings[k].level, row.fixed);
                    if (k + 1 < rl.keys.size())
                        painter_.DrawText(L"/", D2D1::RectF(kr.right, r.top, rl.keys[k + 1].left, r.bottom), small_fmt,
                                          theme.text_disabled, fluent::HorizontalAlignment::Center);
                }
                if (row.bindings.empty()) {
                    const float w = SysCtxTextWidth(&painter_, shortcuts_text::Unassigned(), s);
                    const float right = rl.add.right > rl.add.left ? rl.add.left - 4 * s : r.right - 36 * s;
                    painter_.DrawText(shortcuts_text::Unassigned(), D2D1::RectF(right - w, r.top, right, r.bottom), small_fmt,
                                      theme.text_secondary, fluent::HorizontalAlignment::Right);
                }
            }
            if (row.fixed && !rl.keys.empty())
                DrawIconText(rl.keys.front().left - 20 * s, mid - 8 * s, 16 * s, 16 * s, L"\xE72E", L"", theme.text_disabled, 0.6f);
            if (rl.add.right > rl.add.left && (row_hot || row.bindings.empty())) {
                if (add_hot) painter_.FillRoundedRect(rl.add, 6 * s, theme.fill_hover);
                painter_.StrokeRoundedRect(rl.add, 6 * s, theme.stroke_card, 1);
                const float ax = (rl.add.left + rl.add.right) * 0.5f;
                DrawIconText(ax - 8 * s, mid - 8 * s, 16 * s, 16 * s, L"\xE710", L"+", theme.text_secondary, 0.55f);
            }
        }
    }

    // ---- popover ---------------------------------------------------------------------
    const auto& p = v->pop;
    if (p.kind != ShortcutPopView::None && o.pop.right > o.pop.left) {
        // A soft shadow, then the flyout surface.
        painter_.FillRoundedRect(D2D1::RectF(o.pop.left - 1 * s, o.pop.top + 2 * s, o.pop.right + 1 * s, o.pop.bottom + 6 * s), 12 * s,
                                 WithAlpha(D2D1::ColorF(0x000000), dark ? 0.35f : 0.10f));
        painter_.DrawMenuSurface(o.pop);
        painter_.DrawText(p.title, o.pop_title, title_format, theme.text);
        if (hot(kScPopClose)) painter_.FillRoundedRect(o.pop_close, 4 * s, theme.fill_hover);
        DrawIconText((o.pop_close.left + o.pop_close.right) * 0.5f - 8 * s, (o.pop_close.top + o.pop_close.bottom) * 0.5f - 8 * s,
                     16 * s, 16 * s, kIconClose, L"x", theme.text_secondary, 0.55f);
        if (o.pop_sub.right > o.pop_sub.left)
            painter_.DrawWrappedCaption(p.subtitle, D2D1::Point2F(o.pop_sub.left, o.pop_sub.top), o.pop_sub.right - o.pop_sub.left,
                                        theme.text_secondary);
        for (size_t i = 0; i < o.pop_more.size() && i < p.more_items.size(); ++i) {
            const auto& r = o.pop_more[i];
            if (hot(kScMoreBase + static_cast<int>(i))) painter_.FillRoundedRect(r, 6 * s, theme.fill_hover);
            painter_.DrawText(p.more_items[i], D2D1::RectF(r.left + 10 * s, r.top, r.right - 10 * s, r.bottom),
                              compositor_->TextFormat(), static_cast<int>(i) == kScMoreReset ? red : theme.text);
        }
        if (o.pop_box.right > o.pop_box.left) {
            const bool live = p.kind == ShortcutPopView::Record || p.waiting;
            painter_.FillRoundedRect(o.pop_box, 8 * s, subtle);
            painter_.StrokeRoundedRect(o.pop_box, 8 * s, live ? theme.accent : theme.stroke_card, live ? 1.5f * s : 1.0f);
            const float mid = (o.pop_box.top + o.pop_box.bottom) * 0.5f;
            if (p.caps.empty()) {
                painter_.DrawText(shortcuts_text::PressKeys(), o.pop_box, compositor_->TextFormat(), theme.text_secondary,
                                  fluent::HorizontalAlignment::Center);
            } else {
                const float w = ShortcutCapsWidth(&painter_, p.caps, s);
                draw_caps(p.caps, (o.pop_box.left + o.pop_box.right - w) * 0.5f, mid, p.waiting ? 0 : p.level, false);
            }
        }
        static constexpr const wchar_t* kMods[] = {L"Ctrl", L"Alt", L"Shift"};
        static constexpr uint8_t kModBits[] = {0x2, 0x1, 0x4};
        for (int i = 0; i < 3; ++i)
            push_button(o.pop_mods[i], kMods[i], L"", kScPopModBase + i, true, fluent::ButtonKind::Toggle,
                        (p.range_mods & kModBits[i]) != 0);
        for (size_t i = 0; i < o.pop_digits.size() && i < p.digit_labels.size(); ++i) {
            const auto& r = o.pop_digits[i];
            const uint8_t level = i < p.digit_levels.size() ? p.digit_levels[i] : 0;
            const D2D1_COLOR_F c = level_color(level);
            painter_.FillRoundedRect(r, 5 * s, WithAlpha(c, level >= 2 ? 0.20f : 0.12f));
            painter_.DrawText(p.digit_labels[i], r, small_fmt, level >= 2 ? c : theme.text, fluent::HorizontalAlignment::Center);
        }
        if (o.pop_verdict.right > o.pop_verdict.left) {
            const D2D1_COLOR_F c = level_color(p.level);
            painter_.FillRoundedRect(o.pop_verdict, 8 * s, WithAlpha(c, dark ? 0.16f : 0.09f));
            const wchar_t* glyph = p.level >= 3 ? L"\xEA39" : p.level == 2 ? L"\xE7BA" : p.level == 1 ? L"\xE946" : L"\xE73E";
            DrawIconText(o.pop_verdict.left + 8 * s, o.pop_verdict.top + 10 * s, 20 * s, 20 * s, glyph, L"!", c, 0.72f);
            painter_.DrawText(p.verdict, D2D1::RectF(o.pop_verdict.left + 30 * s, o.pop_verdict.top + 10 * s,
                                                     o.pop_verdict.right - 8 * s, o.pop_verdict.top + 30 * s),
                              title_format, theme.text);
            if (o.pop_detail.right > o.pop_detail.left)
                painter_.DrawWrappedCaption(p.detail, D2D1::Point2F(o.pop_detail.left, o.pop_detail.top),
                                            o.pop_detail.right - o.pop_detail.left, theme.text_secondary);
        }
        for (size_t i = 0; i < o.pop_steps.size() && i < p.steps.size(); ++i) {
            const auto& r = o.pop_steps[i];
            const auto dot = D2D1::RectF(r.left, r.top + 1 * s, r.left + 18 * s, r.top + 19 * s);
            painter_.FillRoundedRect(dot, 9 * s, subtle);
            painter_.DrawText(std::to_wstring(i + 1), dot, small_fmt, theme.text_secondary, fluent::HorizontalAlignment::Center);
            painter_.DrawWrappedCaption(p.steps[i], D2D1::Point2F(r.left + 24 * s, r.top), r.right - r.left - 24 * s, theme.text);
        }
        if (o.pop_suggest_label.right > o.pop_suggest_label.left)
            painter_.DrawText(shortcuts_text::Suggest(), o.pop_suggest_label, small_fmt, theme.text_secondary);
        for (size_t i = 0; i < o.pop_suggest.size() && i < p.suggestions.size(); ++i) {
            const auto& r = o.pop_suggest[i];
            const bool over = hot(kScPopSuggestBase + static_cast<int>(i));
            painter_.FillRoundedRect(r, 6 * s, WithAlpha(theme.accent, over ? 0.18f : 0.08f));
            painter_.StrokeRoundedRect(r, 6 * s, WithAlpha(theme.accent, 0.45f), 1);
            painter_.DrawText(p.suggestions[i], r, small_fmt, theme.text, fluent::HorizontalAlignment::Center);
        }
        push_button(o.pop_ignore, shortcuts_text::Ignore(), L"", kScPopIgnore, true, fluent::ButtonKind::Transparent);
        push_button(o.pop_record, shortcuts_text::Record(), L"", kScPopRecord, true, fluent::ButtonKind::Transparent);
        push_button(o.pop_reset, shortcuts_text::Reset(), L"", kScPopReset, true, fluent::ButtonKind::Transparent);
        push_button(o.pop_remove, shortcuts_text::Remove(), L"", kScPopRemove, true, fluent::ButtonKind::Transparent);
        push_button(o.pop_cancel, shortcuts_text::Cancel(), L"", kScPopClose, true);
        push_button(o.pop_save, p.save_label, L"", kScPopSave, p.can_save, fluent::ButtonKind::Primary);
    }

    // ---- toast -------------------------------------------------------------------------
    if (o.toast.right > o.toast.left) {
        const D2D1_COLOR_F plate = dark ? D2D1::ColorF(0xF3F3F3) : D2D1::ColorF(0x2B2B2B);
        const D2D1_COLOR_F ink = dark ? D2D1::ColorF(0x1B1B1B) : D2D1::ColorF(0xF5F5F5);
        painter_.FillRoundedRect(D2D1::RectF(o.toast.left, o.toast.top + 3 * s, o.toast.right, o.toast.bottom + 3 * s), 10 * s,
                                 WithAlpha(D2D1::ColorF(0x000000), 0.15f));
        painter_.FillRoundedRect(o.toast, 10 * s, plate);
        painter_.DrawText(v->toast, o.toast_text, small_fmt, ink);
        if (o.toast_undo.right > o.toast_undo.left) {
            if (hot(kScToastUndo)) painter_.FillRoundedRect(o.toast_undo, 6 * s, WithAlpha(ink, 0.12f));
            painter_.DrawText(shortcuts_text::Undo(), o.toast_undo, title_format,
                              dark ? D2D1::ColorF(0x005FB8) : D2D1::ColorF(0x7CC4FF), fluent::HorizontalAlignment::Center);
        }
    }
}

float MainRenderer::SettingsShortcutsRevealDelta(const WindowViewModel& vm, float window_w, float window_h) const {
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, window_w, window_h);
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    // The popover together with its row: the user sees what is being edited.
    D2D1_RECT_F pop = lay.shortcuts.pop;
    if (vm.settings_page != 7 || pop.right <= pop.left) return 0.0f;
    pop.top = (std::min)(pop.top, lay.shortcuts.pop_anchor.top);
    pop.bottom = (std::max)(pop.bottom, lay.shortcuts.pop_anchor.bottom);
    const float top = lay.content.top + 8.0f * scale_;
    const float bottom = lay.content.bottom - 8.0f * scale_;
    if (pop.bottom > bottom) return (std::min)(pop.bottom - bottom, pop.top - top);
    if (pop.top < top) return pop.top - top;
    return 0.0f;
}

} // namespace pulse::ui
