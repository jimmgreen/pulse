// office_doc_model.cpp - see office_doc_model.h.
//
// Binary .doc layout ([MS-CFB], [MS-DOC]; same walk as Apache POI HWPF):
//   compound file  -> "WordDocument" stream (FIB, text, FKP pages) and the
//                     "0Table"/"1Table" stream (piece table, style sheet,
//                     section table, FKP indexes)
//   FIB            -> ccpText and the fc/lcb pairs of the table structures
//   Clx/PlcPcd     -> pieces: CP range -> file offset, 8-bit or UTF-16 text
//   PlcBtePapx     -> PAPX FKPs: per paragraph mark istd + sprms (in table,
//                     row end, justification)
//   PlcBteChpx     -> CHPX FKPs: per run sprms (font size, bold)
//   STSH           -> style defaults for what the FKPs leave unset
//   PlcfSed/SEPX   -> first section page size, margins and columns
// Every offset read from the file is range checked; anything inconsistent
// fails the read instead of guessing.
#include "office_doc_model.h"
#include "compound_file.h"
#include <filter.h>
#include <filterr.h>
#include <ntquery.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kMaxBlocks = 40;          // same budget as the .docx reader
constexpr size_t kMaxTableRows = 8;
constexpr size_t kMaxChars = 60000;        // text scanned for the first blocks
constexpr size_t kDocumentChars = 2u << 20;
constexpr size_t kDocumentBlocks = 32768;
constexpr size_t kDocumentRows = 32768;
constexpr size_t kWordStreamBytes = 64u << 20;
constexpr size_t kTableStreamBytes = 32u << 20;
constexpr size_t kRtfBytes = 8u << 20;

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

std::wstring Trim(const std::wstring& text) {
    size_t a = 0, b = text.size();
    while (a < b && (std::iswspace(text[a]) || text[a] == 0x3000)) ++a;
    while (b > a && (std::iswspace(text[b - 1]) || text[b - 1] == 0x3000)) --b;
    return text.substr(a, b - a);
}

uint16_t U16(const std::vector<uint8_t>& b, size_t at) {
    return at + 2 <= b.size() ? static_cast<uint16_t>(b[at] | (b[at + 1] << 8)) : uint16_t{0};
}
uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

// ---- sprms --------------------------------------------------------------------------

struct Sprm {
    uint16_t code = 0;
    const uint8_t* operand = nullptr;
    size_t size = 0;
    uint8_t Byte() const { return size >= 1 ? operand[0] : 0; }
    uint16_t Word() const {
        if (size < 2) return Byte();
        return static_cast<uint16_t>(operand[0] | (operand[1] << 8));
    }
    int32_t Long() const {
        if (size < 4) return Word();
        return static_cast<int32_t>(static_cast<uint32_t>(operand[0]) | (static_cast<uint32_t>(operand[1]) << 8) |
                                    (static_cast<uint32_t>(operand[2]) << 16) |
                                    (static_cast<uint32_t>(operand[3]) << 24));
    }
};

// Calls fn(Sprm) for each property in a grpprl; stops at the first one that
// does not fit.
template <typename Fn>
void ForEachSprm(const uint8_t* data, size_t size, Fn fn) {
    size_t at = 0;
    while (at + 2 <= size) {
        Sprm sprm;
        sprm.code = static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
        at += 2;
        size_t length = 0;
        switch (sprm.code >> 13) {
        case 0: case 1: length = 1; break;
        case 2: case 4: case 5: length = 2; break;
        case 3: length = 4; break;
        case 7: length = 3; break;
        default:   // 6: variable
            if (sprm.code == 0xD606 || sprm.code == 0xD608) {   // sprmTDefTable(10): 2-byte cb, +1
                if (at + 2 > size) return;
                const size_t cb = static_cast<size_t>(data[at] | (data[at + 1] << 8));
                length = cb + 1;
            } else if (sprm.code == 0xC615) {                    // sprmPChgTabs: cb 255 is special
                if (at >= size || data[at] == 255) return;
                length = size_t{1} + data[at];
            } else {
                if (at >= size) return;
                length = size_t{1} + data[at];
            }
            break;
        }
        if (at + length > size) return;
        sprm.operand = data + at;
        sprm.size = length;
        if ((sprm.code >> 13) == 6 && sprm.code != 0xD606 && sprm.code != 0xD608) {
            ++sprm.operand;    // skip the size byte
            --sprm.size;
        }
        fn(sprm);
        at += length;
    }
}

constexpr uint16_t kSprmPJc80 = 0x2403, kSprmPJc = 0x2461;
constexpr uint16_t kSprmPFInTable = 0x2416, kSprmPFTtp = 0x2417;
constexpr uint16_t kSprmPItap = 0x6649, kSprmPFInnerTtp = 0x244C;
constexpr uint16_t kSprmCFBold = 0x0835, kSprmCHps = 0x4A43;
constexpr uint16_t kSprmSXaPage = 0xB01F, kSprmSYaPage = 0xB020;
constexpr uint16_t kSprmSDxaLeft = 0xB021, kSprmSDxaRight = 0xB022, kSprmSDyaTop = 0x9023;
constexpr uint16_t kSprmSCcolumns = 0x500B, kSprmSDxaColumns = 0x900C;

int ToAlign(uint8_t jc) {
    switch (jc) {
    case 1: return static_cast<int>(DocAlign::Center);
    case 2: return static_cast<int>(DocAlign::Right);
    case 3: case 4: case 5: case 6: case 7: case 8: case 9: return static_cast<int>(DocAlign::Justify);
    default: return static_cast<int>(DocAlign::Left);
    }
}

// ---- style sheet ----------------------------------------------------------------------

struct DocStyle {
    uint16_t base = 0x0FFF;
    int half_points = -1;
    int bold = -1;
    int align = -1;
};

struct StyleSheet {
    std::vector<DocStyle> styles;

    template <typename Get>
    int Resolve(uint16_t istd, Get get, int fallback) const {
        for (int guard = 0; guard < 11 && istd < styles.size(); ++guard) {
            const int value = get(styles[istd]);
            if (value >= 0) return value;
            istd = styles[istd].base;
        }
        return fallback;
    }
};

void ApplyChpx(const uint8_t* data, size_t size, DocStyle& style) {
    ForEachSprm(data, size, [&](const Sprm& sprm) {
        if (sprm.code == kSprmCHps) style.half_points = sprm.Word();
        else if (sprm.code == kSprmCFBold) style.bold = (sprm.Byte() == 1 || sprm.Byte() == 0x81) ? 1 : 0;
    });
}

StyleSheet ReadStyleSheet(const std::vector<uint8_t>& table, uint32_t fc, uint32_t lcb) {
    StyleSheet sheet;
    if (lcb < 6 || fc > table.size() || lcb > table.size() - fc) return sheet;
    const std::vector<uint8_t> stsh(table.begin() + fc, table.begin() + fc + lcb);
    const size_t cb_stshi = U16(stsh, 0);
    const size_t count = U16(stsh, 2);
    const size_t cb_base = U16(stsh, 4);
    size_t at = 2 + cb_stshi;
    for (size_t i = 0; i < count && i < 4096 && at + 2 <= stsh.size(); ++i) {
        const size_t cb = U16(stsh, at);
        at += 2;
        DocStyle style;
        if (cb && at + cb <= stsh.size() && cb >= cb_base + 2 && cb_base >= 10) {
            const std::vector<uint8_t> std_(stsh.begin() + at, stsh.begin() + at + cb);
            const uint16_t kind_base = U16(std_, 2);
            const int kind = kind_base & 0xF;                   // 1 paragraph, 2 character
            style.base = static_cast<uint16_t>(kind_base >> 4);
            const int upx_count = U16(std_, 4) & 0xF;
            size_t p = cb_base;
            const size_t name_chars = U16(std_, p);
            p += 2 + name_chars * 2 + 2;                        // Xstz: count, chars, terminator
            for (int u = 0; u < upx_count && p + 2 <= std_.size(); ++u) {
                if (p & 1) ++p;
                if (p + 2 > std_.size()) break;
                const size_t cb_upx = U16(std_, p);
                p += 2;
                if (p + cb_upx > std_.size()) break;
                const uint8_t* upx = std_.data() + p;
                if (kind == 1 && u == 0 && cb_upx >= 2) {        // UPX.papx: istd + grpprl
                    ForEachSprm(upx + 2, cb_upx - 2, [&](const Sprm& sprm) {
                        if (sprm.code == kSprmPJc || sprm.code == kSprmPJc80) style.align = ToAlign(sprm.Byte());
                    });
                } else if ((kind == 1 && u == 1) || (kind == 2 && u == 0)) {
                    ApplyChpx(upx, cb_upx, style);
                }
                p += cb_upx;
            }
        }
        sheet.styles.push_back(style);
        at += cb;
    }
    return sheet;
}

// ---- FKP pages ------------------------------------------------------------------------

struct Run {
    uint32_t fc_begin = 0, fc_end = 0;
    uint16_t istd = 0;
    size_t page = 0;          // index into pages
    size_t offset = 0, size = 0;
};

class FkpIndex {
public:
    // bte: PlcBte* in the table stream; papx selects the page layout.
    bool Load(const std::vector<uint8_t>& table, uint32_t fc, uint32_t lcb,
              const std::vector<uint8_t>& word, bool papx) {
        if (lcb < 4 || fc > table.size() || lcb > table.size() - fc) return false;
        const size_t n = (lcb - 4) / 8;
        for (size_t i = 0; i < n && i < 65536; ++i) {
            const uint32_t pn = U32(table, fc + 4 * (n + 1) + 4 * i) & 0x3FFFFFu;
            const size_t base = static_cast<size_t>(pn) * 512;
            if (base + 512 > word.size()) continue;
            const std::vector<uint8_t> page(word.begin() + base, word.begin() + base + 512);
            const size_t page_index = pages_.size();
            pages_.push_back(page);
            const size_t crun = page[511];
            if (crun == 0 || 4 * (crun + 1) > 511) continue;
            for (size_t k = 0; k < crun; ++k) {
                Run run;
                run.fc_begin = U32(page, 4 * k);
                run.fc_end = U32(page, 4 * (k + 1));
                run.page = page_index;
                if (papx) {
                    const size_t bx = 4 * (crun + 1) + 13 * k;
                    if (bx >= 511) break;
                    const size_t offset = static_cast<size_t>(page[bx]) * 2;
                    if (offset) {
                        size_t q = offset;
                        if (q >= 511) continue;
                        size_t cb = page[q++];
                        size_t length = 0;
                        if (cb == 0) {
                            if (q >= 511) continue;
                            cb = page[q++];
                            length = cb * 2;
                        } else {
                            length = cb * 2 - 1;
                        }
                        if (length < 2 || q + length > 511) continue;
                        run.istd = U16(page, q);
                        run.offset = q + 2;
                        run.size = length - 2;
                    }
                } else {
                    const size_t rgb = 4 * (crun + 1) + k;
                    if (rgb >= 511) break;
                    const size_t offset = static_cast<size_t>(page[rgb]) * 2;
                    if (offset && offset < 511) {
                        const size_t cb = page[offset];
                        if (offset + 1 + cb <= 511) {
                            run.offset = offset + 1;
                            run.size = cb;
                        }
                    }
                }
                runs_.push_back(run);
            }
        }
        std::sort(runs_.begin(), runs_.end(), [](const Run& a, const Run& b) { return a.fc_begin < b.fc_begin; });
        return !runs_.empty();
    }

    const Run* Find(uint32_t fc) const {
        auto it = std::upper_bound(runs_.begin(), runs_.end(), fc,
            [](uint32_t value, const Run& run) { return value < run.fc_begin; });
        if (it == runs_.begin()) return nullptr;
        --it;
        return fc < it->fc_end ? &*it : nullptr;
    }

    const uint8_t* Data(const Run& run) const { return pages_[run.page].data() + run.offset; }

private:
    std::vector<std::vector<uint8_t>> pages_;
    std::vector<Run> runs_;
};

// ---- text -----------------------------------------------------------------------------

struct DocChar {
    wchar_t ch;
    uint32_t fc;
};

// cp1252 bytes 0x80-0x9F that 8-bit ("compressed") pieces map specially.
wchar_t CompressedChar(uint8_t byte) {
    static constexpr wchar_t kHigh[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    return byte >= 0x80 && byte <= 0x9F ? kHigh[byte - 0x80] : static_cast<wchar_t>(byte);
}

bool ReadText(const std::vector<uint8_t>& word, const std::vector<uint8_t>& table, uint32_t fc_clx,
              uint32_t lcb_clx, uint32_t ccp_text, std::vector<DocChar>& text, size_t max_chars) {
    if (lcb_clx < 5 || fc_clx > table.size() || lcb_clx > table.size() - fc_clx) return false;
    size_t at = fc_clx;
    const size_t end = static_cast<size_t>(fc_clx) + lcb_clx;
    while (at < end && table[at] == 1) {                 // Prc: property modifiers, skipped
        at += 3 + static_cast<size_t>(U16(table, at + 1));
    }
    if (at + 5 > end || table[at] != 2) return false;     // Pcdt
    const size_t lcb = U32(table, at + 1);
    const size_t plc = at + 5;
    if (lcb < 4 || (lcb - 4) % 12 != 0 || plc + lcb > end) return false;
    const size_t n = (lcb - 4) / 12;
    const size_t limit = (std::min)(static_cast<size_t>(ccp_text), max_chars);
    for (size_t i = 0; i < n && text.size() < limit; ++i) {
        const uint32_t cp0 = U32(table, plc + 4 * i);
        const uint32_t cp1 = U32(table, plc + 4 * (i + 1));
        if (cp0 != text.size() || cp1 <= cp0) return false;
        if (cp0 >= ccp_text) break;
        const uint32_t raw = U32(table, plc + 4 * (n + 1) + 8 * i + 2);
        const bool compressed = (raw & 0x40000000u) != 0;
        const uint32_t fc = raw & 0x3FFFFFFFu;
        const size_t count = (std::min)(static_cast<size_t>((std::min)(cp1, ccp_text) - cp0),
                                        limit - text.size());
        for (size_t k = 0; k < count; ++k) {
            if (compressed) {
                const size_t off = fc / 2 + k;
                if (off >= word.size()) return false;
                text.push_back({CompressedChar(word[off]), static_cast<uint32_t>(off)});
            } else {
                const size_t off = fc + 2 * k;
                if (off + 2 > word.size()) return false;
                text.push_back({static_cast<wchar_t>(U16(word, off)), static_cast<uint32_t>(off)});
            }
        }
    }
    return text.size() == limit && !text.empty();
}

// Collects paragraphs into DocModel blocks, grouping table paragraphs into
// rows and cells (shared by the binary and RTF readers).
class BlockBuilder {
public:
    explicit BlockBuilder(DocModel& model, size_t max_blocks = kMaxBlocks, size_t max_rows = kMaxTableRows)
        : model_(model), max_blocks_(max_blocks), max_rows_(max_rows) {}

    bool Full() const { return model_.blocks.size() >= max_blocks_; }
    bool Truncated() const { return truncated_; }

    void Paragraph(const std::wstring& text, float points, bool bold, int align) {
        CloseTable();
        if (Full()) { truncated_ = true; return; }
        DocBlock block;
        block.text = Trim(text);
        block.points = (std::clamp)(points, 4.0f, 200.0f);
        block.bold = bold;
        block.align = static_cast<DocAlign>(align);
        model_.blocks.push_back(std::move(block));
    }

    // Text of a paragraph inside a table cell (more may follow in the cell).
    void CellText(const std::wstring& text) {
        in_table_ = true;
        const std::wstring trimmed = Trim(text);
        if (trimmed.empty()) return;
        if (!cell_.empty()) cell_ += L' ';
        cell_ += trimmed;
    }
    void EndCell() {
        in_table_ = true;
        row_.push_back(Trim(cell_));
        cell_.clear();
    }
    void EndRow() {
        in_table_ = true;
        if (!cell_.empty()) EndCell();
        if (!row_.empty()) {
            if (table_.rows.size() < max_rows_) table_.rows.push_back(std::move(row_));
            else truncated_ = true;
        }
        row_.clear();
    }
    void CloseTable() {
        if (!in_table_) return;
        if (!cell_.empty() || !row_.empty()) EndRow();
        if (!table_.rows.empty() && !Full()) {
            table_.table = true;
            model_.blocks.push_back(std::move(table_));
        } else if (!table_.rows.empty()) truncated_ = true;
        table_ = DocBlock{};
        in_table_ = false;
    }
    void Finish() { CloseTable(); }

private:
    DocModel& model_;
    size_t max_blocks_;
    size_t max_rows_;
    bool in_table_ = false;
    bool truncated_ = false;
    DocBlock table_;
    std::vector<std::wstring> row_;
    std::wstring cell_;
};

void ApplySection(const std::vector<uint8_t>& word, const std::vector<uint8_t>& table,
                  uint32_t fc, uint32_t lcb, DocModel& model) {
    if (lcb < 16 || fc > table.size() || lcb > table.size() - fc) return;
    const size_t n = (lcb - 4) / 16;
    if (!n) return;
    const uint32_t fc_sepx = U32(table, fc + 4 * (n + 1) + 2);
    if (fc_sepx == 0xFFFFFFFFu || static_cast<size_t>(fc_sepx) + 2 > word.size()) return;
    const size_t cb = U16(word, fc_sepx);
    if (static_cast<size_t>(fc_sepx) + 2 + cb > word.size()) return;
    float width = 0, height = 0, left = -1, right = -1, top = -1, gap = -1;
    int columns = 0;
    ForEachSprm(word.data() + fc_sepx + 2, cb, [&](const Sprm& sprm) {
        const float twips = static_cast<float>(sprm.Word());
        switch (sprm.code) {
        case kSprmSXaPage: width = twips / 20.0f; break;
        case kSprmSYaPage: height = twips / 20.0f; break;
        case kSprmSDxaLeft: left = twips / 20.0f; break;
        case kSprmSDxaRight: right = twips / 20.0f; break;
        case kSprmSDyaTop: top = std::abs(static_cast<float>(static_cast<int16_t>(sprm.Word()))) / 20.0f; break;
        case kSprmSCcolumns: columns = sprm.Word() + 1; break;
        case kSprmSDxaColumns: gap = twips / 20.0f; break;
        default: break;
        }
    });
    if (width >= 72 && width <= 5000 && height >= 72 && height <= 5000) {
        model.page_width = width;
        model.page_height = height;
    }
    if (left >= 0 && left < model.page_width / 2) model.margin_left = left;
    if (right >= 0 && right < model.page_width / 2) model.margin_right = right;
    if (top >= 0 && top < model.page_height / 2) model.margin_top = top;
    if (columns >= 1 && columns <= 6) model.columns = columns;
    if (gap >= 0 && gap < model.page_width / 4) model.column_gap = gap;
}

// ---- RTF -------------------------------------------------------------------------------

class RtfReader {
public:
    enum class Mode { FirstPage, Text };
    RtfReader(const std::string& data, DocModel& model, size_t max_blocks = kMaxBlocks,
              size_t max_rows = kMaxTableRows, Mode mode = Mode::FirstPage, bool input_cut = false)
        : data_(data), model_(model), out_(model, max_blocks, max_rows), mode_(mode), input_cut_(input_cut) {}

    bool Truncated() const { return out_.Truncated() || (!stop_ && pos_ < data_.size()); }

    bool Run() {
        if (data_.compare(0, 5, "{\\rtf") != 0) return false;
        states_.push_back(State{});
        size_t guard = 0;
        while (pos_ < data_.size() && !out_.Full() && !stop_ && ++guard < data_.size() * 2) {
            const char c = data_[pos_];
            if (c == '{') { if (states_.size() >= 256) return false; FlushBytes(); states_.push_back(states_.back()); ++pos_; continue; }
            if (c == '}') {
                FlushBytes();
                if (states_.size() <= 1) return false;
                states_.pop_back();
                ++pos_;
                continue;
            }
            if (c == '\\') { ControlWord(); continue; }
            ++pos_;
            if (c == '\r' || c == '\n') continue;
            if (Skipping()) continue;
            if (skip_chars_ > 0) { --skip_chars_; continue; }
            FlushBytes();
            Append(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        if (!input_cut_ && pos_ == data_.size() && states_.size() != 1) return false;
        FlushBytes();
        if (!text_.empty()) EndParagraph();
        out_.Finish();
        return true;
    }

private:
    struct State {
        bool skip = false;
        int uc = 1;
        int half_points = 24;
        bool bold = false;
    };

    bool Skipping() const { return states_.back().skip; }

    void Append(wchar_t ch) {
        if (text_.empty() && ch != L' ') {
            para_half_ = states_.back().half_points;
            para_bold_ = states_.back().bold;
        }
        text_ += ch;
    }

    void FlushBytes() {
        if (bytes_.empty()) return;
        const int count = MultiByteToWideChar(codepage_, 0, bytes_.data(), static_cast<int>(bytes_.size()), nullptr, 0);
        if (count > 0) {
            std::wstring wide(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(codepage_, 0, bytes_.data(), static_cast<int>(bytes_.size()), wide.data(), count);
            if (!Skipping()) for (wchar_t ch : wide) Append(ch);
        }
        bytes_.clear();
    }

    void EndParagraph() {
        if (in_table_) out_.CellText(text_);
        else out_.Paragraph(text_, static_cast<float>(para_half_) / 2.0f, para_bold_, align_);
        text_.clear();
    }

    void ControlWord() {
        ++pos_;                                    // backslash
        if (pos_ >= data_.size()) return;
        const char c = data_[pos_];
        if (c == '\'') {                           // \'hh: one byte in the document code page
            if (pos_ + 2 < data_.size()) {
                const std::string hex = data_.substr(pos_ + 1, 2);
                pos_ += 3;
                char* end = nullptr;
                const long value = strtol(hex.c_str(), &end, 16);
                if (Skipping()) return;
                if (skip_chars_ > 0) { --skip_chars_; return; }
                if (end && *end == '\0') bytes_.push_back(static_cast<char>(value));
            } else {
                pos_ = data_.size();
            }
            return;
        }
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            ++pos_;
            FlushBytes();
            if (c == '*') { states_.back().skip = true; return; }
            if (Skipping()) return;
            if (c == '~') Append(L' ');
            else if (c == '_') Append(L'-');
            else if (c == '\\' || c == '{' || c == '}') Append(static_cast<wchar_t>(c));
            else if (c == '\r' || c == '\n') EndParagraph();
            return;
        }
        size_t start = pos_;
        while (pos_ < data_.size() && std::isalpha(static_cast<unsigned char>(data_[pos_]))) ++pos_;
        const std::string word = data_.substr(start, pos_ - start);
        bool has_value = false;
        long value = 0;
        if (pos_ < data_.size() && (data_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(data_[pos_])))) {
            size_t digits = pos_;
            if (data_[digits] == '-') ++digits;
            while (digits < data_.size() && std::isdigit(static_cast<unsigned char>(data_[digits])) && digits - pos_ < 10) ++digits;
            value = strtol(data_.substr(pos_, digits - pos_).c_str(), nullptr, 10);
            has_value = true;
            pos_ = digits;
        }
        if (pos_ < data_.size() && data_[pos_] == ' ') ++pos_;
        if (word != "u") FlushBytes();
        Keyword(word, has_value, value);
    }

    void Keyword(const std::string& word, bool has_value, long value) {
        static const char* const kSkipped[] = {
            "fonttbl", "colortbl", "stylesheet", "info", "pict", "header", "footer", "headerl",
            "headerr", "headerf", "footerl", "footerr", "footerf", "footnote", "annotation",
            "listtable", "listoverridetable", "rsidtbl", "generator", "themedata",
            "colorschememapping", "latentstyles", "datastore", "fldinst", "object", "objdata",
            "xmlnstbl", "filetbl", "revtbl", "mmathPr", "shpinst", "nonshppict", "bkmkstart",
            "bkmkend", "docvar", "userprops", "pgdsctbl", "atnid", "atnauthor"};
        for (const char* skipped : kSkipped) {
            if (word == skipped) { states_.back().skip = true; return; }
        }
        if (word == "bin" && has_value && value >= 0) {
            const size_t count = static_cast<size_t>(value);
            if (count > data_.size() - pos_) { pos_ = data_.size(); return; }
            pos_ += count; return;
        }
        State& state = states_.back();
        if (word == "ansicpg" && has_value && value > 0) { codepage_ = static_cast<UINT>(value); return; }
        if (word == "uc" && has_value) { state.uc = static_cast<int>((std::clamp)(value, 0L, 8L)); return; }
        const float points = static_cast<float>(value) / 20.0f;
        if (word == "paperw" && value > 0) { page_w_ = points; return; }
        if (word == "paperh" && value > 0) { page_h_ = points; return; }
        if (word == "margl" && value >= 0) { model_.margin_left = points; return; }
        if (word == "margr" && value >= 0) { model_.margin_right = points; return; }
        if (word == "margt" && value >= 0) { model_.margin_top = points; return; }
        if (word == "cols" && value >= 1 && value <= 6) { model_.columns = static_cast<int>(value); return; }
        if (Skipping()) return;
        if (word == "u" && has_value) {
            FlushBytes();
            Append(static_cast<wchar_t>(value < 0 ? value + 65536L : value));
            skip_chars_ = state.uc;
            return;
        }
        if (word == "par") { EndParagraph(); return; }
        if (word == "page" || word == "sect") {
            EndParagraph();
            stop_ = mode_ == Mode::FirstPage;
            return;
        }
        if (word == "line" || word == "tab" || word == "emspace" || word == "enspace") { Append(L' '); return; }
        if (word == "cell") { EndCellParagraph(); out_.EndCell(); return; }
        if (word == "nestcell") { EndCellParagraph(); return; }
        if (word == "row") { if (!text_.empty()) EndCellParagraph(); out_.EndRow(); return; }
        if (word == "intbl") { in_table_ = true; return; }
        if (word == "pard") {          // cell paragraphs repeat \intbl after it
            in_table_ = false;
            align_ = static_cast<int>(DocAlign::Left);
            return;
        }
        if (word == "ql") { align_ = static_cast<int>(DocAlign::Left); return; }
        if (word == "qc") { align_ = static_cast<int>(DocAlign::Center); return; }
        if (word == "qr") { align_ = static_cast<int>(DocAlign::Right); return; }
        if (word == "qj" || word == "qd") { align_ = static_cast<int>(DocAlign::Justify); return; }
        if (word == "b") { state.bold = !has_value || value != 0; return; }
        if (word == "fs" && value > 0) { state.half_points = static_cast<int>(value); return; }
        if (word == "plain") { state.bold = false; state.half_points = 24; return; }
        if (word == "emdash") { Append(0x2014); return; }
        if (word == "endash") { Append(0x2013); return; }
        if (word == "bullet") { Append(0x2022); return; }
        if (word == "lquote") { Append(0x2018); return; }
        if (word == "rquote") { Append(0x2019); return; }
        if (word == "ldblquote") { Append(0x201C); return; }
        if (word == "rdblquote") { Append(0x201D); return; }
    }

    void EndCellParagraph() {
        out_.CellText(text_);
        text_.clear();
    }

public:
    float page_w_ = 0, page_h_ = 0;

private:
    const std::string& data_;
    DocModel& model_;
    BlockBuilder out_;
    Mode mode_;
    bool input_cut_ = false;
    std::vector<State> states_;
    std::string bytes_;
    std::wstring text_;
    size_t pos_ = 0;
    int skip_chars_ = 0;
    UINT codepage_ = 1252;
    bool in_table_ = false;
    bool stop_ = false;
    int align_ = 0;
    int para_half_ = 24;
    bool para_bold_ = false;
};

} // namespace

WordFileFormat SniffWordFile(const std::wstring& path) {
    File file;
    uint8_t head[8]{};
    if (!file.Open(path) || file.Size() < 8 || !file.ReadAt(0, head, sizeof(head)))
        return WordFileFormat::Unknown;
    if (head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) return WordFileFormat::OpenXml;
    if (head[0] == 0xD0 && head[1] == 0xCF && head[2] == 0x11 && head[3] == 0xE0) return WordFileFormat::Binary;
    if (memcmp(head, "{\\rtf", 5) == 0) return WordFileFormat::Rtf;
    return WordFileFormat::Unknown;
}

bool ReadDocBinaryModel(const std::wstring& path, DocModel& model, std::wstring* error, bool* truncated) {
    if (truncated) *truncated = false;
    model = DocModel{};
    CompoundFile cfb;
    if (!cfb.Open(path)) { SetError(error, L"doc-not-compound"); return false; }
    std::vector<uint8_t> word;
    if (!cfb.ReadStream(L"WordDocument", kWordStreamBytes, word) || word.size() < 0x200) {
        SetError(error, L"doc-no-word-stream");
        return false;
    }
    if (U16(word, 0) != 0xA5EC) { SetError(error, L"doc-bad-fib"); return false; }
    if (U16(word, 2) < 101) { SetError(error, L"doc-legacy-format"); return false; }   // Word 6/95
    const uint16_t flags = U16(word, 0x0A);
    if (flags & 0x8100) { SetError(error, L"doc-encrypted"); return false; }
    std::vector<uint8_t> table;
    if (!cfb.ReadStream((flags & 0x0200) ? L"1Table" : L"0Table", kTableStreamBytes, table)) {
        SetError(error, L"doc-no-table-stream");
        return false;
    }
    // FIB: FibBase (32) | csw | FibRgW | cslw | FibRgLw | cbRgFcLcb | FibRgFcLcb
    const size_t csw = U16(word, 32);
    const size_t lw = 34 + csw * 2;
    const size_t cslw = U16(word, lw);
    const size_t rg_lw = lw + 2;
    const uint32_t ccp_text = U32(word, rg_lw + 3 * 4);
    const size_t fclcb_count_at = rg_lw + cslw * 4;
    const size_t fclcb_count = U16(word, fclcb_count_at);
    const size_t fclcb = fclcb_count_at + 2;
    if (cslw < 4 || fclcb > word.size() || fclcb_count > (word.size() - fclcb) / 8) {
        SetError(error, L"doc-bad-fib"); return false;
    }
    auto pair = [&](size_t index, uint32_t& fc, uint32_t& lcb) {
        fc = lcb = 0;
        if (index >= fclcb_count) return false;
        fc = U32(word, fclcb + index * 8);
        lcb = U32(word, fclcb + index * 8 + 4);
        return lcb != 0;
    };
    uint32_t fc = 0, lcb = 0;
    if (!ccp_text || !pair(33, fc, lcb)) { SetError(error, L"doc-no-text"); return false; }
    std::vector<DocChar> text;
    if (!ReadText(word, table, fc, lcb, ccp_text, text, truncated ? kDocumentChars : kMaxChars)) { SetError(error, L"doc-no-text"); return false; }

    StyleSheet styles;
    if (pair(1, fc, lcb)) styles = ReadStyleSheet(table, fc, lcb);
    FkpIndex papx, chpx;
    if (pair(13, fc, lcb)) papx.Load(table, fc, lcb, word, true);
    if (pair(12, fc, lcb)) chpx.Load(table, fc, lcb, word, false);
    if (pair(6, fc, lcb)) ApplySection(word, table, fc, lcb, model);

    BlockBuilder out(model, truncated ? kDocumentBlocks : kMaxBlocks, truncated ? kDocumentRows : kMaxTableRows);
    if (truncated) *truncated = text.size() < ccp_text;
    std::wstring paragraph;
    uint32_t first_fc = 0;
    bool have_first = false;
    std::vector<bool> field_code;       // per open field: still in its code part
    auto in_field_code = [&] {
        return std::any_of(field_code.begin(), field_code.end(), [](bool code) { return code; });
    };
    for (const DocChar& c : text) {
        if (out.Full()) { if (truncated) *truncated = true; break; }
        const wchar_t ch = c.ch;
        if (ch == 0x13) { field_code.push_back(true); continue; }
        if (ch == 0x14) { if (!field_code.empty()) field_code.back() = false; continue; }
        if (ch == 0x15) { if (!field_code.empty()) field_code.pop_back(); continue; }
        const bool mark = ch == 0x0D || ch == 0x07 || ch == 0x0C;
        if (!mark && in_field_code()) continue;
        if (!mark) {
            wchar_t shown = ch;
            if (ch == 0x0B || ch == 0x09 || ch == 0x0E) shown = L' ';
            else if (ch == 0x1E) shown = L'-';
            else if (ch < 0x20) continue;              // pictures, objects, notes, optional hyphens
            if (!have_first && !std::iswspace(shown) && shown != 0x3000) { first_fc = c.fc; have_first = true; }
            paragraph += shown;
            continue;
        }
        // Paragraph (or cell / row) mark: its PAPX says where it belongs.
        bool in_table = false, row_end = false, inner_row_end = false;
        int itap = 0, align = -1;
        uint16_t istd = 0;
        if (const Run* run = papx.Find(c.fc)) {
            istd = run->istd;
            ForEachSprm(papx.Data(*run), run->size, [&](const Sprm& sprm) {
                switch (sprm.code) {
                case kSprmPFInTable: in_table = sprm.Byte() != 0; break;
                case kSprmPFTtp: row_end = sprm.Byte() != 0; break;
                case kSprmPItap: itap = sprm.Long(); break;
                case kSprmPFInnerTtp: inner_row_end = sprm.Byte() != 0; break;
                case kSprmPJc80: case kSprmPJc: align = ToAlign(sprm.Byte()); break;
                default: break;
                }
            });
        }
        if (itap >= 1) in_table = true;
        if (in_table) {
            if (itap > 1) {
                // Nested table: its cells read as text of the outer cell.
                if (!inner_row_end) out.CellText(paragraph);
            } else if (row_end) {
                out.EndRow();
            } else if (ch == 0x07) {
                out.CellText(paragraph);
                out.EndCell();
            } else {
                out.CellText(paragraph);
            }
        } else {
            int half = -1, bold = -1;
            if (have_first) {
                if (const Run* run = chpx.Find(first_fc)) {
                    ForEachSprm(chpx.Data(*run), run->size, [&](const Sprm& sprm) {
                        if (sprm.code == kSprmCHps) half = sprm.Word();
                        else if (sprm.code == kSprmCFBold) bold = sprm.Byte();
                    });
                }
            }
            const int style_half = styles.Resolve(istd, [](const DocStyle& s) { return s.half_points; }, 20);
            const bool style_bold = styles.Resolve(istd, [](const DocStyle& s) { return s.bold; }, 0) == 1;
            bool is_bold = style_bold;
            if (bold == 0 || bold == 1) is_bold = bold == 1;
            else if (bold == 0x81) is_bold = !style_bold;
            if (align < 0) align = styles.Resolve(istd, [](const DocStyle& s) { return s.align; }, 0);
            out.Paragraph(paragraph, static_cast<float>(half > 0 ? half : style_half) / 2.0f, is_bold, align);
        }
        paragraph.clear();
        have_first = false;
        if (!truncated && ch == 0x0C && !model.blocks.empty()) break;   // first page only
    }
    if (!paragraph.empty()) out.Paragraph(paragraph, 10.5f, false, 0);
    out.Finish();
    if (truncated) *truncated = *truncated || out.Truncated();
    if (model.blocks.empty()) { SetError(error, L"doc-empty"); return false; }
    return true;
}

bool ReadRtfModel(const std::wstring& path, DocModel& model, std::wstring* error, bool* truncated) {
    if (truncated) *truncated = false;
    model = DocModel{};
    File file;
    if (!file.Open(path)) { SetError(error, L"rtf-open-failed"); return false; }
    std::string data(static_cast<size_t>((std::min<uint64_t>)(file.Size(), kRtfBytes)), '\0');
    if (data.empty() || !file.ReadAt(0, data.data(), data.size())) { SetError(error, L"rtf-read-failed"); return false; }
    RtfReader reader(data, model, truncated ? kDocumentBlocks : kMaxBlocks,
                     truncated ? kDocumentRows : kMaxTableRows,
                     truncated ? RtfReader::Mode::Text : RtfReader::Mode::FirstPage, file.Size() > kRtfBytes);
    if (!reader.Run()) { SetError(error, L"rtf-not-rtf"); return false; }
    if (truncated) *truncated = file.Size() > kRtfBytes || reader.Truncated();
    if (reader.page_w_ >= 72 && reader.page_h_ >= 72 && reader.page_w_ <= 5000 && reader.page_h_ <= 5000) {
        model.page_width = reader.page_w_;
        model.page_height = reader.page_h_;
    }
    model.margin_left = (std::clamp)(model.margin_left, 0.0f, model.page_width / 3);
    model.margin_right = (std::clamp)(model.margin_right, 0.0f, model.page_width / 3);
    model.margin_top = (std::clamp)(model.margin_top, 0.0f, model.page_height / 3);
    if (model.blocks.empty()) { SetError(error, L"rtf-empty"); return false; }
    return true;
}

bool ReadRtfText(const std::wstring& path, std::wstring& text, bool* truncated, std::wstring* error) {
    constexpr size_t kTextBlocks = 4000, kTextRows = 4000, kTextChars = 12000;  // = text preview cap
    text.clear();
    if (truncated) *truncated = false;
    File file;
    if (!file.Open(path)) { SetError(error, L"rtf-open-failed"); return false; }
    std::string data(static_cast<size_t>((std::min<uint64_t>)(file.Size(), kRtfBytes)), '\0');
    if (data.empty() || !file.ReadAt(0, data.data(), data.size())) { SetError(error, L"rtf-read-failed"); return false; }
    DocModel model;
    RtfReader reader(data, model, kTextBlocks, kTextRows, RtfReader::Mode::Text, file.Size() > kRtfBytes);
    if (!reader.Run()) { SetError(error, L"rtf-not-rtf"); return false; }
    bool cut = file.Size() > kRtfBytes || reader.Truncated();
    for (const DocBlock& block : model.blocks) {
        if (text.size() >= kTextChars) { cut = true; break; }
        if (!block.table) {
            text += block.text;
            text += L"\r\n";
            continue;
        }
        for (const auto& row : block.rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) text += L'\t';
                text += row[i];
            }
            text += L"\r\n";
        }
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    if (text.size() > kTextChars) {
        size_t length = kTextChars;
        if (text[length - 1] >= 0xD800 && text[length - 1] <= 0xDBFF &&
            text[length] >= 0xDC00 && text[length] <= 0xDFFF) --length;
        text.resize(length);
        cut = true;
    }
    if (truncated) *truncated = cut;
    if (model.blocks.empty()) { SetError(error, L"rtf-empty"); return false; }
    return true;
}

bool ReadFilterTextModel(const std::wstring& path, DocModel& model, std::wstring* error) {
    model = DocModel{};
    using LoadIFilterFn = HRESULT(WINAPI*)(PCWSTR, IUnknown*, void**);
    static const LoadIFilterFn load = [] {
        const HMODULE module = LoadLibraryExW(L"query.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<LoadIFilterFn>(GetProcAddress(module, "LoadIFilter")) : nullptr;
    }();
    ComPtr<IFilter> filter;
    if (!load || FAILED(load(path.c_str(), nullptr, reinterpret_cast<void**>(filter.GetAddressOf()))) || !filter) {
        SetError(error, L"filter-unavailable");
        return false;
    }
    ULONG flags = 0;
    if (FAILED(filter->Init(IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_CANON_SPACES |
                            IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, nullptr, &flags))) {
        SetError(error, L"filter-init-failed");
        return false;
    }
    std::wstring all;
    STAT_CHUNK chunk{};
    for (int chunks = 0; chunks < 256 && all.size() < kMaxChars; ++chunks) {
        const HRESULT next = filter->GetChunk(&chunk);
        if (next == FILTER_E_END_OF_CHUNKS || FAILED(next)) break;
        if (!(chunk.flags & CHUNK_TEXT)) continue;
        if (chunk.breakType == CHUNK_EOP || chunk.breakType == CHUNK_EOS || chunk.breakType == CHUNK_EOC)
            all += L'\n';
        wchar_t buffer[4096];
        for (int reads = 0; reads < 64 && all.size() < kMaxChars; ++reads) {
            ULONG count = ARRAYSIZE(buffer);
            const HRESULT got = filter->GetText(&count, buffer);
            if (FAILED(got) || count == 0) break;
            all.append(buffer, count);
            if (got == FILTER_S_LAST_TEXT) break;
        }
    }
    BlockBuilder out(model);
    std::wstring line;
    for (wchar_t ch : all) {
        if (ch == L'\r' || ch == L'\n' || ch == 0x0B || ch == 0x0C) {
            if (!Trim(line).empty()) out.Paragraph(line, 10.5f, false, 0);
            line.clear();
            if (out.Full()) break;
        } else {
            line += ch < 0x20 ? L' ' : ch;
        }
    }
    if (!Trim(line).empty()) out.Paragraph(line, 10.5f, false, 0);
    out.Finish();
    if (model.blocks.empty()) { SetError(error, L"filter-empty"); return false; }
    return true;
}

bool ReadWordModel(const std::wstring& path, DocModel& model, std::wstring* error) {
    bool ok = false;
    switch (SniffWordFile(path)) {
    case WordFileFormat::OpenXml: ok = ReadDocxModel(path, model, error); break;
    case WordFileFormat::Binary: ok = ReadDocBinaryModel(path, model, error); break;
    case WordFileFormat::Rtf: ok = ReadRtfModel(path, model, error); break;
    default: SetError(error, L"office-not-word"); break;
    }
    if (ok) return true;
    std::wstring structured = error ? *error : std::wstring();
    if (ReadFilterTextModel(path, model, nullptr)) return true;
    if (error) *error = structured.empty() ? L"office-not-word" : structured;
    return false;
}

} // namespace pulse::preview
