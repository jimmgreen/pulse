// office_sheet_model.cpp - see office_sheet_model.h. Record layouts follow
// [MS-XLS] (the reading order of Apache POI HSSF's record stream).
#include "office_sheet_model.h"
#include "compound_file.h"
#include "../ipc/preview_protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace pulse::preview {
namespace {

constexpr size_t kWorkbookBytes = 64u << 20;
constexpr size_t kMaxSharedStrings = 1u << 17;
constexpr size_t kMaxStringChars = 2001; // one extra character detects the table cell limit
constexpr size_t kMaxCells = 20000;
constexpr int kMaxRow = 5000, kMaxCol = 256;

constexpr uint16_t kFormula = 0x0006;
constexpr uint16_t kEof = 0x000A;
constexpr uint16_t kFilePass = 0x002F;
constexpr uint16_t kContinue = 0x003C;
constexpr uint16_t kCodePage = 0x0042;
constexpr uint16_t kBoundSheet = 0x0085;
constexpr uint16_t kMulRk = 0x00BD;
constexpr uint16_t kSst = 0x00FC;
constexpr uint16_t kLabelSst = 0x00FD;
constexpr uint16_t kNumber = 0x0203;
constexpr uint16_t kLabel = 0x0204;
constexpr uint16_t kBoolErr = 0x0205;
constexpr uint16_t kString = 0x0207;
constexpr uint16_t kArray = 0x0221;
constexpr uint16_t kTable = 0x0236;
constexpr uint16_t kRk = 0x027E;
constexpr uint16_t kShrFmla = 0x04BC;
constexpr uint16_t kBof = 0x0809;

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

struct Record {
    uint16_t type = 0;
    size_t body = 0, size = 0;
};

bool RecordAt(const std::vector<uint8_t>& s, size_t at, Record& r) {
    if (at + 4 > s.size()) return false;
    r.type = CfbU16(s, at);
    r.size = CfbU16(s, at + 2);
    r.body = at + 4;
    return r.body + r.size <= s.size();
}

std::wstring FormatDouble(double v) {
    if (!std::isfinite(v)) return L"#NUM!";
    wchar_t buffer[48];
    swprintf_s(buffer, L"%.15g", v);
    return buffer;
}

double DecodeRk(uint32_t rk) {
    double v = 0;
    if (rk & 2) {
        v = static_cast<double>(static_cast<int32_t>(rk) >> 2);
    } else {
        const uint64_t bits = static_cast<uint64_t>(rk & 0xFFFFFFFCu) << 32;
        std::memcpy(&v, &bits, sizeof(v));
    }
    return (rk & 1) ? v / 100.0 : v;
}

const wchar_t* ErrorText(uint8_t code) {
    switch (code) {
    case 0x00: return L"#NULL!";
    case 0x07: return L"#DIV/0!";
    case 0x0F: return L"#VALUE!";
    case 0x17: return L"#REF!";
    case 0x1D: return L"#NAME?";
    case 0x24: return L"#NUM!";
    case 0x2A: return L"#N/A";
    default: return L"#ERR";
    }
}

std::wstring Decode8Bit(const uint8_t* data, size_t count, UINT codepage) {
    if (!count) return {};
    const int n = MultiByteToWideChar(codepage, 0, reinterpret_cast<const char*>(data), static_cast<int>(count),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(codepage, 0, reinterpret_cast<const char*>(data), static_cast<int>(count), out.data(), n);
    return out;
}

// Record payloads read as one byte stream, where a string's characters may
// continue in the next CONTINUE record behind a fresh option byte.
class SegmentReader {
public:
    explicit SegmentReader(std::vector<std::pair<const uint8_t*, size_t>> segments)
        : segments_(std::move(segments)) {}

    bool U8(uint8_t& v) {
        if (!Ensure()) return false;
        v = segments_[seg_].first[pos_++];
        return true;
    }
    bool U16(uint16_t& v) {
        uint8_t a = 0, b = 0;
        if (!U8(a) || !U8(b)) return false;
        v = static_cast<uint16_t>(a | (b << 8));
        return true;
    }
    bool U32(uint32_t& v) {
        uint16_t a = 0, b = 0;
        if (!U16(a) || !U16(b)) return false;
        v = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 16);
        return true;
    }
    bool Skip(size_t n) {
        while (n) {
            if (!Ensure()) return false;
            const size_t step = (std::min)(n, segments_[seg_].second - pos_);
            pos_ += step;
            n -= step;
        }
        return true;
    }
    // cch characters, 8- or 16-bit; a segment boundary inside the characters
    // is followed by an option byte whose bit 0 gives the new width.
    bool Chars(size_t cch, bool wide, std::wstring& out) {
        out.clear();
        for (size_t i = 0; i < cch; ++i) {
            if (pos_ >= segments_[seg_].second) {
                if (++seg_ >= segments_.size()) return false;
                pos_ = 0;
                uint8_t option = 0;
                if (!U8(option)) return false;
                wide = (option & 1) != 0;
            }
            wchar_t c = 0;
            if (wide) {
                uint16_t v = 0;
                if (!U16(v)) return false;
                c = static_cast<wchar_t>(v);
            } else {
                uint8_t v = 0;
                if (!U8(v)) return false;
                c = static_cast<wchar_t>(v);
            }
            if (out.size() < kMaxStringChars) out += c;
        }
        return true;
    }

private:
    bool Ensure() {
        while (seg_ < segments_.size() && pos_ >= segments_[seg_].second) {
            ++seg_;
            pos_ = 0;
        }
        return seg_ < segments_.size();
    }

    std::vector<std::pair<const uint8_t*, size_t>> segments_;
    size_t seg_ = 0, pos_ = 0;
};

// XLUnicodeRichExtendedString list of the shared string table.
void ReadSst(const std::vector<uint8_t>& s, const Record& sst, std::vector<std::wstring>& strings) {
    std::vector<std::pair<const uint8_t*, size_t>> segments{{s.data() + sst.body, sst.size}};
    Record next;
    for (size_t at = sst.body + sst.size; RecordAt(s, at, next) && next.type == kContinue;
         at = next.body + next.size)
        segments.emplace_back(s.data() + next.body, next.size);
    SegmentReader in(std::move(segments));
    uint32_t total = 0, unique = 0;
    if (!in.U32(total) || !in.U32(unique)) return;
    const size_t count = (std::min<size_t>)(unique, kMaxSharedStrings);
    strings.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        uint16_t cch = 0;
        uint8_t flags = 0;
        if (!in.U16(cch) || !in.U8(flags)) return;
        uint16_t runs = 0;
        uint32_t ext = 0;
        if ((flags & 0x08) && !in.U16(runs)) return;
        if ((flags & 0x04) && !in.U32(ext)) return;
        std::wstring text;
        if (!in.Chars(cch, (flags & 1) != 0, text)) return;
        strings.push_back(std::move(text));
        if (!in.Skip(static_cast<size_t>(runs) * 4) || !in.Skip(ext)) return;
    }
}

// XLUnicodeString (BIFF8) or a codepage string (BIFF5) inside one record.
std::wstring RecordString(const std::vector<uint8_t>& s, size_t at, size_t end, bool biff8, UINT codepage) {
    if (at + 2 > end) return {};
    const size_t cch = CfbU16(s, at);
    if (!biff8) return Decode8Bit(s.data() + at + 2, (std::min)(cch, end - at - 2), codepage);
    if (at + 3 > end) return {};
    const bool wide = (s[at + 2] & 1) != 0;
    std::wstring out;
    for (size_t i = 0, p = at + 3; i < cch && out.size() < kMaxStringChars; ++i) {
        if (wide) {
            if (p + 2 > end) break;
            out += static_cast<wchar_t>(CfbU16(s, p));
            p += 2;
        } else {
            if (p + 1 > end) break;
            out += static_cast<wchar_t>(s[p]);
            p += 1;
        }
    }
    return out;
}

bool Blank(const std::wstring& text) {
    for (wchar_t c : text)
        if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n' && c != 0x3000) return false;
    return true;
}

} // namespace

bool ReadXlsSheet(const std::wstring& path, int rows, int cols, SheetModel& model,
                  std::wstring* error, uint32_t sheet_index, XlsWorkbookInfo& workbook) {
    workbook = {};
    rows = (std::clamp)(rows, 1, kMaxRow);
    cols = (std::clamp)(cols, 1, kMaxCol);
    model = SheetModel{};
    CompoundFile cfb;
    if (!cfb.Open(path)) {
        SetError(error, L"not-compound-file");
        return false;
    }
    std::vector<uint8_t> s;
    bool biff8 = true;
    if (!cfb.ReadStream(L"Workbook", kWorkbookBytes, s)) {
        if (!cfb.ReadStream(L"Book", kWorkbookBytes, s)) {
            SetError(error, L"xls-stream-missing");
            return false;
        }
        biff8 = false;
    }
    Record r;
    if (!RecordAt(s, 0, r) || (r.type != kBof && r.type != 0x0409 && r.type != 0x0209)) {
        SetError(error, L"xls-not-biff");
        return false;
    }
    if (r.size >= 2 && CfbU16(s, r.body) < 0x0600) biff8 = false;

    // Workbook globals: codepage, encryption, shared strings and worksheet order.
    UINT codepage = 1252;
    std::vector<std::wstring> strings;
    std::vector<size_t> sheet_offsets;
    workbook.bytes_read = static_cast<uint32_t>(s.size());
    for (size_t at = r.body + r.size; RecordAt(s, at, r); at = r.body + r.size) {
        if (r.type == kEof) break;
        if (r.type == kFilePass) {
            SetError(error, L"xls-encrypted");
            return false;
        }
        if (r.type == kCodePage && r.size >= 2) {
            const UINT cp = CfbU16(s, r.body);
            codepage = cp == 1200 ? 1252 : cp;   // 1200 marks BIFF8 Unicode; the value is unused then
        } else if (r.type == kSst && biff8) {
            ReadSst(s, r, strings);
        } else if (r.type == kBoundSheet && r.size >= 8) {
            const uint8_t sheet_type = s[r.body + 5];
            if (sheet_type == 0) {    // worksheet (not chart / macro sheet)
                if (sheet_offsets.size() >= 256) { workbook.truncated = true; continue; }
                const size_t count = s[r.body + 6];
                const bool wide = biff8 && (s[r.body + 7] & 1) != 0;
                const size_t begin = r.body + (biff8 ? 8 : 7);
                const size_t length = count * (wide ? 2 : 1);
                if (begin + length > r.body + r.size) {
                    SetError(error, L"xls-sheet-name");
                    return false;
                }
                std::wstring name;
                if (!biff8) name = Decode8Bit(s.data() + begin, count, codepage);
                else for (size_t i = 0; i < count; ++i)
                    name += wide ? static_cast<wchar_t>(CfbU16(s, begin + i * 2))
                                 : static_cast<wchar_t>(s[begin + i]);
                workbook.sheet_names.push_back(std::move(name));
                sheet_offsets.push_back(CfbU32(s, r.body));
            }
        }
    }
    const auto empty_grid = [&] {
        model.grid.assign(static_cast<size_t>(rows), std::vector<std::wstring>(static_cast<size_t>(cols)));
    };
    if (sheet_index >= sheet_offsets.size() ||
        !RecordAt(s, sheet_offsets[sheet_index], r) || r.type != kBof) {
        SetError(error, L"xls-sheet-invalid");
        return false;
    }

    std::map<std::pair<int, int>, std::wstring> cells;
    const auto put = [&](size_t row, size_t col, std::wstring text) {
        if (Blank(text)) return;
        if (row >= kMaxRow || col >= kMaxCol || cells.size() >= kMaxCells) {
            workbook.truncated = true;
            return;
        }
        if (text.size() > 2000) { text.resize(2000); workbook.truncated = true; }
        cells[{static_cast<int>(row) + 1, static_cast<int>(col) + 1}] = std::move(text);
    };
    bool pending_string = false;       // FORMULA with a string result: STRING follows
    size_t pending_row = 0, pending_col = 0;
    bool sheet_complete = false;
    for (size_t at = r.body + r.size; RecordAt(s, at, r); at = r.body + r.size) {
        if (r.type == kEof) { sheet_complete = true; break; }
        if (r.type == kBof) break;
        const size_t b = r.body, end = r.body + r.size;
        const size_t row = r.size >= 4 ? CfbU16(s, b) : 0, col = r.size >= 4 ? CfbU16(s, b + 2) : 0;
        switch (r.type) {
        case kLabelSst:
            if (r.size >= 10) {
                const uint32_t index = CfbU32(s, b + 6);
                if (index < strings.size()) put(row, col, strings[index]);
                else workbook.truncated = true;
            }
            break;
        case kLabel:
            put(row, col, RecordString(s, b + 6, end, biff8, codepage));
            break;
        case kNumber:
            if (r.size >= 14) {
                double v = 0;
                std::memcpy(&v, s.data() + b + 6, sizeof(v));
                put(row, col, FormatDouble(v));
            }
            break;
        case kRk:
            if (r.size >= 10) put(row, col, FormatDouble(DecodeRk(CfbU32(s, b + 6))));
            break;
        case kMulRk:
            if (r.size >= 6) {
                const size_t last = CfbU16(s, end - 2);
                for (size_t c = col, p = b + 4; c <= last && p + 6 <= end - 2; ++c, p += 6)
                    put(row, c, FormatDouble(DecodeRk(CfbU32(s, p + 2))));
            }
            break;
        case kBoolErr:
            if (r.size >= 8)
                put(row, col, s[b + 7] ? ErrorText(s[b + 6]) : (s[b + 6] ? L"TRUE" : L"FALSE"));
            break;
        case kFormula:
            if (r.size >= 14) {
                if (CfbU16(s, b + 12) != 0xFFFF) {
                    double v = 0;
                    std::memcpy(&v, s.data() + b + 6, sizeof(v));
                    put(row, col, FormatDouble(v));
                } else if (s[b + 6] == 0) {
                    pending_string = true;
                    pending_row = row;
                    pending_col = col;
                    continue;
                } else if (s[b + 6] == 1) {
                    put(row, col, s[b + 8] ? L"TRUE" : L"FALSE");
                } else if (s[b + 6] == 2) {
                    put(row, col, ErrorText(s[b + 8]));
                }
            }
            break;
        case kShrFmla: case kArray: case kTable:
            // Shared / array / table formula definitions sit between the
            // first FORMULA of their range and its STRING result: keep
            // waiting, or whole columns of text formulas read empty.
            continue;
        case kString:
            if (pending_string) put(pending_row, pending_col, RecordString(s, b, end, biff8, codepage));
            break;
        default:
            break;
        }
        pending_string = false;
    }

    if (!sheet_complete) {
        SetError(error, L"xls-sheet-incomplete");
        return false;
    }

    // Same window as the .xlsx reader: from the first non-empty row, the
    // leftmost non-empty column within the rows shown.
    if (cells.empty()) {
        empty_grid();
        return true;
    }
    const int first_row = cells.begin()->first.first;
    int min_col = 0;
    for (const auto& [key, text] : cells) {
        if (key.first >= first_row + rows) break;
        min_col = min_col ? (std::min)(min_col, key.second) : key.second;
    }
    model.first_row = first_row;
    model.first_col = min_col;
    empty_grid();
    for (const auto& [key, text] : cells) {
        const int rr = key.first - first_row, cc = key.second - min_col;
        if (rr >= 0 && rr < rows && cc >= 0 && cc < cols) model.grid[rr][cc] = text;
    }
    return true;
}

bool ReadXlsModel(const std::wstring& path, int rows, int cols, SheetModel& model, std::wstring* error) {
    XlsWorkbookInfo workbook;
    return ReadXlsSheet(path, rows, cols, model, error, 0, workbook);
}

bool MakeXlsTable(const std::wstring& path, std::wstring& payload, uint32_t& bytes_read,
                  uint32_t sheet_index) {
    payload.clear();
    bytes_read = 0;
    SheetModel model;
    XlsWorkbookInfo workbook;
    if (!ReadXlsSheet(path, kMaxRow, kMaxCol, model, nullptr, sheet_index, workbook)) return false;
    bytes_read = workbook.bytes_read;
    const auto escape = [](const std::wstring& text) {
        std::wstring out;
        for (const wchar_t c : text) {
            if (c == L'\\') out += L"\\\\";
            else if (c == L'\t') out += L"\\t";
            else if (c == L'\n') out += L"\\n";
            else if (c != L'\r') out += c;
        }
        return out;
    };
    size_t rows = 0, cols = 0;
    for (size_t r = 0; r < model.grid.size(); ++r)
        for (size_t c = 0; c < model.grid[r].size(); ++c)
            if (!model.grid[r][c].empty()) {
                rows = (std::max)(rows, r + static_cast<size_t>(model.first_row));
                cols = (std::max)(cols, c + static_cast<size_t>(model.first_col));
            }
    rows = (std::min)(rows, size_t{kMaxRow});
    cols = (std::min)(cols, size_t{kMaxCol});
    std::wstring data;
    size_t loaded_rows = 0;
    // Reserve enough for all sheet names and protocol metadata.
    constexpr size_t data_limit = ipc::kPreviewMaxTableChars - 65536;
    for (size_t r = 0; r < rows; ++r) {
        std::wstring row = L"R";
        for (size_t c = 0; c < cols; ++c) {
            row += L'\t';
            const int rr = static_cast<int>(r) + 1 - model.first_row;
            const int cc = static_cast<int>(c) + 1 - model.first_col;
            if (rr >= 0 && cc >= 0) row += escape(model.grid[rr][cc]);
        }
        row += L'\n';
        if (data.size() + row.size() > data_limit) { workbook.truncated = true; break; }
        data += row;
        ++loaded_rows;
    }
    payload = L"PULSETBL\t1\n";
    for (size_t i = 0; i < workbook.sheet_names.size(); ++i) {
        const bool selected = i == sheet_index;
        payload += L"S\txls\t" + escape(workbook.sheet_names[i]) + L"\t" +
            std::to_wstring(selected ? cols : 0) + L"\t" +
            std::to_wstring(selected ? loaded_rows : 0) + L"\t" +
            (selected && workbook.truncated ? L"1" : L"0") + L"\t" +
            (selected ? L"" : L"not-loaded") + L"\t0\n";
        if (selected) payload += data;
    }
    payload += L"W\t" + std::to_wstring(workbook.sheet_names.size()) + L"\t1\t" +
        std::to_wstring(sheet_index) + L"\n";
    return payload.size() <= ipc::kPreviewMaxTableChars;
}

bool ReadSheetModel(const std::wstring& path, int rows, int cols, SheetModel& model, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, L"open-failed");
        return false;
    }
    unsigned char head[4]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(file, head, sizeof(head), &got, nullptr);
    CloseHandle(file);
    if (ok && got == 4 && head[0] == 0xD0 && head[1] == 0xCF && head[2] == 0x11 && head[3] == 0xE0)
        return ReadXlsModel(path, rows, cols, model, error);
    return ReadXlsxModel(path, rows, cols, model, error);
}

} // namespace pulse::preview
