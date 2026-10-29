// ai_panel_rich.h — the Pulse 助手 panel's rich text (ai_panel_rich.cpp):
// an answer's Markdown or a message laid out once per text and width, and
// the selection over it. Internal to src/ui.
#pragma once

#include "ai_panel_view.h"
#include "ai_rich_text.h"
#include "syntax_highlight.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::ui {

// One selectable run of text: a paragraph, heading, list item, a code block's
// code, a table cell, or a message.
struct AiRichPiece {
    int md = -1;                          // its block in AiRichDoc::doc (-1: a message)
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    std::wstring text;                    // as laid out; U+FFFC holds a chip's padding (never copied)
    std::vector<uint32_t> to_doc;         // layout offset -> offset in the block's text (size + 1)
    float x = 0, y = 0, w = 0, h = 0;     // px from the rich block's left and top
    float line_h = 0;
    int box = -1;                         // the code / table box it scrolls in
    uint8_t ink = 0;                      // 0 text, 1 text2 (quotes, table headers)
    uint64_t key = 0;                     // what it was laid out from (reuse while streaming)
};

enum class AiRichDecoKind : uint8_t { Dot, Ring, Square, Number, Task, TaskDone, QuoteBar, Rule, CodeSpan, Chip, LinkLine };

struct AiRichDeco {
    AiRichDecoKind kind = AiRichDecoKind::Dot;
    D2D1_RECT_F r{};                      // px, block-relative
    int piece = -1;                       // inline ones move with their piece's box
    int target = -1;                      // Chip / LinkLine: its target (hover)
    std::wstring text;                    // Number: "3."
    bool file = false;                    // Chip: a file rather than a folder
};

struct AiRichBox {
    bool table = false;
    D2D1_RECT_F r{};                      // the bordered box, block-relative
    float view_left = 0, view_right = 0;  // the scrolling viewport, block-relative x
    float body_top = 0, body_bottom = 0;  // its y extent, block-relative
    float content_w = 0;                  // what scrolls inside the viewport
    int md = -1;                          // Code: its doc block; Table: the table
    int lines = 0;                        // Code: lines; Table: rows below the header
    int columns = 0;
    std::wstring lang;                    // Code: the header's language
    std::vector<float> col_x, col_w;      // Table, from the viewport's left
    std::vector<float> row_y;             // Table: row tops, block-relative (rows + 1)
    bool header_row = false;
    int target = -1;                      // its copy button (AiRichDoc::targets)
};

struct AiRichTarget {
    AiPanelPart part = AiPanelPart::None;
    std::wstring text;                    // URL, path, code or TSV
    int box = -1;                         // scrolls with this box
    std::vector<D2D1_RECT_F> rects;       // block-relative, before box scrolling
};

struct AiRichDoc {
    uint64_t sig = 0;
    uint64_t env = 0;                     // width, scale, fonts, brushes: what reuse needs to match
    bool used = false;
    bool user = false;
    airich::Doc doc;
    std::vector<AiRichPiece> pieces;
    std::vector<AiRichDeco> decos;
    std::vector<AiRichBox> boxes;
    std::vector<AiRichTarget> targets;
    float text_w = 0;                     // a message: its widest line
    float height = 0;
    float margin_top = 0, margin_bottom = 0;
};

// A piece as drawn this frame (screen px), for hit tests and the selection.
struct AiRichSpot {
    uint32_t id = 0;
    int block = 0;
    int piece = 0;
    float ox = 0, oy = 0;                 // the layout's origin
    D2D1_RECT_F rect{};                   // the layout's box (clipped to its viewport)
};

struct AiPanelView::RichCache {
    std::unordered_map<uint32_t, AiRichDoc> docs;
    std::vector<uint32_t> ids;            // every block's id by index, last frame
    std::unordered_map<uint32_t, int> ordinal;   // id -> index
    std::vector<AiBlockKind> kinds;       // every block's kind by index
    std::vector<AiRichSpot> spots;
    std::unordered_map<uint64_t, float> box_scroll;   // (id << 16 | box) -> px
    // Drawing effects the layouts carry (links, code); recolored each frame.
    ID2D1DeviceContext* dc = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> link, text2;
    std::array<Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>, static_cast<size_t>(SyntaxToken::Count)> syntax;
    uint32_t brush_gen = 1;
    // Selection (piece offsets are layout offsets).
    using Pos = AiRichSelPos;
    Pos anchor, focus;
    bool active = false;
    bool dragging = false;
    int unit = 1;                         // 1 characters, 2 words, 3 paragraphs
    Pos unit_from, unit_to;               // the word or paragraph the press chose
    bool bar = false;                     // the mini toolbar
    D2D1_RECT_F bar_rect{}, bar_copy{}, bar_quote{}, bar_search{};
    bool bar_search_on = true;
    // 已复制 on a copy button for 2 s.
    uint32_t copied_id = 0;
    int copied_k = -1;
    uint64_t copied_at = 0;
    // The composer's quote chip.
    std::wstring quote_text;
    float quote_w = 0, quote_h = 0;
};

} // namespace pulse::ui
