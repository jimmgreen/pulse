#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../ipc/preview_protocol.h"
#include "../preview_host/table_document.h"
#include "../preview_host/epub_document.h"
#include "../preview_host/docx_document.h"
#include "preview_host_client.h"

namespace {
using Entry = std::pair<std::string, std::string>;
int failures = 0;

void Check(bool pass, const char* message) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", message);
    if (!pass) ++failures;
}

void Put16(std::string& out, uint16_t value) {
    out += static_cast<char>(value);
    out += static_cast<char>(value >> 8);
}

void Put32(std::string& out, uint32_t value) {
    Put16(out, static_cast<uint16_t>(value));
    Put16(out, static_cast<uint16_t>(value >> 16));
}

uint32_t Crc32(const std::string& value) {
    uint32_t crc = 0xffffffffu;
    for (unsigned char c : value) {
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

bool Zip(const std::filesystem::path& path, const std::vector<Entry>& entries) {
    std::string data, directory;
    for (const auto& [name, body] : entries) {
        const auto offset = static_cast<uint32_t>(data.size());
        const auto size = static_cast<uint32_t>(body.size());
        const auto length = static_cast<uint16_t>(name.size());
        const auto crc = Crc32(body);
        Put32(data, 0x04034b50); Put16(data, 20); Put16(data, 0); Put16(data, 0);
        Put16(data, 0); Put16(data, 0); Put32(data, crc); Put32(data, size); Put32(data, size);
        Put16(data, length); Put16(data, 0); data += name; data += body;
        Put32(directory, 0x02014b50); Put16(directory, 20); Put16(directory, 20);
        Put16(directory, 0); Put16(directory, 0); Put16(directory, 0); Put16(directory, 0);
        Put32(directory, crc); Put32(directory, size); Put32(directory, size); Put16(directory, length);
        Put16(directory, 0); Put16(directory, 0); Put16(directory, 0); Put16(directory, 0);
        Put32(directory, 0); Put32(directory, offset); directory += name;
    }
    const auto offset = static_cast<uint32_t>(data.size());
    data += directory;
    Put32(data, 0x06054b50); Put16(data, 0); Put16(data, 0);
    Put16(data, static_cast<uint16_t>(entries.size())); Put16(data, static_cast<uint16_t>(entries.size()));
    Put32(data, static_cast<uint32_t>(directory.size())); Put32(data, offset); Put16(data, 0);
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.good();
}

size_t Count(const std::wstring& text, const std::wstring& needle) {
    size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::wstring::npos) { ++count; at += needle.size(); }
    return count;
}

bool TocPointsTo(const std::wstring& payload, size_t section, size_t block, const std::wstring& marker) {
    const std::wstring record = L"C\t0\t" + std::to_wstring(section) + L'\t' + std::to_wstring(block) + L"\tLast\n";
    if (payload.find(record) == std::wstring::npos) return false;
    size_t current_section = 0, current_block = 0;
    bool in_section = false;
    for (size_t pos = 0; pos < payload.size();) {
        const size_t end = payload.find(L'\n', pos);
        const auto line = payload.substr(pos, end == std::wstring::npos ? end : end - pos);
        if (line.starts_with(L"P\t")) {
            if (in_section) ++current_section;
            in_section = true;
            current_block = 0;
        } else if (in_section && line.starts_with(L"B\t")) {
            if (current_section == section && current_block == block)
                return line.find(marker) != std::wstring::npos;
            ++current_block;
        }
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    const auto root = std::filesystem::path(L"bench_data") /
        (L"document_completeness_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::wstring payload;
    uint32_t bytes = 0;
    bool truncated = false;
    for (const size_t count : {size_t{16}, size_t{17}, size_t{100}}) {
        std::string book = "<workbook><sheets>", rels = "<Relationships>";
        std::vector<Entry> entries;
        for (size_t i = 1; i <= count; ++i) {
            const auto number = std::to_string(i);
            book += "<sheet name=\"Sheet" + number + "\" r:id=\"r" + number + "\"/>";
            rels += "<Relationship Id=\"r" + number + "\" Target=\"worksheets/s" + number + ".xml\"/>";
            entries.emplace_back("xl/worksheets/s" + number + ".xml",
                "<worksheet><sheetData><row r=\"1\"><c r=\"A1\"><v>" + number + "</v></c></row></sheetData></worksheet>");
        }
        book += "</sheets></workbook>"; rels += "</Relationships>";
        entries.emplace_back("xl/workbook.xml", book);
        entries.emplace_back("xl/_rels/workbook.xml.rels", rels);
        for (const auto* extension : {L".xlsx", L".xlsm"}) {
            const auto file = root / (L"sheets" + std::to_wstring(count) + extension);
            Check(Zip(file, entries), "write worksheet boundary fixture");
            Check(pulse::preview::MakeXlsxTable(file.wstring(), payload, bytes), "read worksheet boundary fixture");
            Check(Count(payload, L"\nS\txlsx\t") == count, "all workbook positions retained through sheet 100");
            Check(payload.find(L"W\t" + std::to_wstring(count) + L"\t1\t0\n") != std::wstring::npos &&
                Count(payload, L"\nR\t") == 1 && payload.find(L"\nR\t1\n") != std::wstring::npos,
                "initial workbook response reads only the selected first sheet");
            if (count >= 17)
                Check(payload.find(L"S\txlsx\tSheet17\t0\t0\t0\tnot-loaded\t0\n") != std::wstring::npos,
                      "seventeenth sheet is deferred without claiming truncation");
            Check(pulse::preview::MakeXlsxTable(file.wstring(), payload, bytes, static_cast<uint32_t>(count - 1)) &&
                Count(payload, L"\nR\t") == 1 && payload.find(L"\nR\t" + std::to_wstring(count) + L"\n") != std::wstring::npos &&
                payload.find(L"W\t" + std::to_wstring(count) + L"\t1\t" + std::to_wstring(count - 1) + L"\n") != std::wstring::npos,
                "requested final sheet content loads beyond the former 16-sheet boundary");
            Check(payload.size() <= pulse::ipc::kPreviewMaxTableChars, "workbook payload stays within protocol budget");
            Check(!pulse::preview::MakeXlsxTable(file.wstring(), payload, bytes, static_cast<uint32_t>(count)) && payload.empty(),
                "out-of-range sheet request is rejected without stale response content");
        }
    }
    const auto missing = root / L"missing.xlsx";
    Check(Zip(missing, {{"xl/workbook.xml", "<workbook><sheets><sheet name=\"Missing\" r:id=\"missing\"/><sheet name=\"Hidden\" state=\"hidden\" r:id=\"hidden\"/></sheets></workbook>"}}), "write missing and hidden sheet fixture");
    Check(pulse::preview::MakeXlsxTable(missing.wstring(), payload, bytes) &&
        payload.find(L"S\txlsx\tMissing\t0\t0\t1\tmissing-relationship") != std::wstring::npos &&
        payload.find(L"S\txlsx\tHidden\t0\t0\t1\thidden") != std::wstring::npos &&
        payload.find(L"W\t2\t0\t0\n") != std::wstring::npos, "missing references and hidden sheets keep their original positions");
    Check(pulse::preview::MakeXlsxTable(missing.wstring(), payload, bytes, 1) &&
        payload.find(L"W\t2\t0\t1\n") != std::wstring::npos && Count(payload, L"\nR\t") == 0,
        "requesting a hidden sheet preserves the hidden-sheet policy");
    const auto broken = root / L"missing-entry.xlsx";
    Check(Zip(broken, {
        {"xl/workbook.xml", "<workbook><sheets><sheet name=\"Broken\" r:id=\"r1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id=\"r1\" Target=\"worksheets/missing.xml\"/></Relationships>"}}),
        "write missing worksheet member fixture");
    Check(pulse::preview::MakeXlsxTable(broken.wstring(), payload, bytes) &&
        payload.find(L"S\txlsx\tBroken\t0\t0\t1\tread-failed") != std::wstring::npos,
        "failed selected-sheet read retains its original position and an explicit reason");

    const auto lazy = root / L"lazy.xlsx";
    const std::string label(4096, 'x');
    Check(Zip(lazy, {
        {"xl/workbook.xml", "<workbook><sheets><sheet name='Num' id='n'/><sheet name='Text' id='s'/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id='n' Target='n.xml'/><Relationship Id='s' Target='s.xml'/></Relationships>"},
        {"xl/n.xml", "<worksheet><sheetData><row><c><v>123</v></c></row></sheetData></worksheet>"},
        {"xl/s.xml", "<worksheet><sheetData><row><c t='s'><v>1</v></c></row></sheetData></worksheet>"},
        {"xl/sharedStrings.xml", "<sst><si><t>" + label + "</t></si><si><t>resolved</t></si></sst>"}
    }), "write shared string admission fixture");
    Check(pulse::preview::MakeXlsxTable(lazy.wstring(), payload, bytes, 0) && bytes < label.size() &&
          payload.find(L"\nR\t123\n") != payload.npos, "numeric sheet does not read unused string table");
    Check(pulse::preview::MakeXlsxTable(lazy.wstring(), payload, bytes, 1) && bytes > label.size() &&
          payload.find(L"\nR\tresolved\n") != payload.npos, "shared string sheet loads its table on demand");
    const auto classic = root / L"classic.csv";
    std::ofstream(classic, std::ios::binary) << "City;Note\rA;\"left;right\rnext\"\rB;END\r";
    pulse::ipc::PreviewTextEncoding encoding{};
    Check(pulse::preview::MakeCsvTable(classic.wstring(), L".csv", payload, bytes, truncated, encoding) &&
          Count(payload, L"\nR\t") == 3 && payload.find(L"\nR\tA\tleft;right\\nnext\n") != payload.npos &&
          payload.find(L"\nR\tB\tEND\n") != payload.npos &&
          payload.find(L"X\tCity;Note\\nA;") != payload.npos, "classic CR CSV preserves quoted newline and rows");
    if (argc == 2 && std::string_view(argv[1]) == "--tables") return failures ? 1 : 0;

    const auto epub = root / L"chapters.epub";
    std::vector<Entry> entries{
        {"META-INF/container.xml", "<container><rootfiles><rootfile full-path=\"book.opf\"/></rootfiles></container>"},
        {"book.opf", "<package><manifest><item id=\"first\" href=\"first.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"large\" href=\"large.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"svg\" href=\"page.svg\" media-type=\"image/svg+xml\"/><item id=\"last\" href=\"last.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/></manifest><spine><itemref idref=\"first\"/><itemref idref=\"missing\"/><itemref idref=\"large\"/><itemref idref=\"svg\"/><itemref idref=\"last\"/></spine></package>"},
        {"first.xhtml", "<html><head><title>First</title></head><body><p>first-marker</p></body></html>"},
        {"last.xhtml", "<html><head><title>Last</title></head><body><p>before-anchor</p><p id=\"target\">last-marker</p></body></html>"},
        {"nav.xhtml", "<html><body><nav epub:type=\"toc\"><ol><li><a href=\"last.xhtml#target\">Last</a></li></ol></nav></body></html>"},
        {"large.xhtml", std::string(16u * 1024u * 1024u + 1, 'x')},
        {"page.svg", "<svg><text>Vector page</text></svg>"}
    };
    Check(Zip(epub, entries), "write EPUB missing/oversize/SVG fixture");
    Check(pulse::preview::MakeEpubDocument(epub.wstring(), payload, bytes, truncated), "read incomplete EPUB");
    Check(truncated && Count(payload, L"\nP\t") == 5, "EPUB retains all five chapter positions and marks incomplete");
    Check(payload.find(L"Chapter manifest reference is missing") != std::wstring::npos &&
        payload.find(L"16 MiB preview limit") != std::wstring::npos && payload.find(L"SVG chapter") != std::wstring::npos,
        "EPUB placeholders explain missing, oversize and SVG chapters");
    Check(TocPointsTo(payload, 4, 1, L"last-marker"),
        "EPUB TOC resolves the actual second block after three unavailable chapters");
    Check(payload.size() <= pulse::ipc::kPreviewMaxTableChars, "EPUB payload stays within protocol budget");
    const auto explicit_nav = entries[4].second;
    entries[4].second = "<html><head><title>toc notes</title></head><body><nav><ol><li>"
        "<a href='last.xhtml#target'>Last</a></li></ol></nav></body></html>";
    Check(Zip(epub, entries) && pulse::preview::MakeEpubDocument(epub.wstring(), payload, bytes, truncated) &&
        TocPointsTo(payload, 4, 1, L"last-marker"), "untyped EPUB navigation retains its target despite incidental toc text");
    entries[4].second = explicit_nav;
    const std::string boundary_body = "<html><body><p>exact-boundary-marker</p></body></html><!--";
    entries[5].second = boundary_body + std::string(16u * 1024u * 1024u - boundary_body.size() - 3, 'x') + "-->";
    Check(Zip(epub, entries) && pulse::preview::MakeEpubDocument(epub.wstring(), payload, bytes, truncated) &&
        payload.find(L"exact-boundary-marker") != std::wstring::npos && payload.find(L"16 MiB preview limit") == std::wstring::npos,
        "EPUB chapter exactly at 16 MiB remains readable");
    entries.erase(entries.begin() + 5);
    Check(Zip(epub, entries) && pulse::preview::MakeEpubDocument(epub.wstring(), payload, bytes, truncated) &&
        Count(payload, L"\nP\t") == 5 && payload.find(L"Chapter content could not be read") != std::wstring::npos &&
        TocPointsTo(payload, 4, 1, L"last-marker"),
        "EPUB missing package entry keeps its chapter and following TOC index");
    entries[2].second = "<html><body>";
    for (size_t i = 0; i < 450; ++i) entries[2].second += "<p>" + std::string(5000, 'x') + "</p>";
    entries[2].second += "</body></html>";
    Check(Zip(epub, entries) && pulse::preview::MakeEpubDocument(epub.wstring(), payload, bytes, truncated) &&
        truncated && Count(payload, L"\nP\t") == 5 && payload.size() <= pulse::ipc::kPreviewMaxTableChars &&
        payload.find(L"Preview size limit reached; this chapter was not loaded") != std::wstring::npos,
        "EPUB exhausted content budget retains all remaining chapter placeholders within protocol size");

    const auto docx = root / L"reading.docx";
    Check(Zip(docx, {{"word/document.xml", "<w:document><w:body><w:p><w:r><w:t>document-marker</w:t></w:r></w:p></w:body></w:document>"}}), "write DOCX reading fixture");
    Check(pulse::preview::MakeDocxDocument(docx.wstring(), payload, bytes, truncated) &&
        payload.find(L"Reading preview:") == std::wstring::npos && Count(payload, L"\nB\t") == 1 &&
        payload.find(L"document-marker") != std::wstring::npos, "DOCX body contains only document content, without injected notice blocks");
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "start preview host for document IPC checks");
    if (started) {
        pulse_test::Result result;
        Check(host.Request(std::filesystem::absolute(root / L"sheets100.xlsx").wstring(), result, MAXDWORD, 1024,
                pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagRichText, 99) &&
            result.response.kind == pulse::ipc::PreviewContentKind::Table && Count(result.text, L"\nR\t") == 1 &&
            result.response.integrity.state == pulse::preview::IntegrityState::Partial &&
            result.response.integrity.reason == pulse::preview::IntegrityReason::OnDemand &&
            result.response.integrity.loaded == 1 && result.response.integrity.total == 100 &&
            result.text.find(L"\nR\t100\n") != std::wstring::npos && result.text.find(L"W\t100\t1\t99\n") != std::wstring::npos,
            "spreadsheet RichText IPC frame index requests actual sheet 100 content");
        Check(host.Request(std::filesystem::absolute(docx).wstring(), result, MAXDWORD, 1024,
                pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagRichText) &&
            result.response.kind == pulse::ipc::PreviewContentKind::Markdown && result.error == L"docx-reading" &&
            Count(result.text, L"\nB\t") == 1 && result.text.find(L"document-marker") != std::wstring::npos &&
            result.text.find(L"Reading preview:") == std::wstring::npos,
            "DOCX RichText IPC returns Markdown and fidelity metadata without changing body blocks");
        Check(host.Request(std::filesystem::absolute(epub).wstring(), result, MAXDWORD, 1024,
                pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagRichText) &&
            result.response.kind == pulse::ipc::PreviewContentKind::Markdown &&
            (result.response.flags & pulse::ipc::kPreviewFlagTruncated) != 0 && Count(result.text, L"\nP\t") == 5 &&
            result.text.size() <= pulse::ipc::kPreviewMaxTableChars &&
            result.text.find(L"Preview size limit reached; this chapter was not loaded") != std::wstring::npos,
            "EPUB RichText IPC retains chapter placeholders and reports truncated content");
    }
    return failures ? 1 : 0;
}
