#include "legacy_word_document.h"
#include "office_doc_model.h"
#include "docx_document.h"
#include "doc_payload.h"
#include "compound_file.h"
#include "../ipc/preview_protocol.h"
#include <algorithm>

namespace pulse::preview {
bool MakeLegacyWordDocument(const std::wstring& path, std::wstring& payload,
                            uint32_t& bytes_read, bool& truncated) {
    payload.clear();
    bytes_read = 0;
    truncated = false;
    const WordFileFormat format = SniffWordFile(path);
    if (format == WordFileFormat::OpenXml)
        return MakeDocxDocument(path, payload, bytes_read, truncated);
    DocModel model;
    bool cut = false;
    if (format == WordFileFormat::Binary) {
        if (!ReadDocBinaryModel(path, model, nullptr, &cut)) return false;
    } else if (format == WordFileFormat::Rtf) {
        if (!ReadRtfModel(path, model, nullptr, &cut)) return false;
    } else return false;
    DocPayload out(ipc::kPreviewMaxTableChars - 256);
    size_t words = 0;
    for (const auto& block : model.blocks) {
        if (out.full()) { cut = true; break; }
        if (!block.table) {
            out.Begin(L'p');
            out.Text(block.text, block.bold ? kDocBold : 0u);
            out.End();
            words += CountWords(block.text);
        } else {
            size_t columns = 0;
            for (const auto& row : block.rows) columns = (std::max)(columns, row.size());
            if (!columns) continue;
            out.TableBegin(columns);
            for (const auto& row : block.rows) {
                if (out.full()) { cut = true; break; }
                out.TableRow();
                for (size_t column = 0; column < columns; ++column) {
                    out.Begin(L't');
                    if (column < row.size()) {
                        out.Text(row[column]);
                        words += CountWords(row[column]);
                    }
                    out.End(true);
                }
            }
            out.TableEnd();
        }
    }
    if (!out.blocks()) return false;
    truncated = cut || out.full();
    out.str() += std::wstring(L"M\t") + (format == WordFileFormat::Rtf ? L"rtf" : L"doc") +
                 L"\t" + std::to_wstring(words) + L'\n';
    File file;
    if (file.Open(path)) bytes_read = static_cast<uint32_t>((std::min<uint64_t>)(file.Size(), 0xFFFFFFFFu));
    payload.swap(out.str());
    return true;
}
}
