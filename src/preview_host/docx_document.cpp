// docx_document.cpp — DOCX to the Markdown payload (see docx_document.h).
#include "docx_document.h"

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

constexpr size_t kMaxPartBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxColumns = 64;

std::wstring ReadXmlPart(const std::wstring& path, const std::wstring& name) {
    std::vector<unsigned char> bytes;
    if (!ReadZipEntry(path, ToUtf8(name), kMaxPartBytes, bytes, nullptr)) return {};
    return DecodeDocumentText(bytes);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

int ToInt(const std::wstring& s, int fallback) {
    if (s.empty()) return fallback;
    int v = 0;
    bool any = false;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') break;
        v = v * 10 + (c - L'0');
        any = true;
        if (v > 100000) break;
    }
    return any ? v : fallback;
}

// w:b, w:i ... : on unless w:val says otherwise.
bool On(const XmlPull& x) {
    const std::wstring v = Lower(x.Attr(L"val"));
    return !(v == L"0" || v == L"false" || v == L"off" || v == L"none");
}

struct Relationship { std::wstring target; bool external = false; };

std::map<std::wstring, Relationship> ReadRelationships(const std::wstring& path) {
    std::map<std::wstring, Relationship> rels;
    const std::wstring text = ReadXmlPart(path, L"word/_rels/document.xml.rels");
    XmlPull x(text, false);
    for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
        if (k != XmlPull::Open || x.name() != L"Relationship") continue;
        Relationship r;
        r.external = Lower(x.Attr(L"TargetMode")) == L"external";
        const std::wstring target = x.Attr(L"Target");
        r.target = r.external ? target : ResolvePartName(L"word/", target);
        rels[x.Attr(L"Id")] = r;
    }
    return rels;
}

struct Style {
    int heading = 0;  // 1-6
    bool quote = false;
    int num_id = -1, ilvl = -1;
    std::wstring based_on;
};

std::map<std::wstring, Style> ReadStyles(const std::wstring& path) {
    std::map<std::wstring, Style> styles;
    const std::wstring text = ReadXmlPart(path, L"word/styles.xml");
    XmlPull x(text, false);
    Style* cur = nullptr;
    for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
        if (k == XmlPull::Close && x.name() == L"style") { cur = nullptr; continue; }
        if (k != XmlPull::Open) continue;
        const std::wstring& n = x.name();
        if (n == L"style") {
            cur = &styles[x.Attr(L"styleId")];
            continue;
        }
        if (!cur) continue;
        if (n == L"name") {
            const std::wstring v = Lower(x.Attr(L"val"));
            if (v == L"title") cur->heading = 1;
            else if (v.rfind(L"heading ", 0) == 0) cur->heading = ToInt(v.substr(8), 0);
            else if (v.rfind(L"\x6807\x9898 ", 0) == 0) cur->heading = ToInt(v.substr(3), 0);  // 标题 N
            else if (v == L"quote" || v == L"intense quote") cur->quote = true;
            if (cur->heading > 6) cur->heading = 0;
        } else if (n == L"basedOn") {
            cur->based_on = x.Attr(L"val");
        } else if (n == L"outlineLvl") {
            const int level = ToInt(x.Attr(L"val"), 9);
            if (level < 6 && !cur->heading) cur->heading = level + 1;
        } else if (n == L"numId") {
            cur->num_id = ToInt(x.Attr(L"val"), -1);
        } else if (n == L"ilvl") {
            cur->ilvl = ToInt(x.Attr(L"val"), -1);
        } else if (n == L"rPr") {
            x.SkipElement();
        }
    }
    return styles;
}

struct Level { bool bullet = false, none = false; int start = 1; };
struct Numbering {
    std::map<int, std::vector<Level>> abstracts;
    std::map<int, int> nums;  // numId -> abstractNumId
    const Level* Find(int num_id, int ilvl) const {
        const auto n = nums.find(num_id);
        if (n == nums.end()) return nullptr;
        const auto a = abstracts.find(n->second);
        if (a == abstracts.end() || ilvl < 0 || ilvl >= static_cast<int>(a->second.size())) return nullptr;
        return &a->second[ilvl];
    }
};

Numbering ReadNumbering(const std::wstring& path) {
    Numbering numbering;
    const std::wstring text = ReadXmlPart(path, L"word/numbering.xml");
    XmlPull x(text, false);
    std::vector<Level>* abstract = nullptr;
    Level* level = nullptr;
    int num = -1;
    for (XmlPull::Kind k; (k = x.Next()) != XmlPull::Eof;) {
        const std::wstring& n = x.name();
        if (k == XmlPull::Close) {
            if (n == L"abstractNum") abstract = nullptr;
            else if (n == L"lvl") level = nullptr;
            else if (n == L"num") num = -1;
            continue;
        }
        if (k != XmlPull::Open) continue;
        if (n == L"abstractNum") {
            abstract = &numbering.abstracts[ToInt(x.Attr(L"abstractNumId"), -1)];
            abstract->assign(9, Level{});
        } else if (n == L"lvl" && abstract) {
            const int ilvl = ToInt(x.Attr(L"ilvl"), -1);
            level = ilvl >= 0 && ilvl < 9 ? &(*abstract)[ilvl] : nullptr;
        } else if (n == L"numFmt" && level) {
            const std::wstring v = x.Attr(L"val");
            level->bullet = v == L"bullet";
            level->none = v == L"none";
        } else if (n == L"start" && level) {
            level->start = ToInt(x.Attr(L"val"), 1);
        } else if (n == L"num") {
            num = ToInt(x.Attr(L"numId"), -1);
        } else if (n == L"abstractNumId" && num >= 0) {
            numbering.nums[num] = ToInt(x.Attr(L"val"), -1);
        } else if (n == L"rPr" || n == L"pPr") {
            if (level || abstract) x.SkipElement();
        }
    }
    return numbering;
}

struct Piece { std::wstring text; unsigned flags = 0; std::wstring target; };
struct Picture { std::wstring path, alt; };
struct Para {
    std::vector<Piece> pieces;
    std::vector<Picture> pictures;
    std::wstring style;
    int outline = 0;
    int num_id = -1, ilvl = -1;
    size_t size() const { size_t n = 0; for (const auto& p : pieces) n += p.text.size(); return n; }
};
struct Cell { std::vector<Piece> pieces; };

class Converter {
public:
    Converter(const std::wstring& path, std::wstring_view document, DocPayload& out)
        : path_(path), x_(document, false), out_(out) {
        rels_ = ReadRelationships(path);
        styles_ = ReadStyles(path);
        numbering_ = ReadNumbering(path);
    }

    void Run() {
        for (XmlPull::Kind k; !out_.full() && (k = x_.Next()) != XmlPull::Eof;) {
            if (k != XmlPull::Open) continue;
            const std::wstring& n = x_.name();
            if (n == L"p") {
                Para p;
                ParseParagraph(p);
                Emit(p);
            } else if (n == L"tbl") {
                ParseTable();
            } else if (n == L"sectPr" || n == L"txbxContent" || n == L"Fallback" || n == L"del" ||
                       n == L"moveFrom") {
                x_.SkipElement();
            }
        }
    }

    size_t words() const noexcept { return words_; }

private:
    // One Style lookup through basedOn.
    Style Resolve(const std::wstring& id) const {
        Style result;
        bool heading = false, quote = false, num = false;
        std::wstring cur = id;
        for (int hop = 0; hop < 10 && !cur.empty(); ++hop) {
            const auto it = styles_.find(cur);
            if (it == styles_.end()) break;
            const Style& s = it->second;
            if (!heading && s.heading) { result.heading = s.heading; heading = true; }
            if (!quote && s.quote) { result.quote = true; quote = true; }
            if (!num && s.num_id >= 0) { result.num_id = s.num_id; result.ilvl = s.ilvl < 0 ? 0 : s.ilvl; num = true; }
            cur = s.based_on;
        }
        return result;
    }

    void AddText(Para& p, const std::wstring& text, unsigned flags) {
        if (text.empty()) return;
        const std::wstring& target = link_;
        const unsigned f = flags | (target.empty() ? 0u : kDocLink);
        if (!p.pieces.empty() && p.pieces.back().flags == f && p.pieces.back().target == target) {
            p.pieces.back().text += text;
            return;
        }
        p.pieces.push_back({text, f, target});
    }

    void AddPicture(Para& p, const std::wstring& id, const std::wstring& alt) {
        const auto it = rels_.find(id);
        if (it == rels_.end() || it->second.external) return;
        std::vector<unsigned char> bytes;
        if (!ReadZipEntry(path_, ToUtf8(it->second.target), 24u * 1024u * 1024u, bytes, nullptr)) return;
        const size_t dot = it->second.target.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L".png" : it->second.target.substr(dot);
        const std::wstring file = images_.Store(bytes, ext);
        if (!file.empty()) p.pictures.push_back({file, alt});
    }

    void ParseRun(Para& p) {
        unsigned flags = 0;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x_.name();
            if (k == XmlPull::Close) {
                if (n == L"r") return;
                continue;
            }
            if (k == XmlPull::Text) {
                if (in_text_) AddText(p, x_.text(), flags);
                continue;
            }
            // Open
            if (n == L"rPr") {
                ParseRunProperties(flags);
            } else if (n == L"t") {
                if (x_.self_closing()) continue;
                in_text_ = true;
                for (XmlPull::Kind t; (t = x_.Next()) != XmlPull::Eof;) {
                    if (t == XmlPull::Text) AddText(p, x_.text(), flags);
                    else if (t == XmlPull::Close && x_.name() == L"t") break;
                }
                in_text_ = false;
            } else if (n == L"tab" || n == L"ptab") {
                AddText(p, L"\t", flags);
            } else if (n == L"br") {
                const std::wstring type = x_.Attr(L"type");
                if (type != L"page" && type != L"column") AddText(p, L"\n", flags);
            } else if (n == L"cr") {
                AddText(p, L"\n", flags);
            } else if (n == L"noBreakHyphen") {
                AddText(p, L"-", flags);
            } else if (n == L"drawing") {
                ParseDrawing(p, L"drawing");
            } else if (n == L"pict" || n == L"object") {
                ParseDrawing(p, n == L"pict" ? L"pict" : L"object");
            } else if (n == L"delText" || n == L"instrText" || n == L"footnoteReference" ||
                       n == L"endnoteReference" || n == L"commentReference" || n == L"Fallback") {
                x_.SkipElement();
            }
        }
    }

    void ParseRunProperties(unsigned& flags) {
        if (x_.self_closing()) return;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            if (k == XmlPull::Close && x_.name() == L"rPr") return;
            if (k != XmlPull::Open) continue;
            const std::wstring& n = x_.name();
            if (n == L"b") flags = On(x_) ? flags | kDocBold : flags & ~kDocBold;
            else if (n == L"i") flags = On(x_) ? flags | kDocItalic : flags & ~kDocItalic;
            else if (n == L"u") flags = On(x_) ? flags | kDocUnderline : flags & ~kDocUnderline;
            else if (n == L"strike" || n == L"dstrike") flags = On(x_) ? flags | kDocStrike : flags & ~kDocStrike;
        }
    }

    // Pictures (a:blip / v:imagedata); text boxes inside are skipped.
    void ParseDrawing(Para& p, const std::wstring& element) {
        if (x_.self_closing()) return;
        std::wstring alt;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x_.name();
            if (k == XmlPull::Close && n == element) return;
            if (k != XmlPull::Open) continue;
            if (n == L"txbxContent" || n == L"Fallback") x_.SkipElement();
            else if (n == L"docPr") alt = x_.Attr(L"descr");
            else if (n == L"blip") AddPicture(p, x_.QAttr(L"r:embed").empty() ? x_.Attr(L"embed") : x_.QAttr(L"r:embed"), alt);
            else if (n == L"imagedata") AddPicture(p, x_.QAttr(L"r:id").empty() ? x_.Attr(L"id") : x_.QAttr(L"r:id"), alt);
        }
    }

    void ParseParagraph(Para& p) {
        if (x_.self_closing()) return;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x_.name();
            if (k == XmlPull::Close) {
                if (n == L"p") return;
                if (n == L"hyperlink") link_.clear();
                continue;
            }
            if (k != XmlPull::Open) continue;
            if (n == L"pPr") {
                ParseParagraphProperties(p);
            } else if (n == L"r") {
                if (!x_.self_closing()) ParseRun(p);
            } else if (n == L"hyperlink") {
                const std::wstring id = x_.QAttr(L"r:id").empty() ? x_.Attr(L"id") : x_.QAttr(L"r:id");
                const auto it = rels_.find(id);
                link_.clear();
                if (it != rels_.end() && it->second.external) {
                    const std::wstring lower = Lower(it->second.target);
                    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 ||
                        lower.rfind(L"mailto:", 0) == 0)
                        link_ = it->second.target;
                }
                if (x_.self_closing()) link_.clear();
            } else if (n == L"del" || n == L"moveFrom" || n == L"txbxContent" || n == L"Fallback") {
                x_.SkipElement();
            }
            // ins, smartTag, sdt, sdtContent, fldSimple, customXml: their runs follow.
        }
    }

    void ParseParagraphProperties(Para& p) {
        if (x_.self_closing()) return;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x_.name();
            if (k == XmlPull::Close && n == L"pPr") return;
            if (k != XmlPull::Open) continue;
            if (n == L"pStyle") p.style = x_.Attr(L"val");
            else if (n == L"numId") p.num_id = ToInt(x_.Attr(L"val"), -1);
            else if (n == L"ilvl") p.ilvl = ToInt(x_.Attr(L"val"), 0);
            else if (n == L"outlineLvl") { const int l = ToInt(x_.Attr(L"val"), 9); if (l < 6) p.outline = l + 1; }
            else if (n == L"rPr" || n == L"sectPr" || n == L"pPrChange") x_.SkipElement();
        }
    }

    void Count(const std::vector<Piece>& pieces) {
        for (const auto& piece : pieces) words_ += CountWords(piece.text);
    }

    void Emit(Para& p) {
        const Style style = Resolve(p.style);
        const int heading = p.outline ? p.outline : style.heading;
        int num_id = p.num_id >= 0 ? p.num_id : style.num_id;
        int ilvl = p.num_id >= 0 ? (p.ilvl < 0 ? 0 : p.ilvl) : style.ilvl;
        std::wstring marker;
        int indent = 0;
        if (num_id > 0 && !heading) {
            ilvl = (std::clamp)(ilvl, 0, 8);
            const Level* level = numbering_.Find(num_id, ilvl);
            if (level && !level->none) {
                indent = ilvl + 1;
                if (level->bullet) {
                    marker = L"u";
                } else {
                    auto& counters = counters_[num_id];
                    counters.resize(9, 0);
                    for (int deeper = ilvl + 1; deeper < 9; ++deeper) counters[deeper] = 0;
                    const int value = counters[ilvl] == 0 ? level->start : counters[ilvl] + 1;
                    counters[ilvl] = value;
                    marker = L"o" + std::to_wstring(value);
                }
            }
        }
        Count(p.pieces);
        const int quote = style.quote ? 1 : 0;
        if (p.size()) {
            out_.Begin(heading ? L'h' : L'p', heading ? std::to_wstring(heading) : std::wstring(), quote, indent, marker);
            for (const auto& piece : p.pieces) out_.Text(piece.text, piece.flags, piece.target);
            out_.End();
        }
        for (const auto& picture : p.pictures) out_.Image(picture.path, picture.alt, quote, indent);
    }

    void ParseTable() {
        std::vector<std::vector<Cell>> rows;
        bool header_look = false, header_row = false;
        int nested = 0;
        std::vector<Cell>* row = nullptr;
        Cell* cell = nullptr;
        int span = 1;
        bool continued = false;
        for (XmlPull::Kind k; (k = x_.Next()) != XmlPull::Eof;) {
            const std::wstring& n = x_.name();
            if (k == XmlPull::Close) {
                if (n == L"tbl") {
                    if (nested == 0) break;
                    --nested;
                } else if (nested == 0 && n == L"tc" && row && cell) {
                    if (continued) cell->pieces.clear();
                    for (int extra = 1; extra < span && row->size() < kMaxColumns; ++extra) row->push_back(Cell{});
                    cell = nullptr;
                } else if (nested == 0 && n == L"tr") {
                    row = nullptr;
                }
                continue;
            }
            if (k != XmlPull::Open) continue;
            if (n == L"tbl") { if (!x_.self_closing()) ++nested; continue; }
            if (nested == 0 && n == L"tblLook") {
                const std::wstring first = Lower(x_.Attr(L"firstRow"));
                const std::wstring val = x_.Attr(L"val");
                unsigned bits = 0;
                for (wchar_t c : val) {
                    bits <<= 4;
                    if (c >= L'0' && c <= L'9') bits |= static_cast<unsigned>(c - L'0');
                    else if (c >= L'a' && c <= L'f') bits |= static_cast<unsigned>(c - L'a' + 10);
                    else if (c >= L'A' && c <= L'F') bits |= static_cast<unsigned>(c - L'A' + 10);
                }
                header_look = first == L"1" || first == L"true" || (first.empty() && (bits & 0x20));
            } else if (nested == 0 && n == L"tr") {
                rows.emplace_back();
                row = &rows.back();
            } else if (nested == 0 && n == L"tblHeader" && rows.size() == 1) {
                header_row = On(x_);
            } else if (nested == 0 && n == L"tc" && row) {
                if (row->size() >= kMaxColumns) { x_.SkipElement(); continue; }
                row->push_back(Cell{});
                cell = &row->back();
                span = 1;
                continued = false;
            } else if (nested == 0 && n == L"gridSpan") {
                span = (std::clamp)(ToInt(x_.Attr(L"val"), 1), 1, 64);
            } else if (nested == 0 && n == L"vMerge") {
                const std::wstring v = x_.Attr(L"val");
                continued = v.empty() || v == L"continue";
            } else if (n == L"p") {
                Para p;
                ParseParagraph(p);
                if (!cell) continue;
                Count(p.pieces);
                if (!p.size()) continue;
                if (!cell->pieces.empty()) cell->pieces.push_back({nested ? L" " : L"\n", 0, {}});
                for (auto& piece : p.pieces) cell->pieces.push_back(std::move(piece));
            } else if (n == L"txbxContent" || n == L"Fallback" || n == L"del") {
                x_.SkipElement();
            }
        }
        size_t columns = 0;
        for (const auto& r : rows) columns = (std::max)(columns, r.size());
        columns = (std::min)(columns, kMaxColumns);
        if (!columns || out_.full()) return;
        out_.TableBegin(columns);
        for (size_t r = 0; r < rows.size(); ++r) {
            out_.TableRow();
            const bool header = r == 0 && (header_look || header_row) && rows.size() > 1;
            for (size_t c = 0; c < columns; ++c) {
                out_.Begin(L't', L"", 0, 0, header ? L"h" : L"");
                if (c < rows[r].size())
                    for (const auto& piece : rows[r][c].pieces) {
                        std::wstring text = piece.text;
                        std::replace(text.begin(), text.end(), L'\t', L' ');
                        out_.Text(text, piece.flags, piece.target);
                    }
                out_.End(true);
            }
        }
        out_.TableEnd();
    }

    const std::wstring& path_;
    XmlPull x_;
    DocPayload& out_;
    std::map<std::wstring, Relationship> rels_;
    std::map<std::wstring, Style> styles_;
    Numbering numbering_;
    std::map<int, std::vector<int>> counters_;
    PreviewImageCache images_;
    std::wstring link_;
    bool in_text_ = false;
    size_t words_ = 0;
};

}  // namespace

size_t CountWords(std::wstring_view text) {
    size_t count = 0;
    bool in_word = false;
    for (wchar_t c : text) {
        const bool cjk = (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
                         (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
                         (c >= 0xF900 && c <= 0xFAFF);
        if (cjk) { ++count; in_word = false; continue; }
        const bool word = std::iswalnum(c) || c == L'\'' || (c >= 0xC0 && c < 0x2000 && std::iswalpha(c));
        if (word && !in_word) ++count;
        in_word = word;
    }
    return count;
}

bool IsDocxExtension(std::wstring_view extension) {
    std::wstring e(extension);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    return e == L".docx" || e == L".docm" || e == L".dotx" || e == L".dotm";
}

bool MakeDocxDocument(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                      bool& truncated) {
    const std::wstring document = ReadXmlPart(path, L"word/document.xml");
    if (document.empty() || document.find(L"document") == std::wstring::npos) return false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
    bytes_read = ec ? 0u : static_cast<uint32_t>((std::min)(static_cast<std::uintmax_t>(size), std::uintmax_t{0xFFFFFFFFu}));
    DocPayload out(ipc::kPreviewMaxTableChars - 256);
    Converter converter(path, document, out);
    converter.Run();
    truncated = out.full();
    if (out.blocks() == 0 && !truncated) return false;  // nothing we can show
    // Facts go in after the budget check: they are tiny.
    out.str() += L"M\tdocx\t" + std::to_wstring(converter.words()) + L'\n';
    payload.swap(out.str());
    return true;
}

}  // namespace pulse::preview
