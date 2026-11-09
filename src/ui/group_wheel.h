// group_wheel.h — "Group by" picker drawn as a 3D drum (toolbar Group button).
//
// Left: the options on a vertical cylinder (24° per row, spring-driven roll,
// centre row under a highlighted card). Right: a live preview of the current
// folder under the centred option (colour wash, group headers, skeleton rows).
// Drawn in the main window over everything else; the app forwards input while
// it is open. Honours the Windows animation switch (static list when off).
#pragma once
#include <d2d1_1.h>
#include <dwrite.h>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::ui {

struct Theme;

// One line of the sort preview: a real entry with the sort column's value.
struct GroupWheelFileRow {
    std::wstring name, value;
    uint32_t rgb = 0x94A3B8;    // file-type swatch
};

struct GroupWheelOption {
    int value = 0;              // app::GroupBy
    int icon = 0;               // command_icons::Icon
    std::wstring name;
    std::wstring meta;          // second line (group names / item count)
    std::wstring pill;          // "4 组" / "平铺"
    uint32_t hue1 = 0x94A3B8, hue2 = 0x64748B;   // preview colour wash
    std::vector<std::pair<std::wstring, int>> groups;  // preview; empty = flat
    bool apply_all = true;      // group picker: the footer button shows while it is centred
    // Sort picker only: direction labels (asc, desc) and the preview for
    // each direction x folder mode (index desc * 3 + folder).
    std::array<std::wstring, 2> directions;
    std::array<std::vector<GroupWheelFileRow>, 6> files;
};

struct GroupWheelData {
    std::vector<GroupWheelOption> options;
    int applied = 0;            // index into options
    std::wstring current_text, apply_text, cancel_text;
    std::wstring apply_all_text;   // footer button; empty = hidden
    std::wstring click_apply_text; // centre row pill while the pointer is on it
    // Sort picker: direction + folder segments under the preview, the preview
    // shows `files` instead of group headers, and Left/Right set the direction.
    bool sort_controls = false;
    bool desc = false;
    int folder = 0;             // app::FolderSortMode
    std::wstring direction_text, folder_text;
    std::array<std::wstring, 3> folder_names;
};

namespace fluent { class Painter; }

class GroupWheel {
public:
    // anchor = Group button (client px); client = window client rect (px).
    void Open(GroupWheelData data, const D2D1_RECT_F& anchor, const D2D1_RECT_F& client,
              float scale, bool animate);
    void Close();                          // fades out (instant without animation)
    bool IsOpen() const noexcept { return open_; }
    bool Visible() const noexcept { return open_ || closing_; }
    // Parent-client pixels, including the 6px entrance/exit translation and
    // three-sigma shadow (9px deviation, 6px drop). Stable through the fade.
    D2D1_RECT_F OcclusionBounds() const noexcept {
        if (!Visible()) return {};
        return {panel_.left - 27.0f * scale_, panel_.top - 27.0f * scale_,
                panel_.right + 27.0f * scale_, panel_.bottom + 33.0f * scale_};
    }
    // 0..1 open amount, frame-stable (follows the fade; 0/1 without animation).
    float OpenAmount() const noexcept { return Visible() ? fade_value_ : 0.0f; }
    bool IsSortPicker() const noexcept { return data_.sort_controls; }

    bool Contains(float x, float y) const noexcept;
    // Footer "apply to all folders" button (laid out while drawing).
    bool HitApplyAll(float x, float y) const noexcept;
    // Footer Cancel / Apply buttons (laid out while drawing): 1 cancel, 2 apply, 0 none.
    static constexpr int kFooterCancel = 1, kFooterApply = 2;
    int HitFooter(float x, float y) const noexcept;
    void PressFooter(int button) noexcept { footer_pressed_ = button; }
    int FooterPressed() const noexcept { return footer_pressed_; }
    bool InDrum(float x, float y) const noexcept;
    // Pointer highlight: drum rows, the centre card, footer buttons. True when it changed.
    bool Hover(float x, float y) noexcept;
    void Step(int delta);                  // roll by whole rows (keys, wheel)
    void StepTo(int index);
    bool PointerDown(float x, float y);    // true when a drag started in the drum
    void PointerMove(float y);
    // After a click on the centre row: that option's value. -1 otherwise.
    int PointerUp(float y);
    bool Dragging() const noexcept { return dragging_; }
    int SelectedValue() const;
    // Sort picker segments (no-ops for the group picker).
    bool Desc() const noexcept { return data_.desc; }
    int Folder() const noexcept { return data_.folder; }
    void SetDesc(bool desc) noexcept { data_.desc = desc; }

    // Advances the spring and fades; true while another frame is needed.
    bool Tick(uint64_t now_ms);
    // `painter` (the renderer's, begun for this frame) draws the footer buttons.
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format,
              const Theme& theme, fluent::Painter* painter);

private:
    int Count() const noexcept { return static_cast<int>(data_.options.size()); }
    float Clamp(float v) const noexcept;
    int CentreIndex() const noexcept;
    D2D1_RECT_F DrumRect() const noexcept;
    void EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base_format);
    void DrawRow(ID2D1DeviceContext* dc, const Theme& theme, int index, float alpha);
    void DrawPreview(ID2D1DeviceContext* dc, const Theme& theme, const D2D1_RECT_F& pv,
                     int index, float alpha);
    void DrawFooter(ID2D1DeviceContext* dc, fluent::Painter* painter);
    // Drum row offset under the pointer (0 = centre card); kNoHover when none.
    static constexpr int kNoHover = 1000;
    int RowStepAt(float x, float y) const noexcept;
    // Sort segments: k = 0 direction (2 buttons), 1 folder (3 buttons); i < 0 = whole bar.
    D2D1_RECT_F SegmentRect(int k, int i) const noexcept;
    float ControlsHeight() const noexcept;
    void DrawControls(const Theme& theme, float alpha);
    void ReleaseFormats();

    GroupWheelData data_;
    D2D1_RECT_F panel_{};
    D2D1_RECT_F apply_all_rect_{};
    bool apply_all_hot_ = false;
    D2D1_RECT_F cancel_rect_{}, ok_rect_{};
    int footer_hot_ = 0, footer_pressed_ = 0;
    int hover_step_ = kNoHover;
    int seg_hot_ = -1;  // sort controls segment under the pointer: k * 4 + i
    float scale_ = 1.0f;
    bool animate_ = true;
    bool open_ = false, closing_ = false;
    uint64_t fade_start_ = 0, last_tick_ = 0;
    float p_ = 0.0f, t_ = 0.0f, v_ = 0.0f;  // position, target, velocity (rows)
    bool dragging_ = false, moved_ = false;
    float drag_y0_ = 0.0f, drag_p0_ = 0.0f;
    int shown_ = -1, shown_prev_ = -1;      // preview content (cross-fade)
    uint64_t shown_at_ = 0;
    float fade_value_ = 0.0f;               // 0..1 open amount (frame-stable)

    float format_scale_ = 0.0f;
    IDWriteTextFormat* f_name_ = nullptr;
    IDWriteTextFormat* f_small_ = nullptr;
    IDWriteTextFormat* f_head_ = nullptr;
    IDWriteTextFormat* f_kbd_ = nullptr;
    IDWriteTextFormat* f_seg_ = nullptr;
    ID2D1StrokeStyle* stroke_ = nullptr;
public:
    GroupWheel() = default;
    GroupWheel(const GroupWheel&) = delete;
    GroupWheel& operator=(const GroupWheel&) = delete;
    ~GroupWheel() { ReleaseFormats(); }
};

} // namespace pulse::ui
