// ai_panel_rich.cpp — the Pulse 助手 panel's rich text: answers rendered from
// Markdown (headings, lists, quotes, inline code, links, rules, code blocks
// with a copy button, scrolling tables, local path chips), messages, and the
// selection across them with its mini toolbar; the composer's quote chip.
#include "ai_panel_rich.h"

#include "ai_panel_draw.h"
#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace pulse::ui {
using namespace ai_draw;
namespace wrl = Microsoft::WRL;   // ui has a ComPtr of its own

namespace {

constexpr wchar_t kPad = 0xFFFC;   // room for a chip's or code span's padding
constexpr int kTargetsPerBlock = 256;

std::wstring L10n(l10n::StringId id) { return l10n::Get(id).c_str(); }

std::wstring Format(l10n::StringId id, int a, int b = 0) {
    wchar_t buf[160];
    swprintf_s(buf, l10n::Get(id).c_str(), a, b);
    return buf;
}

uint64_t Hash(std::wstring_view text, uint64_t h = 1469598103934665603ull) {
    for (wchar_t c : text) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t FloatBits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// Empty room in a line (a chip's or code span's padding): never drawn.
class Spacer final : public IDWriteInlineObject {
public:
    Spacer(float width, bool keep_with_next) : width_(width), keep_next_(keep_with_next) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
            *out = static_cast<IDWriteInlineObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT, FLOAT, BOOL, BOOL, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
        m->width = width_;
        m->height = 1.0f;
        m->baseline = 1.0f;
        m->supportsSideways = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* m) override {
        *m = {0.0f, 0.0f, 0.0f, 0.0f};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,
                                                 DWRITE_BREAK_CONDITION* after) override {
        // A left padding stays with the text after it, a right one with the text before.
        *before = keep_next_ ? DWRITE_BREAK_CONDITION_NEUTRAL : DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        *after = keep_next_ ? DWRITE_BREAK_CONDITION_MAY_NOT_BREAK : DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    float width_;
    bool keep_next_;
};

// A block's text with pads around code spans and path chips.
struct Laid {
    std::wstring text;
    std::vector<uint32_t> to_doc;     // layout pos -> doc offset (size + 1)
    std::vector<uint32_t> from_doc;   // doc offset -> layout pos (size + 1)
    struct Pad {
        uint32_t at;
        float width;
        bool left;
    };
    std::vector<Pad> pads;
};

Laid LayText(const airich::Block& b, float s) {
    Laid out;
    const uint32_t n = static_cast<uint32_t>(b.text.size());
    out.from_doc.assign(n + 1, 0);
    const auto padded = [](const airich::Run& r) { return (r.flags & (airich::kCode | airich::kPath)) != 0; };
    for (uint32_t i = 0; i <= n; ++i) {
        for (const airich::Run& r : b.runs)
            if (padded(r) && r.start + r.length == i) {
                out.pads.push_back({static_cast<uint32_t>(out.text.size()), (r.flags & airich::kPath) ? 7.0f * s : 5.0f * s,
                                    false});
                out.text.push_back(kPad);
                out.to_doc.push_back(i);
            }
        for (const airich::Run& r : b.runs)
            if (padded(r) && r.start == i && r.length) {
                out.pads.push_back({static_cast<uint32_t>(out.text.size()), (r.flags & airich::kPath) ? 24.0f * s : 5.0f * s,
                                    true});
                out.text.push_back(kPad);
                out.to_doc.push_back(i);
            }
        out.from_doc[i] = static_cast<uint32_t>(out.text.size());
        if (i < n) {
            out.text.push_back(b.text[i]);
            out.to_doc.push_back(i);
        }
    }
    out.to_doc.push_back(n);
    return out;
}

std::vector<DWRITE_HIT_TEST_METRICS> RangeRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length) {
    std::vector<DWRITE_HIT_TEST_METRICS> rects;
    if (!layout || !length) return rects;
    UINT32 count = 0;
    layout->HitTestTextRange(start, length, 0.0f, 0.0f, nullptr, 0, &count);
    if (!count) return rects;
    rects.resize(count);
    if (FAILED(layout->HitTestTextRange(start, length, 0.0f, 0.0f, rects.data(), count, &count))) rects.clear();
    rects.resize(count);
    return rects;
}

// One rect per line: a chip's pads (inline objects) and its text come back
// as separate runs, and one chip must not draw as three.
std::vector<DWRITE_HIT_TEST_METRICS> LineRects(IDWriteTextLayout* layout, uint32_t start, uint32_t length) {
    std::vector<DWRITE_HIT_TEST_METRICS> out;
    for (const DWRITE_HIT_TEST_METRICS& m : RangeRects(layout, start, length)) {
        if (!out.empty() && std::fabs(out.back().top - m.top) < 1.0f) {
            DWRITE_HIT_TEST_METRICS& b = out.back();
            const float l = (std::min)(b.left, m.left), r = (std::max)(b.left + b.width, m.left + m.width);
            b.left = l;
            b.width = r - l;
            b.length += m.length;
            continue;
        }
        out.push_back(m);
    }
    return out;
}

bool IsCjk(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x3040 && c <= 0x30FF);
}

int CharClass(wchar_t c) {
    if (c == kPad) return 4;
    if (c <= L' ' || c == 0x3000) return 0;
    if (std::iswalnum(c) || c == L'_') return 1;
    if (IsCjk(c)) return 2;
    return 3;
}

} // namespace

// --------------------------------------------------------------- layout --

void AiPanelView::EnsureRichBrushes(ID2D1DeviceContext* dc, const Palette& pal, bool dark) {
    if (!rich_) rich_ = std::make_unique<RichCache>();
    RichCache& r = *rich_;
    if (r.dc != dc) {
        r.link.Reset();
        r.text2.Reset();
        for (auto& b : r.syntax) b.Reset();
        r.dc = dc;
        ++r.brush_gen;   // layouts hold the old brushes: lay out again
        if (!dc) return;
        dc->CreateSolidColorBrush(pal.accent, &r.link);
        dc->CreateSolidColorBrush(pal.text2, &r.text2);
        for (size_t i = 1; i < r.syntax.size(); ++i)
            dc->CreateSolidColorBrush(D2D1::ColorF(SyntaxTokenRgb(static_cast<SyntaxToken>(i), dark)), &r.syntax[i]);
    }
    if (r.link) r.link->SetColor(pal.accent);
    if (r.text2) r.text2->SetColor(pal.text2);
    for (size_t i = 1; i < r.syntax.size(); ++i)
        if (r.syntax[i]) r.syntax[i]->SetColor(D2D1::ColorF(SyntaxTokenRgb(static_cast<SyntaxToken>(i), dark)));
}

void AiPanelView::ReleaseRich() {
    if (!rich_) return;
    rich_->docs.clear();
    rich_->link.Reset();
    rich_->text2.Reset();
    for (auto& b : rich_->syntax) b.Reset();
    rich_->dc = nullptr;
    ++rich_->brush_gen;
}

const AiRichDoc* AiPanelView::RichFor(Compositor* compositor, const AiBlockView& block, float width, float s) {
    if (!rich_) rich_ = std::make_unique<RichCache>();
    RichCache& rc = *rich_;
    const bool user = block.kind == AiBlockKind::User;
    uint64_t env = Mix64(FloatBits(width), FloatBits(s));
    env = Mix64(env, FloatBits(formats_.font_scale));
    env = Mix64(env, rc.brush_gen);
    env = Mix64(env, user ? 1u : 2u);
    const uint64_t sig = Mix64(Hash(block.text), env);
    AiRichDoc& doc = rc.docs[block.id];
    doc.used = true;
    if (doc.sig == sig && doc.sig != 0) return &doc;
    // Keep the layouts of pieces that did not change (the answer streams in).
    std::unordered_map<uint64_t, wrl::ComPtr<IDWriteTextLayout>> reuse;
    if (doc.env == env)
        for (AiRichPiece& p : doc.pieces)
            if (p.key && p.layout) reuse.emplace(p.key, p.layout);
    AiRichDoc next;
    next.sig = sig;
    next.env = env;
    next.used = true;
    next.user = user;
    BuildRich(compositor, block, width, s, next, reuse);
    doc = std::move(next);
    return &doc;
}

void AiPanelView::BuildRich(Compositor* compositor, const AiBlockView& block, float width, float s, AiRichDoc& doc,
                            std::unordered_map<uint64_t, wrl::ComPtr<IDWriteTextLayout>>& reuse) {
    auto* factory = compositor->DwriteFactory();
    RichCache& rc = *rich_;
    if (!factory || !formats_.body) return;
    const float body_size = formats_.body->GetFontSize();
    const float px = body_size / 13.5f;   // one design px of type at this scale and font scale
    wchar_t mono_family[64] = L"Consolas";
    if (formats_.mono125) formats_.mono125->GetFontFamilyName(mono_family, 64);
    const float mono_size = formats_.mono125 ? formats_.mono125->GetFontSize() : 12.5f * px;

    const auto make_layout = [&](const std::wstring& text, IDWriteTextFormat* fmt, float w, bool wrap, float lh,
                                 uint64_t key) -> wrl::ComPtr<IDWriteTextLayout> {
        if (key) {
            auto it = reuse.find(key);
            if (it != reuse.end()) return it->second;
        }
        wrl::ComPtr<IDWriteTextLayout> layout;
        const std::wstring& t = text.empty() ? std::wstring(L" ") : text;
        if (FAILED(factory->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), fmt, (std::max)(1.0f, w),
                                             100000.0f, &layout)))
            return nullptr;
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, lh * 0.78f);
        return layout;
    };

    // ---- A message: plain text, 13.5 / 21 in the bubble. Leading "> " lines
    // are the words it quotes (引用): 12.5 / 18 in text2 by a bar, above it.
    if (block.kind == AiBlockKind::User) {
        std::wstring quote, body = block.text;
        if (body.rfind(L">", 0) == 0) {
            size_t pos = 0;
            while (pos < body.size() && body[pos] == L'>') {
                size_t end = body.find(L'\n', pos);
                if (end == std::wstring::npos) end = body.size();
                size_t from = pos + 1;
                if (from < end && body[from] == L' ') ++from;
                size_t to = end;
                if (to > from && body[to - 1] == L'\r') --to;
                if (!quote.empty()) quote += L'\n';
                quote.append(body, from, to - from);
                pos = end < body.size() ? end + 1 : end;
            }
            // The blank line after it.
            if (pos < body.size() && body[pos] == L'\r') ++pos;
            if (pos < body.size() && body[pos] == L'\n') ++pos;
            if (pos < body.size()) body.erase(0, pos);
            else quote.clear();   // nothing but a quote: show it as typed
        }
        const auto add = [&](const std::wstring& text, IDWriteTextFormat* fmt, float lh, float x, float y, int ink) {
            AiRichPiece p;
            p.md = -1;
            p.text = text;
            p.to_doc.resize(p.text.size() + 1);
            for (size_t i = 0; i <= p.text.size(); ++i) p.to_doc[i] = static_cast<uint32_t>(i);
            p.line_h = lh;
            p.ink = static_cast<uint8_t>(ink);
            p.x = x;
            p.y = y;
            p.layout = make_layout(p.text, fmt, width - x, true, p.line_h, 0);
            DWRITE_TEXT_METRICS m{};
            if (p.layout) p.layout->GetMetrics(&m);
            p.w = (std::max)(1.0f, (std::min)(width - x, m.widthIncludingTrailingWhitespace));
            p.h = (std::max)(p.line_h, m.height);
            doc.text_w = (std::max)(doc.text_w, x + p.w);
            doc.pieces.push_back(std::move(p));
            return doc.pieces.back().h;
        };
        float y = 0.0f;
        if (!quote.empty()) {
            const float qh = add(quote, formats_.small125 ? formats_.small125 : formats_.body, 18.0f * s, 11.0f * s, 0.0f, 1);
            AiRichDeco d;
            d.kind = AiRichDecoKind::QuoteBar;
            d.r = D2D1::RectF(0.0f, 1.0f * s, 3.0f * s, qh - 1.0f * s);
            doc.decos.push_back(std::move(d));
            y = qh + 6.0f * s;
        }
        y += add(body, formats_.body, 21.0f * s, 0.0f, y, 0);
        doc.height = y;
        return;
    }

    doc.doc = airich::Parse(block.text);
    const std::vector<airich::Block>& blocks = doc.doc.blocks;
    float y = 0.0f, pending = 0.0f;
    bool first = true;
    const auto place = [&](float mt) {
        if (first) {
            doc.margin_top = mt;
            first = false;
        } else {
            y += (std::max)(pending, mt);
        }
    };
    const auto add_target = [&](AiPanelPart part, std::wstring text, int box) {
        if (doc.targets.size() >= static_cast<size_t>(kTargetsPerBlock)) return -1;
        AiRichTarget t;
        t.part = part;
        t.text = std::move(text);
        t.box = box;
        doc.targets.push_back(std::move(t));
        return static_cast<int>(doc.targets.size()) - 1;
    };

    // Inline formatting of a new layout: the pads and the runs' fonts. It
    // changes the line breaks, so it comes before the layout is measured.
    const auto format = [&](const airich::Block& b, const Laid& laid, IDWriteTextLayout* layout) {
        for (const Laid::Pad& pad : laid.pads) {
            IDWriteInlineObject* sp = new Spacer(pad.width, pad.left);
            layout->SetInlineObject(sp, {pad.at, 1});
            sp->Release();
        }
        for (const airich::Run& r : b.runs) {
            if (!r.length) continue;
            const uint32_t a = laid.from_doc[r.start];
            const uint32_t e = laid.from_doc[r.start + r.length - 1] + 1;
            const DWRITE_TEXT_RANGE range{a, e - a};
            if (r.flags & airich::kBold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
            if (r.flags & airich::kItalic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
            if (r.flags & airich::kStrike) layout->SetStrikethrough(TRUE, range);
            if (r.flags & airich::kUnderline) layout->SetUnderline(TRUE, range);
            if (r.flags & airich::kCode) {
                layout->SetFontFamilyName(mono_family, range);
                layout->SetFontSize(mono_size, range);
            }
            if (r.flags & airich::kPath) layout->SetFontSize(12.5f * px, range);
            if ((r.flags & airich::kLink) && rc.link) layout->SetDrawingEffect(rc.link.Get(), range);
        }
    };

    // Decorations of a formatted, placed block: chips, code spans, links.
    const auto style = [&](AiRichPiece& p, const airich::Block& b, const Laid& laid, IDWriteTextLayout* layout,
                           int piece_index) {
        for (const airich::Run& r : b.runs) {
            if (!r.length) continue;
            const uint32_t a = laid.from_doc[r.start];
            const uint32_t e = laid.from_doc[r.start + r.length - 1] + 1;
            const float base = p.line_h * 0.78f;
            if (r.flags & (airich::kCode | airich::kPath)) {
                // The pads either side are part of the chip.
                const uint32_t pa = a > 0 && p.text[a - 1] == kPad ? a - 1 : a;
                const uint32_t pe = e < p.text.size() && p.text[e] == kPad ? e + 1 : e;
                const bool path = (r.flags & airich::kPath) != 0;
                int target = -1;
                if (path) target = add_target(AiPanelPart::RichPath, r.target, p.box);
                for (const auto& m : LineRects(layout, pa, pe - pa)) {
                    const float cy = p.y + m.top + base - 4.6f * s;
                    const float half = (path ? 10.5f : 9.5f) * s;
                    AiRichDeco d;
                    d.kind = path ? AiRichDecoKind::Chip : AiRichDecoKind::CodeSpan;
                    d.r = D2D1::RectF(p.x + m.left, cy - half, p.x + m.left + m.width, cy + half);
                    d.piece = piece_index;
                    d.target = target;
                    if (path) {
                        const size_t slash = r.target.find_last_of(L"\\/");
                        const std::wstring leaf = slash == std::wstring::npos ? r.target : r.target.substr(slash + 1);
                        d.file = !leaf.empty() && leaf.find(L'.') != std::wstring::npos && leaf.back() != L'.';
                        if (target >= 0) doc.targets[static_cast<size_t>(target)].rects.push_back(d.r);
                    }
                    doc.decos.push_back(std::move(d));
                }
            }
            if (r.flags & airich::kLink) {
                const int target = add_target(AiPanelPart::RichLink, r.target, p.box);
                for (const auto& m : LineRects(layout, a, e - a)) {
                    AiRichDeco d;
                    d.kind = AiRichDecoKind::LinkLine;
                    const float ly = p.y + m.top + base + 2.0f * s;
                    d.r = D2D1::RectF(p.x + m.left, ly, p.x + m.left + m.width, ly + 1.0f * s);
                    d.piece = piece_index;
                    d.target = target;
                    if (target >= 0)
                        doc.targets[static_cast<size_t>(target)].rects.push_back(
                            D2D1::RectF(p.x + m.left, p.y + m.top, p.x + m.left + m.width, p.y + m.top + m.height));
                    doc.decos.push_back(std::move(d));
                }
            }
        }
    };

    const float list_step = 22.0f * s, quote_step = 15.0f * s;
    struct Bar {
        int depth;
        float top, bottom;
    };
    std::vector<Bar> bars;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const airich::Block& b = blocks[bi];
        const airich::Block* nb = bi + 1 < blocks.size() ? &blocks[bi + 1] : nullptr;
        const float tx = quote_step * static_cast<float>(b.quote) + list_step * static_cast<float>(b.indent);
        const float tw = (std::max)(40.0f * s, width - tx);
        if (b.kind == airich::Kind::Rule) {
            place(14.0f * s);
            AiRichDeco d;
            d.kind = AiRichDecoKind::Rule;
            d.r = D2D1::RectF(tx, y, width, y + 1.0f * s);
            doc.decos.push_back(std::move(d));
            y += 1.0f * s;
            pending = 14.0f * s;
            continue;
        }
        if (b.kind == airich::Kind::Code) {
            place(4.0f * s);
            AiRichBox box;
            box.md = static_cast<int>(bi);
            box.lang = b.lang;
            const int box_index = static_cast<int>(doc.boxes.size());
            box.target = add_target(AiPanelPart::CodeCopy, b.text, -1);
            int lines = 1;
            for (wchar_t c : b.text)
                if (c == L'\n') ++lines;
            box.lines = lines;
            const float lh = 20.0f * s;
            const float head = 34.0f * s;
            const float top = y;
            box.r = D2D1::RectF(tx, top, width, top + (1.0f + 34.0f + 1.0f + 10.0f + 12.0f + 1.0f) * s + lines * lh);
            box.view_left = tx + (1.0f + 46.0f) * s;
            box.view_right = width - 1.0f * s;
            box.body_top = top + 1.0f * s + head + 1.0f * s;
            box.body_bottom = box.r.bottom - 1.0f * s;
            AiRichPiece p;
            p.md = static_cast<int>(bi);
            p.text = b.text;
            p.to_doc.resize(p.text.size() + 1);
            for (size_t i = 0; i <= p.text.size(); ++i) p.to_doc[i] = static_cast<uint32_t>(i);
            p.line_h = lh;
            p.box = box_index;
            p.ink = 0;
            p.key = Mix64(Hash(p.text), 0xC0DEu);
            const bool fresh = !reuse.count(p.key);
            p.layout = make_layout(p.text, formats_.mono125 ? formats_.mono125 : formats_.body, 10000.0f, false, lh,
                                   p.key);
            if (p.layout && fresh) {
                p.layout->SetIncrementalTabStop(mono_size * 0.55f * 4.0f);
                const std::wstring ext = airich::ExtensionForLanguage(b.lang);
                if (!ext.empty()) {
                    for (const SyntaxSpan& span : HighlightSyntax(ext, p.text, 20000)) {
                        const size_t t = static_cast<size_t>(span.token);
                        if (t > 0 && t < rc.syntax.size() && rc.syntax[t])
                            p.layout->SetDrawingEffect(rc.syntax[t].Get(), {span.start, span.length});
                    }
                }
            }
            DWRITE_TEXT_METRICS m{};
            if (p.layout) p.layout->GetMetrics(&m);
            p.x = box.view_left;
            p.y = box.body_top + 10.0f * s;
            p.w = m.widthIncludingTrailingWhitespace;
            p.h = lines * lh;
            box.content_w = p.w + 14.0f * s;
            doc.pieces.push_back(std::move(p));
            doc.boxes.push_back(std::move(box));
            y = doc.boxes.back().r.bottom;
            pending = 12.0f * s;
            continue;
        }
        if (b.kind == airich::Kind::Cell) {
            if (b.table < 0 || (bi > 0 && blocks[bi - 1].table == b.table)) continue;   // laid out with its first cell
            const airich::Table& t = doc.doc.tables[static_cast<size_t>(b.table)];
            place(4.0f * s);
            AiRichBox box;
            box.table = true;
            box.md = b.table;
            box.columns = (std::max)(1, t.columns);
            box.header_row = b.header;
            const int box_index = static_cast<int>(doc.boxes.size());
            box.target = add_target(AiPanelPart::TableCopy, airich::TableTsv(doc.doc, b.table), -1);
            const int rows = (std::max)(1, t.rows);
            box.lines = box.header_row ? rows - 1 : rows;
            const float top = y;
            const float row_h = 32.0f * s, line = 1.0f * s;
            box.col_w.assign(static_cast<size_t>(box.columns), 40.0f * s);
            const size_t first_piece = doc.pieces.size();
            const float lh = 18.0f * s;
            for (int ci = t.first; ci < t.first + t.count && ci < static_cast<int>(blocks.size()); ++ci) {
                const airich::Block& cb = blocks[static_cast<size_t>(ci)];
                AiRichPiece p;
                p.md = ci;
                const Laid laid = LayText(cb, s);
                p.text = laid.text;
                p.to_doc = laid.to_doc;
                p.line_h = lh;
                p.box = box_index;
                p.ink = cb.header ? 1 : 0;
                p.key = Mix64(Mix64(Hash(p.text), cb.header ? 0x7Bu : 0x7Cu), cb.runs.size());
                const bool fresh_cell = !reuse.count(p.key);
                p.layout = make_layout(p.text, cb.header ? formats_.small125b : formats_.small125, 10000.0f, false, lh,
                                       p.key);
                if (!p.layout) continue;
                if (fresh_cell) format(cb, laid, p.layout.Get());   // bold and chips widen the column
                DWRITE_TEXT_METRICS m{};
                p.layout->GetMetrics(&m);
                p.w = m.widthIncludingTrailingWhitespace;
                p.h = lh;
                const size_t col = static_cast<size_t>(std::clamp(cb.col, 0, box.columns - 1));
                box.col_w[col] = (std::max)(box.col_w[col], std::ceil(p.w) + 20.0f * s);
                // Styling needs final positions: done below once the columns are known.
                p.x = static_cast<float>(col);
                p.y = static_cast<float>(cb.row);
                doc.pieces.push_back(std::move(p));
            }
            box.r = D2D1::RectF(tx, top, width, top);
            box.view_left = tx + line;
            box.view_right = width - line;
            const float view_w = box.view_right - box.view_left;
            float grid_w = 0.0f;
            for (float w : box.col_w) grid_w += w;
            if (grid_w < view_w) {
                const float extra = (view_w - grid_w) / static_cast<float>(box.columns);
                for (float& w : box.col_w) w += extra;
                grid_w = view_w;
            }
            box.content_w = grid_w;
            box.col_x.resize(box.col_w.size());
            float cx = 0.0f;
            for (size_t c = 0; c < box.col_w.size(); ++c) {
                box.col_x[c] = cx;
                cx += box.col_w[c];
            }
            box.body_top = top + line;
            box.row_y.resize(static_cast<size_t>(rows) + 1);
            for (int r = 0; r <= rows; ++r) box.row_y[static_cast<size_t>(r)] = box.body_top + r * (row_h + line);
            box.body_bottom = box.row_y.back() - line;
            // Footer: 1 px rule and 38 px (6 + 26 + 6).
            box.r.bottom = box.body_bottom + line + 38.0f * s + line;
            for (size_t pi = first_piece; pi < doc.pieces.size(); ++pi) {
                AiRichPiece& p = doc.pieces[pi];
                const size_t col = static_cast<size_t>(p.x);
                const int row = static_cast<int>(p.y);
                const airich::Block& cb = blocks[static_cast<size_t>(p.md)];
                const float cell_l = box.view_left + box.col_x[col] + 10.0f * s;
                const float cell_w = box.col_w[col] - 20.0f * s;
                wchar_t align = cb.align;
                if (align == L'-' && !cb.header) {
                    // Figures line up on the right like the design's .num cells.
                    bool num = !cb.text.empty();
                    for (wchar_t ch : cb.text)
                        if (!(std::iswdigit(ch) || ch == L'.' || ch == L',' || ch == L' ' || ch == L'%' ||
                              ch == L'-' || ch == L'+' || std::iswalpha(ch)))
                            num = false;
                    num = num && std::iswdigit(cb.text[0]);
                    if (num) align = L'r';
                }
                p.x = align == L'r' ? cell_l + cell_w - p.w : align == L'c' ? cell_l + (cell_w - p.w) * 0.5f : cell_l;
                p.y = box.row_y[static_cast<size_t>(std::clamp(row, 0, rows - 1))] + 7.0f * s;
                const Laid laid = LayText(cb, s);
                style(p, cb, laid, p.layout.Get(), static_cast<int>(pi));
            }
            doc.boxes.push_back(std::move(box));
            y = doc.boxes.back().r.bottom;
            pending = 12.0f * s;
            continue;
        }

        // Paragraph, heading, raw HTML.
        float mt = 2.0f * s, mb = 10.0f * s;
        float lh = 22.0f * s;
        IDWriteTextFormat* fmt = formats_.body;
        float size = 0.0f;
        if (b.kind == airich::Kind::Heading) {
            fmt = formats_.body_b;
            if (b.level <= 1) { size = 17.0f; lh = 24.0f * s; mt = 16.0f * s; mb = 8.0f * s; }
            else if (b.level == 2) { size = 15.0f; mt = 14.0f * s; mb = 6.0f * s; }
            else { mt = 12.0f * s; mb = 4.0f * s; }
        } else if (b.indent > 0) {
            mt = 3.0f * s;
            mb = !nb || nb->indent == 0 ? 10.0f * s : nb->indent < b.indent ? 4.0f * s : 3.0f * s;
            // A numbered list right before a task list (or the reverse) is
            // two lists: the list's own bottom margin parts them.
            if (nb && nb->indent == b.indent && !b.marker.empty() && !nb->marker.empty() &&
                b.marker[0] != nb->marker[0])
                mb = 10.0f * s;
        } else if (b.quote > 0) {
            mb = nb && nb->quote > 0 ? 6.0f * s : 10.0f * s;
        }
        if (b.kind == airich::Kind::Html) fmt = formats_.mono125 ? formats_.mono125 : formats_.body;
        place(mt);
        AiRichPiece p;
        p.md = static_cast<int>(bi);
        const Laid laid = LayText(b, s);
        p.text = laid.text;
        p.to_doc = laid.to_doc;
        p.line_h = lh;
        p.ink = (b.quote > 0 || b.kind == airich::Kind::Html) ? 1 : 0;
        p.x = tx;
        p.y = y;
        uint64_t key = Hash(p.text);
        key = Mix64(key, static_cast<uint64_t>(b.kind) * 131u + static_cast<uint64_t>(b.level));
        key = Mix64(key, FloatBits(tw));
        for (const airich::Run& r : b.runs) key = Mix64(key, (static_cast<uint64_t>(r.start) << 32) ^ r.length ^ (static_cast<uint64_t>(r.flags) << 20));
        p.key = key;
        const bool fresh = !reuse.count(key);
        p.layout = make_layout(p.text, fmt, tw, true, lh, key);
        if (!p.layout) continue;
        if (fresh && size > 0.0f) p.layout->SetFontSize(size * px, {0, static_cast<UINT32>(p.text.size())});
        if (fresh) format(b, laid, p.layout.Get());   // before measuring: pads and fonts move the breaks
        DWRITE_TEXT_METRICS m{};
        p.layout->GetMetrics(&m);
        p.w = tw;
        p.h = (std::max)(lh, m.height);
        const int piece_index = static_cast<int>(doc.pieces.size());
        style(p, b, laid, p.layout.Get(), piece_index);
        // List markers in the gutter left of the text.
        if (!b.marker.empty()) {
            AiRichDeco d;
            const float cy = y + lh * 0.5f;
            if (b.marker[0] == L'o') {
                d.kind = AiRichDecoKind::Number;
                d.text = b.marker.substr(1) + L".";
                d.r = D2D1::RectF(tx - list_step - 4.0f * s, y, tx - 5.0f * s, y + lh);
            } else if (b.marker == L"t0" || b.marker == L"t1") {
                d.kind = b.marker == L"t1" ? AiRichDecoKind::TaskDone : AiRichDecoKind::Task;
                d.r = D2D1::RectF(tx - 19.0f * s, cy - 7.0f * s, tx - 5.0f * s, cy + 7.0f * s);
            } else {
                d.kind = b.indent <= 1 ? AiRichDecoKind::Dot : b.indent == 2 ? AiRichDecoKind::Ring : AiRichDecoKind::Square;
                const float mx = tx - 12.0f * s;
                d.r = D2D1::RectF(mx - 2.6f * s, cy - 2.6f * s + 0.5f * s, mx + 2.6f * s, cy + 2.6f * s + 0.5f * s);
            }
            doc.decos.push_back(std::move(d));
        }
        for (int q = 0; q < b.quote; ++q) bars.push_back({q, y - 2.0f * s, y + p.h + 2.0f * s});
        y += p.h;
        pending = mb;
        doc.pieces.push_back(std::move(p));
    }
    // Quote bars: one per depth over consecutive quoted blocks.
    for (size_t i = 0; i < bars.size(); ++i) {
        Bar bar = bars[i];
        while (i + 1 < bars.size() && bars[i + 1].depth == bar.depth && bars[i + 1].top - bar.bottom < 16.0f * s) {
            bar.bottom = bars[i + 1].bottom;
            ++i;
        }
        AiRichDeco d;
        d.kind = AiRichDecoKind::QuoteBar;
        const float bx = quote_step * static_cast<float>(bar.depth);
        d.r = D2D1::RectF(bx, bar.top, bx + 3.0f * s, bar.bottom);
        doc.decos.push_back(std::move(d));
    }
    doc.height = y;
    doc.margin_bottom = first ? 0.0f : pending;
    if (first) doc.margin_top = 2.0f * s;
}

// -------------------------------------------------------------- drawing --

float AiPanelView::BoxScroll(uint32_t id, int box) const {
    if (!rich_) return 0.0f;
    auto it = rich_->box_scroll.find((static_cast<uint64_t>(id) << 16) | static_cast<uint64_t>(box & 0xFFFF));
    return it == rich_->box_scroll.end() ? 0.0f : it->second;
}

bool AiPanelView::SelectionIn(uint32_t id, int piece, uint32_t length, uint32_t& a, uint32_t& b) const {
    if (!rich_ || !rich_->active) return false;
    const RichCache& rc = *rich_;
    RichCache::Pos s0 = rc.anchor, s1 = rc.focus;
    if (ComparePos(s1, s0) < 0) std::swap(s0, s1);
    if (ComparePos(s0, s1) == 0) return false;
    const RichCache::Pos lo{id, piece, 0}, hi{id, piece, length};
    if (ComparePos(hi, s0) <= 0 || ComparePos(lo, s1) >= 0) return false;
    a = (s0.id == id && s0.piece == piece) ? (std::min)(s0.offset, length) : 0;
    b = (s1.id == id && s1.piece == piece) ? (std::min)(s1.offset, length) : length;
    return b > a;
}

int AiPanelView::ComparePos(const RichCache::Pos& x, const RichCache::Pos& y) const {
    const auto ord = [&](uint32_t id) {
        auto it = rich_->ordinal.find(id);
        return it == rich_->ordinal.end() ? -1 : it->second;
    };
    const int ox = ord(x.id), oy = ord(y.id);
    if (ox != oy) return ox < oy ? -1 : 1;
    if (x.piece != y.piece) return x.piece < y.piece ? -1 : 1;
    if (x.offset != y.offset) return x.offset < y.offset ? -1 : 1;
    return 0;
}

void AiPanelView::DrawRich(Compositor* compositor, const AiRichDoc& doc, const AiBlockView& block, int index, float ox,
                           float oy, const Palette& pal, const D2D1_RECT_F& area, const AiPanelViewModel& vm,
                           float s, uint64_t now_ms, bool draw) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    ID2D1DeviceContext* dc = c.dc;
    RichCache& rc = *rich_;
    const D2D1_COLOR_F sel = Alpha(pal.accent, vm.dark ? 0.32f : 0.26f);
    const auto is_hot = [&](AiPanelPart part, int k) {
        return vm.hover.part == part && vm.hover.index == index * kTargetsPerBlock + k;
    };
    const auto box_view = [&](const AiRichBox& box) {
        return D2D1::RectF(ox + box.view_left, oy + box.body_top, ox + box.view_right, oy + box.body_bottom);
    };
    const auto clip_to = [](D2D1_RECT_F a, const D2D1_RECT_F& b) {
        a.left = (std::max)(a.left, b.left);
        a.top = (std::max)(a.top, b.top);
        a.right = (std::min)(a.right, b.right);
        a.bottom = (std::min)(a.bottom, b.bottom);
        return a;
    };
    const auto shift = [](D2D1_RECT_F r, float dx, float dy) {
        return D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy);
    };

    // Spots (every piece, for hit tests even when off screen) and targets.
    for (size_t pi = 0; pi < doc.pieces.size(); ++pi) {
        const AiRichPiece& p = doc.pieces[pi];
        AiRichSpot spot;
        spot.id = block.id;
        spot.block = index;
        spot.piece = static_cast<int>(pi);
        const float sx = p.box >= 0 ? BoxScroll(block.id, p.box) : 0.0f;
        spot.ox = ox + p.x - sx;
        spot.oy = oy + p.y;
        spot.rect = D2D1::RectF(spot.ox, spot.oy, spot.ox + (std::max)(p.w, 1.0f), spot.oy + p.h);
        if (p.box >= 0) spot.rect = clip_to(spot.rect, box_view(doc.boxes[static_cast<size_t>(p.box)]));
        rc.spots.push_back(spot);
    }
    if (!draw) return;

    // Boxes: frames first.
    for (size_t bi = 0; bi < doc.boxes.size(); ++bi) {
        const AiRichBox& box = doc.boxes[bi];
        const D2D1_RECT_F r = shift(box.r, ox, oy);
        if (r.bottom < area.top || r.top > area.bottom) continue;
        const float radius = 10.0f * s;
        if (!box.table) {
            const D2D1_COLOR_F code_bg = vm.dark ? Hex(0x1b1d1c) : Hex(0xf4f5f1);
            const D2D1_COLOR_F code_head = vm.dark ? Hex(0x252826) : Hex(0xeceee8);
            c.Fill(r, radius, code_bg);
            // Header 34: the language, its line count, 复制.
            const D2D1_RECT_F head = D2D1::RectF(r.left, r.top, r.right, r.top + 1.0f * s + 34.0f * s);
            dc->PushAxisAlignedClip(head, D2D1_ANTIALIAS_MODE_ALIASED);
            c.Fill(r, radius, code_head);
            dc->PopAxisAlignedClip();
            c.Fill(D2D1::RectF(r.left + 1.0f * s, head.bottom, r.right - 1.0f * s, head.bottom + 1.0f * s), 0.0f, pal.line2);
            c.Stroke(r, radius, pal.line);
            const std::wstring ext = airich::ExtensionForLanguage(box.lang);
            std::wstring lang(SyntaxLanguageName(ext));
            if (lang.empty()) lang = box.lang;
            if (lang.empty()) lang = L10n(l10n::StringId::AiRichPlainText);
            const float lx = r.left + 13.0f * s;
            const float lw = c.Measure(lang, formats_.caption_b);
            c.Text(lang, formats_.caption_b, D2D1::RectF(lx, head.top, lx + lw + 2.0f, head.bottom), pal.text2);
            const std::wstring lines = Format(l10n::StringId::AiRichLines, box.lines);
            c.Text(lines, formats_.caption, D2D1::RectF(lx + lw + 8.0f * s, head.top, r.right - 90.0f * s, head.bottom),
                   pal.text3);
            // Line numbers: width 34 + 12, right aligned, text3 at .7.
            const AiRichPiece* code = nullptr;
            for (const AiRichPiece& p : doc.pieces)
                if (p.box == static_cast<int>(bi)) code = &p;
            if (code) {
                TextOptions right;
                right.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
                const float y0 = oy + code->y;
                for (int ln = 0; ln < box.lines; ++ln) {
                    const float ly = y0 + ln * code->line_h;
                    if (ly + code->line_h < area.top || ly > area.bottom) continue;
                    c.Text(std::to_wstring(ln + 1), formats_.mono125 ? formats_.mono125 : formats_.caption,
                           D2D1::RectF(r.left + 1.0f * s, ly, r.left + 1.0f * s + 34.0f * s, ly + code->line_h),
                           Alpha(pal.text3, 0.7f), right);
                }
            }
        } else {
            const D2D1_COLOR_F code_head = vm.dark ? Hex(0x252826) : Hex(0xeceee8);
            c.Fill(r, radius, pal.card);
            const D2D1_RECT_F view = box_view(box);
            dc->PushAxisAlignedClip(clip_to(D2D1::RectF(r.left, r.top, r.right, oy + box.body_bottom), area),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            if (box.header_row && box.row_y.size() > 1) {
                const D2D1_RECT_F hr = D2D1::RectF(r.left, r.top, r.right, oy + box.row_y[1] - 1.0f * s);
                dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                c.Fill(r, radius, code_head);
                dc->PopAxisAlignedClip();
            }
            dc->PopAxisAlignedClip();
            // Row rules: under the header the line color, between rows line2.
            for (size_t ri = 1; ri + 1 < box.row_y.size(); ++ri) {
                const float ly = oy + box.row_y[ri] - 1.0f * s;
                c.Fill(D2D1::RectF(view.left, ly, view.right, ly + 1.0f * s), 0.0f,
                       (ri == 1 && box.header_row) ? pal.line : pal.line2);
            }
            // Footer: 3 行 · 4 列 and 复制表格.
            const float fy = oy + box.body_bottom;
            c.Fill(D2D1::RectF(r.left + 1.0f * s, fy, r.right - 1.0f * s, fy + 1.0f * s), 0.0f, pal.line2);
            const std::wstring size = Format(l10n::StringId::AiRichTableSize, box.lines, box.columns);
            c.Text(size, formats_.caption, D2D1::RectF(r.left + 11.0f * s, fy + 1.0f * s, r.right - 110.0f * s, r.bottom - 1.0f * s),
                   pal.text3);
            c.Stroke(r, radius, pal.line);
        }
    }

    // Quote bars, rules, list markers.
    for (const AiRichDeco& d : doc.decos) {
        if (d.piece >= 0) continue;
        const D2D1_RECT_F r = shift(d.r, ox, oy);
        if (r.bottom < area.top || r.top > area.bottom) continue;
        switch (d.kind) {
        case AiRichDecoKind::QuoteBar: c.Fill(r, 1.5f * s, pal.line); break;
        case AiRichDecoKind::Rule: c.Fill(r, 0.0f, pal.line); break;
        case AiRichDecoKind::Dot:
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f),
                                          (r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f),
                            c.B(pal.text3));
            break;
        case AiRichDecoKind::Ring:
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f),
                                          (r.right - r.left) * 0.5f - 0.5f * s, (r.bottom - r.top) * 0.5f - 0.5f * s),
                            c.B(pal.text3), 1.1f * s);
            break;
        case AiRichDecoKind::Square: c.Fill(Inflate(r, -0.4f * s), 0.0f, pal.text3); break;
        case AiRichDecoKind::Number: {
            TextOptions right;
            right.align = DWRITE_TEXT_ALIGNMENT_TRAILING;
            c.Text(d.text, formats_.body, r, pal.text2, right);
            break;
        }
        case AiRichDecoKind::Task:
            c.Stroke(r, 3.5f * s, pal.text3, 1.3f * s);
            break;
        case AiRichDecoKind::TaskDone:
            c.Fill(r, 3.5f * s, pal.accent);
            c.Icon(AiIcon::Check, r.left + 2.0f * s, r.top + 2.0f * s, (r.right - r.left) - 4.0f * s, pal.on_accent);
            break;
        default: break;
        }
    }

    // Pieces: selection, inline chips, text, link lines.
    for (size_t pi = 0; pi < doc.pieces.size(); ++pi) {
        const AiRichPiece& p = doc.pieces[pi];
        if (!p.layout) continue;
        const float py = oy + p.y;
        if (py + p.h < area.top || py > area.bottom) continue;
        const float sx = p.box >= 0 ? BoxScroll(block.id, p.box) : 0.0f;
        const float px = ox + p.x - sx;
        bool clipped = false;
        if (p.box >= 0) {
            dc->PushAxisAlignedClip(clip_to(box_view(doc.boxes[static_cast<size_t>(p.box)]), area),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            clipped = true;
        }
        uint32_t sa = 0, sb = 0;
        if (SelectionIn(block.id, static_cast<int>(pi), static_cast<uint32_t>(p.text.size()), sa, sb)) {
            for (const auto& m : RangeRects(p.layout.Get(), sa, sb - sa))
                c.Fill(D2D1::RectF(px + m.left, py + m.top, px + m.left + m.width, py + m.top + m.height), 0.0f, sel);
        }
        for (const AiRichDeco& d : doc.decos) {
            if (d.piece != static_cast<int>(pi)) continue;
            const D2D1_RECT_F r = shift(d.r, ox - sx, oy);
            if (d.kind == AiRichDecoKind::CodeSpan) {
                c.Fill(r, 5.0f * s, vm.dark ? Hex(0x1b1d1c) : Hex(0xf4f5f1));
                c.Stroke(r, 5.0f * s, pal.line2);
            } else if (d.kind == AiRichDecoKind::Chip) {
                const bool hot = is_hot(AiPanelPart::RichPath, d.target);
                c.Fill(r, 6.0f * s, hot ? pal.accent_tint : pal.card);
                c.Stroke(r, 6.0f * s, hot ? pal.accent : pal.line);
            }
        }
        compositor->DrawTextLayout(D2D1::Point2F(px, py), p.layout.Get(), c.B(p.ink == 1 ? pal.text2 : pal.text));
        for (const AiRichDeco& d : doc.decos) {
            if (d.piece != static_cast<int>(pi)) continue;
            const D2D1_RECT_F r = shift(d.r, ox - sx, oy);
            if (d.kind == AiRichDecoKind::Chip) {
                // The icon sits in the left pad (5 in, 14 wide), once per chip.
                const AiRichTarget* t = d.target >= 0 ? &doc.targets[static_cast<size_t>(d.target)] : nullptr;
                if (t && !t->rects.empty() && shift(t->rects.front(), ox - sx, oy).left != r.left) continue;
                const float is = 14.0f * s;
                const float iy = (r.top + r.bottom - is) * 0.5f;
                if (d.file) c.Icon(AiIcon::File, r.left + 5.0f * s, iy, is, pal.tint_fg[1]);
                else c.Icon(AiIcon::FolderFilled, r.left + 5.0f * s, iy, is, pal.folder_icon);
            } else if (d.kind == AiRichDecoKind::LinkLine) {
                const bool hot = is_hot(AiPanelPart::RichLink, d.target);
                c.Fill(r, 0.0f, Alpha(pal.accent, hot ? 1.0f : 0.4f));
            }
        }
        if (clipped) dc->PopAxisAlignedClip();
    }

    // Box overlays: the copy buttons, the sideways scroll thumb.
    for (size_t bi = 0; bi < doc.boxes.size(); ++bi) {
        const AiRichBox& box = doc.boxes[bi];
        const D2D1_RECT_F r = shift(box.r, ox, oy);
        if (r.bottom < area.top || r.top > area.bottom) continue;
        const bool copied = rc.copied_id == block.id && rc.copied_k == box.target && now_ms >= rc.copied_at &&
                            now_ms - rc.copied_at < 2000;
        if (copied) conv_decor_ = true;
        const std::wstring label = copied ? L10n(l10n::StringId::CopiedShort)
                                 : box.table ? L10n(l10n::StringId::AiRichCopyTable)
                                             : L10n(l10n::StringId::Copy);
        const float tw = c.Measure(label, formats_.caption);
        const float bw = (8.0f + 14.0f + 6.0f + 8.0f) * s + tw;
        const float by = box.table ? r.bottom - 1.0f * s - 6.0f * s - 26.0f * s : r.top + 1.0f * s + 4.0f * s;
        const D2D1_RECT_F br = D2D1::RectF(r.right - 1.0f * s - 6.0f * s - bw, by, r.right - 1.0f * s - 6.0f * s, by + 26.0f * s);
        const bool hot = is_hot(box.table ? AiPanelPart::TableCopy : AiPanelPart::CodeCopy, box.target);
        if (hot) c.Fill(br, 6.0f * s, pal.hover);
        const D2D1_COLOR_F ink = copied ? pal.ok : hot ? pal.text : pal.text2;
        c.Icon(copied ? AiIcon::Check : AiIcon::Copy, br.left + 8.0f * s, br.top + 6.0f * s, 14.0f * s, ink);
        c.Text(label, formats_.caption, D2D1::RectF(br.left + 28.0f * s, br.top, br.right, br.bottom), ink);
        if (box.target >= 0) {
            const D2D1_RECT_F v = clip_to(br, area);
            if (v.right > v.left && v.bottom > v.top)
                conv_hits_.push_back({v, {box.table ? AiPanelPart::TableCopy : AiPanelPart::CodeCopy,
                                          index * kTargetsPerBlock + box.target}});
        }
        const float view_w = box.view_right - box.view_left;
        if (box.content_w > view_w + 0.5f) {
            const float sx = BoxScroll(block.id, static_cast<int>(bi));
            const float k = view_w / box.content_w;
            const float tw2 = (std::max)(24.0f * s, view_w * k);
            const float tx = ox + box.view_left + (view_w - tw2) * (sx / (box.content_w - view_w));
            const float ty = oy + box.body_bottom - (box.table ? 5.0f : 6.0f) * s;
            c.Fill(D2D1::RectF(tx, ty, tx + tw2, ty + 4.0f * s), 2.0f * s, pal.line);
        }
    }
    // Link and chip hit rects.
    for (size_t k = 0; k < doc.targets.size(); ++k) {
        const AiRichTarget& t = doc.targets[k];
        if (t.part != AiPanelPart::RichLink && t.part != AiPanelPart::RichPath) continue;
        const float sx = t.box >= 0 ? BoxScroll(block.id, t.box) : 0.0f;
        for (const D2D1_RECT_F& rr : t.rects) {
            D2D1_RECT_F v = clip_to(shift(rr, ox - sx, oy), area);
            if (t.box >= 0) v = clip_to(v, box_view(doc.boxes[static_cast<size_t>(t.box)]));
            if (v.right > v.left && v.bottom > v.top)
                conv_hits_.push_back({v, {t.part, index * kTargetsPerBlock + static_cast<int>(k)}});
        }
    }
}

bool AiPanelView::RichCaret(const AiRichDoc& doc, uint32_t id, float ox, float oy, float& cx, float& cy,
                            float& lh) const {
    if (doc.pieces.empty()) return false;
    const AiRichPiece& p = doc.pieces.back();
    if (!p.layout) return false;
    const float sx = p.box >= 0 ? BoxScroll(id, p.box) : 0.0f;
    DWRITE_HIT_TEST_METRICS hm{};
    float px = 0.0f, py = 0.0f;
    const UINT32 n = static_cast<UINT32>(p.text.size());
    lh = p.line_h;
    if (n > 0 && SUCCEEDED(p.layout->HitTestTextPosition(n - 1, TRUE, &px, &py, &hm))) {
        cx = ox + p.x - sx + px;
        cy = oy + p.y + hm.top;
    } else {
        cx = ox + p.x - sx;
        cy = oy + p.y;
    }
    return true;
}

// ------------------------------------------------------------ selection --

void AiPanelView::BeginRichFrame(const AiPanelViewModel& vm) {
    if (!rich_) rich_ = std::make_unique<RichCache>();
    RichCache& rc = *rich_;
    rc.spots.clear();
    rc.ids.clear();
    rc.ordinal.clear();
    rc.ids.reserve(vm.blocks.size());
    for (size_t i = 0; i < vm.blocks.size(); ++i) {
        rc.ids.push_back(vm.blocks[i].id);
        rc.ordinal[vm.blocks[i].id] = static_cast<int>(i);
    }
    rc.kinds.clear();
    for (const AiBlockView& b : vm.blocks) rc.kinds.push_back(b.kind);
    for (auto& kv : rc.docs) kv.second.used = false;
}

void AiPanelView::EndRichFrame() {
    RichCache& rc = *rich_;
    for (auto it = rc.docs.begin(); it != rc.docs.end();) {
        if (!it->second.used) it = rc.docs.erase(it);
        else ++it;
    }
    // A selection in blocks that went (new chat, a retried turn) goes too.
    if (rc.active && (!rc.docs.count(rc.anchor.id) || !rc.docs.count(rc.focus.id))) {
        rc.active = false;
        rc.bar = false;
        rc.dragging = false;
    }
    for (auto it = rc.box_scroll.begin(); it != rc.box_scroll.end();) {
        if (!rc.docs.count(static_cast<uint32_t>(it->first >> 16))) it = rc.box_scroll.erase(it);
        else ++it;
    }
}

bool AiPanelView::RichPosAt(float x, float y, bool strict, RichCache::Pos& out) const {
    if (!rich_ || rich_->spots.empty()) return false;
    const RichCache& rc = *rich_;
    int best = -1;
    float best_d = 1.0e30f;
    for (size_t i = 0; i < rc.spots.size(); ++i) {
        const D2D1_RECT_F& r = rc.spots[i].rect;
        const float dy = y < r.top ? r.top - y : y > r.bottom ? y - r.bottom : 0.0f;
        const float dx = x < r.left ? r.left - x : x > r.right ? x - r.right : 0.0f;
        const float d = dy * 4.0f + dx;
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    if (best < 0 || (strict && best_d > 0.0f)) return false;
    const AiRichSpot& spot = rc.spots[static_cast<size_t>(best)];
    auto it = rc.docs.find(spot.id);
    if (it == rc.docs.end() || spot.piece >= static_cast<int>(it->second.pieces.size())) return false;
    const AiRichPiece& p = it->second.pieces[static_cast<size_t>(spot.piece)];
    if (!p.layout) return false;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    p.layout->HitTestPoint(x - spot.ox, y - spot.oy, &trailing, &inside, &m);
    uint32_t pos = m.textPosition + (trailing ? m.length : 0);
    pos = (std::min)(pos, static_cast<uint32_t>(p.text.size()));
    out = {spot.id, spot.piece, pos};
    return true;
}

bool AiPanelView::OverText(float x, float y) const {
    if (!In(conversation_rect_, x, y)) return false;
    RichCache::Pos p;
    return RichPosAt(x, y, true, p);
}

const AiRichPiece* AiPanelView::PieceOf(const RichCache::Pos& p) const {
    if (!rich_) return nullptr;
    auto it = rich_->docs.find(p.id);
    if (it == rich_->docs.end() || p.piece < 0 || p.piece >= static_cast<int>(it->second.pieces.size())) return nullptr;
    return &it->second.pieces[static_cast<size_t>(p.piece)];
}

void AiPanelView::UnitAround(const RichCache::Pos& p, int unit, RichCache::Pos& from, RichCache::Pos& to) const {
    from = to = p;
    const AiRichPiece* piece = PieceOf(p);
    if (!piece) return;
    const std::wstring& t = piece->text;
    const uint32_t n = static_cast<uint32_t>(t.size());
    if (unit >= 3) {
        from.offset = 0;
        to.offset = n;
        return;
    }
    if (n == 0) return;
    uint32_t at = (std::min)(p.offset, n - 1);
    // On a space between words, the word before it (like a double click past a line's end).
    if (at > 0 && CharClass(t[at]) == 0 && CharClass(t[at - 1]) != 0) --at;
    const int cls = CharClass(t[at]);
    uint32_t a = at, b = at + 1;
    while (a > 0 && CharClass(t[a - 1]) == cls) --a;
    while (b < n && CharClass(t[b]) == cls) ++b;
    from.offset = a;
    to.offset = b;
}

bool AiPanelView::BeginSelect(float x, float y, int clicks, bool extend) {
    if (!rich_) return false;
    RichCache& rc = *rich_;
    RichCache::Pos p;
    rc.bar = false;
    if (!RichPosAt(x, y, false, p)) {
        rc.active = false;
        return false;
    }
    if (extend && rc.active) {
        rc.focus = p;
        rc.unit = 1;
        rc.dragging = true;
        return true;
    }
    rc.unit = std::clamp(clicks, 1, 3);
    if (rc.unit == 1) {
        rc.anchor = rc.focus = p;
    } else {
        UnitAround(p, rc.unit, rc.unit_from, rc.unit_to);
        rc.anchor = rc.unit_from;
        rc.focus = rc.unit_to;
    }
    rc.active = true;
    rc.dragging = true;
    follow_ = false;   // streaming must not move the text under the pointer
    return true;
}

void AiPanelView::DragSelect(float x, float y) {
    if (!rich_ || !rich_->dragging) return;
    RichCache& rc = *rich_;
    RichCache::Pos p;
    if (!RichPosAt(x, y, false, p)) return;
    if (rc.unit == 1) {
        rc.focus = p;
        return;
    }
    RichCache::Pos a, b;
    UnitAround(p, rc.unit, a, b);
    if (ComparePos(a, rc.unit_from) < 0) {
        rc.anchor = rc.unit_to;
        rc.focus = a;
    } else {
        rc.anchor = rc.unit_from;
        rc.focus = ComparePos(b, rc.unit_to) > 0 ? b : rc.unit_to;
    }
}

void AiPanelView::EndSelect() {
    if (!rich_) return;
    RichCache& rc = *rich_;
    rc.dragging = false;
    // A bare click keeps its spot as a caret: Shift+click extends from it and
    // Ctrl+A takes its answer. Only chosen text gets the bar.
    rc.bar = rc.active && ComparePos(rc.anchor, rc.focus) != 0;
    if (rc.bar) {
        // 在 Pulse 中搜索 only for a short, one-line choice.
        const std::wstring text = SelectionText(false);
        rc.bar_search_on = !text.empty() && text.size() <= 80 && text.find(L'\n') == std::wstring::npos;
    }
}

bool AiPanelView::Selecting() const noexcept { return rich_ && rich_->dragging; }

bool AiPanelView::HasSelection() const {
    return rich_ && rich_->active && ComparePos(rich_->anchor, rich_->focus) != 0;
}

void AiPanelView::ClearSelection() {
    if (!rich_) return;
    rich_->active = false;
    rich_->dragging = false;
    rich_->bar = false;
}

void AiPanelView::HideSelectionBar() {
    if (rich_) rich_->bar = false;
}

int AiPanelView::RichBlockAt(float x, float y) const {
    RichCache::Pos p;
    if (!RichPosAt(x, y, false, p)) return -1;
    auto it = rich_->ordinal.find(p.id);
    return it == rich_->ordinal.end() ? -1 : it->second;
}

int AiPanelView::CaretBlock() const {
    if (!rich_ || !rich_->active) return -1;
    auto it = rich_->ordinal.find(rich_->focus.id);
    return it == rich_->ordinal.end() ? -1 : it->second;
}

int AiPanelView::SelectionBlock() const {
    if (!HasSelection()) return -1;
    auto it = rich_->ordinal.find(rich_->focus.id);
    return it == rich_->ordinal.end() ? -1 : it->second;
}

bool AiPanelView::SelectAnswer(int block) {
    if (!rich_) return false;
    RichCache& rc = *rich_;
    const int n = static_cast<int>(rc.ids.size());
    if (n == 0) return false;
    if (block < 0 || block >= n) {
        // The last answer.
        block = -1;
        for (int i = n - 1; i >= 0; --i)
            if (rc.kinds[static_cast<size_t>(i)] == AiBlockKind::Text) { block = i; break; }
        if (block < 0) return false;
    }
    int first = -1, last = -1;
    if (rc.kinds[static_cast<size_t>(block)] == AiBlockKind::User) {
        first = last = block;
    } else {
        int lo = block, hi = block;
        while (lo > 0 && rc.kinds[static_cast<size_t>(lo - 1)] != AiBlockKind::User) --lo;
        while (hi + 1 < n && rc.kinds[static_cast<size_t>(hi + 1)] != AiBlockKind::User) ++hi;
        for (int i = lo; i <= hi; ++i) {
            if (rc.kinds[static_cast<size_t>(i)] != AiBlockKind::Text) continue;
            auto it = rc.docs.find(rc.ids[static_cast<size_t>(i)]);
            if (it == rc.docs.end() || it->second.pieces.empty()) continue;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return false;
    const AiRichDoc& d1 = rc.docs[rc.ids[static_cast<size_t>(last)]];
    if (d1.pieces.empty()) return false;
    rc.anchor = {rc.ids[static_cast<size_t>(first)], 0, 0};
    rc.focus = {rc.ids[static_cast<size_t>(last)], static_cast<int>(d1.pieces.size()) - 1,
                static_cast<uint32_t>(d1.pieces.back().text.size())};
    rc.active = true;
    rc.dragging = false;
    rc.bar = false;
    return true;
}

std::wstring AiPanelView::SelectionText(bool markdown) const {
    std::wstring out;
    if (!HasSelection()) return out;
    const RichCache& rc = *rich_;
    RichCache::Pos s0 = rc.anchor, s1 = rc.focus;
    if (ComparePos(s1, s0) < 0) std::swap(s0, s1);
    const auto i0 = rc.ordinal.find(s0.id), i1 = rc.ordinal.find(s1.id);
    if (i0 == rc.ordinal.end() || i1 == rc.ordinal.end()) return out;
    const int o0 = i0->second, o1 = i1->second;
    const auto doc_pos = [](const AiRichDoc& d, const RichCache::Pos& p) {
        const AiRichPiece& piece = d.pieces[static_cast<size_t>(std::clamp(p.piece, 0, static_cast<int>(d.pieces.size()) - 1))];
        const uint32_t off = (std::min)(p.offset, static_cast<uint32_t>(piece.to_doc.size()) - 1);
        return airich::Pos{(std::max)(piece.md, 0), piece.to_doc[off]};
    };
    for (int o = o0; o <= o1; ++o) {
        auto it = rc.docs.find(rc.ids[static_cast<size_t>(o)]);
        if (it == rc.docs.end() || it->second.pieces.empty()) continue;
        const AiRichDoc& d = it->second;
        std::wstring part;
        if (d.user) {
            // The quote piece and the message, a line apart.
            const int n = static_cast<int>(d.pieces.size());
            const int p0 = o == o0 ? std::clamp(s0.piece, 0, n - 1) : 0;
            const int p1 = o == o1 ? std::clamp(s1.piece, 0, n - 1) : n - 1;
            for (int pi = p0; pi <= p1; ++pi) {
                const std::wstring& t = d.pieces[static_cast<size_t>(pi)].text;
                const uint32_t size = static_cast<uint32_t>(t.size());
                const uint32_t a = (o == o0 && pi == p0) ? (std::min)(s0.offset, size) : 0;
                const uint32_t b = (o == o1 && pi == p1) ? (std::min)(s1.offset, size) : size;
                if (b <= a) continue;
                if (!part.empty()) part += L'\n';
                part.append(t, a, b - a);
            }
        } else {
            airich::Pos from{0, 0};
            airich::Pos to{static_cast<int>(d.doc.blocks.size()) - 1,
                           d.doc.blocks.empty() ? 0u : static_cast<uint32_t>(d.doc.blocks.back().text.size())};
            if (o == o0) from = doc_pos(d, s0);
            if (o == o1) to = doc_pos(d, s1);
            part = airich::CopyRange(d.doc, from, to, markdown);
        }
        if (part.empty()) continue;
        if (!out.empty()) out += L"\n\n";
        out += part;
    }
    return out;
}

float AiPanelView::SelectScrollSpeed(float y) const {
    const float edge = 12.0f * bar_scale_;
    if (y < conversation_rect_.top + edge) return -(std::min)(40.0f * bar_scale_, (conversation_rect_.top + edge - y) * 0.5f + 2.0f);
    if (y > conversation_rect_.bottom - edge)
        return (std::min)(40.0f * bar_scale_, (y - (conversation_rect_.bottom - edge)) * 0.5f + 2.0f);
    return 0.0f;
}

AiPanelView::RichTargetInfo AiPanelView::TargetOf(const AiPanelHit& hit) const {
    RichTargetInfo info;
    if (!rich_ || hit.index < 0) return info;
    const int block = hit.index / kTargetsPerBlock, k = hit.index % kTargetsPerBlock;
    if (block >= static_cast<int>(rich_->ids.size())) return info;
    auto it = rich_->docs.find(rich_->ids[static_cast<size_t>(block)]);
    if (it == rich_->docs.end() || k >= static_cast<int>(it->second.targets.size())) return info;
    const AiRichTarget& t = it->second.targets[static_cast<size_t>(k)];
    info.part = t.part;
    info.text = t.text;
    return info;
}

void AiPanelView::MarkCopied(const AiPanelHit& hit, uint64_t now_ms) {
    if (!rich_ || hit.index < 0) return;
    const int block = hit.index / kTargetsPerBlock;
    if (block >= static_cast<int>(rich_->ids.size())) return;
    rich_->copied_id = rich_->ids[static_cast<size_t>(block)];
    rich_->copied_k = hit.index % kTargetsPerBlock;
    rich_->copied_at = now_ms;
}

bool AiPanelView::ScrollBoxAt(float x, float y, float dx) {
    if (!rich_ || !In(conversation_rect_, x, y)) return false;
    for (const AiRichSpot& spot : rich_->spots) {
        auto it = rich_->docs.find(spot.id);
        if (it == rich_->docs.end()) continue;
        const AiRichDoc& d = it->second;
        const AiRichPiece& p = d.pieces[static_cast<size_t>(spot.piece)];
        if (p.box < 0) continue;
        const AiRichBox& box = d.boxes[static_cast<size_t>(p.box)];
        // The box's frame on screen: the spot's origin less the piece's offset.
        const float oy = spot.oy - p.y;
        const float sx = BoxScroll(spot.id, p.box);
        const float ox = spot.ox - p.x + sx;
        if (!In(D2D1::RectF(ox + box.r.left, oy + box.r.top, ox + box.r.right, oy + box.r.bottom), x, y)) continue;
        const float max_x = (std::max)(0.0f, box.content_w - (box.view_right - box.view_left));
        if (max_x <= 0.0f) return false;
        float& v = rich_->box_scroll[(static_cast<uint64_t>(spot.id) << 16) | static_cast<uint64_t>(p.box & 0xFFFF)];
        // Over a box the wheel is its own, even at an end (the page stays put).
        v = std::clamp(v + dx, 0.0f, max_x);
        return true;
    }
    return false;
}

// ------------------------------------------------- the mini toolbar ------

void AiPanelView::DrawSelectionBar(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float s) {
    if (!rich_) return;
    RichCache& rc = *rich_;
    rc.bar_rect = rc.bar_copy = rc.bar_quote = rc.bar_search = {};
    if (!rc.bar || !HasSelection()) return;
    // The selection's first and last line on screen.
    D2D1_RECT_F first{}, last{};
    bool any = false;
    for (const AiRichSpot& spot : rc.spots) {
        auto it = rc.docs.find(spot.id);
        if (it == rc.docs.end()) continue;
        const AiRichPiece& p = it->second.pieces[static_cast<size_t>(spot.piece)];
        uint32_t a = 0, b = 0;
        if (!SelectionIn(spot.id, spot.piece, static_cast<uint32_t>(p.text.size()), a, b)) continue;
        for (const auto& m : RangeRects(p.layout.Get(), a, b - a)) {
            D2D1_RECT_F r = D2D1::RectF(spot.ox + m.left, spot.oy + m.top, spot.ox + m.left + m.width, spot.oy + m.top + m.height);
            r.left = (std::max)(r.left, spot.rect.left);
            r.right = (std::min)(r.right, spot.rect.right);
            if (r.bottom < conversation_rect_.top || r.top > conversation_rect_.bottom) continue;
            if (!any) first = r;
            last = r;
            any = true;
        }
    }
    if (!any) return;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    const std::wstring copy = L10n(l10n::StringId::Copy), quote = L10n(l10n::StringId::AiSelQuote),
                       search = L10n(l10n::StringId::AiSelSearch);
    const auto bw = [&](const std::wstring& t) { return (9.0f + 14.0f + 6.0f + 9.0f) * s + c.Measure(t, formats_.small125); };
    const float w_copy = bw(copy), w_quote = bw(quote), w_search = rc.bar_search_on ? bw(search) : 0.0f;
    const float w = 3.0f * s * 2 + w_copy + 2.0f * s + w_quote + (rc.bar_search_on ? (2.0f + 1.0f + 2.0f) * s + 2.0f * s + w_search : 0.0f);
    const float h = 34.0f * s;
    float x = std::round((first.left + first.right) * 0.5f - w * 0.5f);
    x = std::clamp(x, panel_rect_.left + 8.0f * s, (std::max)(panel_rect_.left + 8.0f * s, panel_rect_.right - 8.0f * s - w));
    float y = first.top - 6.0f * s - h;
    if (y < conversation_rect_.top + 4.0f * s) y = last.bottom + 6.0f * s;
    if (y + h > conversation_rect_.bottom - 4.0f * s) y = conversation_rect_.top + 4.0f * s;
    y = std::round(y);
    const D2D1_RECT_F box = D2D1::RectF(x, y, x + w, y + h);
    rc.bar_rect = box;
    c.Shadow(box, 9.0f * s, 24.0f * s, 8.0f * s, pal.shadow);
    c.Fill(box, 9.0f * s, pal.card);
    c.Stroke(box, 9.0f * s, pal.line);
    float bx = box.left + 3.0f * s;
    const auto button = [&](const std::wstring& t, AiIcon icon, float bwid, AiPanelPart part, D2D1_RECT_F& out) {
        out = D2D1::RectF(bx, box.top + 3.0f * s, bx + bwid, box.bottom - 3.0f * s);
        const bool hot = vm.hover.part == part;
        if (hot) c.Fill(out, 6.0f * s, pal.hover);
        c.Icon(icon, out.left + 9.0f * s, (out.top + out.bottom) * 0.5f - 7.0f * s, 14.0f * s, pal.text2);
        c.Text(t, formats_.small125, D2D1::RectF(out.left + 29.0f * s, out.top, out.right, out.bottom), pal.text);
        conv_hits_.push_back({out, {part, 0}});
        bx += bwid + 2.0f * s;
    };
    button(copy, AiIcon::Copy, w_copy, AiPanelPart::SelCopy, rc.bar_copy);
    button(quote, AiIcon::Quote, w_quote, AiPanelPart::SelQuote, rc.bar_quote);
    if (rc.bar_search_on) {
        c.Fill(D2D1::RectF(bx, (box.top + box.bottom) * 0.5f - 8.0f * s, bx + 1.0f * s, (box.top + box.bottom) * 0.5f + 8.0f * s),
               0.0f, pal.line);
        bx += 3.0f * s;
        button(search, AiIcon::Search, w_search, AiPanelPart::SelSearch, rc.bar_search);
    }
    conv_hits_.push_back({box, {AiPanelPart::Panel, -1}});   // its gaps swallow the click
    // The bar is pushed after the conversation's own hits: HitTest reads them
    // back to front, so its buttons win. Move the swallow rect before them.
    std::rotate(conv_hits_.end() - (rc.bar_search_on ? 4 : 3), conv_hits_.end() - 1, conv_hits_.end());
}

// ------------------------------------------------- the quote chip --------

float AiPanelView::MeasureQuote(Compositor* compositor, const AiPanelViewModel& vm, float inner_w, float s) {
    if (!rich_) rich_ = std::make_unique<RichCache>();
    RichCache& rc = *rich_;
    if (vm.quote.empty()) {
        rc.quote_h = 0.0f;
        rc.quote_text.clear();
        return 0.0f;
    }
    const float text_w = inner_w - (13.0f + 8.0f + 20.0f + 6.0f) * s;
    if (rc.quote_text != vm.quote || rc.quote_w != text_w) {
        Ctx c{compositor, compositor->Dc(), brush_, s};
        const float lh = 18.0f * s;
        const float th = c.MeasureWrapped(vm.quote, formats_.small125, text_w, lh);
        const int lines = std::clamp(static_cast<int>(std::ceil(th / lh - 0.25f)), 1, 2);
        rc.quote_text = vm.quote;
        rc.quote_w = text_w;
        // padding 6 / 6, margin-bottom 8 (less the -2 it pulls out).
        rc.quote_h = std::round((6.0f + 6.0f + 6.0f) * s + lines * lh);
    }
    return rc.quote_h;
}

void AiPanelView::DrawQuoteChip(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float left,
                                float right, float& y, float s) {
    quote_remove_ = {};
    if (vm.quote.empty() || !rich_ || rich_->quote_h <= 0.0f) return;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    const float h = rich_->quote_h - 8.0f * s;
    const D2D1_RECT_F r = D2D1::RectF(left - 2.0f * s, y - 2.0f * s, right + 2.0f * s, y - 2.0f * s + h);
    c.Fill(r, 9.0f * s, pal.pane);
    c.Fill(D2D1::RectF(r.left, r.top + 3.0f * s, r.left + 3.0f * s, r.bottom - 3.0f * s), 1.5f * s, pal.accent);
    const float tx = r.left + 13.0f * s;
    const float tr = r.right - 6.0f * s - 20.0f * s - 8.0f * s;
    auto* f = compositor->DwriteFactory();
    wrl::ComPtr<IDWriteTextLayout> layout;
    const float lh = 18.0f * s;
    if (f && SUCCEEDED(f->CreateTextLayout(vm.quote.c_str(), static_cast<UINT32>(vm.quote.size()), formats_.small125,
                                           (std::max)(1.0f, tr - tx), h - 12.0f * s, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lh, lh * 0.78f);
        wrl::ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(f->CreateEllipsisTrimmingSign(formats_.small125, &sign))) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trim, sign.Get());
        }
        compositor->DrawTextLayout(D2D1::Point2F(tx, r.top + 6.0f * s), layout.Get(), c.B(pal.text2));
    }
    quote_remove_ = D2D1::RectF(r.right - 6.0f * s - 20.0f * s, r.top + 5.0f * s, r.right - 6.0f * s, r.top + 25.0f * s);
    const bool hot = vm.hover.part == AiPanelPart::QuoteRemove;
    if (hot) c.Fill(quote_remove_, 5.0f * s, pal.hover);
    c.Icon(AiIcon::Close, quote_remove_.left + 4.0f * s, quote_remove_.top + 4.0f * s, 12.0f * s, hot ? pal.text : pal.text3);
    y += rich_->quote_h;
}

} // namespace pulse::ui
