// ai_panel_history.cpp - 55 P3 历史对话: the layer that slides in from the
// left over the assistant panel, below its header.
//
// The panel's own background shows through: DrawContent clips the
// conversation and the composer away where the layer is, so a glass panel
// stays glass. The app puts real EDITs over the search box and a renamed
// title once the layer stands still; until then the words are drawn here.
#include "ai_panel_view.h"
#include "ai_panel_draw.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kRowH = 52.0f;      // .hi height
constexpr float kGroupH = 32.0f;    // .grp: padding 12 10 4, 11.5 / 16
constexpr float kFootH = 40.0f;     // .hfoot: padding 10 16 12, 12 / 18, its rule

}  // namespace

void AiPanelView::DrawHistory(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                              const D2D1_RECT_F& panel, float s, uint64_t now_ms) {
    (void)now_ms;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    const AiHistoryView& h = vm.history;
    hist_hits_.clear();
    hist_search_rect_ = {};
    hist_rename_rect_ = {};
    hist_row_tops_.clear();
    if (hist_k_ <= 0.001f) {
        hist_rect_ = {};
        hist_list_rect_ = {};
        return;
    }
    const auto hot = [&](AiPanelPart part, int index) { return vm.hover.part == part && vm.hover.index == index; };
    const auto down = [&](AiPanelPart part, int index) {
        return vm.pressed.part == part && vm.pressed.index == index && hot(part, index);
    };
    const auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part, int index) { hist_hits_.push_back({r, {part, index}}); };
    const D2D1_COLOR_F hover = pal.glass ? pal.glass_hover : pal.hover;
    const bool settled = hist_k_ >= 0.999f && h.open;

    // translateX(-100%) -> none.
    const float w = panel.right - panel.left;
    const float dx = -w * (1.0f - hist_k_);
    const D2D1_RECT_F r = D2D1::RectF(panel.left + dx, head_rect_.bottom, panel.right + dx, panel.bottom);
    hist_rect_ = D2D1::RectF((std::max)(r.left, panel.left), r.top, r.right, r.bottom);
    c.dc->PushAxisAlignedClip(hist_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
    const float x0 = r.left, x1 = r.right;

    // .hs: padding 12 12 6; the box h 34, card, 1 px line, radius 8, a 15 px
    // search icon, gap 8, 13 px words.
    const D2D1_RECT_F box = D2D1::RectF(x0 + 12.0f * s, r.top + 12.0f * s, x1 - 12.0f * s, r.top + 46.0f * s);
    c.Fill(box, 8.0f * s, pal.card);   // the EDIT's own card (CardColor)
    c.Stroke(box, 8.0f * s, pal.glass ? pal.glass_input_line : pal.line);
    const float bcy = (box.top + box.bottom) * 0.5f;
    c.Icon(AiIcon::Search, box.left + 10.0f * s, bcy - 7.5f * s, 15.0f * s, pal.text3);
    const D2D1_RECT_F words = D2D1::RectF(box.left + 33.0f * s, bcy - 10.0f * s, box.right - 10.0f * s, bcy + 10.0f * s);
    if (settled) {
        hist_search_rect_ = words;
    } else {
        c.Text(h.query.empty() ? h.search_cue : h.query, formats_.body, words, h.query.empty() ? pal.text3 : pal.text);
    }
    hit(box, AiPanelPart::HistSearch, -1);

    // .hfoot: rule (line2), lock 14, gap 7, 12 px text3; the link (accent) at the right.
    const D2D1_RECT_F foot = D2D1::RectF(x0, r.bottom - kFootH * s, x1, r.bottom);
    c.Fill(D2D1::RectF(x0, foot.top, x1, foot.top + 1.0f), 0.0f, pal.glass ? pal.glass_line : pal.line2);
    {
        const float fcy = foot.top + 1.0f + (foot.bottom - foot.top - 3.0f * s) * 0.5f;
        c.Icon(AiIcon::Lock, x0 + 16.0f * s, fcy - 7.0f * s, 14.0f * s, pal.text3);
        const float lw = c.Measure(h.settings, formats_.caption);
        const D2D1_RECT_F link = D2D1::RectF(x1 - 16.0f * s - lw, fcy - 10.0f * s, x1 - 16.0f * s, fcy + 10.0f * s);
        const bool lh = hot(AiPanelPart::HistSettings, -1);
        c.Text(h.settings, formats_.caption, link, lh ? Mix(pal.accent, pal.text, 0.18f) : pal.accent);
        if (lh) c.Fill(D2D1::RectF(link.left, link.bottom - 3.0f * s, link.right, link.bottom - 2.0f * s), 0.0f, pal.accent);
        hit(D2D1::RectF(link.left - 4.0f * s, link.top - 2.0f * s, link.right + 4.0f * s, link.bottom + 2.0f * s),
            AiPanelPart::HistSettings, -1);
        c.Text(h.foot, formats_.caption,
               D2D1::RectF(x0 + (16.0f + 14.0f + 7.0f) * s, fcy - 10.0f * s, link.left - 10.0f * s, fcy + 10.0f * s),
               pal.text3);
    }

    // .hl: scrolls; padding 4 8 8.
    hist_list_rect_ = D2D1::RectF(x0, box.bottom + 6.0f * s, x1, foot.top);
    const float list_h = hist_list_rect_.bottom - hist_list_rect_.top;
    float content = 4.0f * s;
    for (const AiHistoryRowView& row : h.rows) {
        if (!row.group.empty()) content += kGroupH * s;
        hist_row_tops_.push_back(content);
        content += kRowH * s;
    }
    content += 8.0f * s;
    hist_content_h_ = content;
    hist_scroll_ = std::clamp(hist_scroll_, 0.0f, (std::max)(0.0f, content - list_h));

    c.dc->PushAxisAlignedClip(hist_list_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
    if (h.rows.empty() && !h.empty.empty()) {
        TextOptions o;
        o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        c.Text(h.empty, formats_.body13,
               D2D1::RectF(x0 + 16.0f * s, hist_list_rect_.top + 24.0f * s, x1 - 16.0f * s, hist_list_rect_.top + 56.0f * s),
               pal.text3, o);
    }
    const float base = hist_list_rect_.top - hist_scroll_;
    for (size_t i = 0; i < h.rows.size(); ++i) {
        const AiHistoryRowView& row = h.rows[i];
        const int index = static_cast<int>(i);
        const float top = base + hist_row_tops_[i];
        if (!row.group.empty() && top > hist_list_rect_.top - kGroupH * s && top - kGroupH * s < hist_list_rect_.bottom) {
            // .grp: 11.5 semibold text3, padding 12 10 4.
            c.Text(row.group, formats_.heading,
                   D2D1::RectF(x0 + 18.0f * s, top - kGroupH * s + 12.0f * s, x1 - 18.0f * s, top - 4.0f * s), pal.text3);
        }
        if (top + kRowH * s < hist_list_rect_.top || top > hist_list_rect_.bottom) continue;
        const D2D1_RECT_F rr = D2D1::RectF(x0 + 8.0f * s, top, x1 - 8.0f * s, top + kRowH * s);
        const float cy = (rr.top + rr.bottom) * 0.5f;
        const bool deleting = index == h.deleting;
        const bool renaming = index == h.renaming;
        const bool over = !deleting && !renaming &&
                          (hot(AiPanelPart::HistRow, index) || hot(AiPanelPart::HistPin, index) ||
                           hot(AiPanelPart::HistRename, index) || hot(AiPanelPart::HistDelete, index));
        // .hi: radius 8; hover, .cur accent-tint with a 3 px accent bar, .del red 8%.
        if (deleting) c.Fill(rr, 8.0f * s, Alpha(pal.red, 0.08f));
        else if (row.current) c.Fill(rr, 8.0f * s, over ? Mix(pal.accent_tint, pal.text, 0.04f) : pal.accent_tint);
        else if (over) c.Fill(rr, 8.0f * s, down(AiPanelPart::HistRow, index) ? Mix(hover, pal.text, 0.06f) : hover);
        if (row.current && !deleting)
            c.Fill(D2D1::RectF(rr.left, rr.top + 14.0f * s, rr.left + 3.0f * s, rr.bottom - 14.0f * s), 1.5f * s, pal.accent);
        hit(rr, AiPanelPart::HistRow, index);
        // .hic: 28, radius 7, card, 1 px line2, chat 15 text2.
        const D2D1_RECT_F ic = D2D1::RectF(rr.left + 10.0f * s, cy - 14.0f * s, rr.left + 38.0f * s, cy + 14.0f * s);
        c.Fill(ic, 7.0f * s, pal.glass ? pal.glass_card : pal.card);
        c.Stroke(ic, 7.0f * s, pal.glass ? pal.glass_card_line : pal.line2);
        c.Icon(AiIcon::Chat, ic.left + 6.5f * s, cy - 7.5f * s, 15.0f * s, pal.text2);
        const float tx = rr.left + 48.0f * s;

        // The right side: the inline 取消 · 删除, the hover's 固定 · 重命名 · 删除,
        // or the time (a pin for pinned rows).
        float tr = rr.right - 6.0f * s;
        if (deleting) {
            // .btn.sm: h 26, padding 0 9, 12 px, radius 7; .red filled.
            const auto pill = [&](float right, const std::wstring& text, bool red, AiPanelPart part) {
                const float bw = c.Measure(text, formats_.caption) + 18.0f * s;
                const D2D1_RECT_F b = D2D1::RectF(right - bw, cy - 13.0f * s, right, cy + 13.0f * s);
                const bool bh = hot(part, index), bd = down(part, index);
                if (red) {
                    c.Fill(b, 7.0f * s, bh ? Mix(pal.red, pal.text, bd ? 0.2f : 0.12f) : pal.red);
                } else {
                    c.Fill(b, 7.0f * s, bh ? (bd ? Mix(hover, pal.text, 0.06f) : hover) : (pal.glass ? pal.glass_card : pal.card));
                    c.Stroke(b, 7.0f * s, pal.line);
                }
                TextOptions o;
                o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                c.Text(text, formats_.caption, b, red ? Hex(0xffffff) : pal.text, o);
                hit(b, part, index);
                return bw;
            };
            float bx = tr;
            bx -= pill(bx, h.del, true, AiPanelPart::HistDelOk) + 6.0f * s;
            bx -= pill(bx, h.cancel, false, AiPanelPart::HistDelCancel);
            tr = bx - 10.0f * s;
        } else if (over) {
            // .hacts: mini 28 (radius 7), icon 15 text3 -> text when hot.
            const struct { AiPanelPart part; AiIcon icon; } acts[] = {
                {AiPanelPart::HistPin, AiIcon::Pin},
                {AiPanelPart::HistRename, AiIcon::Edit},
                {AiPanelPart::HistDelete, AiIcon::Trash},
            };
            float bx = tr - 28.0f * 3.0f * s;
            tr = bx - 6.0f * s;
            for (const auto& a : acts) {
                const D2D1_RECT_F b = D2D1::RectF(bx, cy - 14.0f * s, bx + 28.0f * s, cy + 14.0f * s);
                const bool bh = hot(a.part, index);
                if (bh) c.Fill(b, 7.0f * s, down(a.part, index) ? Mix(hover, pal.text, 0.12f) : Mix(hover, pal.text, 0.05f));
                const bool lit = a.part == AiPanelPart::HistPin && row.pinned;
                c.Icon(a.icon, bx + 6.5f * s, cy - 7.5f * s, 15.0f * s, bh ? pal.text : lit ? pal.accent : pal.text3);
                hit(b, a.part, index);
                bx += 28.0f * s;
            }
        } else if (!renaming) {
            if (row.pinned) {
                c.Icon(AiIcon::Pin, tr - 13.0f * s, cy - 6.5f * s, 13.0f * s, pal.text3);
                tr -= (13.0f + 10.0f) * s;
            } else if (!row.when.empty()) {
                const float ww = c.Measure(row.when, formats_.meta);
                c.Text(row.when, formats_.meta, D2D1::RectF(tr - ww, cy - 9.0f * s, tr, cy + 9.0f * s), pal.text3);
                tr -= ww + 10.0f * s;
            }
        }

        // .tx: title 13 (one line, …), the line under it 11.5 text3 (margin-top 1).
        const D2D1_RECT_F title = D2D1::RectF(tx, cy - 18.5f * s, (std::max)(tx, tr), cy + 0.5f * s);
        if (renaming) {
            // .renaming .tt: card, 1 px accent, radius 5, padding 1 6, margin-left -7.
            const D2D1_RECT_F tb = D2D1::RectF(tx - 7.0f * s, title.top - 1.0f * s, rr.right - 6.0f * s, title.bottom + 1.0f * s);
            c.Fill(tb, 5.0f * s, pal.card);
            c.Stroke(tb, 5.0f * s, pal.accent);
            const D2D1_RECT_F inner = D2D1::RectF(tb.left + 6.0f * s, tb.top + 1.0f * s, tb.right - 6.0f * s, tb.bottom - 1.0f * s);
            const bool whole = inner.top >= hist_list_rect_.top && inner.bottom <= hist_list_rect_.bottom;
            if (settled && whole) hist_rename_rect_ = inner;
            else c.Text(h.rename_text, formats_.body, inner, pal.text);
            hit(tb, AiPanelPart::HistRenameBox, index);
        } else {
            c.Text(row.title, formats_.body13, title, pal.text);
        }
        const std::wstring& sub = deleting ? h.delete_note : renaming ? h.rename_hint : row.snippet;
        c.Text(sub, formats_.meta, D2D1::RectF(tx, cy + 1.5f * s, (std::max)(tx, renaming ? rr.right - 6.0f * s : tr), cy + 18.5f * s),
               pal.text3);
    }
    // A thin thumb while the list overflows (the wheel scrolls it).
    if (content > list_h + 0.5f) {
        const float th = (std::max)(24.0f * s, list_h * list_h / content);
        const float ty = hist_list_rect_.top + (list_h - th) * (hist_scroll_ / (content - list_h));
        c.Fill(D2D1::RectF(x1 - 6.0f * s, ty + 2.0f * s, x1 - 3.0f * s, ty + th - 2.0f * s), 1.5f * s, Alpha(pal.text3, 0.35f));
    }
    c.dc->PopAxisAlignedClip();
    c.dc->PopAxisAlignedClip();
}

float AiPanelView::HistorySlide(bool open, uint64_t now_ms) {
    const float target = open ? 1.0f : 0.0f;
    const float p = hist_ms_ && now_ms >= hist_start_
                        ? std::clamp(static_cast<float>(now_ms - hist_start_) / static_cast<float>(hist_ms_), 0.0f, 1.0f)
                        : 1.0f;
    const float value = hist_from_ + (hist_to_ - hist_from_) * EaseDesign(p);
    if (target != hist_to_) {
        hist_from_ = value;
        hist_to_ = target;
        hist_start_ = now_ms;
        hist_ms_ = motion::SystemAnimationsEnabled() ? 320u : 0u;   // .hist: transform .32s
        if (hist_ms_) conv_anim_ = true;
        return value;
    }
    if (p < 1.0f) conv_anim_ = true;
    return value;
}

AiPanelHit AiPanelView::HistoryHitTest(float x, float y) const {
    for (auto it = hist_hits_.rbegin(); it != hist_hits_.rend(); ++it) {
        // Rows and their buttons only where the list shows them.
        const bool listed = it->hit.part != AiPanelPart::HistSearch && it->hit.part != AiPanelPart::HistSettings;
        if (listed && !In(hist_list_rect_, x, y)) continue;
        if (In(it->rect, x, y)) return it->hit;
    }
    return {AiPanelPart::HistArea, -1};
}

bool AiPanelView::HistoryScrollBy(float dy) {
    if (hist_k_ <= 0.001f) return false;
    const float list_h = hist_list_rect_.bottom - hist_list_rect_.top;
    hist_scroll_ = std::clamp(hist_scroll_ + dy, 0.0f, (std::max)(0.0f, hist_content_h_ - list_h));
    return true;
}

void AiPanelView::HistoryRevealRow(int row) {
    if (row < 0 || row >= static_cast<int>(hist_row_tops_.size())) return;
    const float list_h = hist_list_rect_.bottom - hist_list_rect_.top;
    const float top = hist_row_tops_[static_cast<size_t>(row)];
    const float bottom = top + kRowH * bar_scale_;
    if (top < hist_scroll_) hist_scroll_ = top;
    else if (bottom > hist_scroll_ + list_h) hist_scroll_ = bottom - list_h;
}

}  // namespace pulse::ui
