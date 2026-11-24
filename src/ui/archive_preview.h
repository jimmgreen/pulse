// archive_preview.h — archive contents view shared by the details pane and
// Quick Look: summary, file-type mix and an expandable folder tree.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <string>
#include <memory>
#include <vector>

#include "FluentTokens.h"

namespace pulse::ui {

class Compositor;

class ArchivePreview {
    friend struct PreviewBufferLifetimeTest;
public:
    // Parses the Pulse.Preview.exe archive payload (ipc::PreviewContentKind::
    // Archive). Re-parsing only happens when the payload or file changes, so
    // expansion and scroll survive repaints. Returns false for a payload that
    // is not an archive listing.
    bool SetPayload(const std::shared_ptr<const std::wstring>& payload, const std::wstring& file_name, uint64_t file_size);
    bool SetPayload(const std::wstring& payload, const std::wstring& file_name,
                    uint64_t file_size);
    void Clear();
    bool HasData() const noexcept { return parsed_; }

    // large = Quick Look (summary column + full columns); otherwise the
    // compact details-pane layout. Coordinates are device pixels.
    void Draw(ID2D1DeviceContext* dc, Compositor* compositor, const D2D1_RECT_F& rect,
              const Theme& theme, float scale, bool large);

    // Input, all in the same pixel space as Draw. Each returns true when the
    // view changed and needs a repaint.
    bool Scroll(float wheel_steps);
    bool ScrollPixels(float dy);
    bool OverScrollbar(float x, float y) const noexcept;
    bool ScrollbarMouseDown(float x, float y);
    bool ScrollbarMouseMove(float x, float y);
    void EndScrollbarDrag() noexcept { scrollbar_drag_ = false; }
    bool DraggingScrollbar() const noexcept { return scrollbar_drag_; }
    bool Click(float x, float y);
    // Select the hit row without expanding it; blank space clears selection.
    bool SelectAt(float x, float y);
    bool SelectedIsDirectory() const;
    bool Hover(float x, float y);
    bool Leave();
    bool Key(UINT vk);
    bool SetFilter(const std::wstring& query);
    bool Contains(float x, float y) const noexcept;
    bool HasSelection() const noexcept { return selected_ >= 0; }
    std::wstring StateNote() const;
    // Footer facts: "ZIP archive", "7 files", "unpacked 29.1 KB", size;
    // folders: files, folders, total.
    std::vector<std::wstring> StatusParts() const;
    // Keyboard hint for the footer's right side.
    std::wstring Hint() const;
    uint32_t FilterHits() const noexcept { return filter_hits_; }
    // Quick Look folder contents (payload format DIR, folder_listing.h).
    bool IsFolderListing() const noexcept { return parsed_ && folder_; }
    // Selected row as a path relative to the listed folder; empty if none.
    std::wstring SelectedPath() const;

    // Thumbnail view: one folder's items as tiles, entered with a click and
    // left with the back button or Backspace, instead of the tree. The
    // list/thumbnail toggle sits in the header; the last choice holds for the
    // next archive this process previews.
    bool GridMode() const noexcept { return grid_; }
    bool SetGridMode(bool on);
    // Folder the thumbnail view shows, relative ('\'); empty at the top.
    std::wstring GridFolderPath() const;
    // Draws the picture of a file tile (`path` relative to the archive or
    // listed folder, '\' separators) into `rect`. False (no picture yet, not
    // a picture) keeps the type tile.
    using TileArt = std::function<bool(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect,
                                       const std::wstring& path, uint64_t size)>;
    void SetTileArt(TileArt art) { tile_art_ = std::move(art); }

private:
    struct Node {
        std::wstring name;
        std::wstring chip;      // upper-case extension, up to 4 chars
        std::wstring date;      // "YYYY-MM-DD HH:MM" or empty
        uint64_t size = 0;
        uint64_t packed = 0;
        uint32_t rgb = 0x64748B;
        uint32_t files = 0;     // folders: files inside, recursively
        int parent = -1;
        int depth = 0;
        std::vector<int> kids;
        bool dir = false;
        bool encrypted = false;
        bool open = false;
        bool match = true;      // filter: itself or a descendant matches
        bool self_match = false;
    };
    struct Family { uint32_t rgb; uint64_t bytes; int label; };

    friend struct QuickPreviewAuditTest;
    struct ScrollbarGeometry {
        D2D1_RECT_F hit{}, thumb{};
        float travel = 0.0f, maximum = 0.0f;
    };
    ScrollbarGeometry Scrollbar() const noexcept;
    void DrawScrollbar(ID2D1DeviceContext* dc, const Theme& theme);
    void RebuildVisible();
    void AppendVisible(int index);
    void EnsureResources(ID2D1DeviceContext* dc, Compositor* compositor, float scale);
    void DrawText(ID2D1DeviceContext* dc, IDWriteTextFormat* format, const std::wstring& text,
                  const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void Fill(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius,
              const D2D1_COLOR_F& color);
    float Measure(IDWriteTextFormat* format, const std::wstring& text) const;
    float StateNoteHeight(float width) const;
    void DrawStateNote(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme);
    float DrawHeader(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme,
                     bool large);
    float DrawMix(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme,
                  size_t legend_items);
    void DrawBiggest(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawTree(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme,
                  bool large);
    void DrawChip(ID2D1DeviceContext* dc, const Node& node, float x, float cy);
    int RowAt(float x, float y) const;
    // Thumbnail view.
    std::wstring NodePath(int index) const;
    float DrawViewToggle(ID2D1DeviceContext* dc, float left, float cy, const Theme& theme);
    void DrawCrumb(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawGrid(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const Theme& theme);
    void DrawFolderArt(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect);
    int TileAt(float x, float y) const;
    float TileHeight() const noexcept;
    float ContentHeight() const noexcept;
    bool CanGoUp() const noexcept;
    bool GridUp();
    bool EnterGridFolder(int index);
    bool GridKey(UINT vk);
    void ClampScroll();
    void RevealSelection();
    float RowHeight() const noexcept;

    // Model
    bool parsed_ = false;
    bool SetPayloadImpl(const std::wstring& payload, const std::wstring& file_name, uint64_t file_size,
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
    std::wstring payload_;
    std::wstring file_name_;
    uint64_t file_size_ = 0;
    std::wstring format_;
    bool has_packed_ = false;
    uint64_t packed_total_ = 0;
    bool incomplete_ = false;
    bool folder_ = false;    // folder listing rather than an archive
    bool counting_ = false;  // folder: quick first pass, full count pending
    std::vector<Node> nodes_;
    std::vector<int> roots_;
    std::vector<int> visible_;
    std::vector<Family> mix_;
    std::vector<int> biggest_;
    uint64_t unpacked_ = 0;
    uint32_t file_count_ = 0;
    uint32_t dir_count_ = 0;
    bool any_encrypted_ = false;
    std::wstring filter_;
    uint32_t filter_hits_ = 0;

    // View state
    float scroll_ = 0.0f;
    bool scrollbar_drag_ = false;
    bool scrollbar_hover_ = false;
    float scrollbar_press_ = 0.0f;
    float scrollbar_start_ = 0.0f;
    int hover_ = -1;       // index into visible_
    int selected_ = -1;    // index into visible_
    D2D1_RECT_F tree_rect_{};
    D2D1_RECT_F view_rect_{};
    bool large_ = false;
    float scale_ = 1.0f;
    bool grid_ = false;
    int grid_dir_ = -1;        // node shown by the thumbnail view, -1 = top
    int grid_cols_ = 1;
    float tile_w_ = 100.0f;
    D2D1_RECT_F toggle_list_{}, toggle_grid_{}, crumb_up_{};
    TileArt tile_art_;

    // Device resources
    ID2D1DeviceContext* brush_owner_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    IDWriteFactory2* factory_ = nullptr;
    Compositor* compositor_ = nullptr;
    float format_scale_ = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_, small_, chip_, value_, title_, icon_, tile_chip_;
};

} // namespace pulse::ui
