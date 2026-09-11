#pragma once

#include "ui_compositor.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace pulse::ui::fluent {

// Immediate-mode state. The owner keeps input and animation state; controls only draw it.
struct ControlState {
    bool enabled = true;
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    bool checked = false;
    bool selected = false;
    bool keyboard_focus = false;
    // Negative means snap to checked; otherwise this is the eased 0..1 check transition.
    float check_progress = -1.0f;
};

enum class HorizontalAlignment { Left, Center, Right };
enum class ButtonKind { Standard, Primary, Danger, Transparent, Toggle, TransparentToggle };
enum class TitleBarButtonRole { Custom, Minimize, Maximize, Restore, Close };
enum class BadgeKind { Neutral, Accent, Success, Warning, Danger, Keycap };
enum class SegmentPosition { Single, First, Middle, Last };
enum class SortDirection { None, Ascending, Descending };
enum class InfoBarKind { Informational, Success, Warning, Error };
enum class EasingCurve { Linear, OutQuad, OutCubic, InOutQuad, InOutSine, OutSine };
// Small pane-layout diagrams for the split menu (not Segoe glyphs).
enum class MenuPictogram {
    None = 0,
    LayoutSingle,
    LayoutSideBySide,
    LayoutStacked,
    LayoutThree,
    LayoutFour,
};

inline constexpr wchar_t kOmnibarHintBadge[] = L"\u547D\u4EE4";
inline constexpr wchar_t kOmnibarHintKey[] = L"Ctrl+K";

struct MotionSpec {
    float duration_ms = 0.0f;
    EasingCurve easing = EasingCurve::Linear;
    float delay_ms = 0.0f;
};

// QFluent motion values. The app can map these directly to DirectComposition curves.
namespace motion {
inline constexpr float MenuTravelDip = 8.0f;
inline constexpr MotionSpec SwitchSlide{120.0f, EasingCurve::Linear};
inline constexpr MotionSpec ProgressShortBar{833.0f, EasingCurve::Linear};
inline constexpr MotionSpec ProgressLongBar{1167.0f, EasingCurve::OutQuad, 785.0f};
} // namespace motion

float EvaluateMotion(const MotionSpec& motion, float elapsed_ms) noexcept;

struct ButtonSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view glyph;
    ButtonKind kind = ButtonKind::Standard;
    ControlState state{};
    bool icon_only = false;
    bool drop_down = false;
    bool bordered = true;
    bool skip_glyph = false;
};

struct TextFieldSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view placeholder;
    std::wstring_view leading_glyph;
    std::wstring_view trailing_glyph;
    ControlState state{};
    float trailing_width = 0.0f;
    bool suppress_text = false;
    std::wstring_view trailing_badge;   // right-side hint, e.g. L"> 命令"
    std::wstring_view trailing_keycap;  // right-most keycap, e.g. L"Ctrl+K"
    bool compact_leading_glyph = false;
    // Opaque hosted EDIT/Luma overlay sits on the frame; rest fill must match it.
    bool hosted_edit = false;
};

struct TabSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view glyph;
    ControlState state{};
    bool show_close = false;
};

struct MenuItemSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view shortcut;
    std::wstring_view badge_text;
    std::wstring_view glyph;
    ControlState state{};
    bool has_submenu = false;
    bool separator_after = false;
    bool has_swatch = false;
    bool checked = false;
    bool mixed = false;
    bool radio = false;
    bool radio_group = false;      // reserve a leading selection-dot column
    MenuPictogram pictogram = MenuPictogram::None;
    D2D1_COLOR_F swatch_color{};
    float glyph_scale = 1.0f;
    bool shortcut_inline = false;
    float inline_label_width = 0.0f;
};

struct ScrollbarSpec {
    D2D1_RECT_F viewport{};
    float offset = 0.0f;
    float viewport_extent = 0.0f;
    float content_extent = 0.0f;
    float expand_progress = 0.0f;
    bool enabled = true;
};

D2D1_RECT_F ScrollbarThumbRect(const ScrollbarSpec& spec, float scale,
                              bool high_contrast = false) noexcept;

struct ListRowSpec {
    D2D1_RECT_F bounds{};
    ControlState state{};
    bool cut = false;
    bool drop_target = false;
};

struct SidebarItemSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view detail;
    std::wstring_view glyph;
    std::wstring_view badge_text;
    D2D1_COLOR_F badge_color{};
    bool custom_badge_color = false;
    ControlState state{};
    int badge_count = 0;
    bool show_count = false;
    bool drop_target = false;
    bool tag_dot = false;
    bool status_dot = false;
    bool suppress_text = false;
    bool skip_glyph = false;
    float trailing_reserve = 0.0f;
    D2D1_COLOR_F icon_color{};
    D2D1_COLOR_F tag_color{};
    D2D1_COLOR_F status_color{};
};

struct TrayCardSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view summary;
    std::wstring_view detail;
    int item_count = 0;
    bool move_intent = false;
    ControlState state{};
};

struct ProgressSpec {
    D2D1_RECT_F bounds{};
    float value = 0.0f;
    bool indeterminate = false;
    bool paused = false;
    bool error = false;
    float animation_progress = 0.0f;
};

struct BreadcrumbSegmentSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view glyph;
    ControlState state{};
    bool current = false;
    bool has_separator = true;
    bool has_menu = false;
};

struct CommandSearchBoxSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view placeholder;
    std::wstring_view keycap;
    ControlState state{};
};

struct SplitButtonSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view glyph;
    ControlState primary_state{};
    ControlState menu_state{};
};

struct SplitterSpec {
    D2D1_RECT_F bounds{};
    ControlState state{};
    bool vertical = true; // vertical bar between left/right panes
};

struct SegmentedItemSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    std::wstring_view glyph;
    ControlState state{};
    SegmentPosition position = SegmentPosition::Single;
    bool shared_track = false;
};

struct BadgeSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    BadgeKind kind = BadgeKind::Neutral;
    D2D1_COLOR_F custom_background{};
    D2D1_COLOR_F custom_foreground{};
    bool use_custom_colors = false;
    bool dot = false;
};

// Compact filled tag used for user-assigned labels (starred/quick-access
// labels and file-row labels). This is intentionally separate from BadgeSpec:
// badges also cover status pills and keyboard hints with different semantics.
struct TagSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    D2D1_COLOR_F color{};
    bool bordered = true;
};

struct SidebarSectionHeaderSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    ControlState state{};
    bool expanded = true;
};

struct DriveSidebarItemSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view name;
    std::wstring_view detail;
    std::wstring_view glyph;
    float capacity = 0.0f;
    ControlState state{};
    bool drop_target = false;
    bool skip_glyph = false;
    D2D1_COLOR_F icon_color{};
    D2D1_COLOR_F bar_color{};
};

struct PaneHeaderSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view title;
    std::wstring_view summary;
    std::wstring_view filter_text;
    ControlState filter_state{};
    ControlState sort_state{};
};

struct ColumnHeaderSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view text;
    ControlState state{};
    SortDirection sort = SortDirection::None;
    HorizontalAlignment alignment = HorizontalAlignment::Left;
};

struct FileRowContentSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view name;
    std::wstring_view modified;
    std::wstring_view type;
    std::wstring_view size;
    std::wstring_view glyph;
    std::wstring_view status;
    ControlState state{};
    BadgeKind status_kind = BadgeKind::Neutral;
    std::array<D2D1_COLOR_F, 3> tag_colors{};
    size_t tag_count = 0;
    bool cut = false;
    bool drop_target = false;
};

struct StagingTrayPanelSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view title;
    std::wstring_view helper;
    std::wstring_view count_label;
    std::wstring_view action_text;
    int item_count = 0;
    ControlState state{};
    bool action_hovered = false; // 释放 button under the cursor
    bool expanded = true;
};

struct StagingItemSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view name;
    ControlState state{};
    bool missing = false;
};

struct StatusBarSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view left_text;
    std::wstring_view center_text;
    std::wstring_view right_text;
    bool ready = true;
};

struct InfoBarSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view title;
    std::wstring_view message;
    std::wstring_view glyph;
    InfoBarKind kind = InfoBarKind::Informational;
    ControlState state{};
    bool show_close = true;
};

struct EmptyStateSpec {
    D2D1_RECT_F bounds{};
    std::wstring_view glyph;
    std::wstring_view title;
    std::wstring_view message;
};

class Painter {
public:
    explicit Painter(Compositor* compositor = nullptr) noexcept;

    void SetCompositor(Compositor* compositor) noexcept;
    void SetScale(float scale) noexcept;
    void InvalidateTypography() noexcept;
    float Scale() const noexcept { return scale_; }

    // Call once before drawing a frame. Updating brush colors does not allocate resources.
    bool BeginFrame(const Theme& theme, bool high_contrast = false);

    void FillRoundedRect(const D2D1_RECT_F& bounds, float radius,
                         const D2D1_COLOR_F& color);
    void StrokeRoundedRect(const D2D1_RECT_F& bounds, float radius,
                           const D2D1_COLOR_F& color, float width = 1.0f);
    void DrawText(std::wstring_view text, const D2D1_RECT_F& bounds,
                  IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                  HorizontalAlignment alignment = HorizontalAlignment::Left);
    void DrawText(std::wstring_view text, const D2D1_RECT_F& bounds,
                  IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                  HorizontalAlignment alignment, const D2D1_COLOR_F& background);
    void DrawGlyph(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                   const D2D1_COLOR_F& color);
    void DrawFocusRing(const D2D1_RECT_F& bounds, float radius);

    void DrawButton(const ButtonSpec& spec);
    void DrawTextFieldFrame(const D2D1_RECT_F& bounds, const ControlState& state,
                           bool hosted_edit = false);
    void DrawTextField(const TextFieldSpec& spec);
    void DrawCheckBox(const D2D1_RECT_F& bounds, std::wstring_view text,
                      const ControlState& state);
    void DrawRadioButton(const D2D1_RECT_F& bounds, std::wstring_view text,
                         const ControlState& state);
    void DrawSwitch(const D2D1_RECT_F& bounds, std::wstring_view text,
                    const ControlState& state);
    void DrawTitleBarButton(const D2D1_RECT_F& bounds, TitleBarButtonRole role,
                            std::wstring_view custom_glyph, const ControlState& state);
    void DrawTab(const TabSpec& spec);
    void DrawMenuSurface(const D2D1_RECT_F& bounds);
    void DrawMenuItem(const MenuItemSpec& spec);
    void DrawTooltip(const D2D1_RECT_F& bounds, std::wstring_view text);
    void DrawListRowBackground(const ListRowSpec& spec);
    void DrawSidebarItem(const SidebarItemSpec& spec);
    void DrawTrayCard(const TrayCardSpec& spec);
    void DrawProgressBar(const ProgressSpec& spec);
    void DrawProgressRing(const ProgressSpec& spec);
    void DrawTagDot(D2D1_POINT_2F center, float radius, const D2D1_COLOR_F& color);
    void DrawTagDotState(D2D1_POINT_2F center, float radius, const D2D1_COLOR_F& color,
                         bool checked, bool mixed, bool hovered = false);
    void DrawScrollbar(const ScrollbarSpec& spec);
    void DrawBreadcrumbSegment(const BreadcrumbSegmentSpec& spec);
    void DrawCommandSearchBox(const CommandSearchBoxSpec& spec);
    void DrawSplitButton(const SplitButtonSpec& spec);
    void DrawSplitter(const SplitterSpec& spec);
    void DrawSegmentedItem(const SegmentedItemSpec& spec);
    void DrawSegmentedTrack(const D2D1_RECT_F& bounds);
    void DrawBadge(const BadgeSpec& spec);
    float MeasureBadgeWidth(std::wstring_view text) const;
    float MeasureButtonWidth(std::wstring_view text,
                             std::wstring_view glyph = {},
                             bool drop_down = false) const;
    float MeasureButtonHeight() const;
    D2D1_RECT_F FitButtonBounds(D2D1_RECT_F bounds, std::wstring_view text,
                                std::wstring_view glyph = {},
                                bool drop_down = false) const;
    void DrawTag(const TagSpec& spec);
    float MeasureTagWidth(std::wstring_view text) const;
    float OmnibarHintReservePx() const;
    D2D1_RECT_F DrawOmnibarHints(const D2D1_RECT_F& field, bool skip_search_glyph = false);
    D2D1_RECT_F ButtonGlyphRect(const D2D1_RECT_F& bounds, bool icon_only) const;
    void DrawSidebarSectionHeader(const SidebarSectionHeaderSpec& spec);
    D2D1_RECT_F SidebarItemIconRect(const D2D1_RECT_F& bounds, bool status_dot) const;
    D2D1_RECT_F DriveSidebarItemIconRect(const D2D1_RECT_F& bounds) const;
    void DrawDriveSidebarItem(const DriveSidebarItemSpec& spec);
    void DrawPaneHeader(const PaneHeaderSpec& spec);
    void DrawColumnHeader(const ColumnHeaderSpec& spec);
    void DrawFileRowContent(const FileRowContentSpec& spec);
    void DrawStagingTrayPanel(const StagingTrayPanelSpec& spec);
    void DrawStagingItem(const StagingItemSpec& spec);
    void DrawStatusBar(const StatusBarSpec& spec);
    void DrawInfoBar(const InfoBarSpec& spec);
    void DrawEmptyState(const EmptyStateSpec& spec);

    D2D1_RECT_F ScrollbarThumbRect(const ScrollbarSpec& spec) const noexcept;
    static float ListRowContentOpacity(const ListRowSpec& spec) noexcept;
    static bool Contains(const D2D1_RECT_F& bounds, float x, float y) noexcept;

private:
    enum class BrushId : size_t {
        Background,
        Text,
        TextSecondary,
        TextDisabled,
        Hover,
        Pressed,
        Selected,
        Input,
        InputHover,
        StrokeCard,
        Divider,
        InputBottom,
        Flyout,
        Accent,
        AccentHover,
        AccentPressed,
        AccentText,
        AccentTextPressed,
        Danger,
        DangerHover,
        CloseText,
        Scrollbar,
        TabActive,
        Count
    };

    ID2D1SolidColorBrush* Brush(BrushId id) const noexcept;
    ID2D1SolidColorBrush* ScratchBrush(const D2D1_COLOR_F& color);
    void UpdateBrush(BrushId id, const D2D1_COLOR_F& color);
    void Fill(const D2D1_RECT_F& bounds, BrushId brush);
    void FillRounded(const D2D1_RECT_F& bounds, float radius, BrushId brush);
    void StrokeRounded(const D2D1_RECT_F& bounds, float radius, BrushId brush,
                       float width = 1.0f);
    void DrawTextWithBrush(std::wstring_view text, const D2D1_RECT_F& bounds,
                           IDWriteTextFormat* format, BrushId brush,
                           HorizontalAlignment alignment = HorizontalAlignment::Left);
    void DrawGlyphWithBrush(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                            BrushId brush);
    void DrawGlyphWithFormat(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                             const D2D1_COLOR_F& color, IDWriteTextFormat* format);
    void DrawCheckMark(const D2D1_RECT_F& bounds, const D2D1_COLOR_F& color);
    void DrawPaneLayoutIcon(const D2D1_RECT_F& bounds, MenuPictogram kind,
                            const D2D1_COLOR_F& color);
    void DrawArc(D2D1_POINT_2F center, float radius, float start_degrees,
                 float span_degrees, float stroke_width, const D2D1_COLOR_F& color);
    void EnsureTextFormats();
    void EnsureStrokeStyle();
    IDWriteTextFormat* BodyFormat() const noexcept;
    IDWriteTextFormat* CaptionFormat() const noexcept;
    IDWriteTextFormat* NavFormat() const noexcept;     // sidebar rows: 13px
    IDWriteTextFormat* SectionFormat() const noexcept; // sidebar group headers: 13px semibold
    IDWriteTextFormat* SmallIconFormat() const noexcept;
    IDWriteTextFormat* MicroIconFormat() const noexcept;
    float Px(float dips) const noexcept;

    Compositor* compositor_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    const Theme* theme_ = nullptr;
    float scale_ = 1.0f;
    bool high_contrast_ = false;
    bool dark_ = false;
    float format_scale_ = 0.0f;
    std::array<ComPtr<ID2D1SolidColorBrush>, static_cast<size_t>(BrushId::Count)> brushes_{};
    ComPtr<ID2D1SolidColorBrush> scratch_brush_;
    ComPtr<ID2D1StrokeStyle> round_stroke_;
    ComPtr<IDWriteTextFormat> body_format_;
    ComPtr<IDWriteTextFormat> nav_format_;
    ComPtr<IDWriteTextFormat> section_format_;
    ComPtr<IDWriteTextFormat> caption_format_;
    ComPtr<IDWriteTextFormat> small_icon_format_;
    ComPtr<IDWriteTextFormat> micro_icon_format_;
    ComPtr<IDWriteInlineObject> ellipsis_sign_;
};

} // namespace pulse::ui::fluent
