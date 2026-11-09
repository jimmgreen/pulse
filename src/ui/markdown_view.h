// markdown_view.h — Quick Look rendered Markdown: one DirectWrite layout per
// block (headings, paragraphs, lists, quotes, code, tables, rules, images)
// from the preview host's block payload (preview_host/markdown_document.h).
#pragma once
#include "search_highlights.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <memory>
#include <vector>

#include "FluentTokens.h"
#include "markdown_image_metadata.h"

namespace pulse::ui {

class ThumbnailCache;
struct DocumentImageSession;

class MarkdownView {
    friend struct PreviewBufferLifetimeTest;
    friend struct DetailsPreviewProbe;
    friend struct MarkdownScrollbarTest;
    friend struct ThumbnailCacheTestAccess;
public:
    ~MarkdownView();
    // Parses the payload; a repeat of the same payload keeps scroll and layout.
    // Returns false for anything that is not a Markdown payload.
    bool SetPayload(const std::shared_ptr<const std::wstring>& payload, const std::wstring& file_path);
    bool SetPayload(const std::wstring& payload, const std::wstring& file_path);
    void Clear();
    void SetNotifyWindow(HWND hwnd) noexcept { notify_ = hwnd; }
    bool HasData() const noexcept { return parsed_; }
    const std::wstring& Source() const noexcept { return source_; }
    // Block texts joined by newlines (table cells by tabs): what find, select
    // and copy work on. Offsets below index this text.
    uint64_t TextRevision() const noexcept { return text_revision_; }
    const std::wstring& PlainText() const noexcept { return plain_; }
    // Jupyter notebook converted by the host (I record): kernel and cells.
    bool IsNotebook() const noexcept { return notebook_; }
    bool HasToc() const noexcept { return !toc_.empty(); }
    void SetTocHidden(bool hidden) noexcept { toc_hidden_ = hidden; }
    const std::wstring& NotebookKernel() const noexcept { return kernel_; }
    uint32_t NotebookCells() const noexcept { return cells_; }
    // Document facts from the M record (DOCX, EPUB): format is "docx", "epub"
    // or empty for Markdown and notebooks.
    const std::wstring& DocFormat() const noexcept { return format_; }
    const std::wstring& DocTitle() const noexcept { return doc_title_; }
    const std::wstring& DocAuthor() const noexcept { return doc_author_; }
    uint32_t WordCount() const noexcept { return words_; }
    uint32_t HeadingCount() const noexcept { return headings_; }
    // Chaptered payloads (P records, EPUB): one section is laid out at a time
    // while PlainText, find, selection and copy still span the whole book.
    int SectionCount() const noexcept { return static_cast<int>(sections_.size()); }
    int SectionIndex() const noexcept { return section_; }
    // Shows a section scrolled to one of its blocks, or to its end.
    bool ShowSection(int index, int block = 0, bool at_end = false);
    // The contents sidebar and the next-chapter link. Click returns true when
    // the point belonged to either (the click is spent; repaint).
    bool Click(float x, float y);
    bool IsClickable(float x, float y) const;
    // Wheel at a point: the sidebar scrolls itself, and a few notches past the
    // end of a chapter turn to the next one.
    bool ScrollAt(float x, float y, float wheel_steps);

    using Highlight = SearchHighlight;
    // images: local pictures are drawn through Quick Look's thumbnail cache.
    // Returns true when an image's size became known and the view should be
    // painted again with the new layout.
    bool Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
              const Theme& theme, bool dark, float scale, ThumbnailCache* images,
              uint64_t generation, uint32_t sel_start, uint32_t sel_end,
              SearchHighlights matches);

    bool Scroll(float wheel_steps);
    bool ScrollHorizontal(float wheel_steps);
    bool OverScrollbar(float x, float y) const;
    bool ScrollbarMouseDown(float x, float y);
    bool ScrollbarMouseMove(float x, float y);
    void EndScrollbarDrag() noexcept { scrollbar_drag_ = 0; }
    bool DraggingScrollbar() const noexcept { return scrollbar_drag_ != 0; }
    bool Key(UINT vk);  // PgUp / PgDn / Home / End / Up / Down
    void Reveal(uint32_t offset);
    // Plain-text offset under a point (clamped into the nearest block).
    bool HitTest(float x, float y, uint32_t& offset) const;
    // Link target under a point, empty if none.
    std::wstring LinkAt(float x, float y) const;

private:
    struct Run { uint32_t start = 0, length = 0, flags = 0; std::wstring target; };
    struct Block {
        wchar_t kind = L'p';
        std::wstring arg, marker, text;
        std::vector<Run> runs;
        int quote = 0, indent = 0;
        int table = -1, row = -1, col = -1;
        bool header = false;
        uint32_t plain_start = 0;
        // Image paragraph: resolved local path, or empty for a placeholder.
        bool image = false;
        bool image_metadata_ready = false;
        std::wstring image_path;
        DWORD image_attrs = 0;
        uint64_t image_size = 0, image_modified = 0;
        float image_aspect = 0.0f;  // height / width once known
        // Layout (device pixels, relative to the document top).
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float x = 0, y = 0, w = 0, h = 0;      // text box
        float box_top = 0, box_bottom = 0;     // decoration extent (code, quote)
    };
    struct Table {
        int columns = 0, quote = 0, indent = 0;
        std::vector<int> cells;  // block indices, row-major
        int rows = 0;
        std::vector<float> col_x, col_w, row_y, row_h;
        float x = 0, y = 0, w = 0, h = 0;
    };
    struct Section { std::wstring name; size_t begin = 0, end = 0; };  // all_blocks_ range
    struct TocEntry { int level = 0, section = 0, block = 0; std::wstring title; };

    bool DrawDocument(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                      const Theme& theme, bool dark, float scale, ThumbnailCache* images,
                      uint64_t generation, uint32_t sel_start, uint32_t sel_end,
                      SearchHighlights matches);
    void DrawToc(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme, bool dark);
    bool SidebarVisible(float width) const noexcept;
    bool toc_hidden_ = false;  // the window's session-wide contents toggle
    int ActiveTocEntry() const;
    int TocRowAt(float x, float y) const;
    float BlockTop(int index) const;
    void ClampTocScroll();

    void Layout(IDWriteFactory2* factory, float width, float scale);
    float ContentWidth(float view_width) const noexcept;
    bool EnsureFormats(IDWriteFactory2* factory, float scale);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> MakeLayout(IDWriteFactory2* factory, const Block& block,
                                                         float width, bool wrap = true);
    void EnsureBrushes(ID2D1DeviceContext* dc, bool dark);
    std::shared_ptr<DocumentImageSession> image_session_;
    void DrawRanges(ID2D1DeviceContext* dc, const Block& block, float ox, float oy,
                    uint32_t start, uint32_t end, const D2D1_COLOR_F& color);
    int BlockAt(float x, float doc_y) const;
    void ClampScroll();
    struct ScrollbarGeometry {
        D2D1_RECT_F hit{}, thumb{};
        float travel = 0.0f, maximum = 0.0f;
    };
    ScrollbarGeometry Scrollbar(bool horizontal) const;
    int scrollbar_drag_ = 0; // 1 vertical, 2 horizontal
    float scrollbar_press_ = 0.0f, scrollbar_start_ = 0.0f;

    MarkdownImageMetadata image_metadata_;
    HWND notify_ = nullptr;
    bool parsed_ = false;
    uint64_t text_revision_ = 0;
    bool SetPayloadImpl(const std::wstring& payload, const std::wstring& file_path,
                        std::shared_ptr<const std::wstring> owner);
    const std::wstring& Payload() const noexcept { return shared_payload_ ? *shared_payload_ : payload_; }
    void StorePayload(const std::wstring& value, std::shared_ptr<const std::wstring> owner) {
        if (owner) {
            shared_payload_ = std::move(owner);
            std::wstring{}.swap(payload_);
        } else {
            payload_ = value;
            shared_payload_.reset();
        }
    }
    std::shared_ptr<const std::wstring> shared_payload_;
    std::weak_ptr<const std::wstring> payload_identity_;
    std::wstring payload_, path_, base_dir_;
    std::wstring source_, plain_;
    std::vector<Block> blocks_;
    std::vector<Table> tables_;
    int gutter_ = 0;  // G record: indent levels taken by notebook labels
    bool notebook_ = false;
    std::wstring kernel_;
    uint32_t cells_ = 0;
    std::wstring format_, doc_title_, doc_author_;
    uint32_t words_ = 0;     // DOCX: from the host; Markdown: counted here
    uint32_t headings_ = 0;
    // Chapters: every block of the book, and the section now in blocks_.
    std::vector<Block> all_blocks_;
    std::vector<Table> all_tables_;
    std::vector<Section> sections_;
    std::vector<TocEntry> toc_;
    int section_ = 0;
    int pending_block_ = -1;       // applied after the next layout
    bool pending_end_ = false;
    bool pending_reveal_ = false;
    uint32_t pending_offset_ = 0;
    float overscroll_ = 0.0f;
    float footer_y_ = -1.0f;       // next-chapter link, document coordinates
    D2D1_RECT_F footer_rect_{};    // ... and on screen once drawn
    D2D1_RECT_F toc_rect_{};
    float toc_scroll_ = 0.0f;
    bool toc_follow_ = true;       // bring the active entry into view

    // Layout cache
    float layout_width_ = 0.0f, layout_scale_ = 0.0f;
    bool layout_dark_ = false;
    bool relayout_ = true;
    float doc_height_ = 0.0f;
    float scroll_ = 0.0f;
    float scroll_x_ = 0.0f, doc_width_ = 0.0f;
    D2D1_RECT_F view_{};
    float origin_x_ = 0.0f;
    float scale_ = 1.0f;

    IDWriteFactory2* factory_ = nullptr;
    float format_scale_ = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_, code_, marker_, label_, toc_format_, toc_sub_format_;
    std::wstring mono_family_, serif_cjk_, serif_latin_;  // serif: EPUB reading
    ID2D1DeviceContext* brush_owner_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_, link_brush_, dim_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> syntax_brushes_[16];
    bool brushes_dark_ = false;
};

} // namespace pulse::ui
