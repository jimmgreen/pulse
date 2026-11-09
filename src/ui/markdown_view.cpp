// markdown_view.cpp — see markdown_view.h.
#include "markdown_view.h"
#include "math_formula.h"
#include "syntax_highlight.h"
#include "thumbnail_cache.h"
#include "typography.h"
#include "../common/localization.h"
#include "../ipc/preview_protocol.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <cwctype>
#include <initializer_list>

namespace pulse::ui {
namespace {

// Words as a reader counts them: each CJK character is one, a run of letters
// or digits is another.
uint32_t CountWords(const std::wstring& text) {
    uint32_t words = 0;
    bool in_word = false;
    for (const wchar_t c : text) {
        const bool cjk = (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
                         (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
                         (c >= 0xF900 && c <= 0xFAFF);
        if (cjk) {
            ++words;
            in_word = false;
        } else if (iswalnum(c) || c == L'\'' || c == L'_') {
            if (!in_word) ++words;
            in_word = true;
        } else {
            in_word = false;
        }
    }
    return words;
}

template <class T> using WrlPtr = Microsoft::WRL::ComPtr<T>;

enum : uint32_t {
    kBold = 1, kItalic = 2, kCode = 4, kStrike = 8, kLink = 16, kImage = 32,
    kUnderline = 64, kMath = 128, kDisplayMath = 256
};

std::wstring Unescape(std::wstring_view field) {
    std::wstring out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == L'\\' && i + 1 < field.size()) {
            const wchar_t n = field[++i];
            out += n == L't' ? L'\t' : n == L'n' ? L'\n' : n;
        } else {
            out += field[i];
        }
    }
    return out;
}

std::wstring PercentDecode(std::wstring_view text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'%' && i + 2 < text.size() && std::iswxdigit(text[i + 1]) && std::iswxdigit(text[i + 2])) {
            out += static_cast<wchar_t>(std::wcstoul(std::wstring(text.substr(i + 1, 2)).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

uint32_t ToU32(std::wstring_view v) {
    uint32_t n = 0;
    for (wchar_t c : v) { if (c < L'0' || c > L'9') break; n = n * 10 + static_cast<uint32_t>(c - L'0'); }
    return n;
}

std::vector<std::wstring_view> Split(std::wstring_view line, wchar_t separator) {
    std::vector<std::wstring_view> fields;
    size_t start = 0;
    while (true) {
        const size_t at = line.find(separator, start);
        if (at == std::wstring_view::npos) { fields.push_back(line.substr(start)); break; }
        fields.push_back(line.substr(start, at - start));
        start = at + 1;
    }
    return fields;
}

// Fenced code info string -> an extension HighlightSyntax knows.
std::wstring FenceExtension(std::wstring lang) {
    for (auto& c : lang) c = static_cast<wchar_t>(std::towlower(c));
    static const struct { const wchar_t* name; const wchar_t* ext; } kAlias[] = {
        {L"c++", L".cpp"}, {L"cpp", L".cpp"}, {L"cxx", L".cpp"}, {L"c", L".c"}, {L"h", L".h"},
        {L"c#", L".cs"}, {L"cs", L".cs"}, {L"csharp", L".cs"}, {L"java", L".java"},
        {L"js", L".js"}, {L"javascript", L".js"}, {L"jsx", L".jsx"}, {L"ts", L".ts"},
        {L"typescript", L".ts"}, {L"tsx", L".tsx"}, {L"py", L".py"}, {L"python", L".py"},
        {L"sh", L".sh"}, {L"bash", L".sh"}, {L"shell", L".sh"}, {L"zsh", L".sh"}, {L"console", L".sh"},
        {L"ps", L".ps1"}, {L"ps1", L".ps1"}, {L"powershell", L".ps1"}, {L"pwsh", L".ps1"},
        {L"bat", L".bat"}, {L"cmd", L".bat"}, {L"batch", L".bat"}, {L"json", L".json"},
        {L"jsonc", L".jsonc"}, {L"xml", L".xml"}, {L"html", L".html"}, {L"svg", L".svg"},
        {L"yaml", L".yaml"}, {L"yml", L".yaml"}, {L"toml", L".toml"}, {L"ini", L".ini"},
        {L"css", L".css"}, {L"scss", L".scss"}, {L"sql", L".sql"}, {L"rust", L".rs"}, {L"rs", L".rs"},
        {L"go", L".go"}, {L"golang", L".go"}, {L"php", L".php"}, {L"cmake", L".cmake"},
        {L"qml", L".qml"}, {L"md", L".md"}, {L"markdown", L".md"}, {L"log", L".log"},
    };
    for (const auto& a : kAlias) if (lang == a.name) return a.ext;
    return lang.empty() ? std::wstring() : L"." + lang;
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

float HeadingSize(int level) {
    static constexpr float kSizes[] = {26.0f, 21.0f, 17.5f, 15.5f, 14.5f, 14.0f};
    return kSizes[std::clamp(level, 1, 6) - 1];
}

} // namespace

// Notebook markers: l<label> code cell, n<label> cell output (no box).
static bool NotebookMarker(const std::wstring& marker) {
    return !marker.empty() && (marker[0] == L'l' || marker[0] == L'n');
}
static bool NotebookOutput(const std::wstring& marker) { return !marker.empty() && marker[0] == L'n'; }

// ---------------------------------------------------------------- model

MarkdownView::~MarkdownView() {
    if (image_session_) image_session_->active = false;
}

void MarkdownView::Clear() {
    EndScrollbarDrag();
    payload_identity_.reset();
    ++text_revision_;
    image_metadata_.Reset();
    if (image_session_) image_session_->active = false;
    image_session_.reset();
    parsed_ = false;
    decltype(payload_){}.swap(payload_);
    shared_payload_.reset();
    path_.clear();
    decltype(source_){}.swap(source_);
    decltype(plain_){}.swap(plain_);
    decltype(blocks_){}.swap(blocks_);
    decltype(tables_){}.swap(tables_);
    gutter_ = 0;
    notebook_ = false;
    kernel_.clear();
    cells_ = 0;
    format_.clear();
    doc_title_.clear();
    doc_author_.clear();
    words_ = 0;
    headings_ = 0;
    decltype(all_blocks_){}.swap(all_blocks_);
    decltype(all_tables_){}.swap(all_tables_);
    decltype(sections_){}.swap(sections_);
    decltype(toc_){}.swap(toc_);
    section_ = 0;
    pending_block_ = -1;
    pending_end_ = pending_reveal_ = false;
    overscroll_ = 0.0f;
    footer_y_ = -1.0f;
    footer_rect_ = {};
    toc_rect_ = {};
    toc_scroll_ = 0.0f;
    toc_follow_ = true;
    scroll_ = 0.0f;
    scroll_x_ = 0.0f;
    relayout_ = true;
    doc_height_ = 0.0f;
}

bool MarkdownView::SetPayload(const std::shared_ptr<const std::wstring>& payload, const std::wstring& file_path) {
    if (payload && payload_identity_.lock() == payload && parsed_ && file_path == path_) {
        return true;
    }
    if (!payload) return false;
    const bool accepted = SetPayloadImpl(*payload, file_path, payload);
    if (accepted) {
        StorePayload(*payload, payload);
        payload_identity_ = payload;
    }
    return accepted;
}

bool MarkdownView::SetPayload(const std::wstring& payload, const std::wstring& file_path) {
    return SetPayloadImpl(payload, file_path, {});
}

bool MarkdownView::SetPayloadImpl(const std::wstring& payload, const std::wstring& file_path,
                           std::shared_ptr<const std::wstring> owner) {
    payload_identity_.reset();
    if (parsed_ && payload == Payload() && file_path == path_) return true;
    Clear();
    if (payload.rfind(L"PULSEMD\t1\n", 0) != 0) return false;
    image_session_ = std::make_shared<DocumentImageSession>();
    StorePayload(payload, std::move(owner));
    path_ = file_path;
    const size_t slash = file_path.find_last_of(L"\\/");
    base_dir_ = slash == std::wstring::npos ? std::wstring() : file_path.substr(0, slash);

    const std::wstring_view all(Payload());
    size_t pos = all.find(L'\n') + 1;
    int table = -1, row = -1, col = 0;
    size_t math_count = 0, math_chars = 0;
    while (pos < all.size()) {
        size_t end = all.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = all.size();
        const std::wstring_view line = all.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;
        const auto f = Split(line, L'\t');
        if (f[0] == L"S" && f.size() >= 2) {
            source_ = Unescape(f[1]);
        } else if (f[0] == L"G" && f.size() >= 2) {
            gutter_ = static_cast<int>((std::min)(ToU32(f[1]), 8u));
        } else if (f[0] == L"P" && f.size() >= 2) {
            Section section;
            section.name = Unescape(f[1]);
            section.begin = blocks_.size();
            sections_.push_back(std::move(section));
        } else if (f[0] == L"C" && f.size() >= 5) {
            TocEntry entry;
            entry.level = static_cast<int>((std::min)(ToU32(f[1]), 6u));
            entry.section = static_cast<int>(ToU32(f[2]));
            entry.block = static_cast<int>(ToU32(f[3]));
            entry.title = Unescape(f[4]);
            toc_.push_back(std::move(entry));
        } else if (f[0] == L"M" && f.size() >= 2) {
            format_.assign(f[1]);
            if (format_ == L"docx" && f.size() >= 3) words_ = ToU32(f[2]);
            if (f.size() >= 3 && format_ != L"docx") doc_title_ = Unescape(f[2]);
            if (f.size() >= 4) doc_author_ = Unescape(f[3]);
        } else if (f[0] == L"I" && f.size() >= 3) {
            notebook_ = true;
            kernel_ = Unescape(f[1]);
            cells_ = ToU32(f[2]);
        } else if (f[0] == L"T" && f.size() >= 4) {
            Table t;
            t.columns = static_cast<int>((std::min)(ToU32(f[1]), 64u));
            t.quote = static_cast<int>(ToU32(f[2]));
            t.indent = static_cast<int>(ToU32(f[3]));
            tables_.push_back(std::move(t));
            table = static_cast<int>(tables_.size()) - 1;
            row = -1;
        } else if (f[0] == L"R") {
            if (table >= 0) { ++row; col = 0; tables_[table].rows = row + 1; }
        } else if (f[0] == L"E") {
            table = -1;
        } else if (f[0] == L"B" && f.size() >= 8) {
            Block b;
            b.kind = f[1].empty() ? L'p' : f[1][0];
            b.arg = Unescape(f[2]);
            b.quote = static_cast<int>((std::min)(ToU32(f[3]), 16u));
            b.indent = static_cast<int>((std::min)(ToU32(f[4]), 16u));
            b.marker.assign(f[5]);
            b.text = Unescape(f[6]);
            for (const auto run_text : Split(f[7], L';')) {
                if (run_text.empty()) continue;
                const auto parts = Split(run_text, L',');
                if (parts.size() < 3) continue;
                Run run;
                run.start = ToU32(parts[0]);
                run.length = ToU32(parts[1]);
                run.flags = ToU32(parts[2]);
                if (parts.size() >= 4) run.target = PercentDecode(Unescape(parts[3]));
                if (run.start > b.text.size()) continue;
                run.length = (std::min)(run.length, static_cast<uint32_t>(b.text.size()) - run.start);
                if (run.flags & (kMath | kDisplayMath)) {
                    // Bound layout work across the document as well as inside each formula.
                    if (math_count >= 256 || run.length > 4096 || math_chars + run.length > 65536)
                        run.flags &= ~(kMath | kDisplayMath);
                    else {
                        ++math_count;
                        math_chars += run.length;
                    }
                }
                b.runs.push_back(std::move(run));
            }
            if (b.kind == L't') {
                if (table < 0 || row < 0) continue;
                b.table = table;
                b.row = row;
                b.col = col++;
                b.header = b.marker == L"h";
                b.marker.clear();
                if (b.col >= tables_[table].columns) continue;  // ragged row
                tables_[table].cells.push_back(static_cast<int>(blocks_.size()));
            }
            b.image = b.kind == L'p' && b.runs.size() == 1 && (b.runs[0].flags & kImage) &&
                      b.runs[0].start == 0 && b.runs[0].length == b.text.size();

            blocks_.push_back(std::move(b));
        }
    }
    for (size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        if (i) plain_ += b.table >= 0 && b.col > 0 ? L'\t' : L'\n';
        b.plain_start = static_cast<uint32_t>(plain_.size());
        plain_ += b.text;
    }
    parsed_ = true;
    for (const Block& b : blocks_)
        if (b.kind == L'h') ++headings_;
    if (format_.empty()) words_ = CountWords(plain_);
    if (!sections_.empty()) {
        if (sections_[0].begin > 0) sections_.insert(sections_.begin(), Section{});
        for (size_t i = 0; i < sections_.size(); ++i)
            sections_[i].end = i + 1 < sections_.size() ? sections_[i + 1].begin : blocks_.size();
        toc_.erase(std::remove_if(toc_.begin(), toc_.end(),
                                  [&](const TocEntry& e) { return e.section >= SectionCount(); }),
                   toc_.end());
        all_blocks_ = std::move(blocks_);
        all_tables_ = std::move(tables_);
        blocks_.clear();
        tables_.clear();
        ShowSection(0);
    } else {
        // Plain Markdown: a contents list from h1-h3 once there are three of them.
        toc_.clear();
        int top = 6;
        for (const Block& b : blocks_)
            if (b.kind == L'h') top = (std::min)(top, static_cast<int>(ToU32(b.arg)));
        for (size_t i = 0; i < blocks_.size(); ++i) {
            const Block& b = blocks_[i];
            const int level = static_cast<int>(ToU32(b.arg));
            if (b.kind != L'h' || level > top + 2 || b.text.empty()) continue;
            TocEntry entry;
            entry.level = (std::max)(0, level - top);
            entry.block = static_cast<int>(i);
            entry.title = b.text;
            toc_.push_back(std::move(entry));
        }
        if (toc_.size() < 3) toc_.clear();
    }
    return true;
}

bool MarkdownView::ShowSection(int index, int block, bool at_end) {
    if (sections_.empty() || index < 0 || index >= SectionCount()) return false;
    EndScrollbarDrag();
    // Keep the picture sizes learnt while the old section was on screen.
    if (section_ >= 0 && section_ < SectionCount()) {
        const Section& old = sections_[section_];
        if (blocks_.size() == old.end - old.begin)
            for (size_t i = 0; i < blocks_.size(); ++i)
                if (blocks_[i].image) all_blocks_[old.begin + i].image_aspect = blocks_[i].image_aspect;
    }
    const Section& section = sections_[index];
    blocks_.assign(all_blocks_.begin() + static_cast<std::ptrdiff_t>(section.begin),
                   all_blocks_.begin() + static_cast<std::ptrdiff_t>(section.end));
    tables_.clear();
    std::vector<int> remap(all_tables_.size(), -1);
    for (Block& b : blocks_) {
        b.layout.Reset();
        if (b.table < 0 || b.table >= static_cast<int>(remap.size())) continue;
        int& mapped = remap[b.table];
        if (mapped < 0) {
            Table copy = all_tables_[b.table];
            for (int& cell : copy.cells) cell -= static_cast<int>(section.begin);
            tables_.push_back(std::move(copy));
            mapped = static_cast<int>(tables_.size()) - 1;
        }
        b.table = mapped;
    }
    section_ = index;
    scroll_ = 0.0f;
    scroll_x_ = 0.0f;
    overscroll_ = 0.0f;
    doc_height_ = 0.0f;
    relayout_ = true;
    pending_end_ = at_end;
    pending_block_ = at_end ? -1 : (std::max)(0, block);
    pending_reveal_ = false;
    toc_follow_ = true;
    return true;
}

// ---------------------------------------------------------------- layout

bool MarkdownView::EnsureFormats(IDWriteFactory2* factory, float scale) {
    if (!factory) return false;
    if (factory == factory_ && scale == format_scale_ && body_) return true;
    factory_ = factory;
    format_scale_ = scale;
    body_.Reset(); code_.Reset(); marker_.Reset(); label_.Reset();
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 14.5f * scale}, &body_);
    typography::CreateTextFormat(factory, {typography::FontRole::Monospace, 13.0f * scale}, &code_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 14.5f * scale}, &marker_);
    typography::CreateTextFormat(factory, {typography::FontRole::Monospace, 11.5f * scale}, &label_);
    if (!body_ || !code_ || !marker_ || !label_) return false;
    label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    for (IDWriteTextFormat* f : {body_.Get(), code_.Get(), marker_.Get()}) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    body_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    code_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    marker_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    wchar_t family[128]{};
    mono_family_.clear();
    if (SUCCEEDED(code_->GetFontFamilyName(family, static_cast<UINT32>(std::size(family))))) mono_family_ = family;
    // Contents sidebar: one line per entry, ellipsis when it does not fit.
    toc_format_.Reset(); toc_sub_format_.Reset();
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.5f * scale}, &toc_format_);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.0f * scale}, &toc_sub_format_);
    for (IDWriteTextFormat* f : {toc_format_.Get(), toc_sub_format_.Get()}) {
        if (!f) continue;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        WrlPtr<IDWriteInlineObject> sign;
        factory->CreateEllipsisTrimmingSign(f, &sign);
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trimming, sign.Get());
    }
    // Book text reads in a serif face (mockup .read), when one is installed.
    serif_cjk_.clear();
    serif_latin_.clear();
    WrlPtr<IDWriteFontCollection> fonts;
    if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE)) && fonts) {
        auto find = [&](std::initializer_list<const wchar_t*> names) {
            for (const wchar_t* name : names) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) return std::wstring(name);
            }
            return std::wstring();
        };
        serif_cjk_ = find({L"Noto Serif SC", L"Source Han Serif SC", L"思源宋体", L"SimSun"});
        serif_latin_ = find({L"Georgia", L"Cambria", L"Times New Roman"});
    }
    relayout_ = true;
    return true;
}

void MarkdownView::EnsureBrushes(ID2D1DeviceContext* dc, bool dark) {
    if (brush_owner_ == dc && brush_ && brushes_dark_ == dark) return;
    brush_owner_ = dc;
    brushes_dark_ = dark;
    brush_.Reset(); link_brush_.Reset(); dim_brush_.Reset();
    for (auto& b : syntax_brushes_) b.Reset();
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_);
    dc->CreateSolidColorBrush(D2D1::ColorF(dark ? 0x60CDFF : 0x005FB8), &link_brush_);
    dc->CreateSolidColorBrush(dark ? D2D1::ColorF(0xFFFFFF, 0.55f) : D2D1::ColorF(0x000000, 0.55f), &dim_brush_);
    for (size_t i = 1; i < static_cast<size_t>(SyntaxToken::Count) && i < std::size(syntax_brushes_); ++i)
        dc->CreateSolidColorBrush(D2D1::ColorF(SyntaxTokenRgb(static_cast<SyntaxToken>(i), dark)), &syntax_brushes_[i]);
    relayout_ = true;  // drawing effects hold brushes
}

WrlPtr<IDWriteTextLayout> MarkdownView::MakeLayout(IDWriteFactory2* factory, const Block& b,
                                                   float width, bool wrap) {
    WrlPtr<IDWriteTextLayout> layout;
    const bool mono = b.kind == L'c' || b.kind == L'x';
    IDWriteTextFormat* format = mono ? code_.Get() : body_.Get();
    if (!format) return layout;
    if (FAILED(factory->CreateTextLayout(b.text.data(), static_cast<UINT32>(b.text.size()), format,
                                         (std::max)(1.0f, width), 1.0e6f, &layout)) || !layout)
        return layout;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(b.text.size())};
    const float base = mono ? 13.0f * scale_ : 14.5f * scale_;
    if (b.kind == L'h') {
        const int level = static_cast<int>(ToU32(b.arg));
        layout->SetFontSize(HeadingSize(level) * scale_, all);
        layout->SetFontWeight(level <= 2 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
    }
    if (b.header) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
    if (b.kind == L'c' && brush_owner_) {
        const std::wstring ext = FenceExtension(b.arg);
        if (!ext.empty())
            for (const SyntaxSpan& span : HighlightSyntax(ext, b.text)) {
                const auto index = static_cast<size_t>(span.token);
                if (index > 0 && index < std::size(syntax_brushes_) && syntax_brushes_[index])
                    layout->SetDrawingEffect(syntax_brushes_[index].Get(), {span.start, span.length});
            }
    }
    if (b.kind == L'x' && dim_brush_) layout->SetDrawingEffect(dim_brush_.Get(), all);
    if (format_ == L"epub" && (b.kind == L'p' || b.kind == L'h') && b.table < 0 && !b.image) {
        bool cjk = false;
        for (size_t i = 0; i < b.text.size() && i < 64 && !cjk; ++i) cjk = b.text[i] >= 0x3000 && b.text[i] <= 0x9FFF;
        const std::wstring& family = cjk ? serif_cjk_ : serif_latin_;
        if (!family.empty()) layout->SetFontFamilyName(family.c_str(), all);
        if (b.kind == L'p') {
            const float spacing = 14.5f * scale_ * 1.8f;
            layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spacing, spacing * 0.72f);
        }
    }
    for (const Run& run : b.runs) {
        const DWRITE_TEXT_RANGE range{run.start, run.length};
        if (run.flags & kBold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (run.flags & kItalic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (run.flags & kStrike) layout->SetStrikethrough(TRUE, range);
        if (run.flags & kUnderline) layout->SetUnderline(TRUE, range);
        if ((run.flags & kCode) && !mono_family_.empty()) {
            layout->SetFontFamilyName(mono_family_.c_str(), range);
            FLOAT size = base;
            layout->GetFontSize(run.start, &size);
            layout->SetFontSize(size * 0.9f, range);
        }
        if ((run.flags & kLink) && link_brush_) {
            layout->SetDrawingEffect(link_brush_.Get(), range);
            layout->SetUnderline(TRUE, range);
        }
        if ((run.flags & kImage) && !b.image && dim_brush_) {
            layout->SetDrawingEffect(dim_brush_.Get(), range);
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        }
    }
    bool display_rendered = false;
    for (const Run& run : b.runs) {
        if (!(run.flags & (kMath | kDisplayMath))) continue;
        const bool display = (run.flags & kDisplayMath) != 0;
        const std::wstring_view text(b.text.data() + run.start, run.length);
        size_t delimiter = display ? 2 : 1;
        if (text.starts_with(display ? L"\\[" : L"\\(") &&
            text.ends_with(display ? L"\\]" : L"\\)")) {
            delimiter = 2;
        } else if (!text.starts_with(display ? L"$$" : L"$") ||
                   !text.ends_with(display ? L"$$" : L"$")) {
            continue;
        }
        if (text.size() <= delimiter * 2) continue;
        float size = base;
        layout->GetFontSize(run.start, &size);
        const bool applied = ApplyMathInline(factory, layout.Get(), {run.start, run.length},
            text.substr(delimiter, text.size() - delimiter * 2),
            display ? (std::max)(size, 18.0f * scale_) : size, display, width, scale_);
        display_rendered = display_rendered || (display && applied);
    }
    if (b.kind == L'm' && display_rendered)
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return layout;
}

float MarkdownView::ContentWidth(float view_width) const noexcept {
    return (std::max)(40.0f * scale_, (std::min)(view_width - 56.0f * scale_, 880.0f * scale_));
}

void MarkdownView::Layout(IDWriteFactory2* factory, float width, float scale) {
    scale_ = scale;
    const float s = scale;
    const float W = ContentWidth(width);
    float y = 22.0f * s;
    const Block* prev = nullptr;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        Block& b = blocks_[i];
        const float x0 = b.indent * 26.0f * s + b.quote * 18.0f * s;
        const float avail = (std::max)(40.0f * s, W - x0);
        if (b.table >= 0) {
            Table& t = tables_[b.table];
            if (t.cells.empty() || t.cells[0] != static_cast<int>(i)) continue;
            if (prev) y += 6.0f * s;
            const float padx = 10.0f * s, pady = 6.0f * s;
            std::vector<float> natural(t.columns, 24.0f * s);
            for (int cell : t.cells) {
                Block& c = blocks_[cell];
                c.layout = MakeLayout(factory, c, 1.0e5f, false);
                DWRITE_TEXT_METRICS m{};
                if (c.layout && SUCCEEDED(c.layout->GetMetrics(&m)))
                    natural[c.col] = (std::max)(natural[c.col], m.widthIncludingTrailingWhitespace);
            }
            float sum = 0.0f;
            for (float n : natural) sum += n;
            const float room = avail - t.columns * padx * 2.0f;
            t.col_w.assign(t.columns, 0.0f);
            for (int c = 0; c < t.columns; ++c) {
                const float w = sum <= room ? natural[c] : (std::max)(48.0f * s, natural[c] * room / sum);
                t.col_w[c] = w + padx * 2.0f;
            }
            t.col_x.assign(t.columns, 0.0f);
            float cx = 0.0f;
            for (int c = 0; c < t.columns; ++c) { t.col_x[c] = cx; cx += t.col_w[c]; }
            t.x = x0; t.y = y; t.w = cx;
            t.row_y.assign(t.rows, 0.0f);
            t.row_h.assign(t.rows, 0.0f);
            for (int cell : t.cells) {
                Block& c = blocks_[cell];
                c.layout = MakeLayout(factory, c, t.col_w[c.col] - padx * 2.0f);
                DWRITE_TEXT_METRICS m{};
                if (c.layout && SUCCEEDED(c.layout->GetMetrics(&m)))
                    t.row_h[c.row] = (std::max)(t.row_h[c.row], m.height + pady * 2.0f);
                if (c.layout) {
                    const DWRITE_TEXT_ALIGNMENT align = c.arg == L"c" ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : c.arg == L"r" ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
                    c.layout->SetTextAlignment(align);
                }
            }
            float ry = 0.0f;
            for (int r = 0; r < t.rows; ++r) { t.row_y[r] = ry; if (t.row_h[r] <= 0) t.row_h[r] = 28.0f * s; ry += t.row_h[r]; }
            t.h = ry;
            for (int cell : t.cells) {
                Block& c = blocks_[cell];
                c.x = t.x + t.col_x[c.col] + padx;
                c.y = t.y + t.row_y[c.row] + pady;
                c.w = t.col_w[c.col] - padx * 2.0f;
                c.h = t.row_h[c.row] - pady * 2.0f;
                c.box_top = t.y + t.row_y[c.row];
                c.box_bottom = c.box_top + t.row_h[c.row];
            }
            y += t.h + 16.0f * s;
            prev = &b;
            continue;
        }
        // Space above.
        if (prev) {
            if (b.kind == L'h') y += (ToU32(b.arg) <= 2 ? 14.0f : 10.0f) * s;
            else if (b.indent > 0 && prev->indent > 0 && prev->kind == L'p' && b.kind == L'p' && !b.marker.empty() &&
                     !NotebookMarker(b.marker))
                y -= 6.0f * s;  // list items sit closer than paragraphs
        }
        b.x = x0;
        b.w = avail;
        b.layout.Reset();
        if (b.kind == L'r') {
            b.y = y + 8.0f * s;
            b.h = 2.0f * s;
            b.box_top = b.y; b.box_bottom = b.y + b.h;
            y = b.y + b.h + 18.0f * s;
        } else if (b.image) {
            b.y = y;
            if (b.image_path.empty()) {
                b.w = (std::min)(avail, 360.0f * s);
                b.h = 44.0f * s;
            } else if (b.image_aspect > 0.0f) {
                b.w = avail;
                b.h = (std::min)(b.w * b.image_aspect, 520.0f * s);
                b.w = b.h / b.image_aspect;
            } else {
                b.w = avail;
                b.h = 160.0f * s;
            }
            b.box_top = b.y; b.box_bottom = b.y + b.h;
            y += b.h + 14.0f * s;
        } else if (b.kind == L'c' && NotebookOutput(b.marker)) {
            // Notebook text output: monospace, no box (mockup .out).
            b.layout = MakeLayout(factory, b, avail);
            DWRITE_TEXT_METRICS m{};
            if (b.layout) b.layout->GetMetrics(&m);
            b.box_top = y;
            b.y = y + 2.0f * s;
            b.h = (std::max)(m.height, 1.0f);
            b.box_bottom = b.y + b.h + 2.0f * s;
            y = b.box_bottom + 12.0f * s;
        } else if (b.kind == L'c' || b.kind == L'x') {
            const float pad = 12.0f * s;
            b.layout = MakeLayout(factory, b, avail - pad * 2.0f);
            DWRITE_TEXT_METRICS m{};
            if (b.layout) b.layout->GetMetrics(&m);
            b.box_top = y;
            b.x = x0 + pad;
            b.y = y + pad * 0.85f;
            b.w = avail - pad * 2.0f;
            b.h = m.height;
            b.box_bottom = b.y + b.h + pad * 0.85f;
            y = b.box_bottom + 14.0f * s;
        } else {
            b.layout = MakeLayout(factory, b, avail);
            DWRITE_TEXT_METRICS m{};
            if (b.layout) b.layout->GetMetrics(&m);
            b.y = y;
            b.h = (std::max)(m.height, 1.0f);
            b.box_top = b.y; b.box_bottom = b.y + b.h;
            y += b.h;
            if (b.kind == L'h') y += (ToU32(b.arg) <= 2 ? 16.0f : 8.0f) * s;  // room for the rule under h1/h2
            else y += 12.0f * s;
        }
        prev = &b;
    }
    footer_y_ = -1.0f;
    if (!sections_.empty() && section_ + 1 < SectionCount()) {
        footer_y_ = y + 8.0f * s;
        y += 64.0f * s;
    }
    doc_width_ = W;
    for (const auto& table : tables_) doc_width_ = (std::max)(doc_width_, table.x + table.w);
    doc_height_ = y + 24.0f * s;
    relayout_ = false;
    ClampScroll();
}

void MarkdownView::ClampScroll() {
    const float view = view_.bottom - view_.top;
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, doc_height_ - view));
    const float width = view_.right - view_.left;
    const float margin = (std::max)(28.0f * scale_, (width - ContentWidth(width)) * 0.5f);
    scroll_x_ = std::clamp(scroll_x_, 0.0f, (std::max)(0.0f, doc_width_ + margin * 2 - width));
    origin_x_ = view_.left + margin - scroll_x_;
}

// ---------------------------------------------------------------- drawing

void MarkdownView::DrawRanges(ID2D1DeviceContext* dc, const Block& b, float ox, float oy,
                              uint32_t start, uint32_t end, const D2D1_COLOR_F& color) {
    if (!b.layout || end <= start) return;
    const uint32_t bs = b.plain_start, be = b.plain_start + static_cast<uint32_t>(b.text.size());
    const uint32_t a = (std::max)(start, bs), z = (std::min)(end, be);
    if (z <= a) return;
    UINT32 count = 0;
    b.layout->HitTestTextRange(a - bs, z - a, 0, 0, nullptr, 0, &count);
    if (!count) return;
    std::vector<DWRITE_HIT_TEST_METRICS> m(count);
    if (FAILED(b.layout->HitTestTextRange(a - bs, z - a, 0, 0, m.data(), count, &count))) return;
    brush_->SetColor(color);
    for (UINT32 i = 0; i < count; ++i)
        dc->FillRectangle(R(ox + m[i].left, oy + m[i].top, ox + m[i].left + m[i].width,
                            oy + m[i].top + m[i].height), brush_.Get());
}

bool MarkdownView::SidebarVisible(float width) const noexcept {
    return !toc_hidden_ && !toc_.empty() && width >= 640.0f * scale_;
}

bool MarkdownView::Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                        const Theme& theme, bool dark, float scale, ThumbnailCache* images,
                        uint64_t generation, uint32_t sel_start, uint32_t sel_end,
                        SearchHighlights matches) {
    if (!dc || !parsed_ || !EnsureFormats(factory, scale)) return false;
    scale_ = scale;
    D2D1_RECT_F doc = rect;
    toc_rect_ = {};
    if (SidebarVisible(rect.right - rect.left)) {
        toc_rect_ = R(rect.left, rect.top, rect.left + 190.0f * scale, rect.bottom);
        doc.left = toc_rect_.right;
    }
    const bool repaint = DrawDocument(dc, factory, doc, theme, dark, scale, images, generation,
                                      sel_start, sel_end, matches);
    if (toc_rect_.right > toc_rect_.left) DrawToc(dc, toc_rect_, theme, dark);
    return repaint;
}

bool MarkdownView::DrawDocument(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                                const Theme& theme, bool dark, float scale, ThumbnailCache* images,
                                uint64_t generation, uint32_t sel_start, uint32_t sel_end,
                                SearchHighlights matches) {
    if (rect.left != view_.left || rect.top != view_.top || rect.right != view_.right ||
        rect.bottom != view_.bottom || scale != layout_scale_) EndScrollbarDrag();
    view_ = rect;
    footer_rect_ = {};
    scale_ = scale;
    EnsureBrushes(dc, dark);
    if (!brush_) return false;
    const float width = rect.right - rect.left;
    if (relayout_ || std::abs(width - layout_width_) > 0.5f || scale != layout_scale_ || dark != layout_dark_) {
        Layout(factory, width, scale);
        layout_width_ = width;
        layout_scale_ = scale;
        layout_dark_ = dark;
    }
    if (pending_block_ >= 0 || pending_end_ || pending_reveal_) {
        if (pending_end_) scroll_ = doc_height_;
        else if (pending_block_ > 0 && pending_block_ < static_cast<int>(blocks_.size()))
            scroll_ = BlockTop(pending_block_) - 16.0f * scale;
        else if (pending_block_ == 0) scroll_ = 0.0f;
        pending_block_ = -1;
        pending_end_ = false;
        ClampScroll();
        if (pending_reveal_) { pending_reveal_ = false; Reveal(pending_offset_); }
    }
    ClampScroll();
    const float s = scale;
    const float W = ContentWidth(width);
    origin_x_ = rect.left + (std::max)(28.0f * s, (width - W) * 0.5f) - scroll_x_;
    const float oy = rect.top - scroll_;
    const float top_doc = scroll_ - 40.0f * s, bottom_doc = scroll_ + (rect.bottom - rect.top) + 40.0f * s;
    bool repaint = false;
    const D2D1_COLOR_F subtle = WithAlpha(theme.text, dark ? 0.07f : 0.05f);
    const D2D1_COLOR_F line = WithAlpha(theme.text, dark ? 0.16f : 0.14f);
    const D2D1_COLOR_F selection_color = WithAlpha(theme.accent, dark ? 0.40f : 0.28f);

    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Quote bars: one per level across consecutive blocks inside the quote.
    for (int level = 1; level <= 8; ++level) {
        float run_top = -1.0f, run_bottom = 0.0f;
        auto flush = [&] {
            if (run_top < 0.0f) return;
            const float bx = origin_x_ + (level - 1) * 18.0f * s;
            brush_->SetColor(WithAlpha(theme.text, dark ? 0.28f : 0.22f));
            dc->FillRectangle(R(bx, oy + run_top - 2.0f * s, bx + 3.0f * s, oy + run_bottom + 2.0f * s), brush_.Get());
            run_top = -1.0f;
        };
        bool any = false;
        for (const Block& b : blocks_) {
            if (b.table >= 0 && (tables_[b.table].cells.empty() || &blocks_[tables_[b.table].cells[0]] != &b)) continue;
            const float top = b.table >= 0 ? tables_[b.table].y : b.box_top;
            const float bottom = b.table >= 0 ? tables_[b.table].y + tables_[b.table].h : b.box_bottom;
            if (b.quote >= level) {
                any = true;
                if (run_top < 0.0f) run_top = top;
                run_bottom = bottom;
            } else {
                flush();
            }
        }
        flush();
        if (!any) break;
    }

    for (const Table& t : tables_) {
        if (t.y > bottom_doc || t.y + t.h < top_doc || t.cells.empty()) continue;
        const float tx = origin_x_ + t.x, ty = oy + t.y;
        if (t.rows > 0 && !t.row_h.empty() && blocks_[t.cells[0]].header) {
            brush_->SetColor(subtle);
            dc->FillRectangle(R(tx, ty, tx + t.w, ty + t.row_h[0]), brush_.Get());
        }
        brush_->SetColor(line);
        for (int r = 0; r <= t.rows; ++r) {
            const float yy = ty + (r < t.rows ? t.row_y[r] : t.h);
            dc->FillRectangle(R(tx, yy, tx + t.w, yy + 1.0f), brush_.Get());
        }
        for (int c = 0; c <= t.columns; ++c) {
            const float xx = tx + (c < t.columns ? t.col_x[c] : t.w);
            dc->FillRectangle(R(xx, ty, xx + 1.0f, ty + t.h), brush_.Get());
        }
    }

    for (Block& b : blocks_) {
        if (b.box_bottom < top_doc || b.box_top > bottom_doc) continue;
        const float bx = origin_x_ + b.x, by = oy + b.y;
        // Notebook In [n]: / Out[n]: label, right-aligned in the gutter.
        if (NotebookMarker(b.marker) && b.marker.size() > 1 && label_ && (b.kind == L'c' || b.image)) {
            float lh = 20.0f * s;
            if (b.layout && !b.image) {
                DWRITE_LINE_METRICS lm{};
                UINT32 lines = 0;
                b.layout->GetLineMetrics(&lm, 1, &lines);
                if (lm.height > 0) lh = lm.height;
            }
            const float right = origin_x_ + b.indent * 26.0f * s + b.quote * 18.0f * s - 10.0f * s;
            const std::wstring label = b.marker.substr(1);
            brush_->SetColor(WithAlpha(theme.text, 0.5f));
            dc->DrawTextW(label.data(), static_cast<UINT32>(label.size()), label_.Get(),
                          R(right - 64.0f * s, by, right, by + lh), brush_.Get());
        }
        if (b.kind == L'c' && NotebookOutput(b.marker)) {
            // no box
        } else if (b.kind == L'c' || b.kind == L'x') {
            brush_->SetColor(subtle);
            const float left = origin_x_ + b.indent * 26.0f * s + b.quote * 18.0f * s;
            dc->FillRoundedRectangle(D2D1::RoundedRect(R(left, oy + b.box_top, left + b.w + 24.0f * s,
                                                         oy + b.box_bottom), 6.0f * s, 6.0f * s), brush_.Get());
        } else if (b.kind == L'r') {
            brush_->SetColor(line);
            dc->FillRectangle(R(bx, by, bx + b.w, by + b.h), brush_.Get());
            continue;
        } else if (b.image) {
            if (!b.image_metadata_ready && !b.runs.empty()) {
                const auto info = image_metadata_.Lookup(b.runs[0].target, base_dir_, notify_);
                if (info.ready) {
                    b.image_metadata_ready = true;
                    b.image_path = info.path;
                    b.image_attrs = info.attrs;
                    b.image_size = info.size;
                    b.image_modified = info.modified;
                    if (!info.path.empty()) { relayout_ = true; repaint = true; }
                }
            }
            const D2D1_RECT_F box = R(bx, by, bx + b.w, by + b.h);
            bool drawn = false;
            if (!b.image_path.empty() && images) {
                const uint32_t pixels = ipc::BucketPreviewPixelSize(
                    static_cast<uint32_t>((std::max)(b.w, b.h)));
                uint32_t src_w = 0, src_h = 0, dec_w = 0, dec_h = 0;
                const D2D1_RECT_F dest = b.image_aspect > 0.0f ? box : R(bx, by, bx + 1.0f, by + 1.0f);
                const PreviewDrawResult r = images->Draw(dc, dest, b.image_path, b.image_attrs, pixels,
                    generation, b.image_modified, b.image_size, b.image_aspect > 0.0f ? 1.0f : 0.0f,
                    nullptr, nullptr, nullptr, false, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                    nullptr, nullptr, nullptr, &dec_w, &dec_h, &src_w, &src_h,
                    nullptr, nullptr, false, nullptr, nullptr, nullptr, image_session_);
                if (!src_w || !src_h) { src_w = dec_w; src_h = dec_h; }
                if (r == PreviewDrawResult::Bitmap && src_w && src_h) {
                    if (b.image_aspect <= 0.0f) {
                        b.image_aspect = static_cast<float>(src_h) / static_cast<float>(src_w);
                        relayout_ = true;
                        repaint = true;
                    }
                    drawn = b.image_aspect > 0.0f;
                }
            }
            if (!drawn) {
                brush_->SetColor(subtle);
                dc->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f * s, 6.0f * s), brush_.Get());
                std::wstring label = L"\xD83D\xDDBC  " + b.text;
                if (b.image_metadata_ready && b.image_path.empty())
                    label += pulse::l10n::Get(pulse::l10n::StringId::RemoteOrMissingImageNotLoaded).c_str();
                WrlPtr<IDWriteTextLayout> tl;
                factory->CreateTextLayout(label.data(), static_cast<UINT32>(label.size()), body_.Get(),
                                          (std::max)(1.0f, b.w - 24.0f * s), b.h, &tl);
                if (tl) {
                    tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    brush_->SetColor(theme.text_secondary);
                    dc->DrawTextLayout(D2D1::Point2F(bx + 12.0f * s, by), tl.Get(), brush_.Get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            DrawRanges(dc, b, bx, by, sel_start, sel_end, selection_color);
            continue;
        }
        if (!b.layout) continue;
        // Inline code chips.
        for (const Run& run : b.runs) {
            if (!(run.flags & kCode) || !run.length) continue;
            UINT32 count = 0;
            b.layout->HitTestTextRange(run.start, run.length, 0, 0, nullptr, 0, &count);
            if (!count) continue;
            std::vector<DWRITE_HIT_TEST_METRICS> m(count);
            if (FAILED(b.layout->HitTestTextRange(run.start, run.length, 0, 0, m.data(), count, &count))) continue;
            brush_->SetColor(WithAlpha(theme.text, dark ? 0.12f : 0.08f));
            for (UINT32 k = 0; k < count; ++k)
                dc->FillRoundedRectangle(D2D1::RoundedRect(R(bx + m[k].left - 3.0f * s, by + m[k].top + 1.0f * s,
                    bx + m[k].left + m[k].width + 3.0f * s, by + m[k].top + m[k].height - 1.0f * s), 3.0f * s, 3.0f * s),
                    brush_.Get());
        }
        for (const Highlight h : matches.Intersecting(b.plain_start, uint64_t{b.plain_start} + b.text.size()))
            DrawRanges(dc, b, bx, by, h.start, h.start + h.length,
                       h.current ? HexColor(0xF59E0B, 0.75f) : HexColor(0xFACC15, dark ? 0.35f : 0.45f));
        DrawRanges(dc, b, bx, by, sel_start, sel_end, selection_color);
        brush_->SetColor(b.quote > 0 ? theme.text_secondary : theme.text);
        const bool has_math = std::any_of(b.runs.begin(), b.runs.end(), [](const Run& run) {
            return (run.flags & (kMath | kDisplayMath)) != 0;
        });
        if (has_math) DrawMathTextLayout(dc, b.layout.Get(), D2D1::Point2F(bx, by), brush_.Get());
        else dc->DrawTextLayout(D2D1::Point2F(bx, by), b.layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (b.kind == L'h' && ToU32(b.arg) <= 2) {
            brush_->SetColor(line);
            dc->FillRectangle(R(bx, by + b.h + 6.0f * s, bx + b.w, by + b.h + 7.0f * s), brush_.Get());
        }
        // List marker in the gutter left of the first line.
        if (!b.marker.empty() && b.table < 0 && !NotebookMarker(b.marker)) {
            DWRITE_LINE_METRICS lm{};
            UINT32 lines = 0;
            b.layout->GetLineMetrics(&lm, 1, &lines);
            const float lh = lm.height > 0 ? lm.height : 20.0f * s;
            if (b.marker[0] == L't') {
                const float box = 14.0f * s;
                const float cx = bx - 20.0f * s, cy = by + lh * 0.5f;
                const D2D1_RECT_F r = R(cx - box * 0.5f, cy - box * 0.5f, cx + box * 0.5f, cy + box * 0.5f);
                const bool checked = b.marker == L"t1";
                if (checked) {
                    brush_->SetColor(theme.accent);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 3.0f * s, 3.0f * s), brush_.Get());
                    brush_->SetColor(D2D1::ColorF(0xFFFFFF));
                    dc->DrawLine({cx - 4.0f * s, cy}, {cx - 1.2f * s, cy + 3.0f * s}, brush_.Get(), 1.6f * s);
                    dc->DrawLine({cx - 1.2f * s, cy + 3.0f * s}, {cx + 4.2f * s, cy - 3.2f * s}, brush_.Get(), 1.6f * s);
                } else {
                    brush_->SetColor(theme.text_secondary);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(R(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                                                               3.0f * s, 3.0f * s), brush_.Get(), 1.2f * s);
                }
            } else {
                std::wstring mark;
                if (b.marker[0] == L'o') mark = b.marker.substr(1) + L".";
                else {
                    const int level = b.indent - gutter_;
                    mark = level <= 1 ? L"\x2022" : level == 2 ? L"\x25E6" : L"\x25AA";
                }
                brush_->SetColor(theme.text_secondary);
                dc->DrawTextW(mark.data(), static_cast<UINT32>(mark.size()), marker_.Get(),
                              R(bx - 40.0f * s, by, bx - 8.0f * s, by + lh), brush_.Get());
            }
        }
    }

    // Next chapter: a rule and a centred link under the last block.
    if (footer_y_ >= 0.0f && section_ + 1 < SectionCount() && factory && link_brush_) {
        const float fy = oy + footer_y_;
        brush_->SetColor(line);
        dc->FillRectangle(R(origin_x_, fy, origin_x_ + W, fy + 1.0f * s), brush_.Get());
        std::wstring label = pulse::l10n::Get(pulse::l10n::StringId::NextChapter).c_str();
        const std::wstring& name = sections_[section_ + 1].name;
        if (!name.empty()) label += L"  ·  " + name.substr(0, 60);
        label += L"  ›";
        WrlPtr<IDWriteTextLayout> text;
        if (SUCCEEDED(factory->CreateTextLayout(label.data(), static_cast<UINT32>(label.size()), body_.Get(),
                                                W, 30.0f * s, &text)) && text) {
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS m{};
            text->GetMetrics(&m);
            const float tw = (std::min)(m.width, W);
            const float tx = origin_x_ + (W - tw) * 0.5f, ty = fy + 20.0f * s;
            dc->DrawTextLayout(D2D1::Point2F(tx, ty), text.Get(), link_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            footer_rect_ = R(tx - 12.0f * s, ty - 8.0f * s, tx + tw + 12.0f * s, ty + m.height + 8.0f * s);
        }
    }

    // Drawing and input share geometry; the hit lane is wider than the ink.
    for (bool horizontal : {false, true}) {
        const auto bar = Scrollbar(horizontal);
        if (bar.maximum <= 0.0f) continue;
        brush_->SetColor(WithAlpha(theme.text, 0.25f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(bar.thumb, 1.5f * s, 1.5f * s), brush_.Get());
    }
    dc->PopAxisAlignedClip();
    return repaint;
}

// ---------------------------------------------------------------- input

MarkdownView::ScrollbarGeometry MarkdownView::Scrollbar(bool horizontal) const {
    ScrollbarGeometry bar;
    if (!parsed_ || relayout_) return bar;
    const float width = view_.right - view_.left, height = view_.bottom - view_.top;
    if (width <= 0.0f || height <= 0.0f) return bar;
    const float margin = (std::max)(28.0f * scale_, (width - ContentWidth(width)) * 0.5f);
    const float extent_x = doc_width_ + margin * 2.0f;
    const float viewport = horizontal ? width : height;
    const float extent = horizontal ? extent_x : doc_height_;
    if (extent <= viewport + 1.0f) return bar;
    const bool other = horizontal ? doc_height_ > height + 1.0f : extent_x > width + 1.0f;
    const float start = (horizontal ? view_.left : view_.top) + 4.0f * scale_;
    const float track = (std::max)(0.0f, viewport - (other ? 16.0f : 8.0f) * scale_);
    const float thumb = (std::min)(track, (std::max)(24.0f * scale_, track * viewport / extent));
    bar.maximum = extent - viewport;
    bar.travel = track - thumb;
    const float position = horizontal ? scroll_x_ : scroll_;
    const float offset = start + bar.travel * std::clamp(position / bar.maximum, 0.0f, 1.0f);
    if (horizontal) {
        bar.hit = R(start, (std::max)(view_.top, view_.bottom - 12.0f * scale_), start + track, view_.bottom);
        bar.thumb = R(offset, view_.bottom - 6.0f * scale_, offset + thumb, view_.bottom - 3.0f * scale_);
    } else {
        bar.hit = R((std::max)(view_.left, view_.right - 12.0f * scale_), start, view_.right, start + track);
        bar.thumb = R(view_.right - 6.0f * scale_, offset, view_.right - 3.0f * scale_, offset + thumb);
    }
    return bar;
}

bool MarkdownView::OverScrollbar(float x, float y) const {
    for (bool horizontal : {false, true}) {
        const auto bar = Scrollbar(horizontal);
        if (bar.maximum > 0.0f && x >= bar.hit.left && x < bar.hit.right &&
            y >= bar.hit.top && y < bar.hit.bottom) return true;
    }
    return false;
}

bool MarkdownView::ScrollbarMouseDown(float x, float y) {
    EndScrollbarDrag();
    for (bool horizontal : {false, true}) {
        const auto bar = Scrollbar(horizontal);
        if (bar.maximum <= 0.0f || x < bar.hit.left || x >= bar.hit.right ||
            y < bar.hit.top || y >= bar.hit.bottom) continue;
        const float point = horizontal ? x : y;
        const float start = horizontal ? bar.thumb.left : bar.thumb.top;
        const float end = horizontal ? bar.thumb.right : bar.thumb.bottom;
        float& position = horizontal ? scroll_x_ : scroll_;
        if (point >= start && point < end && bar.travel > 0.0f) {
            scrollbar_drag_ = horizontal ? 2 : 1;
            scrollbar_press_ = point;
            scrollbar_start_ = position;
        } else {
            const float viewport = horizontal ? view_.right - view_.left : view_.bottom - view_.top;
            position += (point < start ? -1.0f : 1.0f) * viewport * 0.9f;
            ClampScroll();
        }
        return true;
    }
    return false;
}

bool MarkdownView::ScrollbarMouseMove(float x, float y) {
    if (!DraggingScrollbar()) return false;
    const bool horizontal = scrollbar_drag_ == 2;
    const auto bar = Scrollbar(horizontal);
    if (bar.travel <= 0.0f) { EndScrollbarDrag(); return false; }
    float& position = horizontal ? scroll_x_ : scroll_;
    const float before = position;
    position = scrollbar_start_ + ((horizontal ? x : y) - scrollbar_press_) * bar.maximum / bar.travel;
    ClampScroll();
    return before != position;
}

bool MarkdownView::Scroll(float wheel_steps) {
    const float before = scroll_;
    scroll_ -= wheel_steps * 56.0f * scale_;
    ClampScroll();
    return scroll_ != before;
}

bool MarkdownView::ScrollHorizontal(float wheel_steps) {
    const float before = scroll_x_;
    scroll_x_ -= wheel_steps * 56.0f * scale_;
    ClampScroll();
    return before != scroll_x_;
}

bool MarkdownView::Key(UINT vk) {
    if (vk == VK_LEFT || vk == VK_RIGHT) return ScrollHorizontal(vk == VK_LEFT ? 1.0f : -1.0f);
    const float view = view_.bottom - view_.top;
    const float before = scroll_;
    if (!sections_.empty() && !relayout_) {
        const float end = (std::max)(0.0f, doc_height_ - view);
        if (vk == VK_NEXT && scroll_ >= end - 0.5f && section_ + 1 < SectionCount()) return ShowSection(section_ + 1);
        if (vk == VK_PRIOR && scroll_ <= 0.5f && section_ > 0) return ShowSection(section_ - 1, 0, true);
    }
    switch (vk) {
    case VK_PRIOR: scroll_ -= view * 0.9f; break;
    case VK_NEXT: scroll_ += view * 0.9f; break;
    case VK_HOME: scroll_ = 0.0f; break;
    case VK_END: scroll_ = doc_height_; break;
    case VK_UP: scroll_ -= 40.0f * scale_; break;
    case VK_DOWN: scroll_ += 40.0f * scale_; break;
    default: return false;
    }
    ClampScroll();
    return scroll_ != before;
}

int MarkdownView::BlockAt(float x, float doc_y) const {
    int best = -1;
    float best_distance = 1.0e9f;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        if (b.table >= 0 && b.box_bottom <= b.box_top) continue;
        const float top = b.table >= 0 ? b.box_top : b.y;
        const float bottom = b.table >= 0 ? b.box_bottom : b.y + b.h;
        float dy = doc_y < top ? top - doc_y : doc_y > bottom ? doc_y - bottom : 0.0f;
        if (b.table >= 0) {
            const float left = origin_x_ + b.x - 10.0f * scale_, right = origin_x_ + b.x + b.w + 10.0f * scale_;
            dy += x < left ? left - x : x > right ? x - right : 0.0f;
        }
        if (dy < best_distance) { best_distance = dy; best = static_cast<int>(i); }
        if (dy == 0.0f && b.table < 0) break;
    }
    return best;
}

bool MarkdownView::HitTest(float x, float y, uint32_t& offset) const {
    if (!parsed_ || blocks_.empty()) return false;
    const float doc_y = y - view_.top + scroll_;
    const int index = BlockAt(x, doc_y);
    if (index < 0) return false;
    const Block& b = blocks_[index];
    offset = b.plain_start;
    if (!b.layout) {
        if (doc_y > b.y + b.h * 0.5f) offset += static_cast<uint32_t>(b.text.size());
        return true;
    }
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(b.layout->HitTestPoint(x - (origin_x_ + b.x), doc_y - b.y, &trailing, &inside, &m))) return true;
    const uint32_t trailing_length = m.length;
    offset += (std::min)(m.textPosition + (trailing ? trailing_length : 0u), static_cast<uint32_t>(b.text.size()));
    return true;
}

std::wstring MarkdownView::LinkAt(float x, float y) const {
    if (!parsed_) return {};
    const float doc_y = y - view_.top + scroll_;
    const int index = BlockAt(x, doc_y);
    if (index < 0) return {};
    const Block& b = blocks_[index];
    if (!b.layout || doc_y < b.y || doc_y > b.y + b.h) return {};
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(b.layout->HitTestPoint(x - (origin_x_ + b.x), doc_y - b.y, &trailing, &inside, &m)) || !inside)
        return {};
    for (const Run& run : b.runs)
        if ((run.flags & kLink) && m.textPosition >= run.start && m.textPosition < run.start + run.length)
            return run.target;
    return {};
}

void MarkdownView::Reveal(uint32_t offset) {
    if (!sections_.empty()) {
        // The offset may sit in another chapter: show it, reveal after layout.
        size_t lo = 0, hi = all_blocks_.size();
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (all_blocks_[mid].plain_start <= offset) lo = mid; else hi = mid;
        }
        int target = section_;
        for (size_t i = 0; i < sections_.size(); ++i)
            if (lo >= sections_[i].begin && lo < sections_[i].end) { target = static_cast<int>(i); break; }
        if (target != section_ || relayout_) {
            if (target != section_) ShowSection(target);
            pending_block_ = -1;
            pending_reveal_ = true;
            pending_offset_ = offset;
            return;
        }
    }
    if (blocks_.empty()) return;
    size_t index = 0;
    for (size_t i = 0; i < blocks_.size(); ++i) if (blocks_[i].plain_start <= offset) index = i;
    const Block& b = blocks_[index];
    float y = b.y, h = (std::max)(b.h, 20.0f * scale_);
    float x = b.x, hit_width = 1.0f;
    if (b.layout) {
        FLOAT px = 0, py = 0;
        DWRITE_HIT_TEST_METRICS m{};
        const uint32_t local = (std::min)(offset - b.plain_start, static_cast<uint32_t>(b.text.size()));
        if (SUCCEEDED(b.layout->HitTestTextPosition(local, FALSE, &px, &py, &m))) { y = b.y + py; h = m.height; x += px; hit_width = m.width; }
    }
    // A table lookup must expose the cell, not merely the first matched glyph.
    // Oversized cells still use the caret range so their text remains traversable.
    if (b.table >= 0 && b.w <= view_.right - view_.left - 40.0f * scale_) {
        x = b.x;
        hit_width = b.w;
    }
    const float left = origin_x_ + x, right = left + hit_width;
    if (left < view_.left + 20.0f * scale_) scroll_x_ -= view_.left + 20.0f * scale_ - left;
    else if (right > view_.right - 20.0f * scale_) scroll_x_ += right - view_.right + 20.0f * scale_;
    const float view = view_.bottom - view_.top;
    if (y < scroll_ + 20.0f * scale_) scroll_ = y - 40.0f * scale_;
    else if (y + h > scroll_ + view - 20.0f * scale_) scroll_ = y + h - view + 60.0f * scale_;
    ClampScroll();
}

// ---------------------------------------------------------------- chapters

float MarkdownView::BlockTop(int index) const {
    if (index < 0 || index >= static_cast<int>(blocks_.size())) return 0.0f;
    const Block& b = blocks_[index];
    return b.table >= 0 && b.table < static_cast<int>(tables_.size()) ? tables_[b.table].y : b.box_top;
}

int MarkdownView::ActiveTocEntry() const {
    int best = -1;
    const float top = scroll_ + 32.0f * scale_;
    for (size_t i = 0; i < toc_.size(); ++i) {
        const TocEntry& e = toc_[i];
        if (e.section < section_) best = static_cast<int>(i);
        else if (e.section == section_ &&
                 (e.block <= 0 || relayout_ || e.block >= static_cast<int>(blocks_.size()) || BlockTop(e.block) <= top))
            best = static_cast<int>(i);
    }
    return best;
}

void MarkdownView::ClampTocScroll() {
    const float content = 24.0f * scale_ + static_cast<float>(toc_.size()) * 26.0f * scale_;
    const float view = toc_rect_.bottom - toc_rect_.top;
    toc_scroll_ = std::clamp(toc_scroll_, 0.0f, (std::max)(0.0f, content - view));
}

int MarkdownView::TocRowAt(float x, float y) const {
    if (toc_rect_.right <= toc_rect_.left || x < toc_rect_.left || x >= toc_rect_.right ||
        y < toc_rect_.top || y >= toc_rect_.bottom)
        return -1;
    const float at = y - toc_rect_.top - 12.0f * scale_ + toc_scroll_;
    if (at < 0.0f) return -1;
    const size_t row = static_cast<size_t>(at / (26.0f * scale_));
    return row < toc_.size() ? static_cast<int>(row) : -1;
}

void MarkdownView::DrawToc(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme, bool dark) {
    if (!brush_ || !toc_format_ || !factory_) return;
    const float s = scale_;
    const float row_h = 26.0f * s;
    brush_->SetColor(dark ? D2D1::ColorF(0x1C1C1C) : D2D1::ColorF(0xEEEEEE));
    dc->FillRectangle(rect, brush_.Get());
    brush_->SetColor(dark ? D2D1::ColorF(0x333333) : D2D1::ColorF(0xDDDDDD));
    dc->FillRectangle(R(rect.right - 1.0f * s, rect.top, rect.right, rect.bottom), brush_.Get());
    const int active = ActiveTocEntry();
    if (toc_follow_ && active >= 0) {
        const float y = 12.0f * s + active * row_h;
        const float view = rect.bottom - rect.top;
        if (y < toc_scroll_ + row_h) toc_scroll_ = y - row_h * 2.0f;
        else if (y + row_h > toc_scroll_ + view - row_h) toc_scroll_ = y + row_h * 3.0f - view;
        toc_follow_ = false;
    }
    ClampTocScroll();
    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = rect.top + 12.0f * s - toc_scroll_;
    for (size_t i = 0; i < toc_.size(); ++i, y += row_h) {
        if (y + row_h < rect.top) continue;
        if (y > rect.bottom) break;
        const TocEntry& e = toc_[i];
        const bool on = static_cast<int>(i) == active;
        const D2D1_RECT_F row = R(rect.left + 8.0f * s, y, rect.right - 9.0f * s, y + row_h);
        if (on) {
            brush_->SetColor(dark ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0xFFFFFF));
            dc->FillRoundedRectangle(D2D1::RoundedRect(row, 5.0f * s, 5.0f * s), brush_.Get());
        }
        const float pad = (10.0f + 14.0f * static_cast<float>((std::min)(e.level, 4))) * s;
        IDWriteTextFormat* format = e.level > 0 && toc_sub_format_ ? toc_sub_format_.Get() : toc_format_.Get();
        const float width = (std::max)(1.0f, row.right - row.left - pad - 8.0f * s);
        WrlPtr<IDWriteTextLayout> text;
        if (FAILED(factory_->CreateTextLayout(e.title.data(), static_cast<UINT32>(e.title.size()), format,
                                              width, row_h, &text)) || !text)
            continue;
        if (on) text->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, static_cast<UINT32>(e.title.size())});
        brush_->SetColor(WithAlpha(theme.text, on ? 1.0f : 0.8f));
        dc->DrawTextLayout(D2D1::Point2F(row.left + pad, y), text.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    dc->PopAxisAlignedClip();
}

bool MarkdownView::Click(float x, float y) {
    if (!parsed_) return false;
    if (toc_rect_.right > toc_rect_.left && x >= toc_rect_.left && x < toc_rect_.right &&
        y >= toc_rect_.top && y < toc_rect_.bottom) {
        const int row = TocRowAt(x, y);
        if (row >= 0) {
            if (sections_.empty()) pending_block_ = toc_[row].block;
            else ShowSection(toc_[row].section, toc_[row].block);
            toc_follow_ = false;  // the clicked row is already in view
        }
        return true;
    }
    if (footer_rect_.right > footer_rect_.left && x >= footer_rect_.left && x < footer_rect_.right &&
        y >= footer_rect_.top && y < footer_rect_.bottom) {
        ShowSection(section_ + 1);
        return true;
    }
    return false;
}

bool MarkdownView::IsClickable(float x, float y) const {
    if (TocRowAt(x, y) >= 0) return true;
    return footer_rect_.right > footer_rect_.left && x >= footer_rect_.left && x < footer_rect_.right &&
           y >= footer_rect_.top && y < footer_rect_.bottom;
}

bool MarkdownView::ScrollAt(float x, float y, float wheel_steps) {
    if (toc_rect_.right > toc_rect_.left && x >= toc_rect_.left && x < toc_rect_.right &&
        y >= toc_rect_.top && y < toc_rect_.bottom) {
        const float before = toc_scroll_;
        toc_scroll_ -= wheel_steps * 78.0f * scale_;
        ClampTocScroll();
        return toc_scroll_ != before;
    }
    const bool moved = Scroll(wheel_steps);
    if (moved || sections_.empty() || relayout_) {
        overscroll_ = 0.0f;
        return moved;
    }
    // Already at the end of the chapter and still wheeling down.
    if (wheel_steps < 0.0f && section_ + 1 < SectionCount()) {
        overscroll_ -= wheel_steps;
        if (overscroll_ >= 4.0f) return ShowSection(section_ + 1);
    } else {
        overscroll_ = 0.0f;
    }
    return false;
}

} // namespace pulse::ui
