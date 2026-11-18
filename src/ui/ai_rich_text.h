// ai_rich_text.h — Pulse 助手's answers as rich text: Markdown (md4c through
// preview_host/markdown_document, the parser Quick Look uses) turned into
// leaf blocks with inline runs, local paths found in the text, and the text a
// selection copies (plain or Markdown). No drawing here: ai_panel_rich.cpp
// lays the blocks out and src/bench/ai_rich_text_test.cpp checks this part.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui::airich {

// Inline run flags (markdown_document.h's, plus kPath found here).
enum RunFlag : uint32_t {
    kBold = 1,
    kItalic = 2,
    kCode = 4,
    kStrike = 8,
    kLink = 16,
    kImage = 32,
    kUnderline = 64,
    kMath = 128,
    kDisplayMath = 256,
    kPath = 1u << 16,   // a local path (C:\…, \\server\share\…): a chip that opens it
};

struct Run {
    uint32_t start = 0, length = 0;
    uint32_t flags = 0;
    std::wstring target;   // link URL, or the path of a kPath run
};

enum class Kind : uint8_t { Para, Heading, Code, Rule, Cell, Html };

struct Block {
    Kind kind = Kind::Para;
    int level = 0;              // Heading 1..6
    std::wstring lang;          // Code: the fence's language as written
    int quote = 0;              // enclosing block quotes
    int indent = 0;             // enclosing lists
    // First block of a list item: u bullet, o<N> ordered, t0 / t1 task.
    std::wstring marker;
    std::wstring text;
    std::vector<Run> runs;      // ascending, non-overlapping
    // Cell: its table (index into Doc::tables), row, column, alignment
    // (l c r, or - when the source gave none) and whether it is a header.
    int table = -1, row = -1, col = -1;
    wchar_t align = L'-';
    bool header = false;
};

struct Table {
    int columns = 0, rows = 0;
    int first = 0, count = 0;   // its cells in Doc::blocks, row-major
};

struct Doc {
    std::vector<Block> blocks;
    std::vector<Table> tables;
};

// Parses an answer (it may be cut mid-way while it streams: an open fence is a
// code block to the end). Math spans stay literal text.
Doc Parse(const std::wstring& markdown);

// Finds local paths in `text` outside code and links (kPath runs are merged
// into `runs`); an inline code span that is a whole path becomes a path run.
void FindPaths(const std::wstring& text, std::vector<Run>& runs);
// True for C:\… or \\server\share… (the text a path run may hold).
bool LooksLikePath(std::wstring_view text);

// A point in a document: block, UTF-16 offset in its text.
struct Pos {
    int block = 0;
    uint32_t offset = 0;
};

// The list marker of block `b` as copied: "- ", "1. ", "- [ ] ", "- [x] ", or
// empty; nested lists are indented two spaces per level.
std::wstring MarkerText(const Block& b);

// Text between `from` and `to` (from <= to). Plain: no Markdown, list
// markers kept when a selection spans blocks, table cells by tabs and rows by
// new lines, blank lines between paragraphs. Markdown: the same range written
// back as Markdown (headings, emphasis, code, links, fences, tables).
std::wstring CopyRange(const Doc& doc, Pos from, Pos to, bool markdown);
// The whole document.
std::wstring CopyAll(const Doc& doc, bool markdown);
// A table as tab separated values (Excel pastes it as cells).
std::wstring TableTsv(const Doc& doc, int table);

// An answer as plain text for 复制回答 (the whole answer, no Markdown).
std::wstring ToPlain(const std::wstring& markdown);

// The syntax extension for a fence language ("powershell" -> ".ps1"); empty
// when unknown.
std::wstring ExtensionForLanguage(std::wstring_view lang);

} // namespace pulse::ui::airich
