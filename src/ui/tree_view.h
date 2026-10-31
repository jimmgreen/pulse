// tree_view.h — Quick Look tree for JSON and XML: the preview host's tree
// payload (preview_host/tree_document.h) drawn as a virtualized, foldable
// outline with indent guides, type colours, a current row, find highlights
// and value / path copying.
#pragma once
#include "search_highlights.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <utility>

#include "FluentTokens.h"

namespace pulse::ui {

class TreeView {
    friend struct PreviewBufferLifetimeTest;
public:
    // Parses the payload; the same payload again keeps folding, scroll and
    // the current row. False for anything that is not a tree payload.
    bool SetPayload(const std::shared_ptr<const std::wstring>& payload);
    bool SetPayload(const std::wstring& payload);
    void Clear();
    bool HasData() const noexcept { return !nodes_.empty(); }
    bool IsXml() const noexcept { return xml_; }
    bool Truncated() const noexcept { return truncated_; }
    // Parse failure: the window shows the source with this in the pill.
    bool HasError() const noexcept { return !error_.empty(); }
    std::wstring ErrorText() const;  // "\u7B2C 3 \u884C\uFF0C\u7B2C 7 \u5217\uFF1A\u7F3A\u5C11\u9017\u53F7"
    const std::wstring& Source() const noexcept { return source_; }
    // Every node's searchable text, one line per node in document order.
    uint64_t TextRevision() const noexcept { return text_revision_; }
    const std::wstring& PlainText() const noexcept { return plain_; }
    // "JSON" | current path (or node count) — the window adds the size.
    std::vector<std::wstring> StatusParts() const;

    using Highlight = SearchHighlight;
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
              const Theme& theme, bool dark, float scale, SearchHighlights matches);

    bool Scroll(float wheel_steps);
    // Up/Down move the current row, Left/Right fold (or go to the parent /
    // first child), Enter toggles, paging keys scroll. False for keys the
    // window should handle (arrows with no current row step through files).
    bool Key(UINT vk);
    void Reveal(uint32_t offset);  // expands ancestors, makes it current

    bool MouseDown(float x, float y);  // true when handled (repaint)
    bool MouseMove(float x, float y);  // scrollbar drag; true to repaint
    void MouseUp() noexcept { dragging_ = false; }
    bool Dragging() const noexcept { return dragging_; }
    bool Hover(float x, float y);      // true to repaint
    bool Leave();

    bool HasCurrent() const noexcept { return current_ >= 0; }
    bool ClearCurrent();
    std::wstring CurrentValue() const;  // Ctrl+C: scalar text or the subtree
    std::wstring CurrentPath() const;   // Ctrl+Shift+C: $.a[3] / /root/item[2]
    std::wstring CurrentType() const;   // object / array / string / number / boolean / null
    // Footer chips: unfold every container, or fold all but the top level.
    bool AnyCollapsed() const noexcept;
    void ExpandAll(bool expand);

private:
    struct Node {
        uint32_t depth = 0;
        wchar_t type = L's';
        std::wstring key, value, text;
        std::vector<std::pair<std::wstring, std::wstring>> attributes;
        bool attributes_known = false;
        uint32_t children = 0;  // as in the document (may exceed what was sent)
        int parent = -1;
        uint32_t end = 0;       // one past the last node of the subtree
        uint32_t offset = 0;    // in plain_
        uint32_t index = 0;     // position among the parent's children
    };
    struct Piece { uint32_t start, length; int color; };

    bool IsContainer(size_t i) const noexcept;
    bool Expandable(size_t i) const noexcept { return nodes_[i].end > i + 1; }
    std::wstring RowText(size_t i, std::vector<Piece>* pieces) const;
    std::wstring Suffix(size_t i, std::vector<Piece>* pieces, size_t base) const;
    void BuildRows();
    void SetExpanded(size_t i, bool expanded);
    int RowOf(size_t node) const;
    void EnsureVisible(int row);
    void Clamp();
    float RowHeight() const noexcept { return 22.0f * scale_; }
    float ContentHeight() const noexcept;
    bool EnsureFormats(IDWriteFactory2* factory, float scale);
    std::wstring PathOf(size_t i) const;
    void AppendJson(size_t i, int indent, std::wstring& out) const;
    void AppendXml(size_t i, int indent, std::wstring& out, bool subtree_root = false) const;

    uint64_t text_revision_ = 0;
    bool SetPayloadImpl(const std::wstring& payload,
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
    std::wstring payload_, source_, plain_, error_;
    uint32_t error_line_ = 0, error_column_ = 0;
    bool xml_ = false, truncated_ = false;
    std::vector<Node> nodes_;
    std::vector<uint8_t> expanded_;
    std::vector<uint32_t> rows_;  // visible node indices
    int current_ = -1;            // node index
    int hover_row_ = -1;
    float sy_ = 0.0f;
    bool dragging_ = false;
    float drag_origin_ = 0.0f, drag_scroll_ = 0.0f;
    D2D1_RECT_F thumb_{};

    D2D1_RECT_F view_{};
    float scale_ = 1.0f;
    IDWriteFactory2* factory_ = nullptr;
    float format_scale_ = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> mono_;
    ID2D1DeviceContext* brush_owner_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
};

}  // namespace pulse::ui
