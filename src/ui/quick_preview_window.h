#pragma once

#include "ui_compositor.h"
#include "thumbnail_cache.h"
#include "preview_handler_host.h"
#include "window_material.h"
#include "fluent_menu.h"

#include <string>
#include <vector>

namespace pulse::ui {

struct QuickPreviewItem {
    std::wstring path;
    std::wstring name;
    DWORD attrs = 0;
    uint64_t modified = 0;
    uint64_t size = 0;
};

class QuickPreviewWindow {
public:
    QuickPreviewWindow() = default;
    ~QuickPreviewWindow();
    QuickPreviewWindow(const QuickPreviewWindow&) = delete;
    QuickPreviewWindow& operator=(const QuickPreviewWindow&) = delete;

    bool Initialize(HWND owner, UINT navigate_message, UINT open_message);
    void Show(const QuickPreviewItem& item, bool dark, WindowEffect effect, bool safe_mode);
    void Update(const QuickPreviewItem& item);
    void Close();
    bool visible() const noexcept;
    HWND hwnd() const noexcept { return hwnd_; }

private:
    enum class NativeKind { None, Bitmap, Text, Hex };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void Render();
    void Resize();
    void ResetView();
    void RecreateFormats();
    bool OfflinePlaceholder() const noexcept;
    void ResetAnimation();
    void ResetTextState();

    D2D1_RECT_F ContentRect() const;
    D2D1_RECT_F FindBarRect() const;
    D2D1_RECT_F FindFieldRect() const;
    D2D1_RECT_F FindEditCell() const;
    float FindBarHeight() const noexcept;
    float FitScale(float view_w, float view_h) const noexcept;
    bool CanPanImage() const;
    bool HasTextSelection() const noexcept;
    void SetFitMode();
    void SetActualPixels();
    void ToggleFitActual();
    void ZoomAt(float cursor_x, float cursor_y, float factor);
    void ClampPan(float view_w, float view_h);
    D2D1_RECT_F ImageDest(const D2D1_RECT_F& content) const;
    uint32_t RequestedPixelSize(const D2D1_RECT_F& content) const;
    void DrawHud(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                 ID2D1SolidColorBrush* text_brush);
    void DrawFindBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar);
    void EnsureTextLayout(const std::wstring& text, bool hex, float width);
    bool HitTestText(float x, float y, uint32_t& index);
    void CopyTextSelection(bool require_selection) const;
    void SelectAllText();
    void OpenFind();
    void CloseFind();
    void UpdateFindMatches();
    void FindNext(int direction);
    void ScrollMatchIntoView(uint32_t start);
    void DrawTextPreview(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                          ID2D1SolidColorBrush* text_brush);
    bool ClientPoint(LPARAM lparam, POINT& out) const;
    HCURSOR ContentCursor(POINT client) const;
    bool EnsureFindEdit();
    void LayoutFindEdit();
    void SyncFindFromEdit();
    void DestroyFindEdit();
    void PaintFindEditLuma(HWND hwnd, HDC hdc);
    LRESULT ForwardFindEditKeepLuma(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void ShowTextContextMenu(POINT screen);
    static LRESULT CALLBACK FindEditProc(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam, UINT_PTR id, DWORD_PTR data);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    UINT navigate_message_ = 0;
    UINT open_message_ = 0;
    Compositor compositor_;
    ThumbnailCache thumbnails_;
    PreviewHandlerHost handler_;
    ComPtr<IDWriteTextFormat> close_format_;
    ComPtr<IDWriteTextFormat> preview_text_format_;
    ComPtr<IDWriteTextLayout> text_layout_;
    QuickPreviewItem item_;
    uint64_t generation_ = 1;
    bool dark_ = false;
    WindowEffect effect_ = WindowEffect::MicaAlt;
    bool safe_mode_ = false;
    bool handler_immediate_ = false;
    float scale_ = 1.0f;
    float text_scroll_ = 0.0f;
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;
    bool image_fit_ = true;
    float image_zoom_ = 1.0f;
    uint32_t preview_pixels_ = 0;
    uint32_t decoded_w_ = 0;
    uint32_t decoded_h_ = 0;
    uint32_t source_w_ = 0;
    uint32_t source_h_ = 0;
    NativeKind native_kind_ = NativeKind::None;
    bool panning_ = false;
    POINT pan_anchor_{};
    float pan_start_x_ = 0.0f;
    float pan_start_y_ = 0.0f;
    bool close_hover_ = false;
    uint32_t frame_index_ = 0;
    uint32_t requested_frame_ = 0;
    uint32_t frame_count_ = 1;
    uint32_t frame_delay_ms_ = 0;
    uint32_t loop_count_ = 0;
    uint32_t completed_loops_ = 0;
    bool animation_active_ = false;
    bool waiting_for_frame_ = false;
    bool animation_started_ = false;

    std::wstring preview_text_;
    float text_layout_width_ = 0.0f;
    float text_layout_scale_ = 0.0f;
    bool text_layout_hex_ = false;
    uint32_t sel_anchor_ = 0;
    uint32_t sel_focus_ = 0;
    bool selecting_ = false;
    bool find_open_ = false;
    std::wstring find_query_;
    std::vector<uint32_t> find_matches_;
    uint32_t find_index_ = 0;
    HWND find_edit_ = nullptr;
    HFONT find_edit_font_ = nullptr;
    HBRUSH find_edit_brush_ = nullptr;
    FluentMenu text_menu_;
    fluent::Painter find_painter_;
};

} // namespace pulse::ui
