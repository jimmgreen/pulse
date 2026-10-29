// ai_rich_text.cpp — see ai_rich_text.h.
#include "ai_rich_text.h"

#include "../preview_host/markdown_document.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace pulse::ui::airich {
namespace {

std::vector<std::wstring_view> Split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring_view> out;
    size_t pos = 0;
    while (true) {
        const size_t end = s.find(sep, pos);
        if (end == std::wstring_view::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

std::wstring Unescape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            out.push_back(n == L't' ? L'\t' : n == L'n' ? L'\n' : n);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

int Hex(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// markdown_document.h percent-encodes only % , ; in a target.
std::wstring PercentDecode(std::wstring_view s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && Hex(s[i + 1]) >= 0 && Hex(s[i + 2]) >= 0) {
            const wchar_t c = static_cast<wchar_t>(Hex(s[i + 1]) * 16 + Hex(s[i + 2]));
            if (c == L'%' || c == L',' || c == L';') {
                out.push_back(c);
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

uint32_t ToU32(std::wstring_view s) {
    uint32_t v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') break;
        v = v * 10 + static_cast<uint32_t>(c - L'0');
        if (v > 100000000u) break;
    }
    return v;
}

// Characters that end a path written in prose (not inside `code`).
bool EndsPath(wchar_t c) {
    if (c <= L' ') return true;
    switch (c) {
    case L'<': case L'>': case L'"': case L'|': case L'?': case L'*': case L'`':
    case 0x3000: case 0xFF0C: case 0x3002: case 0xFF1B: case 0xFF1A: case 0xFF01: case 0xFF1F:
    case 0x3001: case 0xFF09: case 0x3011: case 0x300D: case 0x300F: case 0x201D: case 0x2019:
    case 0xFF08: case 0x3010: case 0x300C: case 0x300E: case 0x201C: case 0x2018:
        return true;
    default:
        return false;
    }
}

bool IsDriveStart(std::wstring_view t, size_t i) {
    return i + 2 < t.size() && ((t[i] >= L'A' && t[i] <= L'Z') || (t[i] >= L'a' && t[i] <= L'z')) &&
           t[i + 1] == L':' && (t[i + 2] == L'\\' || t[i + 2] == L'/');
}

bool IsUncStart(std::wstring_view t, size_t i) {
    return i + 2 < t.size() && t[i] == L'\\' && t[i + 1] == L'\\' && (std::iswalnum(t[i + 2]) || t[i + 2] > 0x7F);
}

bool WordChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

std::wstring Repeat(std::wstring_view s, int n) {
    std::wstring out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// Markdown for text[a, b) of a block with its runs re-applied.
std::wstring InlineMarkdown(const Block& b, uint32_t a, uint32_t e) {
    std::wstring out;
    // Boundaries where runs open or close.
    struct Edge {
        uint32_t at;
        bool open;
        const Run* run;
    };
    std::vector<Edge> edges;
    for (const Run& r : b.runs) {
        const uint32_t s0 = (std::max)(r.start, a), s1 = (std::min)(r.start + r.length, e);
        if (s1 <= s0) continue;
        edges.push_back({s0, true, &r});
        edges.push_back({s1, false, &r});
    }
    std::stable_sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
        if (x.at != y.at) return x.at < y.at;
        return !x.open && y.open;   // close before open at the same point
    });
    const auto open_of = [](const Run& r) {
        std::wstring m;
        if (r.flags & kLink) m += L"[";
        if (r.flags & kBold) m += L"**";
        if (r.flags & kItalic) m += L"*";
        if (r.flags & kStrike) m += L"~~";
        if (r.flags & (kCode | kPath)) m += L"`";
        return m;
    };
    const auto close_of = [](const Run& r) {
        std::wstring m;
        if (r.flags & (kCode | kPath)) m += L"`";
        if (r.flags & kStrike) m += L"~~";
        if (r.flags & kItalic) m += L"*";
        if (r.flags & kBold) m += L"**";
        if (r.flags & kLink) m += L"](" + r.target + L")";
        return m;
    };
    uint32_t at = a;
    size_t k = 0;
    while (at < e || k < edges.size()) {
        const uint32_t next = k < edges.size() ? edges[k].at : e;
        if (next > at) {
            out.append(b.text, at, next - at);
            at = next;
        }
        if (k >= edges.size()) break;
        out += edges[k].open ? open_of(*edges[k].run) : close_of(*edges[k].run);
        ++k;
    }
    return out;
}

bool SameTableRow(const Block& x, const Block& y) {
    return x.kind == Kind::Cell && y.kind == Kind::Cell && x.table == y.table && x.row == y.row;
}

bool ListLike(const Block& b) { return b.indent > 0; }

// What goes between block i-1 and block i when both are copied.
std::wstring Separator(const Block& prev, const Block& cur, bool markdown) {
    if (SameTableRow(prev, cur)) return markdown ? L" | " : L"\t";
    if (prev.kind == Kind::Cell && cur.kind == Kind::Cell && prev.table == cur.table) return L"\n";
    if (ListLike(prev) && ListLike(cur)) return L"\n";
    return L"\n\n";
}

} // namespace

bool LooksLikePath(std::wstring_view t) {
    if (t.size() < 3) return false;
    if (IsDriveStart(t, 0)) return t.find_first_of(L"<>\"|?*\n") == std::wstring_view::npos;
    if (IsUncStart(t, 0)) {
        const size_t sep = t.find(L'\\', 2);
        return sep != std::wstring_view::npos && sep + 1 < t.size() &&
               t.find_first_of(L"<>\"|?*\n") == std::wstring_view::npos;
    }
    return false;
}

void FindPaths(const std::wstring& text, std::vector<Run>& runs) {
    std::vector<Run> found;
    const auto covered = [&](uint32_t i, uint32_t* end, const Run** which) {
        for (const Run& r : runs)
            if (i >= r.start && i < r.start + r.length) {
                *end = r.start + r.length;
                *which = &r;
                return true;
            }
        return false;
    };
    const std::wstring_view t(text);
    for (uint32_t i = 0; i < t.size();) {
        uint32_t end = 0;
        const Run* r = nullptr;
        if (covered(i, &end, &r)) {
            // `C:\Users\…`: the whole code span is the path (spaces allowed).
            if ((r->flags & kCode) && !(r->flags & kLink) && i == r->start &&
                LooksLikePath(t.substr(r->start, r->length))) {
                Run p = *r;
                p.flags = (p.flags & ~kCode) | kPath;
                p.target = std::wstring(t.substr(r->start, r->length));
                found.push_back(p);
            }
            i = end;
            continue;
        }
        // Not inside a word or a URL (http://x.com/C:\…, a=C:\…).
        bool word_before = i > 0 && (WordChar(t[i - 1]) || std::wcschr(L"/\\:.@=", t[i - 1]) != nullptr);
        if (!word_before && i > 0) {
            uint32_t k = i;
            while (k > 0 && !std::iswspace(t[k - 1])) --k;
            word_before = t.substr(k, i - k).find(L"://") != std::wstring_view::npos;
        }
        if (!word_before && (IsDriveStart(t, i) || IsUncStart(t, i))) {
            uint32_t j = i + 2;
            while (j < t.size() && !EndsPath(t[j])) {
                uint32_t e2 = 0;
                const Run* r2 = nullptr;
                if (covered(j, &e2, &r2)) break;
                ++j;
            }
            // Sentence punctuation after a path is not part of it.
            while (j > i + 3 && (t[j - 1] == L'.' || t[j - 1] == L',' || t[j - 1] == L';' || t[j - 1] == L':' ||
                                 t[j - 1] == L')' || t[j - 1] == L']' || t[j - 1] == L'\''))
                --j;
            const std::wstring_view cand = t.substr(i, j - i);
            if (j > i + 2 && LooksLikePath(cand)) {
                Run p;
                p.start = i;
                p.length = j - i;
                p.flags = kPath;
                p.target = std::wstring(cand);
                found.push_back(p);
                i = j;
                continue;
            }
        }
        ++i;
    }
    if (found.empty()) return;
    // A code run that became a path is replaced; the rest merge in order.
    std::vector<Run> merged;
    for (const Run& r : runs) {
        bool replaced = false;
        for (const Run& p : found)
            if (p.start == r.start && p.length == r.length && (r.flags & kCode)) replaced = true;
        if (!replaced) merged.push_back(r);
    }
    merged.insert(merged.end(), found.begin(), found.end());
    std::stable_sort(merged.begin(), merged.end(), [](const Run& x, const Run& y) { return x.start < y.start; });
    runs.swap(merged);
}

Doc Parse(const std::wstring& markdown) {
    Doc doc;
    std::wstring payload;
    preview::AppendMarkdownBlocks(markdown, payload, 4u * 1024u * 1024u, 0);
    const std::wstring_view all(payload);
    size_t pos = 0;
    int table = -1, row = -1;
    while (pos < all.size()) {
        size_t end = all.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = all.size();
        const std::wstring_view line = all.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;
        const auto f = Split(line, L'\t');
        if (f[0] == L"T" && f.size() >= 2) {
            Table t;
            t.columns = static_cast<int>((std::min)(ToU32(f[1]), 64u));
            t.first = static_cast<int>(doc.blocks.size());
            doc.tables.push_back(t);
            table = static_cast<int>(doc.tables.size()) - 1;
            row = -1;
        } else if (f[0] == L"R") {
            if (table >= 0) {
                ++row;
                doc.tables[static_cast<size_t>(table)].rows = row + 1;
            }
        } else if (f[0] == L"E") {
            table = -1;
        } else if (f[0] == L"B" && f.size() >= 8) {
            Block b;
            const wchar_t k = f[1].empty() ? L'p' : f[1][0];
            b.kind = k == L'h' ? Kind::Heading : k == L'c' ? Kind::Code : k == L'r' ? Kind::Rule
                   : k == L't' ? Kind::Cell : k == L'x' ? Kind::Html : Kind::Para;
            const std::wstring arg = Unescape(f[2]);
            if (b.kind == Kind::Heading) b.level = std::clamp(static_cast<int>(ToU32(arg)), 1, 6);
            if (b.kind == Kind::Code) b.lang = arg;
            b.quote = static_cast<int>((std::min)(ToU32(f[3]), 8u));
            b.indent = static_cast<int>((std::min)(ToU32(f[4]), 8u));
            b.marker.assign(f[5]);
            b.text = Unescape(f[6]);
            for (const auto run_text : Split(f[7], L';')) {
                if (run_text.empty()) continue;
                const auto parts = Split(run_text, L',');
                if (parts.size() < 3) continue;
                Run run;
                run.start = ToU32(parts[0]);
                run.length = ToU32(parts[1]);
                run.flags = ToU32(parts[2]) & ~(kMath | kDisplayMath | kImage);
                if (parts.size() >= 4) run.target = PercentDecode(Unescape(parts[3]));
                if (run.start >= b.text.size() || !run.flags) continue;
                run.length = (std::min)(run.length, static_cast<uint32_t>(b.text.size()) - run.start);
                if (run.length) b.runs.push_back(std::move(run));
            }
            std::stable_sort(b.runs.begin(), b.runs.end(), [](const Run& x, const Run& y) { return x.start < y.start; });
            if (b.kind == Kind::Cell && table >= 0) {
                Table& t = doc.tables[static_cast<size_t>(table)];
                b.table = table;
                b.row = (std::max)(row, 0);
                b.col = 0;
                for (int i = static_cast<int>(doc.blocks.size()) - 1; i >= t.first; --i) {
                    const Block& p = doc.blocks[static_cast<size_t>(i)];
                    if (p.table == table && p.row == b.row) { b.col = p.col + 1; break; }
                    if (p.table == table && p.row < b.row) break;
                }
                b.align = arg.empty() ? L'-' : arg[0];
                b.header = b.marker == L"h";
                b.marker.clear();
                ++t.count;
            }
            if (b.kind != Kind::Code && b.kind != Kind::Html && b.kind != Kind::Rule) FindPaths(b.text, b.runs);
            doc.blocks.push_back(std::move(b));
        }
    }
    return doc;
}

std::wstring MarkerText(const Block& b) {
    if (b.marker.empty()) return {};
    const std::wstring pad = Repeat(L"  ", (std::max)(0, b.indent - 1));
    if (b.marker[0] == L'o') return pad + b.marker.substr(1) + L". ";
    if (b.marker == L"t0") return pad + L"- [ ] ";
    if (b.marker == L"t1") return pad + L"- [x] ";
    return pad + L"- ";
}

std::wstring CopyRange(const Doc& doc, Pos from, Pos to, bool markdown) {
    std::wstring out;
    if (doc.blocks.empty()) return out;
    const int last = static_cast<int>(doc.blocks.size()) - 1;
    from.block = std::clamp(from.block, 0, last);
    to.block = std::clamp(to.block, 0, last);
    if (to.block < from.block || (to.block == from.block && to.offset <= from.offset)) return out;
    const bool spans = from.block != to.block;
    for (int i = from.block; i <= to.block; ++i) {
        const Block& b = doc.blocks[static_cast<size_t>(i)];
        const Block* p = i > from.block ? &doc.blocks[static_cast<size_t>(i - 1)] : nullptr;
        const uint32_t n = static_cast<uint32_t>(b.text.size());
        const uint32_t a = i == from.block ? (std::min)(from.offset, n) : 0;
        const uint32_t e = i == to.block ? (std::min)(to.offset, n) : n;
        if (markdown && b.kind == Kind::Cell) {
            if (p && SameTableRow(*p, b)) {
                out += L" | ";
            } else {
                if (p && p->kind == Kind::Cell && p->table == b.table) {
                    out += L"\n";
                    if (p->header) {
                        // The header's separator row.
                        out += L"|";
                        const int cols = doc.tables[static_cast<size_t>(b.table)].columns;
                        for (int c = 0; c < cols; ++c) out += L" --- |";
                        out += L"\n";
                    }
                } else if (p) {
                    out += L"\n\n";
                }
                out += L"| ";
            }
            out += InlineMarkdown(b, a, e);
            const bool row_ends = i == to.block || !SameTableRow(b, doc.blocks[static_cast<size_t>(i + 1)]);
            if (row_ends) out += L" |";
            continue;
        }
        if (!markdown && b.kind == Kind::Rule) continue;   // plain text: no line for it, one gap
        if (p && !out.empty()) out += (markdown && p->kind == Kind::Cell) ? L"\n\n" : Separator(*p, b, markdown);
        if (b.kind == Kind::Rule) {
            out += L"---";
            continue;
        }
        if (markdown) {
            const std::wstring quote = Repeat(L"> ", b.quote);
            if (b.kind == Kind::Code) {
                std::wstring body;
                for (size_t k = a; k < e; ++k) {
                    body.push_back(b.text[k]);
                    if (b.text[k] == L'\n') body += quote;
                }
                out += quote + L"```" + b.lang + L"\n" + quote + body + L"\n" + quote + L"```";
                continue;
            }
            out += quote;
            if (a == 0) out += MarkerText(b);
            else if (b.indent > 0) out += Repeat(L"  ", b.indent);
            if (b.kind == Kind::Heading) out += Repeat(L"#", b.level) + L" ";
            out += InlineMarkdown(b, a, e);
            continue;
        }
        if (spans && a == 0 && b.kind != Kind::Cell && b.kind != Kind::Code) out += MarkerText(b);
        out.append(b.text, a, e - a);
    }
    return out;
}

std::wstring CopyAll(const Doc& doc, bool markdown) {
    if (doc.blocks.empty()) return {};
    const Block& last = doc.blocks.back();
    return CopyRange(doc, {0, 0}, {static_cast<int>(doc.blocks.size()) - 1, static_cast<uint32_t>(last.text.size())},
                     markdown);
}

std::wstring TableTsv(const Doc& doc, int table) {
    std::wstring out;
    if (table < 0 || table >= static_cast<int>(doc.tables.size())) return out;
    const Table& t = doc.tables[static_cast<size_t>(table)];
    int row = -1;
    for (int i = t.first; i < t.first + t.count && i < static_cast<int>(doc.blocks.size()); ++i) {
        const Block& b = doc.blocks[static_cast<size_t>(i)];
        if (b.row != row) {
            if (row >= 0) out += L"\n";
            row = b.row;
        } else {
            out += L"\t";
        }
        // A tab or new line inside a cell would break the grid.
        for (wchar_t c : b.text) out.push_back(c == L'\t' || c == L'\n' ? L' ' : c);
    }
    return out;
}

std::wstring ToPlain(const std::wstring& markdown) { return CopyAll(Parse(markdown), false); }

std::wstring ExtensionForLanguage(std::wstring_view lang) {
    std::wstring l;
    for (wchar_t c : lang) {
        if (c == L' ' || c == L'{' || c == L',') break;
        l.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    if (l.empty()) return {};
    struct Alias {
        const wchar_t* name;
        const wchar_t* ext;
    };
    static const Alias kAliases[] = {
        {L"powershell", L".ps1"}, {L"pwsh", L".ps1"}, {L"ps", L".ps1"}, {L"ps1", L".ps1"},
        {L"python", L".py"}, {L"py", L".py"}, {L"javascript", L".js"}, {L"js", L".js"}, {L"jsx", L".jsx"},
        {L"typescript", L".ts"}, {L"ts", L".ts"}, {L"tsx", L".tsx"}, {L"c", L".c"}, {L"cpp", L".cpp"},
        {L"c++", L".cpp"}, {L"cc", L".cpp"}, {L"h", L".h"}, {L"hpp", L".hpp"}, {L"csharp", L".cs"},
        {L"cs", L".cs"}, {L"c#", L".cs"}, {L"java", L".java"}, {L"kotlin", L".kt"}, {L"go", L".go"},
        {L"golang", L".go"}, {L"rust", L".rs"}, {L"rs", L".rs"}, {L"json", L".json"}, {L"jsonc", L".json"},
        {L"bash", L".sh"}, {L"sh", L".sh"}, {L"shell", L".sh"}, {L"zsh", L".sh"}, {L"bat", L".bat"},
        {L"batch", L".bat"}, {L"cmd", L".bat"}, {L"sql", L".sql"}, {L"xml", L".xml"}, {L"html", L".html"},
        {L"css", L".css"}, {L"scss", L".scss"}, {L"yaml", L".yaml"}, {L"yml", L".yaml"}, {L"toml", L".toml"},
        {L"ini", L".ini"}, {L"reg", L".reg"}, {L"lua", L".lua"}, {L"php", L".php"}, {L"ruby", L".rb"},
        {L"rb", L".rb"}, {L"swift", L".swift"}, {L"vb", L".vb"}, {L"diff", L".diff"}, {L"patch", L".diff"},
        {L"markdown", L".md"}, {L"md", L".md"}, {L"makefile", L".mk"}, {L"cmake", L".cmake"},
        {L"dockerfile", L".dockerfile"},
    };
    for (const Alias& a : kAliases)
        if (l == a.name) return a.ext;
    return L"." + l;
}

} // namespace pulse::ui::airich
