#include "../preview_host/legacy_word_document.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
namespace {
std::vector<unsigned char> BuildWord97Doc(bool encrypted) {
    constexpr size_t kStream = 4096;  // at the mini-stream cutoff: regular sectors
    constexpr uint32_t kFc0 = 0x600, kEnd = 0xFFFFFFFE, kFree = 0xFFFFFFFF;
    std::vector<unsigned char> word(kStream), table(kStream);
    const auto put16 = [](std::vector<unsigned char>& b, size_t at, uint32_t v) {
        b[at] = static_cast<unsigned char>(v); b[at + 1] = static_cast<unsigned char>(v >> 8);
    };
    const auto put32 = [&](std::vector<unsigned char>& b, size_t at, uint32_t v) {
        put16(b, at, v & 0xFFFF); put16(b, at + 2, v >> 16);
    };
    const std::wstring text = L"Title\rBody \u4E2D\u6587\rA\aB\a\aAfter\r";
    const uint32_t ccp = static_cast<uint32_t>(text.size());
    for (uint32_t i = 0; i < ccp; ++i) put16(word, kFc0 + 2 * i, text[i]);
    put16(word, 0, 0xA5EC);
    put16(word, 2, 193);
    put16(word, 0x0A, 0x0200u | (encrypted ? 0x0100u : 0u));  // fWhichTblStm, fEncrypted
    put16(word, 32, 14);
    put16(word, 62, 22);
    put32(word, 76, ccp);                                        // ccpText
    put16(word, 152, 93);                                        // cbRgFcLcb
    const auto pair = [&](int index, uint32_t fc, uint32_t lcb) {
        put32(word, 154 + 8 * index, fc); put32(word, 158 + 8 * index, lcb);
    };
    // PAPX FKP (page 5): one run per paragraph / cell / row mark.
    std::vector<uint32_t> marks;
    for (uint32_t i = 0; i < ccp; ++i) if (text[i] == L'\r' || text[i] == L'\a') marks.push_back(i);
    const size_t crun = marks.size(), papx = 5 * 512;
    for (size_t k = 0; k < crun; ++k) put32(word, papx + 4 * k, kFc0 + 2 * (k ? marks[k - 1] + 1 : 0));
    put32(word, papx + 4 * crun, kFc0 + 2 * (marks.back() + 1));
    const std::vector<std::vector<unsigned char>> grpprl = {
        {0x61, 0x24, 1}, {}, {0x16, 0x24, 1}, {0x16, 0x24, 1}, {0x16, 0x24, 1, 0x17, 0x24, 1}, {}};
    size_t offset = 0x100;
    for (size_t k = 0; k < crun; ++k) {
        if (grpprl[k].empty()) continue;                         // bx 0: istd 0, no sprms
        std::vector<unsigned char> blob{0, 0};                   // istd 0
        blob.insert(blob.end(), grpprl[k].begin(), grpprl[k].end());
        if (blob.size() % 2) blob.insert(blob.begin(), static_cast<unsigned char>((blob.size() + 1) / 2));
        else { const auto cb = static_cast<unsigned char>(blob.size() / 2); blob.insert(blob.begin(), {0, cb}); }
        std::copy(blob.begin(), blob.end(), word.begin() + papx + offset);
        word[papx + 4 * (crun + 1) + 13 * k] = static_cast<unsigned char>(offset / 2);
        offset += blob.size() + (blob.size() % 2);
    }
    word[papx + 511] = static_cast<unsigned char>(crun);
    // CHPX FKP (page 6): "Title" bold 22 pt, the rest plain.
    const size_t chpx = 6 * 512;
    put32(word, chpx, kFc0); put32(word, chpx + 4, kFc0 + 10); put32(word, chpx + 8, kFc0 + 2 * ccp);
    const unsigned char chp[] = {0x43, 0x4A, 44, 0, 0x35, 0x08, 1};
    word[chpx + 0x100] = static_cast<unsigned char>(sizeof(chp));
    std::copy(std::begin(chp), std::end(chp), word.begin() + chpx + 0x101);
    word[chpx + 12] = 0x80;
    word[chpx + 511] = 2;
    // SEPX at 3584: A4, margins 90 / 90 / 72 pt.
    const uint16_t sep[] = {0xB01F, 11906, 0xB020, 16838, 0xB021, 1800, 0xB022, 1800, 0x9023, 1440};
    put16(word, 3584, static_cast<uint32_t>(sizeof(sep)));
    for (size_t i = 0; i < std::size(sep); ++i) put16(word, 3586 + 2 * i, sep[i]);
    // 1Table: STSH (one paragraph style "Normal", 10.5 pt), PlcfSed, BTEs, Clx.
    const std::vector<unsigned char> stsh = {
        18, 0, 1, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // cbStshi + Stshi
        36, 0, 0, 0, 0xF1, 0xFF, 2, 0, 0, 0, 0, 0,                      // cbStd + StdfBase
        6, 0, 'N', 0, 'o', 0, 'r', 0, 'm', 0, 'a', 0, 'l', 0, 0, 0,     // xstzName
        2, 0, 0, 0, 4, 0, 0x43, 0x4A, 21, 0};                           // UPX papx, chpx
    std::copy(stsh.begin(), stsh.end(), table.begin());
    pair(1, 0, static_cast<uint32_t>(stsh.size()));
    put32(table, 64, 0); put32(table, 68, ccp); put32(table, 74, 3584);              // PlcfSed
    pair(6, 64, 20);
    put32(table, 96, kFc0); put32(table, 100, kFc0 + 2 * ccp); put32(table, 104, 5); // PlcBtePapx
    pair(13, 96, 12);
    put32(table, 112, kFc0); put32(table, 116, kFc0 + 2 * ccp); put32(table, 120, 6);
    pair(12, 112, 12);
    table[128] = 2; put32(table, 129, 16); put32(table, 133, 0); put32(table, 137, ccp);  // Clx
    put32(table, 143, kFc0);                                                           // PCD fc
    pair(33, 128, 21);
    // Compound file: sector 0 FAT, 1 directory, 2-9 WordDocument, 10-17 1Table.
    std::vector<unsigned char> file(512 * 2);
    const unsigned char magic[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    std::copy(std::begin(magic), std::end(magic), file.begin());
    put16(file, 24, 0x3E); put16(file, 26, 3); put16(file, 28, 0xFFFE); put16(file, 30, 9); put16(file, 32, 6);
    put32(file, 44, 1); put32(file, 48, 1); put32(file, 56, 4096);
    put32(file, 60, kEnd); put32(file, 68, kEnd);
    for (size_t i = 0; i < 109; ++i) put32(file, 76 + 4 * i, i ? kFree : 0);
    std::vector<uint32_t> fat(128, kFree);
    fat[0] = 0xFFFFFFFD; fat[1] = kEnd;
    for (uint32_t s = 2; s < 18; ++s) fat[s] = (s == 9 || s == 17) ? kEnd : s + 1;
    for (size_t i = 0; i < fat.size(); ++i) put32(file, 512 + 4 * i, fat[i]);
    std::vector<unsigned char> dir(512);
    const auto entry = [&](size_t index, const wchar_t* name, unsigned char type, uint32_t right,
                           uint32_t child, uint32_t start, uint32_t size) {
        const size_t at = 128 * index;
        size_t n = 0;
        for (; name[n]; ++n) put16(dir, at + 2 * n, name[n]);
        put16(dir, at + 64, static_cast<uint32_t>(2 * (n + 1)));
        dir[at + 66] = type; dir[at + 67] = 1;
        put32(dir, at + 68, kFree); put32(dir, at + 72, right); put32(dir, at + 76, child);
        put32(dir, at + 116, start); put32(dir, at + 120, size);
    };
    entry(0, L"Root Entry", 5, kFree, 1, kEnd, 0);
    entry(1, L"WordDocument", 2, 2, kFree, 2, static_cast<uint32_t>(kStream));
    entry(2, L"1Table", 2, kFree, kFree, 10, static_cast<uint32_t>(kStream));
    file.insert(file.end(), dir.begin(), dir.end());
    file.insert(file.end(), word.begin(), word.end());
    file.insert(file.end(), table.begin(), table.end());
    return file;
}


}
int wmain() {
    using namespace pulse::preview;
    auto root = std::filesystem::current_path() / L"bench_data/office_native_fix/word_tests";
    std::filesystem::create_directories(root);
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    auto write = [&](const wchar_t* name, const auto& data) {
        auto path = root / name;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        stream.close(); return path.wstring();
    };
    std::wstring payload; uint32_t bytes = 0; bool cut = false;
    std::string rtf = "{\\rtf1 ";
    for (int i = 0; i < 70; ++i) rtf += "paragraph" + std::to_string(i) + "\\par ";
    rtf += "\\page \\u20013?\\u25991?\\par ";
    for (int i = 0; i < 12; ++i) rtf += "\\intbl row" + std::to_string(i) + "\\cell B\\cell\\row ";
    rtf += "\\pard tail}";
    check(MakeLegacyWordDocument(write(L"disguised.doc", rtf), payload, bytes, cut) && !cut &&
          payload.find(L"paragraph69") != payload.npos && payload.find(L"row11") != payload.npos &&
          payload.find(L"\u4e2d\u6587") != payload.npos && payload.find(L"M\trtf\t") != payload.npos,
          "disguised RTF: 70 paragraphs, 12 rows, Unicode, later pages");
    check(!MakeLegacyWordDocument(write(L"broken.rtf", std::string("{\\rtf1 broken")), payload, bytes, cut), "broken RTF fails");
    check(!MakeLegacyWordDocument(write(L"fake.doc", std::string("not a Word file")), payload, bytes, cut), "unknown content fails");
    check(!MakeLegacyWordDocument(write(L"encrypted.doc", BuildWord97Doc(true)), payload, bytes, cut), "encrypted DOC fails");
    check(MakeLegacyWordDocument(write(L"native.wps", BuildWord97Doc(false)), payload, bytes, cut) && !cut && payload.find(L"After") != payload.npos, "binary DOC in WPS extension renders");
    auto many = BuildWord97Doc(false);
    auto put32 = [&](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) many[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    std::wstring body;
    for (int i = 0; i < 70; ++i) body += L"x\r";
    body += L"FINAL\r";
    constexpr size_t word_at = 1536, table_at = word_at + 4096;
    put32(word_at + 76, static_cast<uint32_t>(body.size()));
    put32(table_at + 137, static_cast<uint32_t>(body.size()));
    put32(word_at + 158 + 8 * 13, 0); // plain paragraphs: no PAPX table properties
    put32(word_at + 158 + 8 * 12, 0);
    for (size_t i = 0; i < body.size(); ++i) {
        many[word_at + 0x600 + 2 * i] = static_cast<unsigned char>(body[i]);
        many[word_at + 0x600 + 2 * i + 1] = static_cast<unsigned char>(body[i] >> 8);
    }
    check(MakeLegacyWordDocument(write(L"many.doc", many), payload, bytes, cut) && !cut &&
          payload.find(L"FINAL") != payload.npos, "binary DOC reads beyond 40 paragraphs");
    auto broken = BuildWord97Doc(false); broken.resize(1800);
    check(!MakeLegacyWordDocument(write(L"broken.doc", broken), payload, bytes, cut), "damaged DOC fails");
    rtf = "{\\rtf1 " + std::string(3u << 20, 'x') + "}";
    check(MakeLegacyWordDocument(write(L"limited.rtf", rtf), payload, bytes, cut) && cut, "payload cap reports partial");
    return failures ? 1 : 0;
}
