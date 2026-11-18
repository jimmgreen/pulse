// ai_list_overlay.cpp — Pulse 助手 in the file list (see ai_list_overlay.h).
//
// Sizes and timings are the design's, CSS pixels used as DIPs.
#include "ai_list_overlay.h"
#include "ai_panel_draw.h"

#include "typography.h"
#include "FluentTokens.h"
#include "ui_compositor.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr uint64_t kSeenTailMs = 800;     // readList: +300 spark off, +500 dots cleared
constexpr uint64_t kSparkTailMs = 300;
constexpr float kReadFlashMs = 1100.0f;   // @keyframes readrow 1.1 s ease-out
constexpr float kReadPopMs = 500.0f;      // @keyframes readic .5 s
constexpr float kLeaveMs = 380.0f;        // .row.leaving: transform .38 s, opacity .3 s
constexpr float kFlightMs = 560.0f;
constexpr float kDestMs = 300.0f;
constexpr uint64_t kDestStepMs = 70;

float Clamp01(float v) { return (std::clamp)(v, 0.0f, 1.0f); }

float Bezier(float x1, float y1, float x2, float y2, float t) {
    t = Clamp01(t);
    float u = t;
    for (int i = 0; i < 8; ++i) {
        const float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        const float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = (std::clamp)(u - x / dx, 0.0f, 1.0f);
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}
float Ease(float t) { return Bezier(0.25f, 0.1f, 0.25f, 1.0f, t); }       // CSS ease
float EaseOut(float t) { return Bezier(0.0f, 0.0f, 0.58f, 1.0f, t); }
float Since(uint64_t now, uint64_t at) { return now >= at ? static_cast<float>(now - at) : -1.0f; }

// Image, Document, App, Video, Archive, Neutral (ai_panel_view.cpp's tints).
D2D1_COLOR_F TintFg(int i, bool dark) {
    static const uint32_t light[6] = {0x8257c2, 0x2f6bc0, 0x16816f, 0xc03f66, 0xa86c12, 0x5b626c};
    static const uint32_t darks[6] = {0xb393e6, 0x7eaaf0, 0x5cc4ae, 0xee86a6, 0xe3ab52, 0xb1b6b0};
    return Hex((dark ? darks : light)[(std::clamp)(i, 0, 5)]);
}
D2D1_COLOR_F TintBg(int i, bool dark) {
    static const uint32_t light[6] = {0xf1eafb, 0xe6effb, 0xddf3ee, 0xfbe7ee, 0xfbf0d9, 0xe9ebe4};
    static const uint32_t darks[6] = {0x352c45, 0x253248, 0x20372f, 0x43262f, 0x3a3020, 0x2d312e};
    return Hex((dark ? darks : light)[(std::clamp)(i, 0, 5)]);
}

struct Formats {
    float scale = 0.0f;
    float font_scale = 0.0f;
    IDWriteFactory2* factory = nullptr;
    IDWriteTextFormat* watch = nullptr;   // 12.5 semibold
    IDWriteTextFormat* tag = nullptr;     // 11.5 semibold
    IDWriteTextFormat* pill = nullptr;    // 11 semibold
    void Release() {
        for (IDWriteTextFormat** f : {&watch, &tag, &pill})
            if (*f) { (*f)->Release(); *f = nullptr; }
        scale = 0.0f;
        factory = nullptr;
    }
};
Formats g_formats;

const Formats& EnsureFormats(Compositor* compositor, float scale) {
    IDWriteFactory2* f = compositor ? compositor->DwriteFactory() : nullptr;
    const float font_scale = typography::UiFontScale();
    if (g_formats.watch && g_formats.scale == scale && g_formats.font_scale == font_scale && g_formats.factory == f)
        return g_formats;
    g_formats.Release();
    if (!f) return g_formats;
    auto make = [&](float dip, IDWriteTextFormat*& out) {
        typography::TextFormatSpec spec;
        spec.size = dip * scale;
        spec.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        typography::CreateTextFormat(f, spec, &out);
    };
    make(12.5f, g_formats.watch);
    make(11.5f, g_formats.tag);
    make(11.0f, g_formats.pill);
    g_formats.scale = scale;
    g_formats.font_scale = font_scale;
    g_formats.factory = f;
    return g_formats;
}

}  // namespace

// What one pane remembers between frames.
struct AiListPainter::State {
    const AiListOverlay* overlay = nullptr;
    int read_seen = 0;                    // read rows on screen last frame
    int read_count = 0;                   // ... this frame so far
    int dest_count = 0;                   // dest rows on screen this frame (stagger order)
    // The reader spark.
    bool spark_has = false;
    float spark_x = 0.0f;
    float spark_target = 0.0f;            // centre y this frame
    float spark_from = 0.0f;
    float spark_to = 0.0f;
    uint64_t spark_moved = 0;
    float spark_alpha = 0.0f;
    uint64_t spark_alpha_at = 0;
    bool spark_shown = false;
    float hprog_alpha = 0.0f;
    float watch_alpha = 0.0f;
    uint64_t last_frame = 0;
    // Rows of interest seen this frame / last seen.
    std::unordered_map<std::wstring, D2D1_RECT_F> rows;
    struct Flight {
        std::wstring name;                // lower-case item
        std::wstring folder;              // lower-case folder
        D2D1_RECT_F from{};
        uint64_t start = 0;
    };
    std::vector<Flight> flights;
    std::unordered_set<std::wstring> flown;
};

namespace {
std::unordered_map<int, AiListPainter::State*>& States() {
    static std::unordered_map<int, AiListPainter::State*> states;
    return states;
}
}  // namespace

std::wstring AiListLower(const std::wstring& s) {
    std::wstring out = s;
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

size_t AiListReadK(const AiListOverlay& o, uint64_t now) {
    if (!o.read_start || !o.read_total) return 0;
    if (now < o.read_start) return 0;
    const double pace = static_cast<double>((std::max<uint64_t>)(o.read_ms, 1)) / static_cast<double>(o.read_total);
    const size_t k = static_cast<size_t>(static_cast<double>(now - o.read_start) / pace) + 1;
    return (std::min)(k, o.read_total);
}

bool AiListReading(const AiListOverlay& o, uint64_t now) {
    return o.read_start && now + 300 >= o.read_start && now < o.read_start + o.read_ms + kSparkTailMs;
}

AiListPainter::AiListPainter(const AiListOverlay* overlay, Compositor* compositor, const Theme& theme, float scale,
                             uint64_t now, int pane_key)
    : o_(overlay), compositor_(compositor), theme_(theme), s_(scale), now_(now) {
    auto& states = States();
    auto it = states.find(pane_key);
    if (!o_ || !compositor_ || !compositor_->Dc()) {
        // Let the spark and the line fade out on their own next time; just
        // forget the pane's state when nothing is shown.
        if (it != states.end()) {
            delete it->second;
            states.erase(it);
        }
        o_ = nullptr;
        return;
    }
    if (it == states.end()) it = states.emplace(pane_key, new State()).first;
    st_ = it->second;
    if (st_->overlay != o_) {
        *st_ = State();
        st_->overlay = o_;
    }
    st_->read_count = 0;
    st_->dest_count = 0;
    st_->spark_has = false;
    st_->rows.clear();
}

AiListPainter::~AiListPainter() {
    if (!st_) return;
    if (st_->read_count > 0) st_->read_seen = st_->read_count;
    st_->last_frame = now_;
}

AiListPainter::RowMotion AiListPainter::Row(const std::wstring& name, const D2D1_RECT_F& cell) {
    RowMotion m;
    row_name_.clear();
    row_read_ = -1;
    row_seen_ = false;
    row_flash_ = 0.0f;
    row_bump_ = 0.0f;
    row_done_ = 0.0f;
    row_ring_ = 0.0f;
    row_morph_ = {};
    row_icon_scale_ = 1.0f;
    if (!o_) return m;
    row_name_ = AiListLower(name);
    const uint64_t now = now_;

    // Renamed: @keyframes flash - accent 0 → 18% at 15% → 0 over 1.4 s.
    if (auto fl = o_->flash.find(row_name_); fl != o_->flash.end()) {
        if (now < fl->second) animating_ = true;
        else if (const float age = static_cast<float>(now - fl->second); age < 1400.0f) {
            const float t = age / 1400.0f;
            row_done_ = t < 0.15f ? t / 0.15f : 1.0f - EaseOut((t - 0.15f) / 0.85f);
            animating_ = true;
        }
    }

    // Renamed by the assistant: .nw rnin .45 s cubic-bezier(.3,.7,.2,1) from
    // translateY(10) and opacity 0; .row.rn @keyframes flash 1.3 s - an inset
    // 2 px accent ring, in by 25 %, held to 70 %.
    if (!o_->morph.empty()) {
        if (auto mo = o_->morph.find(row_name_); mo != o_->morph.end()) {
            const float age = Since(now, mo->second.at);
            if (age < 0.0f) {
                row_morph_.was = &mo->second.was;
                animating_ = true;
            } else if (age < 1300.0f) {
                if (age < 450.0f) {
                    const float e = Bezier(0.3f, 0.7f, 0.2f, 1.0f, age / 450.0f);
                    row_morph_.alpha = e;
                    row_morph_.dy = 10.0f * s_ * (1.0f - e);
                }
                const float t = age / 1300.0f;
                row_ring_ = t < 0.25f ? Ease(t / 0.25f) : t < 0.7f ? 1.0f : 1.0f - Ease((t - 0.7f) / 0.3f);
                animating_ = true;
            }
        }
    }

    // Reading.
    if (o_->read_start && now < o_->read_start + o_->read_ms + kSeenTailMs && o_->read_names.count(row_name_)) {
        const int j = st_->read_count++;
        const int total = st_->read_seen > 0 ? (std::max)(st_->read_seen, j + 1)
                                             : static_cast<int>((std::max<size_t>)(o_->read_total, 1));
        const uint64_t at = o_->read_start + o_->read_ms * static_cast<uint64_t>(j) / static_cast<uint64_t>(total);
        row_read_ = j;
        row_read_at_ = at;
        if (now >= at) {
            row_seen_ = true;
            const float age = static_cast<float>(now - at);
            if (age < kReadFlashMs) row_flash_ = 1.0f - EaseOut(age / kReadFlashMs);
            if (age < kReadPopMs) {
                // readic: 1 → 1.3 at 45% → 1, cubic-bezier(.3,1.6,.5,1)
                const float t = age / kReadPopMs;
                const float e = t < 0.45f ? Bezier(0.3f, 1.6f, 0.5f, 1.0f, t / 0.45f)
                                          : 1.0f - Bezier(0.3f, 1.6f, 0.5f, 1.0f, (t - 0.45f) / 0.55f);
                row_icon_scale_ = 1.0f + 0.3f * e;
            }
            // The spark sits on the last row that lit up.
            if (now < o_->read_start + o_->read_ms + kSparkTailMs) {
                st_->spark_has = true;
                st_->spark_target = (cell.top + cell.bottom) * 0.5f;
                st_->spark_x = cell.left - 10.5f * s_;
            }
        }
        animating_ = true;
    }

    // Leaving: slide 26 px right and fade; a card of it flies to its folder.
    if (auto lv = o_->leaving.find(row_name_); lv != o_->leaving.end()) {
        const float age = Since(now, lv->second);
        if (age >= 0.0f) {
            const float t = Clamp01(age / kLeaveMs);
            m.dx = 26.0f * s_ * Ease(t);
            m.opacity = 1.0f - Clamp01(age / 300.0f);
            m.gone = t >= 1.0f;
            if (!st_->flown.count(row_name_) && age < kFlightMs * 0.5f && o_->target.count(row_name_)) {
                st_->flown.insert(row_name_);
                State::Flight f;
                f.name = row_name_;
                if (auto tg = o_->target.find(row_name_); tg != o_->target.end()) f.folder = tg->second;
                f.from = cell;
                f.start = lv->second;
                st_->flights.push_back(std::move(f));
                m.fly = true;
            }
            animating_ = true;
        }
    }

    // Folders: remember where they are (cards fly to them) and bump.
    const bool is_target = o_->targets.count(row_name_) != 0 || o_->new_folders.count(row_name_) != 0;
    if (is_target) {
        st_->rows[row_name_] = cell;
        if (auto b = o_->bumps.find(row_name_); b != o_->bumps.end()) {
            uint64_t latest = 0;
            for (uint64_t at : b->second)
                if (at <= now && at > latest) latest = at;
            for (uint64_t at : b->second)
                if (at > now) animating_ = true;
            if (latest) {
                const float age = static_cast<float>(now - latest);
                if (age < 800.0f) {
                    row_bump_ = age / 800.0f;
                    if (age < 450.0f) {
                        // @keyframes bump .45 s ease: 1 → 1.28 at 40% → 1
                        const float t = age / 450.0f;
                        const float e = t < 0.4f ? Ease(t / 0.4f) : 1.0f - Ease((t - 0.4f) / 0.6f);
                        row_icon_scale_ = (std::max)(row_icon_scale_, 1.0f + 0.28f * e);
                    }
                    animating_ = true;
                }
            }
        }
    }
    if (!o_->dests.empty() && o_->dests.count(row_name_)) {
        row_ordinal_ = st_->dest_count++;
        if (o_->dest_start && now < o_->dest_start + kDestStepMs * static_cast<uint64_t>(row_ordinal_) + 400)
            animating_ = true;
        if (o_->dest_end && now < o_->dest_end + 400) animating_ = true;
    }
    if (!o_->new_folders.empty()) {
        if (auto nf = o_->new_folders.find(row_name_); nf != o_->new_folders.end() &&
            (now < nf->second + 400 || (o_->end_ms && now + 400 >= o_->end_ms)))
            animating_ = true;
    }
    return m;
}

void AiListPainter::Background(const D2D1_RECT_F& bg, float inset, float radius) {
    if (!o_ || row_name_.empty()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (row_flash_ <= 0.0f && row_bump_ <= 0.0f && row_done_ <= 0.0f && row_ring_ <= 0.0f) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
    Ctx c{compositor_, dc, brush, s_};
    const D2D1_RECT_F r = D2D1::RectF(bg.left + inset, bg.top + s_, bg.right - inset, bg.bottom - s_);
    D2D1_COLOR_F accent = theme_.accent;
    accent.a = 1.0f;
    if (row_bump_ > 0.0f) {
        // .row.bump: accent-tint, background transition .3 s, held to 500 ms.
        const float age = row_bump_ * 800.0f;
        const float in = Clamp01(age / 300.0f);
        const float out = age > 500.0f ? 1.0f - Clamp01((age - 500.0f) / 300.0f) : 1.0f;
        const bool dark = theme_.bg.r < 0.5f;
        c.Fill(r, radius, Alpha(accent, (dark ? 0.16f : 0.12f) * Ease(in) * out));
    }
    if (row_done_ > 0.0f) c.Fill(r, radius, Alpha(accent, 0.18f * row_done_));
    if (row_ring_ > 0.0f) c.Stroke(r, radius, Alpha(accent, row_ring_), 2.0f * s_);
    if (row_flash_ > 0.0f) {
        // readrow: accent 22% and an inset 3 px accent bar, fading out.
        c.Fill(r, radius, Alpha(accent, 0.22f * row_flash_));
        const D2D1_RECT_F bar = D2D1::RectF(r.left, r.top, r.left + 3.0f * s_, r.bottom);
        dc->PushAxisAlignedClip(bar, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        c.Fill(r, radius, Alpha(accent, row_flash_));
        dc->PopAxisAlignedClip();
    }
    brush->Release();
}

float AiListPainter::IconScale() const { return o_ ? row_icon_scale_ : 1.0f; }

void AiListPainter::IconDecor(const D2D1_RECT_F& icon) {
    if (!o_ || !row_seen_) return;
    if (now_ >= o_->read_start + o_->read_ms + kSeenTailMs) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
    // .seen: 6 px accent dot at the icon's top-right (right -3, top -2 of an
    // 18 px icon) with a 2 px ring of the background; pops in over .3 s.
    const float k = (icon.right - icon.left) / (18.0f * s_);
    const float d = 6.0f * s_ * (std::clamp)(k, 0.9f, 1.4f);
    const float cx = icon.right + 3.0f * s_ - d * 0.5f;
    const float cy = icon.top - 2.0f * s_ + d * 0.5f;
    const float pop = Clamp01(static_cast<float>(now_ - row_read_at_) / 300.0f);
    const float sc = 0.3f + 0.7f * Ease(pop);
    const float r = d * 0.5f * sc;
    D2D1_COLOR_F ring = theme_.bg;
    ring.a = 1.0f;
    D2D1_COLOR_F accent = theme_.accent;
    accent.a = Ease(pop);
    ring.a = Ease(pop);
    brush->SetColor(ring);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.0f * s_ * sc, r + 2.0f * s_ * sc), brush);
    brush->SetColor(accent);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush);
    brush->Release();
}

void AiListPainter::AfterName(float left, float right, float text_top, float text_h) {
    if (!o_ || row_name_.empty()) return;
    const bool dark = theme_.bg.r < 0.5f;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const Formats& f = EnsureFormats(compositor_, s_);
    const float mid = text_top + text_h * 0.5f;

    // .newpill on a folder the sort created: 11 px semibold, h 18, r 9, 0 7.
    if (auto nf = o_->new_folders.find(row_name_); nf != o_->new_folders.end() && f.pill && !o_->new_badge.empty()) {
        const float age = Since(now_, nf->second);
        if (age < 0.0f) return;
        ID2D1SolidColorBrush* brush = nullptr;
        if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
        Ctx c{compositor_, dc, brush, s_};
        float a = Clamp01(age / 300.0f);
        if (o_->end_ms) a = (std::min)(a, Clamp01(Since(o_->end_ms, now_) / 300.0f));
        const float x = left + 4.0f * s_;
        const float tw = c.Measure(o_->new_badge, f.pill);
        const float w = tw + 14.0f * s_;
        if (x + w <= right) {
            const D2D1_RECT_F r = D2D1::RectF(x, std::round(mid - 9.0f * s_), x + w, std::round(mid - 9.0f * s_) + 18.0f * s_);
            D2D1_COLOR_F accent = theme_.accent;
            accent.a = 1.0f;
            const D2D1_COLOR_F card = Over(theme_.surface_card, theme_.bg);
            c.Fill(r, 9.0f * s_, Alpha(Mix(card, accent, dark ? 0.20f : 0.13f), a));
            TextOptions o;
            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            c.Text(o_->new_badge, f.pill, r, Alpha(accent, a), o);
        }
        brush->Release();
        return;
    }

    // .dest: the folder an item goes to.
    auto d = o_->dests.find(row_name_);
    if (d == o_->dests.end() || !o_->dest_start || !f.tag) return;
    float show = Clamp01(Since(now_, o_->dest_start + kDestStepMs * static_cast<uint64_t>(row_ordinal_)) / kDestMs);
    if (o_->dest_end) show = (std::min)(show, 1.0f - Clamp01(Since(now_, o_->dest_end) / kDestMs));
    if (auto lv = o_->leaving.find(row_name_); lv != o_->leaving.end())
        show = (std::min)(show, 1.0f - Clamp01(Since(now_, lv->second) / kDestMs));
    if (show <= 0.0f) return;
    const float e = Ease(show);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
    Ctx c{compositor_, dc, brush, s_};
    // 11.5 semibold, h 20, r 10, padding 0 8 0 6, arrow 12, gap 4; 10 after the name.
    const float tw = c.Measure(d->second.label, f.tag);
    const float full_w = 6.0f * s_ + 12.0f * s_ + 4.0f * s_ + tw + 8.0f * s_;
    const float x = left + 4.0f * s_ - 6.0f * s_ * (1.0f - e);
    // A new name may be cut (…) to its reserved room; others need it all.
    float w = full_w;
    if (d->second.tint == AiListOverlay::kDestAccent && x + w > right + 1.0f && right - x >= 60.0f * s_) w = right - x;
    if (x + w <= right + 1.0f) {
        const float top = std::round(mid - 10.0f * s_);
        const D2D1_RECT_F r = D2D1::RectF(x, top, x + w, top + 20.0f * s_);
        const int tint = d->second.tint;
        D2D1_COLOR_F bgc, fgc;
        if (tint == AiListOverlay::kDestAccent) {
            // .dest.nm2: accent on accent-soft.
            fgc = theme_.accent;
            fgc.a = 1.0f;
            bgc = Mix(Over(theme_.surface_card, theme_.bg), fgc, dark ? 0.20f : 0.13f);
        } else {
            bgc = TintBg(tint, dark);
            fgc = TintFg(tint, dark);
        }
        c.Fill(r, 10.0f * s_, Alpha(bgc, e));
        const D2D1_COLOR_F fg = Alpha(fgc, e);
        c.Icon(AiIcon::Arrow, x + 6.0f * s_, top + 4.0f * s_, 12.0f * s_, fg);
        c.Text(d->second.label, f.tag, D2D1::RectF(x + 22.0f * s_, top, r.right - 6.0f * s_, r.bottom), fg);
    }
    brush->Release();
}

float AiListPainter::AfterNameReserve() const {
    if (!o_ || row_name_.empty() || !o_->dest_start) return 0.0f;
    auto d = o_->dests.find(row_name_);
    if (d == o_->dests.end() || d->second.tint != AiListOverlay::kDestAccent) return 0.0f;
    const Formats& f = EnsureFormats(compositor_, s_);
    if (!f.tag) return 0.0f;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(compositor_->Dc()->CreateSolidColorBrush(theme_.accent, &brush))) return 0.0f;
    Ctx c{compositor_, compositor_->Dc(), brush, s_};
    const float w = 6.0f * s_ + 12.0f * s_ + 4.0f * s_ + c.Measure(d->second.label, f.tag) + 8.0f * s_;
    brush->Release();
    return 6.0f * s_ + 4.0f * s_ + w;
}

void AiListPainter::Flights(const D2D1_RECT_F& list,
                            const std::function<void(const std::wstring&, const D2D1_RECT_F&)>& content) {
    if (!o_ || st_->flights.empty()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
    Ctx c{compositor_, dc, brush, s_};
    const bool dark = theme_.bg.r < 0.5f;
    const D2D1_COLOR_F card = Over(theme_.surface_card, Over(theme_.bg, dark ? Hex(0x1c1e1d) : Hex(0xffffff)));
    D2D1::Matrix3x2F base;
    dc->GetTransform(&base);
    for (size_t i = 0; i < st_->flights.size();) {
        const State::Flight& f = st_->flights[i];
        const float age = Since(now_, f.start);
        if (age >= kFlightMs) {
            st_->flights.erase(st_->flights.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
        if (age < 0.0f) continue;
        animating_ = true;
        // Where its folder is now; above the list when it is out of sight.
        float dy = list.top - f.from.bottom;
        if (auto it = st_->rows.find(f.folder); it != st_->rows.end()) dy = it->second.top - f.from.top;
        // keyframes: (0,0) s1 o1 → 55%: (60, .55dy) s.92 o.95 → (24, dy) s.55 o0;
        // 560 ms, cubic-bezier(.45,.05,.4,1) over the whole run.
        const float p = Bezier(0.45f, 0.05f, 0.4f, 1.0f, age / kFlightMs);
        float tx, ty, sc, op;
        if (p < 0.55f) {
            const float q = p / 0.55f;
            tx = 60.0f * q;
            ty = 0.55f * q;
            sc = 1.0f - 0.08f * q;
            op = 1.0f - 0.05f * q;
        } else {
            const float q = (p - 0.55f) / 0.45f;
            tx = 60.0f - 36.0f * q;
            ty = 0.55f + 0.45f * q;
            sc = 0.92f - 0.37f * q;
            op = 0.95f * (1.0f - q);
        }
        const D2D1_RECT_F r = f.from;
        const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, mid) *
                         D2D1::Matrix3x2F::Translation(tx * s_, ty * dy) * base);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), op, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        // .ghost: card, box-shadow 0 6 18 rgba(0,0,0,.16), radius 8.
        c.Shadow(r, 8.0f * s_, 18.0f * s_, 6.0f * s_, Hex(0x000000, dark ? 0.40f : 0.16f));
        c.Fill(r, 8.0f * s_, card);
        content(f.name, r);
        dc->PopLayer();
        dc->SetTransform(base);
    }
    brush->Release();
}

void AiListPainter::Margin(const D2D1_RECT_F& list) {
    if (!o_) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const uint64_t now = now_;
    const bool reading = AiListReading(*o_, now) && now >= o_->read_start;
    D2D1_COLOR_F accent = theme_.accent;
    accent.a = 1.0f;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(accent, &brush))) return;
    Ctx c{compositor_, dc, brush, s_};
    const float dt = st_->last_frame && now > st_->last_frame ? static_cast<float>(now - st_->last_frame) : 16.0f;

    // .hprog: 2 px under the column header, a 30% accent gradient sweeping
    // left to right every 1.1 s; opacity .3 s.
    const float hp_target = reading ? 1.0f : 0.0f;
    st_->hprog_alpha += (hp_target - st_->hprog_alpha) * Clamp01(dt / 300.0f * 2.2f);
    if (std::fabs(st_->hprog_alpha - hp_target) < 0.02f) st_->hprog_alpha = hp_target;
    if (st_->hprog_alpha > 0.0f) {
        const float w = list.right - list.left;
        const float t = static_cast<float>(now % 1100) / 1100.0f;
        const float x0 = list.left + w * (-0.3f + 1.3f * t);
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, Alpha(accent, 0.0f)},
                                            {0.5f, Alpha(accent, st_->hprog_alpha)},
                                            {1.0f, Alpha(accent, 0.0f)}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* grad = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &coll)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, 0.0f), D2D1::Point2F(x0 + w * 0.3f, 0.0f)),
                coll, &grad))) {
            dc->PushAxisAlignedClip(D2D1::RectF(list.left, list.top, list.right, list.top + 2.0f * s_),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, list.top, x0 + w * 0.3f, list.top + 2.0f * s_),
                                                       2.0f * s_, 2.0f * s_),
                                     grad);
            dc->PopAxisAlignedClip();
        }
        if (grad) grad->Release();
        if (coll) coll->Release();
        animating_ = true;
    }

    // .reader: 17 px spark in the left margin, top .17 s cubic-bezier(.3,.7,.2,1),
    // opacity .25 s; the icon turns (spark 1.2 s) with an accent glow.
    if (st_->spark_has) {
        if (!st_->spark_shown) {
            st_->spark_from = st_->spark_to = st_->spark_target;
            st_->spark_shown = true;
        } else if (std::fabs(st_->spark_target - st_->spark_to) > 0.5f) {
            const float k = Clamp01(static_cast<float>(now - st_->spark_moved) / 170.0f);
            st_->spark_from = st_->spark_from + (st_->spark_to - st_->spark_from) * Bezier(0.3f, 0.7f, 0.2f, 1.0f, k);
            st_->spark_to = st_->spark_target;
            st_->spark_moved = now;
        }
    }
    const float sp_target = st_->spark_has && reading ? 1.0f : 0.0f;
    st_->spark_alpha += (sp_target - st_->spark_alpha) * Clamp01(dt / 250.0f * 2.2f);
    if (std::fabs(st_->spark_alpha - sp_target) < 0.02f) st_->spark_alpha = sp_target;
    if (st_->spark_alpha > 0.0f && st_->spark_shown) {
        const float k = Clamp01(static_cast<float>(now - st_->spark_moved) / 170.0f);
        const float y = st_->spark_from + (st_->spark_to - st_->spark_from) * Bezier(0.3f, 0.7f, 0.2f, 1.0f, k);
        const float size = 17.0f * s_;
        const float x = st_->spark_x - size * 0.5f;
        const float t = static_cast<float>(now % 1200) / 1200.0f;
        const float e = Bezier(0.5f, 0.0f, 0.5f, 1.0f, t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f);
        AiIconPose pose;
        pose.rotate_deg = t < 0.5f ? 90.0f * e : 90.0f + 90.0f * e;
        pose.scale = t < 0.5f ? 1.0f - 0.22f * e : 0.78f + 0.22f * e;
        // drop-shadow(0 0 4px accent 55%)
        for (int i = 3; i >= 1; --i) {
            const float g = static_cast<float>(i) * 1.6f * s_;
            c.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + size * 0.5f, y), size * 0.30f + g, size * 0.30f + g),
                              c.B(Alpha(accent, 0.09f * st_->spark_alpha)));
        }
        pose.opacity = st_->spark_alpha;
        c.Icon(AiIcon::Spark, x, y - size * 0.5f, size, accent, pose);
        animating_ = true;
    } else if (!st_->spark_has && !reading) {
        st_->spark_shown = false;
    }
    brush->Release();
}

void AiListPainter::Watch(float right, float top, float h, float min_left) {
    if (!o_ || o_->watch_text.empty()) return;
    const uint64_t now = now_;
    const bool on = o_->read_start && now + 300 >= o_->read_start && now < o_->read_start + o_->read_ms + kSeenTailMs;
    const float dt = st_->last_frame && now > st_->last_frame ? static_cast<float>(now - st_->last_frame) : 16.0f;
    const float target = on ? 1.0f : 0.0f;
    st_->watch_alpha += (target - st_->watch_alpha) * Clamp01(dt / 300.0f * 2.2f);
    if (std::fabs(st_->watch_alpha - target) < 0.02f) st_->watch_alpha = target;
    if (st_->watch_alpha <= 0.0f) return;
    animating_ = true;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const Formats& f = EnsureFormats(compositor_, s_);
    if (!f.watch) return;
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(theme_.accent, &brush))) return;
    Ctx c{compositor_, dc, brush, s_};
    const bool dark = theme_.bg.r < 0.5f;
    const size_t k = (std::max<size_t>)(AiListReadK(*o_, now), 1);
    std::wstring text = o_->watch_text;
    auto fill = [&text](std::wstring_view key, const std::wstring& v) {
        if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), v);
    };
    fill(L"{k}", std::to_wstring(k));
    fill(L"{n}", std::to_wstring(o_->read_total));
    // h 30, r 15, padding 0 12 0 10, gap 7, eye 15; translateY(-6) → 0.
    const float tw = c.Measure(text, f.watch);
    const float w = 10.0f * s_ + 15.0f * s_ + 7.0f * s_ + tw + 12.0f * s_;
    const float x = right - w;
    if (x < min_left) { brush->Release(); return; }
    const float a = st_->watch_alpha;
    const float y = std::round(top + (h - 30.0f * s_) * 0.5f - 6.0f * s_ * (1.0f - Ease(a)));
    const D2D1_RECT_F r = D2D1::RectF(x, y, right, y + 30.0f * s_);
    D2D1_COLOR_F accent = theme_.accent;
    accent.a = 1.0f;
    const D2D1_COLOR_F card = Over(theme_.surface_card, theme_.bg);
    c.Fill(r, 15.0f * s_, Alpha(Mix(card, accent, dark ? 0.10f : 0.07f), a));
    c.Stroke(r, 15.0f * s_, Alpha(Mix(card, accent, dark ? 0.20f : 0.13f), a));
    c.Icon(AiIcon::Eye, x + 10.0f * s_, y + 7.5f * s_, 15.0f * s_, Alpha(accent, a));
    // .shimmer: text2 with an accent band every 1.8 s.
    const float tx = x + 32.0f * s_;
    const D2D1_RECT_F tr = D2D1::RectF(tx, y, tx + tw + 2.0f, y + 30.0f * s_);
    D2D1_COLOR_F text2 = theme_.text_secondary;
    text2.a = 1.0f;
    c.Text(text, f.watch, tr, Alpha(text2, a));
    const float t = static_cast<float>(now % 1800) / 1800.0f;
    const float gx = tx - 1.2f * tw * (1.0f - 2.2f * t);
    const D2D1_GRADIENT_STOP stops[] = {{0.35f, Alpha(accent, 0.0f)}, {0.5f, accent}, {0.65f, Alpha(accent, 0.0f)}};
    ID2D1GradientStopCollection* coll = nullptr;
    ID2D1LinearGradientBrush* mask = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &coll)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(gx, 0.0f), D2D1::Point2F(gx + 2.2f * tw, 0.0f)), coll,
            &mask))) {
        dc->PushLayer(D2D1::LayerParameters1(tr, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                             a, mask, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        c.Text(text, f.watch, tr, accent);
        dc->PopLayer();
    }
    if (mask) mask->Release();
    if (coll) coll->Release();
    brush->Release();
}

void ReleaseAiListOverlayResources() { g_formats.Release(); }

}  // namespace pulse::ui
