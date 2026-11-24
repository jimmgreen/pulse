// epub_document.cpp — EPUB to the Markdown payload (see epub_document.h).
#include "epub_document.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <map>
#include <system_error>
#include <vector>

#include "../ipc/preview_protocol.h"
#include "doc_payload.h"
#include "zip_entry.h"

namespace pulse::preview {
namespace {

constexpr size_t kMaxPartBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxSpine = 3000;
constexpr size_t kMaxToc = 3000;
constexpr size_t kTocReserve = 160u * 1024u;  // C and M records after the text
constexpr size_t kMaxColumns = 64;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring DirOf(const std::wstring& part) {
    const size_t slash = part.find_last_of(L'/');
    return slash == std::wstring::npos ? std::wstring() : part.substr(0, slash + 1);
}

std::wstring Fragment(const std::wstring& href) {
    const size_t hash = href.find(L'#');
    return hash == std::wstring::npos ? std::wstring() : href.substr(hash + 1);
}

bool ReadPart(const std::wstring& path, const std::wstring& name, std::wstring& text,
              std::wstring* error = nullptr) {
    std::vector<unsigned char> bytes;
    if (!ReadZipEntry(path, ToUtf8(name), kMaxPartBytes, bytes, error)) return false;
    text = DecodeDocumentText(bytes);
    return true;
}

int ToInt(const std::wstring& s, int fallback) {
    int v = 0;
    bool any = false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') break;
        v = v * 10 + (c - L'0');
        any = true;
        if (v > 1000000) break;
    }
    return any ? v : fallback;
}

// Whitespace runs to one space (HTML text outside <pre>).
std::wstring Collapse(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool space = false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f') {
            space = true;
            continue;
        }
        if (space) out += L' ';
        space = false;
        out += c;
    }
    if (space) out += L' ';
    return out;
}

struct TocEntry { int level = 0; std::wstring file, fragment, title; };
using ChapterAnchors = std::map<std::wstring, size_t>;

class HtmlConverter {
public:
    HtmlConverter(const std::wstring& zip, DocPayload& out, PreviewImageCache& images)
        : zip_(zip), out_(out), images_(images) {}

    // Converts one spine document; title: its <title>.
    void Convert(const std::wstring& part, std::wstring_view html, ChapterAnchors* anchors, size_t base,
                 std::wstring& title) {
        anchors_ = anchors;
        dir_ = DirOf(part);
        base_ = base;
        quote_ = pre_ = svg_ = heading_ = 0;
        lists_.clear();
        inline_.clear();
        marker_.clear();
        XmlPull x(html, true);
        for (XmlPull::Kind k; !out_.full() && (k = x.Next()) != XmlPull::Eof;) {
            if (k == XmlPull::Text) { OnText(x.text()); continue; }
            const std::wstring& n = x.name();
            if (k == XmlPull::Open) {
                if (n == L"head") { ReadHead(x, title); continue; }
                RecordAnchor(x);
                if (n == L"table" && !x.self_closing()) { ParseTable(x); continue; }
                OnOpen(x);
                if (x.self_closing()) OnClose(n);
            } else {
                OnClose(n);
            }
        }
        out_.End();
    }

private:
    void RecordAnchor(const XmlPull& x) {
        if (!anchors_) return;
        const auto it = anchors_->find(x.Attr(L"id"));
        if (it != anchors_->end()) it->second = out_.blocks() - base_;
    }

    struct Inline { std::wstring name; unsigned flags; std::wstring target; };
    struct List { bool ordered; int next; };

    static bool IsBlock(const std::wstring& n) {
        static constexpr std::wstring_view kBlocks[] = {
            L"p", L"div", L"section", L"article", L"header", L"footer", L"aside", L"main",
            L"figure", L"figcaption", L"address", L"center", L"dt", L"dd", L"dl", L"nav",
            L"li", L"body", L"hgroup", L"caption", L"details", L"summary"};
        for (auto b : kBlocks) if (n == b) return true;
        return false;
    }
    static int Heading(const std::wstring& n) {
        return n.size() == 2 && n[0] == L'h' && n[1] >= L'1' && n[1] <= L'6' ? n[1] - L'0' : 0;
    }
    static unsigned InlineFlags(const std::wstring& n) {
        if (n == L"b" || n == L"strong") return kDocBold;
        if (n == L"i" || n == L"em" || n == L"cite" || n == L"var" || n == L"dfn") return kDocItalic;
        if (n == L"u" || n == L"ins") return kDocUnderline;
        if (n == L"s" || n == L"strike" || n == L"del") return kDocStrike;
        if (n == L"code" || n == L"kbd" || n == L"tt" || n == L"samp") return kDocCode;
        return 0;
    }

    void ReadHead(XmlPull& x, std::wstring& title) {
        if (x.self_closing()) return;
        bool in_title = false;
        for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
            if (k == XmlPull::Open && x.name() == L"title") in_title = !x.self_closing();
            else if (k == XmlPull::Close && x.name() == L"title") in_title = false;
            else if (k == XmlPull::Close && x.name() == L"head") return;
            else if (k == XmlPull::Text && in_title) title += x.text();
        }
    }

    void EndBlock() { out_.End(); }

    void OnOpen(const XmlPull& x) {
        const std::wstring& n = x.name();
        if (const int h = Heading(n)) { EndBlock(); heading_ = h; return; }
        if (n == L"blockquote") { EndBlock(); ++quote_; return; }
        if (n == L"ul" || n == L"ol") {
            EndBlock();
            lists_.push_back({n == L"ol", ToInt(x.Attr(L"start"), 1)});
            return;
        }
        if (n == L"li") {
            EndBlock();
            if (!lists_.empty() && lists_.back().ordered) marker_ = L"o" + std::to_wstring(lists_.back().next++);
            else marker_ = L"u";
            return;
        }
        if (n == L"pre") { EndBlock(); ++pre_; return; }
        if (n == L"svg") { ++svg_; return; }
        if (n == L"br") { if (out_.open()) out_.Text(L"\n"); return; }
        if (n == L"hr") { EndBlock(); out_.Rule(); return; }
        if (n == L"img" || n == L"image") {
            std::wstring src = n == L"img" ? x.Attr(L"src") : x.QAttr(L"xlink:href");
            if (src.empty()) src = x.Attr(L"href");
            Picture(src, x.Attr(L"alt"));
            return;
        }
        if (IsBlock(n)) { EndBlock(); return; }
        unsigned flags = InlineFlags(n);
        std::wstring target;
        if (n == L"a") {
            const std::wstring href = x.Attr(L"href");
            const std::wstring lower = Lower(href);
            if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 || lower.rfind(L"mailto:", 0) == 0) {
                flags |= kDocLink;
                target = href;
            }
        }
        if (flags || n == L"a" || n == L"span") inline_.push_back({n, flags, target});
    }

    void OnClose(const std::wstring& n) {
        if (Heading(n)) { EndBlock(); heading_ = 0; return; }
        if (n == L"blockquote") { EndBlock(); if (quote_ > 0) --quote_; return; }
        if (n == L"ul" || n == L"ol") { EndBlock(); if (!lists_.empty()) lists_.pop_back(); marker_.clear(); return; }
        if (n == L"pre") { EndBlock(); if (pre_ > 0) --pre_; return; }
        if (n == L"svg") { if (svg_ > 0) --svg_; return; }
        if (IsBlock(n)) { EndBlock(); return; }
        for (size_t i = inline_.size(); i-- > 0;)
            if (inline_[i].name == n) { inline_.erase(inline_.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    }

    void OnText(const std::wstring& raw) {
        if (svg_ > 0) return;
        if (pre_ > 0) {
            std::wstring text;
            for (wchar_t c : raw) if (c != L'\r') text += c;
            if (!out_.open()) {
                // The first line break right after <pre> is not content.
                if (!text.empty() && text[0] == L'\n') text.erase(0, 1);
                if (text.empty()) return;
                out_.Begin(L'c', L"", quote_, static_cast<int>(lists_.size()));
            }
            out_.Text(text);
            return;
        }
        std::wstring text = Collapse(raw);
        if (!out_.open() || out_.text().empty() || out_.text().back() == L' ' || out_.text().back() == L'\n') {
            size_t lead = 0;
            while (lead < text.size() && text[lead] == L' ') ++lead;
            text.erase(0, lead);
        }
        if (text.empty()) return;
        if (!out_.open()) {
            out_.Begin(heading_ ? L'h' : L'p', heading_ ? std::to_wstring(heading_) : std::wstring(), quote_,
                       static_cast<int>(lists_.size()), marker_);
            marker_.clear();
        }
        unsigned flags = 0;
        std::wstring target;
        for (const Inline& in : inline_) {
            flags |= in.flags;
            if (!in.target.empty()) target = in.target;
        }
        out_.Text(text, flags, target);
    }

    void Picture(const std::wstring& src, const std::wstring& alt) {
        if (src.empty() || svg_ > 1) return;
        const std::wstring lower = Lower(src);
        if (lower.find(L"://") != std::wstring::npos || lower.rfind(L"data:", 0) == 0) return;
        const std::wstring name = ResolvePartName(dir_, src);
        std::vector<unsigned char> bytes;
        if (!ReadZipEntry(zip_, ToUtf8(name), 24u * 1024u * 1024u, bytes, nullptr)) return;
        const size_t dot = name.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L".png" : name.substr(dot);
        const std::wstring file = images_.Store(bytes, ext);
        if (file.empty()) return;
        out_.End();
        out_.Image(file, alt, quote_, static_cast<int>(lists_.size()));
    }

    struct Cell { std::vector<std::pair<std::wstring, unsigned>> pieces; bool header = false; };

    void ParseTable(XmlPull& x) {
        EndBlock();
        std::vector<std::vector<Cell>> rows;
        std::vector<Cell>* row = nullptr;
        Cell* cell = nullptr;
        int nested = 0, span = 1;
        std::vector<std::pair<std::wstring, unsigned>> stack;
        const auto append = [&](std::wstring text, unsigned flags) {
            if (!cell) return;
            if (cell->pieces.empty() || cell->pieces.back().first.empty() ||
                cell->pieces.back().first.back() == L' ' || cell->pieces.back().first.back() == L'\n') {
                size_t lead = 0;
                while (lead < text.size() && text[lead] == L' ') ++lead;
                text.erase(0, lead);
            }
            if (text.empty()) return;
            if (!cell->pieces.empty() && cell->pieces.back().second == flags) cell->pieces.back().first += text;
            else cell->pieces.push_back({text, flags});
        };
        for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x.name();
            if (k == XmlPull::Text) {
                unsigned flags = 0;
                for (const auto& s : stack) flags |= s.second;
                append(Collapse(x.text()), flags);
                continue;
            }
            if (k == XmlPull::Open) {
                RecordAnchor(x);
                if (n == L"table") { if (!x.self_closing()) ++nested; continue; }
                if (nested == 0 && n == L"tr") { rows.emplace_back(); row = &rows.back(); cell = nullptr; continue; }
                if (nested == 0 && (n == L"td" || n == L"th") && row) {
                    if (row->size() >= kMaxColumns) { cell = nullptr; continue; }
                    row->push_back(Cell{});
                    cell = &row->back();
                    cell->header = n == L"th";
                    span = std::clamp(ToInt(x.Attr(L"colspan"), 1), 1, 64);
                    if (x.self_closing()) cell = nullptr;
                    continue;
                }
                if (n == L"br" || IsBlock(n)) {
                    if (cell && !cell->pieces.empty()) append(L"\n", 0);
                    continue;
                }
                if (const unsigned f = InlineFlags(n); f && !x.self_closing()) stack.push_back({n, f});
                continue;
            }
            // Close
            if (n == L"table") { if (nested == 0) break; --nested; continue; }
            if (nested == 0 && (n == L"td" || n == L"th") && row) {
                for (int extra = 1; extra < span && row->size() < kMaxColumns; ++extra) row->push_back(Cell{});
                span = 1;
                cell = nullptr;
                continue;
            }
            if (nested == 0 && n == L"tr") { row = nullptr; cell = nullptr; continue; }
            for (size_t i = stack.size(); i-- > 0;)
                if (stack[i].first == n) { stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(i)); break; }
        }
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& r) { return r.empty(); }), rows.end());
        size_t columns = 0;
        for (const auto& r : rows) columns = (std::max)(columns, r.size());
        columns = (std::min)(columns, kMaxColumns);
        if (!columns || out_.full()) return;
        out_.TableBegin(columns, quote_, static_cast<int>(lists_.size()));
        for (size_t r = 0; r < rows.size(); ++r) {
            out_.TableRow();
            bool header = rows.size() > 1;
            for (const Cell& c : rows[r]) header = header && c.header;
            for (size_t c = 0; c < columns; ++c) {
                out_.Begin(L't', L"", 0, 0, header ? L"h" : L"");
                if (c < rows[r].size())
                    for (const auto& piece : rows[r][c].pieces) {
                        std::wstring text = piece.first;
                        while (!text.empty() && text.back() == L'\n') text.pop_back();
                        out_.Text(text, piece.second);
                    }
                out_.End(true);
            }
        }
        out_.TableEnd();
    }

    const std::wstring& zip_;
    DocPayload& out_;
    PreviewImageCache& images_;
    ChapterAnchors* anchors_ = nullptr;
    std::wstring dir_;
    size_t base_ = 0;
    int quote_ = 0, pre_ = 0, svg_ = 0, heading_ = 0;
    std::vector<List> lists_;
    std::vector<Inline> inline_;
    std::wstring marker_;
};

bool HasToken(std::wstring_view value, std::wstring_view token) {
    constexpr std::wstring_view spaces = L" \t\n\r\f";
    for (size_t start = value.find_first_not_of(spaces); start != value.npos;) {
        const auto end = value.find_first_of(spaces, start);
        if (value.substr(start, end - start) == token) return true;
        if (end == value.npos) break;
        start = value.find_first_not_of(spaces, end);
    }
    return false;
}

void ReadNav(const std::wstring& zip, const std::wstring& part, std::vector<TocEntry>& toc) {
    std::wstring text;
    if (!ReadPart(zip, part, text)) return;
    const std::wstring dir = DirOf(part);
    XmlPull x(text, true);
    bool in_toc = false, in_link = false, preferred = false, have_fallback = false;
    int depth = 0;
    TocEntry entry;
    // Prefer <nav epub:type="toc">; else the first <nav>.
    for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
        const std::wstring& n = x.name();
        if (k == XmlPull::Open) {
            if (n == L"nav" && !in_toc) {
                if (x.self_closing()) continue;
                preferred = HasToken(x.QAttr(L"epub:type"), L"toc") ||
                            HasToken(x.Attr(L"role"), L"doc-toc") || x.Attr(L"id") == L"toc";
                if (!preferred && have_fallback) { x.SkipElement(); continue; }
                if (preferred) toc.clear();
                in_toc = true;
                depth = 0;
                in_link = false;
            } else if (in_toc && n == L"ol") {
                ++depth;
            } else if (in_toc && toc.size() < kMaxToc && n == L"a" && !x.self_closing()) {
                in_link = true;
                entry = TocEntry{};
                entry.level = (std::max)(0, depth - 1);
                const std::wstring href = x.Attr(L"href");
                entry.file = ResolvePartName(dir, href);
                entry.fragment = Fragment(href);
            }
        } else if (k == XmlPull::Close) {
            if (in_toc && n == L"ol") --depth;
            else if (in_toc && n == L"a" && in_link) {
                in_link = false;
                entry.title = Collapse(entry.title);
                while (!entry.title.empty() && entry.title.front() == L' ') entry.title.erase(0, 1);
                while (!entry.title.empty() && entry.title.back() == L' ') entry.title.pop_back();
                if (!entry.title.empty() && !entry.file.empty()) toc.push_back(entry);
                if (preferred && toc.size() == kMaxToc) return;
            } else if (in_toc && n == L"nav") {
                if (preferred) return;
                have_fallback = true;
                in_toc = false;
            }
        } else if (k == XmlPull::Text && in_link) {
            entry.title += x.text();
        }
    }
}

void ReadNcx(const std::wstring& zip, const std::wstring& part, std::vector<TocEntry>& toc) {
    std::wstring text;
    if (!ReadPart(zip, part, text)) return;
    const std::wstring dir = DirOf(part);
    XmlPull x(text, false);
    int depth = 0;
    bool in_text = false;
    std::vector<std::wstring> titles;  // per open navPoint
    for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof && toc.size() < kMaxToc;) {
        const std::wstring& n = x.name();
        if (k == XmlPull::Open) {
            if (n == L"navPoint") { ++depth; titles.emplace_back(); }
            else if (n == L"text" && depth > 0) in_text = !x.self_closing();
            else if (n == L"content" && depth > 0) {
                TocEntry e;
                e.level = depth - 1;
                const std::wstring src = x.Attr(L"src");
                e.file = ResolvePartName(dir, src);
                e.fragment = Fragment(src);
                e.title = Collapse(titles.back());
                while (!e.title.empty() && e.title.front() == L' ') e.title.erase(0, 1);
                while (!e.title.empty() && e.title.back() == L' ') e.title.pop_back();
                if (!e.title.empty() && !e.file.empty()) toc.push_back(e);
            }
        } else if (k == XmlPull::Close) {
            if (n == L"navPoint" && depth > 0) { --depth; titles.pop_back(); }
            else if (n == L"text") in_text = false;
            else if (n == L"navMap") break;
        } else if (k == XmlPull::Text && in_text && !titles.empty()) {
            titles.back() += x.text();
        }
    }
}

}  // namespace

bool IsEpubExtension(std::wstring_view extension) {
    std::wstring e(extension);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    return e == L".epub";
}

bool MakeEpubDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                      bool& truncated) {
    // container.xml -> package document.
    std::wstring text;
    if (!ReadPart(path, L"META-INF/container.xml", text)) return false;
    std::wstring opf;
    {
        XmlPull x(text, false);
        for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;)
            if (k == XmlPull::Open && x.name() == L"rootfile") { opf = ResolvePartName(L"", x.Attr(L"full-path")); break; }
    }
    if (opf.empty() || !ReadPart(path, opf, text)) return false;
    const std::wstring opf_dir = DirOf(opf);

    struct Item { std::wstring href, type, properties; };
    std::map<std::wstring, Item> manifest;
    std::vector<std::wstring> spine;
    size_t total_spine = 0;
    std::wstring title, author, ncx_id, nav_part;
    {
        XmlPull x(text, false);
        std::wstring* capture = nullptr;
        bool in_metadata = false;
        for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x.name();
            if (k == XmlPull::Open) {
                if (n == L"metadata") in_metadata = true;
                else if (in_metadata && n == L"title" && title.empty() && !x.self_closing()) capture = &title;
                else if (in_metadata && n == L"creator" && author.empty() && !x.self_closing()) capture = &author;
                else if (n == L"item") {
                    Item item{ResolvePartName(opf_dir, x.Attr(L"href")), Lower(x.Attr(L"media-type")), x.Attr(L"properties")};
                    if (item.properties.find(L"nav") != std::wstring::npos) nav_part = item.href;
                    manifest[x.Attr(L"id")] = std::move(item);
                } else if (n == L"spine") {
                    ncx_id = x.Attr(L"toc");
                } else if (n == L"itemref") {
                    ++total_spine;
                    if (spine.size() < kMaxSpine) spine.push_back(x.Attr(L"idref"));
                }
            } else if (k == XmlPull::Close) {
                if (n == L"metadata") in_metadata = false;
                else if (n == L"title" || n == L"creator") capture = nullptr;
            } else if (k == XmlPull::Text && capture) {
                *capture += x.text();
            }
        }
    }
    title = Collapse(title);
    author = Collapse(author);
    for (std::wstring* s : {&title, &author}) {
        while (!s->empty() && s->front() == L' ') s->erase(0, 1);
        while (!s->empty() && s->back() == L' ') s->pop_back();
    }
    if (spine.empty()) return false;

    std::vector<TocEntry> toc;
    if (!nav_part.empty()) ReadNav(path, nav_part, toc);
    if (toc.empty() && !ncx_id.empty()) {
        const auto it = manifest.find(ncx_id);
        if (it != manifest.end()) ReadNcx(path, it->second.href, toc);
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
    bytes_read = ec ? 0u : static_cast<uint32_t>((std::min)(static_cast<std::uintmax_t>(size), std::uintmax_t{0xFFFFFFFFu}));

    // Keep enough room for every spine position even after content fills up.
    DocPayload out(ipc::kPreviewMaxTableChars - kTocReserve - spine.size() * 512);
    PreviewImageCache images;
    std::map<std::wstring, ChapterAnchors> anchors;
    std::map<std::wstring, std::wstring> first_title;
    for (const auto& entry : toc) {
        first_title.try_emplace(entry.file, entry.title.substr(0, 120));
        if (!entry.fragment.empty()) anchors[entry.file].try_emplace(entry.fragment, 0);
    }
    std::map<std::wstring, size_t> section_of;  // part -> section
    std::vector<std::wstring> section_titles;
    HtmlConverter converter(path, out, images);
    bool incomplete = total_spine > spine.size();
    size_t loaded_chapters = 0;
    for (const std::wstring& idref : spine) {
        const auto it = manifest.find(idref);
        const Item item = it == manifest.end() ? Item{} : it->second;
        const std::wstring lower = Lower(item.href);
        const bool html = item.type.find(L"html") != std::wstring::npos ||
                          lower.ends_with(L".xhtml") || lower.ends_with(L".html") || lower.ends_with(L".htm");
        std::wstring body, error, reason;
        if (out.full()) reason = L"Preview size limit reached; this chapter was not loaded.";
        else if (it == manifest.end()) reason = L"Chapter manifest reference is missing.";
        else if (!html) reason = item.type == L"image/svg+xml" || lower.ends_with(L".svg")
            ? L"SVG chapter is not supported by the reading preview. Open the original book to view it."
            : L"This chapter format is not supported by the reading preview.";
        else if (!ReadPart(path, item.href, body, &error)) reason = error == L"entry-too-large"
            ? L"Chapter exceeds the 16 MiB preview limit. Open the original book to read it."
            : L"Chapter content could not be read (missing or damaged package entry).";
        // The chapter's name: its first TOC entry, else its <title>.
        std::wstring name;
        if (const auto it = first_title.find(item.href); it != first_title.end()) name = it->second;
        const size_t section = section_titles.size();
        if (!reason.empty()) {
            incomplete = true;
            if (name.empty()) name = L"Chapter " + std::to_wstring(section + 1);
            DocPayload placeholder(512);
            std::wstring record = L"P\t";
            AppendPayloadField(record, name);
            placeholder.Record(record);
            placeholder.Begin(L'p');
            placeholder.Text(reason);
            placeholder.End();
            out.str() += placeholder.str().substr(placeholder.str().find(L'\n') + 1);
            if (!item.href.empty()) section_of.try_emplace(item.href, section);
            section_titles.push_back(name);
            continue;
        }
        const size_t base = out.blocks();
        std::wstring record = L"P\t";
        const size_t record_at = out.str().size();
        AppendPayloadField(record, name);
        out.Record(record);
        // The reserved placeholder budget also covers a P record at the limit.
        if (out.full()) out.str() += record + L'\n';
        std::wstring html_title;
        const auto targets = anchors.find(item.href);
        converter.Convert(item.href, body, targets == anchors.end() ? nullptr : &targets->second, base, html_title);
        if (!out.full()) ++loaded_chapters;
        if (out.full()) {
            DocPayload notice(256);
            notice.Begin(L'p');
            notice.Text(L"Preview size limit reached; this chapter may be incomplete.");
            notice.End();
            out.str() += notice.str().substr(notice.str().find(L'\n') + 1);
        }
        if (name.empty()) {
            name = Collapse(html_title).substr(0, 120);
            while (!name.empty() && name.front() == L' ') name.erase(0, 1);
            while (!name.empty() && name.back() == L' ') name.pop_back();
            if (!name.empty() && name != title) {
                // Name the section after its <title> (rewrite the P record).
                std::wstring named = L"P\t";
                AppendPayloadField(named, name);
                out.str().replace(record_at, record.size(), named);
            } else {
                name.clear();
            }
        }
        section_of.try_emplace(item.href, section);
        section_titles.push_back(name);
    }
    if (section_titles.empty()) return false;
    if (total_spine > spine.size()) {
        DocPayload notice(256);
        notice.Begin(L'p');
        notice.Text(L"Chapter limit reached: preview lists the first " + std::to_wstring(spine.size()) +
                    L" of " + std::to_wstring(total_spine) + L" chapters.");
        notice.End();
        out.str() += notice.str().substr(notice.str().find(L'\n') + 1);
    }
    truncated = out.full() || incomplete;

    // Table of contents: the book's own, else one entry per named chapter.
    std::wstring& s = out.str();
    const size_t cap = ipc::kPreviewMaxTableChars - 2048;  // M still fits after
    size_t entries = 0;
    for (const TocEntry& e : toc) {
        const auto sec = section_of.find(e.file);
        if (sec == section_of.end()) continue;
        size_t block = 0;
        if (!e.fragment.empty()) {
            block = anchors.at(e.file).at(e.fragment);
        }
        std::wstring record = L"C\t" + std::to_wstring((std::min)(e.level, 6)) + L'\t' +
            std::to_wstring(sec->second) + L'\t' + std::to_wstring(block) + L'\t';
        AppendPayloadField(record, e.title);
        record += L'\n';
        if (s.size() + record.size() > cap) { truncated = true; break; }
        s += record;
        ++entries;
    }
    if (!entries)
        for (size_t i = 0; i < section_titles.size(); ++i) {
            if (section_titles[i].empty()) continue;
            std::wstring record = L"C\t0\t" + std::to_wstring(i) + L"\t0\t";
            AppendPayloadField(record, section_titles[i]);
            record += L'\n';
            if (s.size() + record.size() > cap) { truncated = true; break; }
            s += record;
        }
    s += L"V\tchapters\t" + std::to_wstring(loaded_chapters) + L'\t' + std::to_wstring(total_spine) + L'\n';
    s += L"M\tepub\t";
    AppendPayloadField(s, title.substr(0, 400));
    s += L'\t';
    AppendPayloadField(s, author.substr(0, 400));
    s += L'\n';
    payload.swap(s);
    return true;
}

}  // namespace pulse::preview
