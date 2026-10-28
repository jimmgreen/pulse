// table_document.cpp — see table_document.h.
#include "table_document.h"
#include "xlsx_parts.h"

#include "zip_entry.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string_view>
#include <vector>

namespace pulse::preview {
namespace {

constexpr size_t kMaxCsvBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxRows = 5000;
constexpr size_t kMaxColumns = 256;
constexpr size_t kMaxCellChars = 2000;
constexpr size_t kMaxSourceChars = 64u * 1024u;
constexpr size_t kMaxXlsxEntryBytes = 48u * 1024u * 1024u;
constexpr ULONGLONG kXlsxBudgetMs = 2500;

using Row = std::vector<std::wstring>;
size_t SafePrefix(std::wstring_view text, size_t limit) {
    size_t count = (std::min)(text.size(), limit);
    if (count < text.size() && count && text[count - 1] >= 0xD800 && text[count - 1] <= 0xDBFF &&
        text[count] >= 0xDC00 && text[count] <= 0xDFFF) --count;
    return count;
}

void AppendEscaped(std::wstring& out, std::wstring_view text) {
    for (const wchar_t c : text) {
        if (c == L'\x0001') out += L"\\B";  // bold marker (XLSX)
        else if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') continue;
        else out += c;
    }
}

// Appends one sheet; rows that would push the payload past `budget` characters
// are dropped and the sheet is marked truncated.
bool AppendSheet(std::wstring& out, const wchar_t* kind, std::wstring_view name,
                 const std::vector<Row>& rows, size_t columns, bool truncated,
                 std::wstring_view detail, bool header, size_t budget) {
    std::wstring body;
    size_t written = 0;
    for (const Row& row : rows) {
        const size_t mark = body.size();
        body += L'R';
        for (size_t c = 0; c < columns; ++c) {
            body += L'\t';
            if (c < row.size()) {
                const std::wstring_view cell(row[c]);
                AppendEscaped(body, std::wstring_view(kind) == L"xlsx" && cell.size() >= 9 && cell[0] == L'\x0002' ? cell.substr(9) : cell);
            }
        }
        body += L'\n';
        for (size_t c = 0; c < row.size(); ++c) {
            if (std::wstring_view(kind) == L"xlsx" && row[c].size() >= 9 && row[c][0] == L'\x0002')
                body += L"F\t" + std::to_wstring(written) + L"\t" + std::to_wstring(c) + L"\t" + row[c].substr(1, 8) + L"\n";
        }
        if (out.size() + body.size() + 512 > budget) { body.resize(mark); truncated = true; break; }
        ++written;
    }
    out += L"S\t";
    out += kind;
    out += L'\t';
    AppendEscaped(out, name);
    out += L'\t' + std::to_wstring(columns) + L'\t' + std::to_wstring(written) + L'\t' +
           (truncated ? L"1" : L"0") + L'\t';
    AppendEscaped(out, detail);
    out += header ? L"\t1\n" : L"\t0\n";
    out += body;
    return truncated;
}

bool LooksNumeric(std::wstring_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) ++i;
    if (i < s.size() && (s[i] == L'$' || s[i] == L'\x00A5' || s[i] == L'\x20AC')) ++i;
    bool digit = false;
    for (; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= L'0' && c <= L'9') digit = true;
        else if (c != L'.' && c != L',' && c != L'%' && c != L' ' && c != L'e' && c != L'E')
            return false;
    }
    return digit;
}

// ---------------------------------------------------------------- CSV ----

bool DecodeBytes(std::vector<char>& bytes, bool cut, std::wstring& text,
                 ipc::PreviewTextEncoding& encoding) {
    encoding = ipc::PreviewTextEncoding::Utf8;
    size_t offset = 0;
    if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF &&
        static_cast<uint8_t>(bytes[1]) == 0xFE) {
        encoding = ipc::PreviewTextEncoding::Utf16Le;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>(static_cast<uint8_t>(bytes[i]) |
                                                (static_cast<uint8_t>(bytes[i + 1]) << 8)));
        return true;
    }
    if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFE &&
        static_cast<uint8_t>(bytes[1]) == 0xFF) {
        encoding = ipc::PreviewTextEncoding::Utf16Be;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((static_cast<uint8_t>(bytes[i]) << 8) |
                                                static_cast<uint8_t>(bytes[i + 1])));
        return true;
    }
    if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
        static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF) {
        offset = 3;
        encoding = ipc::PreviewTextEncoding::Utf8Bom;
    }
    size_t size = bytes.size();
    if (cut) {
        // A partial read may end inside a multi-byte sequence: stop at the last line.
        while (size > offset && bytes[size - 1] != '\n') --size;
    }
    const char* raw = bytes.data() + offset;
    const int raw_size = static_cast<int>(size - offset);
    if (raw_size <= 0) return true;
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
    if (chars <= 0) {
        code_page = CP_ACP;
        flags = 0;
        encoding = ipc::PreviewTextEncoding::Ansi;
        chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
    }
    if (chars <= 0) return false;
    text.resize(static_cast<size_t>(chars));
    MultiByteToWideChar(code_page, flags, raw, raw_size, text.data(), chars);
    return true;
}

wchar_t DetectDelimiter(std::wstring_view text, std::wstring_view extension) {
    if (extension == L".tsv" || extension == L".tab") return L'\t';
    if (extension == L".psv") return L'|';
    const wchar_t candidates[] = {L',', L';', L'\t', L'|'};
    size_t best_score = 0;
    wchar_t best = L',';
    for (const wchar_t d : candidates) {
        // Count per line outside quotes over the first lines; prefer
        // delimiters that appear the same number of times on every line.
        std::vector<size_t> counts;
        size_t count = 0;
        bool quoted = false;
        for (size_t i = 0; i < text.size() && counts.size() < 30; ++i) {
            const wchar_t c = text[i];
            if (c == L'"') quoted = !quoted;
            else if (!quoted && c == d) ++count;
            else if (!quoted && c == L'\n') { counts.push_back(count); count = 0; }
        }
        if (count) counts.push_back(count);
        if (counts.empty()) continue;
        std::map<size_t, size_t> histogram;
        for (const size_t n : counts) if (n) ++histogram[n];
        size_t mode = 0, freq = 0;
        for (const auto& [n, f] : histogram) if (f > freq) { mode = n; freq = f; }
        const size_t score = freq * 1000 + mode;
        if (mode && score > best_score) { best_score = score; best = d; }
    }
    return best;
}

bool ParseCsv(std::wstring_view text, wchar_t delimiter, std::vector<Row>& rows,
              size_t& columns, bool& truncated) {
    Row row;
    std::wstring cell;
    bool quoted = false, cell_started = false, capped = false;
    auto end_cell = [&] {
        if (row.size() < kMaxColumns) row.push_back(std::move(cell));
        else truncated = true;
        cell.clear();
        cell_started = false;
        capped = false;
    };
    auto end_row = [&] {
        end_cell();
        const bool blank = row.size() == 1 && row[0].empty();
        if (!blank) {
            columns = (std::max)(columns, row.size());
            rows.push_back(std::move(row));
        }
        row.clear();
        return rows.size() < kMaxRows;
    };
    auto push = [&](wchar_t c) {
        if (c == L'\x0001') return;  // reserved for the bold marker
        if (cell.size() < kMaxCellChars) cell += c;
        else if (!capped) {
            if (!cell.empty() && cell.back() >= 0xD800 && cell.back() <= 0xDBFF &&
                c >= 0xDC00 && c <= 0xDFFF) cell.pop_back();
            cell += L'\x2026'; capped = true; truncated = true;
        }
    };
    size_t i = 0;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (quoted) {
            if (c == L'"') {
                if (i + 1 < text.size() && text[i + 1] == L'"') { push(L'"'); ++i; }
                else quoted = false;
            } else {
                push(c);
            }
            continue;
        }
        if (c == L'"' && !cell_started) { quoted = true; cell_started = true; }
        else if (c == delimiter) end_cell();
        else if (c == L'\r') continue;
        else if (c == L'\n') {
            if (!end_row()) { ++i; break; }
        } else { push(c); cell_started = true; }
    }
    if (i < text.size()) {
        // Stopped at the row cap: anything but trailing blank lines means more rows.
        for (; i < text.size(); ++i)
            if (text[i] != L'\r' && text[i] != L'\n' && text[i] != L' ') { truncated = true; break; }
    } else if (!cell.empty() || !row.empty() || cell_started) {
        end_row();
    }
    return !rows.empty();
}

bool GuessHeader(const std::vector<Row>& rows) {
    if (rows.size() < 2) return false;
    size_t filled = 0;
    for (const auto& cell : rows[0]) {
        if (LooksNumeric(cell)) return false;
        if (!cell.empty()) ++filled;
    }
    if (filled * 2 < rows[0].size()) return false;
    // Header when some column is numeric below the first row, or every
    // first-row cell is filled.
    for (size_t r = 1; r < (std::min)(rows.size(), size_t{20}); ++r)
        for (const auto& cell : rows[r])
            if (LooksNumeric(cell)) return true;
    return filled == rows[0].size();
}

// --------------------------------------------------------------- XLSX ----

// Reads one package part (UTF-8 XML); a part larger than the cap keeps its
// start and sets *cut.
bool ReadPart(const std::wstring& path, std::string_view name, std::string& out, bool* cut,
              uint64_t& total) {
    std::vector<unsigned char> bytes;
    bool truncated = false;
    if (!ReadZipEntryPrefix(path, name, kMaxXlsxEntryBytes, bytes, &truncated, nullptr)) return false;
    total += bytes.size();
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (cut) *cut = truncated;
    return true;
}

// Minimal pull parser for the well-formed XML inside OOXML packages.
class XmlReader {
public:
    explicit XmlReader(std::string_view xml) : s_(xml) {}
    enum class Kind { Open, Close, Text, End };
    // Advances to the next element or text node. Self-closing elements report
    // Open with self_closing() true and no matching Close.
    Kind Next() {
        attrs_.clear();
        self_closing_ = false;
        while (pos_ < s_.size()) {
            if (s_[pos_] != '<') {
                const size_t end = s_.find('<', pos_);
                text_ = s_.substr(pos_, end == std::string_view::npos ? s_.size() - pos_ : end - pos_);
                pos_ = end == std::string_view::npos ? s_.size() : end;
                return Kind::Text;
            }
            if (s_.compare(pos_, 4, "<!--") == 0) { Skip("-->"); continue; }
            if (s_.compare(pos_, 9, "<![CDATA[") == 0) {
                const size_t end = s_.find("]]>", pos_ + 9);
                text_ = s_.substr(pos_ + 9, (end == std::string_view::npos ? s_.size() : end) - pos_ - 9);
                pos_ = end == std::string_view::npos ? s_.size() : end + 3;
                cdata_ = true;
                return Kind::Text;
            }
            if (s_.compare(pos_, 2, "<?") == 0 || s_.compare(pos_, 2, "<!") == 0) { Skip(">"); continue; }
            const bool close = pos_ + 1 < s_.size() && s_[pos_ + 1] == '/';
            size_t i = pos_ + (close ? 2 : 1);
            const size_t name_start = i;
            while (i < s_.size() && !IsSpace(s_[i]) && s_[i] != '>' && s_[i] != '/') ++i;
            name_ = LocalName(s_.substr(name_start, i - name_start));
            if (close) {
                const size_t end = s_.find('>', i);
                pos_ = end == std::string_view::npos ? s_.size() : end + 1;
                return Kind::Close;
            }
            // Attributes.
            while (i < s_.size()) {
                while (i < s_.size() && IsSpace(s_[i])) ++i;
                if (i >= s_.size()) break;
                if (s_[i] == '>') { ++i; break; }
                if (s_[i] == '/') { self_closing_ = true; ++i; continue; }
                const size_t key_start = i;
                while (i < s_.size() && s_[i] != '=' && !IsSpace(s_[i]) && s_[i] != '>') ++i;
                const std::string_view key = LocalName(s_.substr(key_start, i - key_start));
                while (i < s_.size() && (IsSpace(s_[i]) || s_[i] == '=')) ++i;
                if (i < s_.size() && (s_[i] == '"' || s_[i] == '\'')) {
                    const char q = s_[i++];
                    const size_t value_start = i;
                    while (i < s_.size() && s_[i] != q) ++i;
                    attrs_.push_back({key, s_.substr(value_start, i - value_start)});
                    if (i < s_.size()) ++i;
                }
            }
            pos_ = i;
            return Kind::Open;
        }
        return Kind::End;
    }
    std::string_view name() const { return name_; }
    bool self_closing() const { return self_closing_; }
    std::string_view Attr(std::string_view key) const {
        for (const auto& [k, v] : attrs_) if (k == key) return v;
        return {};
    }
    // Decoded text of the current Text node.
    std::wstring Text() {
        std::wstring out = Utf8(text_);
        if (cdata_) { cdata_ = false; return out; }
        return Unescape(out);
    }
    // Skips to the end of the element just opened.
    void SkipElement() {
        if (self_closing_) return;
        int depth = 1;
        for (Kind k = Next(); k != Kind::End; k = Next()) {
            if (k == Kind::Open && !self_closing_) ++depth;
            else if (k == Kind::Close && --depth == 0) return;
        }
    }
    static std::wstring Utf8(std::string_view s) {
        std::wstring out;
        if (s.empty()) return out;
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return out;
        out.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }
    static std::wstring Unescape(const std::wstring& s) {
        if (s.find(L'&') == std::wstring::npos) return s;
        std::wstring out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != L'&') { out += s[i]; continue; }
            const size_t semi = s.find(L';', i);
            if (semi == std::wstring::npos || semi - i > 10) { out += s[i]; continue; }
            const std::wstring_view entity(s.data() + i + 1, semi - i - 1);
            if (entity == L"amp") out += L'&';
            else if (entity == L"lt") out += L'<';
            else if (entity == L"gt") out += L'>';
            else if (entity == L"quot") out += L'"';
            else if (entity == L"apos") out += L'\'';
            else if (!entity.empty() && entity[0] == L'#') {
                const bool hex = entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X');
                const unsigned long code = std::wcstoul(std::wstring(entity.substr(hex ? 2 : 1)).c_str(),
                                                        nullptr, hex ? 16 : 10);
                if (code >= 0x10000 && code <= 0x10FFFF) {
                    out += static_cast<wchar_t>(0xD800 + ((code - 0x10000) >> 10));
                    out += static_cast<wchar_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
                } else if (code) {
                    out += static_cast<wchar_t>(code);
                }
            } else {
                out.append(s, i, semi - i + 1);
            }
            i = semi;
        }
        return out;
    }

private:
    static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    static std::string_view LocalName(std::string_view name) {
        const size_t colon = name.find(':');
        return colon == std::string_view::npos ? name : name.substr(colon + 1);
    }
    void Skip(const char* terminator) {
        const size_t end = s_.find(terminator, pos_);
        pos_ = end == std::string_view::npos ? s_.size() : end + std::strlen(terminator);
    }
    std::string_view s_;
    size_t pos_ = 0;
    std::string_view name_, text_;
    bool self_closing_ = false, cdata_ = false;
    std::vector<std::pair<std::string_view, std::string_view>> attrs_;
};

unsigned ToUnsigned(std::string_view s) {
    unsigned v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') break;
        v = v * 10 + static_cast<unsigned>(c - '0');
        if (v > 100000000u) break;
    }
    return v;
}

std::vector<std::wstring> ReadSharedStrings(std::string_view xml) {
    std::vector<std::wstring> strings;
    XmlReader x(xml);
    std::wstring current;
    bool in_si = false, in_t = false;
    int skip_depth = 0;  // inside <rPh> / <phoneticPr>
    for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
        if (k == XmlReader::Kind::Open) {
            if (x.name() == "si") { in_si = true; current.clear(); if (x.self_closing()) { strings.push_back({}); in_si = false; } }
            else if (x.name() == "rPh") { if (!x.self_closing()) ++skip_depth; }
            else if (x.name() == "t" && !x.self_closing()) in_t = true;
        } else if (k == XmlReader::Kind::Close) {
            if (x.name() == "si") { strings.push_back(std::move(current)); current.clear(); in_si = false; }
            else if (x.name() == "rPh") { if (skip_depth) --skip_depth; }
            else if (x.name() == "t") in_t = false;
        } else if (k == XmlReader::Kind::Text && in_si && in_t && !skip_depth) {
            current += x.Text();
        }
    }
    return strings;
}

struct Styles {
    std::vector<unsigned> fill;
    std::vector<bool> bold;          // per cellXfs index
    std::vector<std::wstring> format; // number format code per cellXfs index ("" = General)
};

std::wstring BuiltinFormat(unsigned id) {
    switch (id) {
    case 1: return L"0";
    case 2: return L"0.00";
    case 3: return L"#,##0";
    case 4: return L"#,##0.00";
    case 9: return L"0%";
    case 10: return L"0.00%";
    case 11: return L"0.00E+00";
    case 14: return L"yyyy-mm-dd";
    case 15: case 16: case 17: return L"yyyy-mm-dd";
    case 18: case 19: return L"hh:mm:ss";
    case 20: case 21: return L"hh:mm:ss";
    case 22: return L"yyyy-mm-dd hh:mm";
    case 37: case 38: return L"#,##0";
    case 39: case 40: return L"#,##0.00";
    case 45: case 46: case 47: return L"hh:mm:ss";
    case 49: return L"@";
    default: return {};
    }
}

Styles ReadStyles(std::string_view xml, std::string_view theme_xml) {
    std::vector<unsigned> theme;
    XmlReader tx(theme_xml);
    for (auto k = tx.Next(); k != XmlReader::Kind::End; k = tx.Next()) {
        if (k != XmlReader::Kind::Open) continue;
        if (tx.name() == "srgbClr" || tx.name() == "sysClr") {
            const std::string value(tx.Attr(tx.name() == "sysClr" ? "lastClr" : "val"));
            theme.push_back(static_cast<unsigned>(std::strtoul(value.c_str(), nullptr, 16)));
            if (theme.size() == 12) break;
        }
    }
    if (theme.size() >= 4) { std::swap(theme[0], theme[1]); std::swap(theme[2], theme[3]); }
    Styles styles;
    std::map<unsigned, std::wstring> custom;
    std::vector<bool> font_bold;
    std::vector<unsigned> fills;
    bool solid = false, in_fills = false;
    XmlReader x(xml);
    enum class Section { None, Fonts, CellXfs } section = Section::None;
    bool in_font = false;
    for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
        if (k == XmlReader::Kind::Open) {
            const auto n = x.name();
            if (n == "numFmt") {
                custom[ToUnsigned(x.Attr("numFmtId"))] =
                    XmlReader::Unescape(XmlReader::Utf8(x.Attr("formatCode")));
            } else if (n == "fills") {
                in_fills = true;
            } else if (in_fills && n == "fill") {
                fills.push_back(0); solid = false;
            } else if (in_fills && n == "patternFill") {
                solid = x.Attr("patternType") == "solid";
            } else if (n == "fgColor" && in_fills && solid && !fills.empty()) {
                static constexpr unsigned indexed[] = {
                    0x000000,0xFFFFFF,0xFF0000,0x00FF00,0x0000FF,0xFFFF00,0xFF00FF,0x00FFFF,
                    0x000000,0xFFFFFF,0xFF0000,0x00FF00,0x0000FF,0xFFFF00,0xFF00FF,0x00FFFF,
                    0x800000,0x008000,0x000080,0x808000,0x800080,0x008080,0xC0C0C0,0x808080,
                    0x9999FF,0x993366,0xFFFFCC,0xCCFFFF,0x660066,0xFF8080,0x0066CC,0xCCCCFF,
                    0x000080,0xFF00FF,0xFFFF00,0x00FFFF,0x800080,0x800000,0x008080,0x0000FF,
                    0x00CCFF,0xCCFFFF,0xCCFFCC,0xFFFF99,0x99CCFF,0xFF99CC,0xCC99FF,0xFFCC99,
                    0x3366FF,0x33CCCC,0x99CC00,0xFFCC00,0xFF9900,0xFF6600,0x666699,0x969696,
                    0x003366,0x339966,0x003300,0x333300,0x993300,0x993366,0x333399,0x333333};
                if (!x.Attr("indexed").empty() && ToUnsigned(x.Attr("indexed")) < 64)
                    fills.back() = 0xFF000000u | indexed[ToUnsigned(x.Attr("indexed"))];
                const std::string rgb(x.Attr("rgb"));
                const auto theme_id = x.Attr("theme");
                if (!theme_id.empty() && ToUnsigned(theme_id) < theme.size()) {
                    unsigned color = theme[ToUnsigned(theme_id)];
                    const std::string tint_text(x.Attr("tint"));
                    const double tint = (std::max)(-1.0, (std::min)(1.0, std::strtod(tint_text.c_str(), nullptr)));
                    const double red = (color >> 16) & 255u, green = (color >> 8) & 255u, blue = color & 255u;
                    const double hi = (std::max)({red, green, blue}), lo = (std::min)({red, green, blue});
                    const double light = (hi + lo) / 510.0;
                    const double next = tint < 0 ? light * (1 + tint) : light * (1 - tint) + tint;
                    const double chroma = hi - lo;
                    const double saturation = chroma == 0 ? 0 : chroma / (255 * (1 - std::abs(2 * light - 1)));
                    const double new_chroma = (1 - std::abs(2 * next - 1)) * saturation * 255;
                    unsigned adjusted = 0;
                    for (unsigned shift : {0u, 8u, 16u}) {
                        const double channel = (color >> shift) & 255u;
                        const double value = (next * 255 - new_chroma / 2) + (chroma == 0 ? 0 : (channel - lo) * new_chroma / chroma);
                        adjusted |= static_cast<unsigned>(std::round((std::clamp)(value, 0.0, 255.0))) << shift;
                    }
                    fills.back() = 0xFF000000u | adjusted;
                }
                if (rgb.size() == 8 || rgb.size() == 6) fills.back() = 0xFF000000u | static_cast<unsigned>(std::strtoul(rgb.c_str(), nullptr, 16));
            } else if (n == "fonts") {
                section = Section::Fonts;
            } else if (n == "cellXfs") {
                section = Section::CellXfs;
            } else if (n == "cellStyleXfs" || n == "dxfs") {
                x.SkipElement();
            } else if (section == Section::Fonts && n == "font") {
                font_bold.push_back(false);
                in_font = !x.self_closing();
            } else if (section == Section::Fonts && in_font && n == "b") {
                const auto val = x.Attr("val");
                if (!font_bold.empty()) font_bold.back() = val.empty() || val == "1" || val == "true";
            } else if (section == Section::CellXfs && n == "xf") {
                const unsigned font = ToUnsigned(x.Attr("fontId"));
                const unsigned fmt = ToUnsigned(x.Attr("numFmtId"));
                const unsigned fill = ToUnsigned(x.Attr("fillId"));
                styles.fill.push_back(fill < fills.size() ? fills[fill] : 0);
                styles.bold.push_back(font < font_bold.size() && font_bold[font]);
                const auto it = custom.find(fmt);
                styles.format.push_back(it != custom.end() ? it->second : BuiltinFormat(fmt));
            }
        } else if (k == XmlReader::Kind::Close) {
            if (x.name() == "fills") in_fills = false;
            else if (x.name() == "font") in_font = false;
            else if (x.name() == "fonts" || x.name() == "cellXfs") section = Section::None;
        }
    }
    return styles;
}

// Formats a cell's cached numeric value with the parts of its number format
// that matter for reading: grouping, decimals, percent, dates and times.
std::wstring FormatNumber(const std::wstring& raw, const std::wstring& format, bool date1904) {
    wchar_t* end = nullptr;
    const double value = std::wcstod(raw.c_str(), &end);
    if (!end || *end || !std::isfinite(value)) return raw;
    std::wstring f = format;
    const size_t section = f.find(L';');
    if (section != std::wstring::npos) f.resize(section);
    // Strip quoted literals, escapes and [colour]/[locale] tags for inspection.
    std::wstring plain;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == L'"') { const size_t e = f.find(L'"', i + 1); if (e == std::wstring::npos) break; i = e; continue; }
        if (f[i] == L'[') { const size_t e = f.find(L']', i + 1); if (e == std::wstring::npos) break; i = e; continue; }
        if (f[i] == L'\\' || f[i] == L'_' || f[i] == L'*') { ++i; continue; }
        plain += static_cast<wchar_t>(std::towlower(f[i]));
    }
    const bool has_date = plain.find(L'y') != std::wstring::npos || plain.find(L'd') != std::wstring::npos;
    const bool has_time = plain.find(L'h') != std::wstring::npos || plain.find(L's') != std::wstring::npos;
    if ((has_date || has_time) && value >= 0 && value < (date1904 ? 2957004 : 2958466)) {
        // Excel serial dates (1900 system; serial 60 is the fictional 1900-02-29).
        const double whole = std::floor(value);
        long long serial = static_cast<long long>(whole);
        long long secs = std::llround((value - whole) * 86400.0);
        if (secs == 86400) { ++serial; secs = 0; }
        if (serial >= (date1904 ? 2957004 : 2958466)) return raw;
        // Serial 1 is 1900-01-01; Excel counts a fictional 1900-02-29 (serial 60).
        const long long unix_days = date1904 ? serial - 24107 : serial - (serial >= 61 ? 25569 : 25568);
        const long long z = unix_days + 719468;
        const long long era = (z >= 0 ? z : z - 146096) / 146097;
        const long long doe = z - era * 146097;
        const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long long y = yoe + era * 400;
        const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const long long mp = (5 * doy + 2) / 153;
        long long d = doy - (153 * mp + 2) / 5 + 1;
        long long m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) ++y;
        if (!date1904 && serial == 60) { y = 1900; m = 2; d = 29; }
        wchar_t out[48];
        if (has_date && has_time)
            swprintf_s(out, L"%04lld-%02lld-%02lld %02lld:%02lld", y, m, d, secs / 3600, (secs / 60) % 60);
        else if (has_date)
            swprintf_s(out, L"%04lld-%02lld-%02lld", y, m, d);
        else
            swprintf_s(out, L"%02lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
        return out;
    }
    if (plain.empty() || plain == L"general" || plain == L"@") {
        wchar_t out[48];
        swprintf_s(out, L"%.15g", value);
        return out;
    }
    const bool percent = plain.find(L'%') != std::wstring::npos;
    const bool grouping = plain.find(L",") != std::wstring::npos && plain.find(L'0') != std::wstring::npos;
    int decimals = 0;
    if (const size_t dot = plain.find(L'.'); dot != std::wstring::npos)
        for (size_t i = dot + 1; i < plain.size() && (plain[i] == L'0' || plain[i] == L'#'); ++i) ++decimals;
    if (plain.find(L'e') != std::wstring::npos) {
        wchar_t out[48];
        swprintf_s(out, L"%.*E", decimals, value);
        return out;
    }
    const double shown = percent ? value * 100.0 : value;
    wchar_t digits[64];
    swprintf_s(digits, L"%.*f", (std::min)(decimals, 12), std::fabs(shown));
    std::wstring number = digits;
    if (grouping) {
        const size_t dot = number.find(L'.');
        const size_t int_end = dot == std::wstring::npos ? number.size() : dot;
        std::wstring grouped;
        for (size_t i = 0; i < int_end; ++i) {
            if (i && (int_end - i) % 3 == 0) grouped += L',';
            grouped += number[i];
        }
        number = grouped + number.substr(int_end);
    }
    if (shown < 0 && std::llround(std::fabs(shown) * std::pow(10.0, decimals)) != 0) number = L"-" + number;
    if (percent) number += L'%';
    return number;
}

size_t ColumnIndex(std::string_view ref) {
    size_t col = 0;
    bool any = false;
    for (const char c : ref) {
        if (c >= 'A' && c <= 'Z') { col = col * 26 + static_cast<size_t>(c - 'A' + 1); any = true; }
        else if (c >= 'a' && c <= 'z') { col = col * 26 + static_cast<size_t>(c - 'a' + 1); any = true; }
        else break;
        if (col > 100000) break;
    }
    return any ? col - 1 : static_cast<size_t>(-1);
}

void ReadSheet(std::string_view xml, const std::vector<std::wstring>& shared, const Styles& styles,
               std::vector<Row>& rows, size_t& columns, bool& truncated, bool date1904) {
    XmlReader x(xml);
    size_t row_index = 0, next_col = 0;
    bool in_row = false, in_cell = false, in_value = false, in_inline = false, in_t = false;
    std::string type;
    unsigned style = 0;
    size_t col = 0;
    std::wstring value, formula;
    bool in_formula = false, formula_shared = false;
    unsigned formula_group = 0;
    SharedFormulas formulas;
    auto store = [&] {
        if (col >= kMaxColumns) { truncated = true; return; }
        if (row_index >= kMaxRows) { truncated = true; return; }
        std::wstring text;
        if (value.empty() && !formula.empty()) {
            // Never calculated (generators often write no cached <v>): an
            // empty cell hid whole formula columns, so show the formula.
            text = L"=" + formula;
        } else if (type == "s") {
            unsigned idx = 0;
            for (const wchar_t ch : value) {
                if (ch < L'0' || ch > L'9' || idx > 100000000u) break;
                idx = idx * 10 + static_cast<unsigned>(ch - L'0');
            }
            if (idx < shared.size()) text = shared[idx];
        } else if (type == "b") {
            text = value == L"1" ? L"TRUE" : L"FALSE";
        } else if (type == "str" || type == "inlineStr" || type == "e") {
            text = value;
        } else if (!value.empty()) {
            text = FormatNumber(value, style < styles.format.size() ? styles.format[style] : std::wstring(), date1904);
        }
        if (text.size() > kMaxCellChars) {
            text.resize(SafePrefix(text, kMaxCellChars)); text += L'\x2026'; truncated = true;
        }
        const unsigned fill = style < styles.fill.size() ? styles.fill[style] : 0;
        if (text.empty() && !fill) return;
        if (rows.size() <= row_index) rows.resize(row_index + 1);
        Row& row = rows[row_index];
        if (row.size() <= col) row.resize(col + 1);
        const bool bold = style < styles.bold.size() && styles.bold[style];
        row[col] = bold ? std::wstring(1, L'\x0001') + text : text;
        if (fill) { wchar_t color[9]; swprintf_s(color, L"%08X", fill); row[col] = std::wstring(1, L'\x0002') + color + row[col]; }
        columns = (std::max)(columns, col + 1);
    };
    for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
        if (k == XmlReader::Kind::Open) {
            const auto n = x.name();
            if (n == "row") {
                const unsigned r = ToUnsigned(x.Attr("r"));
                row_index = r ? r - 1 : (in_row ? row_index + 1 : rows.size());
                if (row_index >= kMaxRows) { truncated = true; break; }
                in_row = !x.self_closing();
                next_col = 0;
            } else if (n == "c" && in_row) {
                const auto ref = x.Attr("r");
                const size_t parsed = ref.empty() ? static_cast<size_t>(-1) : ColumnIndex(ref);
                col = parsed == static_cast<size_t>(-1) ? next_col : parsed;
                next_col = col + 1;
                type = std::string(x.Attr("t"));
                style = ToUnsigned(x.Attr("s"));
                value.clear();
                formula.clear();
                in_cell = !x.self_closing();
                if (!in_cell) { value.clear(); store(); }
            } else if (in_cell && n == "v") {
                in_value = !x.self_closing();
            } else if (in_cell && n == "is") {
                in_inline = !x.self_closing();
            } else if (in_inline && n == "t") {
                in_t = !x.self_closing();
            } else if (in_inline && n == "rPh") {
                x.SkipElement();
            } else if (in_cell && n == "f") {
                // Shown only without a cached <v>. Members of a shared formula
                // carry just the group index and take the anchor's text.
                formula_shared = x.Attr("t") == "shared" && !x.Attr("si").empty();
                formula_group = ToUnsigned(x.Attr("si"));
                in_formula = !x.self_closing();
                if (!in_formula && formula_shared)
                    formulas.Resolve(formula_group, static_cast<long long>(row_index),
                                     static_cast<long long>(col), formula);
            }
        } else if (k == XmlReader::Kind::Close) {
            const auto n = x.name();
            if (n == "c" && in_cell) { store(); in_cell = false; }
            else if (n == "v") in_value = false;
            else if (n == "f" && in_formula) {
                in_formula = false;
                if (formula_shared)
                    formulas.Resolve(formula_group, static_cast<long long>(row_index),
                                     static_cast<long long>(col), formula);
            }
            else if (n == "is") in_inline = false;
            else if (n == "t") in_t = false;
            else if (n == "row") in_row = false;
            else if (n == "sheetData") break;
        } else if (k == XmlReader::Kind::Text) {
            if (in_value || (in_inline && in_t)) value += x.Text();
            else if (in_formula) formula += x.Text();
        }
    }
    // Drop trailing empty rows.
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
}

constexpr std::string_view kDefaultWorkbook = "xl/workbook.xml";

}  // namespace

bool IsTableExtension(std::wstring_view extension) {
    return extension == L".csv" || extension == L".tsv" || extension == L".tab" ||
           extension == L".psv";
}

bool IsSpreadsheetExtension(std::wstring_view extension) {
    return extension == L".xlsx" || extension == L".xlsm";
}

bool ReadTextFile(const std::wstring& path, size_t max_bytes, std::wstring& text, bool& cut,
                  uint32_t& bytes_read, ipc::PreviewTextEncoding& encoding) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    const size_t want = static_cast<size_t>((std::min)(static_cast<unsigned long long>(size.QuadPart),
                                                       static_cast<unsigned long long>(max_bytes)));
    std::vector<char> bytes(want);
    DWORD got = 0;
    size_t total = 0;
    while (total < want && ReadFile(file, bytes.data() + total,
                                    static_cast<DWORD>((std::min)(want - total, size_t{1} << 20)), &got, nullptr) && got)
        total += got;
    CloseHandle(file);
    bytes.resize(total);
    cut = static_cast<unsigned long long>(size.QuadPart) > total;
    bytes_read = static_cast<uint32_t>(total);
    text.clear();
    return DecodeBytes(bytes, cut, text, encoding);
}

bool MakeCsvTable(const std::wstring& path, std::wstring_view extension, std::wstring& payload,
                  uint32_t& bytes_read, bool& truncated, ipc::PreviewTextEncoding& encoding) {
    std::wstring text;
    bool cut = false;
    if (!ReadTextFile(path, kMaxCsvBytes, text, cut, bytes_read, encoding)) return false;
    if (text.find(L'\0') != std::wstring::npos) return false;  // binary
    const wchar_t delimiter = DetectDelimiter(text, extension);
    std::vector<Row> rows;
    size_t columns = 0;
    bool more = cut;
    if (!ParseCsv(text, delimiter, rows, columns, more)) return false;
    const wchar_t* detail = delimiter == L'\t' ? L"tab" : delimiter == L';' ? L"semicolon"
                          : delimiter == L'|' ? L"pipe" : L"comma";
    payload = L"PULSETBL\t1\n";
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring source;
    AppendEscaped(source, std::wstring_view(text).substr(0, SafePrefix(text, kMaxSourceChars)));
    more |= text.size() > kMaxSourceChars;
    truncated = AppendSheet(payload, L"csv", slash == std::wstring::npos ? path : path.substr(slash + 1), rows,
                columns, more, detail, GuessHeader(rows),
                ipc::kPreviewMaxTableChars - source.size() - 64);
    payload += L"X\t";
    payload += source;
    payload += L'\n';
    return true;
}

bool MakeXlsxTable(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                   uint32_t sheet_index) {
    payload.clear();
    bytes_read = 0;
    const ULONGLONG started = GetTickCount64();
    uint64_t total = 0;
    std::string workbook, rels, shared_xml, styles_xml;
    // Parts are found through relationships: not every generator names the
    // workbook, its sheets, shared strings and styles the way Excel does,
    // and a missed shared-strings part left every text cell empty.
    std::string workbook_part(kDefaultWorkbook);
    {
        std::string package_rels;
        if (ReadPart(path, "_rels/.rels", package_rels, nullptr, total)) {
            XmlReader x(package_rels);
            for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
                if (k == XmlReader::Kind::Open && x.name() == "Relationship" &&
                    RelationshipTypeIs(x.Attr("Type"), "officeDocument") && !x.Attr("Target").empty()) {
                    workbook_part = ResolvePartTarget("", x.Attr("Target"));
                    break;
                }
            }
        }
    }
    bool workbook_cut = false;
    bool have_workbook = ReadPart(path, workbook_part, workbook, &workbook_cut, total);
    if (!have_workbook && workbook_part != kDefaultWorkbook) {
        workbook_part = kDefaultWorkbook;
        have_workbook = ReadPart(path, workbook_part, workbook, &workbook_cut, total);
    }
    if (!have_workbook || workbook_cut) return false;
    ReadPart(path, RelationshipsPart(workbook_part), rels, nullptr, total);
    // Sheet order and names from the workbook, targets from its relationships.
    std::map<std::string, std::string> targets;
    std::string shared_part = "xl/sharedStrings.xml", styles_part = "xl/styles.xml", theme_part = "xl/theme/theme1.xml";
    {
        XmlReader x(rels);
        for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
            if (k != XmlReader::Kind::Open || x.name() != "Relationship") continue;
            const std::string target = ResolvePartTarget(workbook_part, x.Attr("Target"));
            const auto type = x.Attr("Type");
            if (RelationshipTypeIs(type, "sharedStrings")) shared_part = target;
            else if (RelationshipTypeIs(type, "styles")) styles_part = target;
            else if (RelationshipTypeIs(type, "theme")) theme_part = target;
            targets[std::string(x.Attr("Id"))] = target;
        }
    }
    struct SheetRef { std::wstring name; std::string part; bool hidden; bool name_cut; };
    std::vector<SheetRef> sheets;
    size_t total_sheets = 0, metadata_chars = 0;
    bool metadata_full = false;
    bool date1904 = false;
    {
        XmlReader x(workbook);
        for (auto k = x.Next(); k != XmlReader::Kind::End; k = x.Next()) {
            if (k != XmlReader::Kind::Open) continue;
            if (x.name() == "workbookPr") {
                const auto value = x.Attr("date1904");
                date1904 = value == "1" || value == "true";
            }
            if (x.name() != "sheet") continue;
            const auto state = x.Attr("state");
            ++total_sheets;
            const auto rel = targets.find(std::string(x.Attr("id")));
            auto name = XmlReader::Unescape(XmlReader::Utf8(x.Attr("name")));
            const bool name_cut = name.size() > 256;
            name.resize(SafePrefix(name, 256));
            // Reserve metadata before cell data so content cannot displace later tabs.
            const size_t reserve = name.size() * 2 + 512;
            if (metadata_full || metadata_chars + reserve > ipc::kPreviewMaxTableChars / 2) {
                metadata_full = true;
                continue;
            }
            metadata_chars += reserve;
            sheets.push_back({std::move(name), rel == targets.end() ? std::string() : rel->second,
                              state == "hidden" || state == "veryHidden", name_cut});
        }
    }
    if (sheet_index >= sheets.size()) return false;
    std::vector<std::wstring> shared;
    Styles styles;
    if (!sheets[sheet_index].hidden && !sheets[sheet_index].part.empty()) {
        if (ReadPart(path, shared_part, shared_xml, nullptr, total)) shared = ReadSharedStrings(shared_xml);
        if (ReadPart(path, styles_part, styles_xml, nullptr, total)) {
            std::string theme_xml;
            ReadPart(path, theme_part, theme_xml, nullptr, total);
            styles = ReadStyles(styles_xml, theme_xml);
        }
    }
    payload = L"PULSETBL\t1\n";
    size_t loaded = 0;
    for (size_t index = 0; index < sheets.size(); ++index) {
        const SheetRef& sheet = sheets[index];
        std::vector<Row> rows;
        size_t columns = 0;
        bool truncated = sheet.name_cut, cut = false;
        std::string xml;
        std::wstring reason;
        bool read = false;
        if (sheet.hidden) reason = L"hidden";
        else if (sheet.part.empty()) reason = L"missing-relationship";
        else if (index != sheet_index) reason = L"not-loaded";
        else if (GetTickCount64() - started >= kXlsxBudgetMs) reason = L"time-limit";
        else if (payload.size() + metadata_chars + 512 >= ipc::kPreviewMaxTableChars) reason = L"payload-limit";
        else {
            read = ReadPart(path, sheet.part, xml, &cut, total);
            if (!read) reason = L"read-failed";
        }
        if (read) { ++loaded; ReadSheet(xml, shared, styles, rows, columns, truncated, date1904); }
        metadata_chars -= sheet.name.size() * 2 + 512;
        AppendSheet(payload, L"xlsx", sheet.name, rows, columns,
                    truncated || cut || (!read && reason != L"not-loaded"), reason, false,
                    ipc::kPreviewMaxTableChars - metadata_chars - 128);
    }
    payload += L"W\t" + std::to_wstring(total_sheets) + L'\t' + std::to_wstring(loaded) +
               L'\t' + std::to_wstring(sheet_index) + L'\n';
    bytes_read = static_cast<uint32_t>((std::min)(total, uint64_t{0xFFFFFFFFu}));
    return true;
}

}  // namespace pulse::preview
