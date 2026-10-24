#pragma once

#include "ui_compositor.h"
#include "thumbnail_cache.h"
#include "archive_preview.h"
#include "markdown_view.h"
#include "table_view.h"
#include "tree_view.h"
#include "preview_handler_host.h"
#include "window_material.h"
#include "fluent_menu.h"
#include "video_preview.h"
#include "audio_waveform.h"
#include "quick_preview_command.h"
#include "pe_preview_tabs.h"

#include <string>
#include <vector>

namespace pulse::ui {

class QuickPreviewWindow {
public:
    QuickPreviewWindow() = default;
    ~QuickPreviewWindow();
    QuickPreviewWindow(const QuickPreviewWindow&) = delete;
    QuickPreviewWindow& operator=(const QuickPreviewWindow&) = delete;

    bool Initialize(HWND owner, UINT navigate_message, UINT open_message,
                    UINT command_message = 0);
    // zoom_from: screen point of the previewed item's icon; the window grows
    // out of it and shrinks back into it on Close (ui_motion.h rules).
    void Show(const QuickPreviewItem& item, bool dark, WindowEffect effect, bool safe_mode,
              const POINT* zoom_from = nullptr);
    void Update(const QuickPreviewItem& item);
    void SetStarred(bool starred);
    // The FFmpeg preview pack offer on the codec cards; repaints on change.
    void SetMediaPackOffer(const MediaPackOffer& offer);
    // A pack was installed: a file it can play now is opened again.
    void OnMediaPackInstalled();
    // The image preview pack offer on the picture cards, and its install.
    void SetImagePackOffer(const MediaPackOffer& offer);
    void OnImagePackInstalled();
    void OnExtraPackInstalled();
    void Close();
    bool visible() const noexcept;
    HWND hwnd() const noexcept { return hwnd_; }
    const QuickPreviewItem& item() const noexcept { return item_; }
    bool TakeCommand(UINT_PTR token, QuickPreviewCommand& command) { return commands_.Take(token, command); }
    // Path chosen inside a folder listing, handed over once with open_message_.
    std::wstring TakeOpenPath() { std::wstring path; path.swap(open_path_); return path; }

private:
#ifdef PULSE_WITH_SELFTEST
    friend struct QuickPreviewAuditTest;
#endif
    friend struct QuickPreviewPlaybackProbe;
    friend struct PreviewDeadlineProbe;
    // Pages: multi-page PDF / AI read as a continuous scroll (quick_preview_pages.cpp).
    enum class NativeKind { None, Bitmap, Text, Hex, Archive, Pages, Markdown, Table, Tree };
    enum class ChromeButton { None, Prev, Next, More };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void Render();
    void SchedulePreviewDeadline();
    uint64_t preview_deadline_ = 0;
    void Resize();
    void ResetView();
    void RecreateFormats();
    bool OfflinePlaceholder() const noexcept;
    void ResetAnimation();
    void BeginVideo();
    void ResetPlayback();
    bool HasPlayback() const noexcept;
    float PlaybackHeight() const noexcept;
    D2D1_RECT_F PlaybackRect() const;
    D2D1_RECT_F PlaybackButtonRect(int button) const;
    D2D1_RECT_F PlaybackTrackRect() const;
    void TogglePlayback();
    void StepPlayback(int direction);
    void SeekPlayback(float x);
    void SubmitPlaybackSeek(bool immediate);
    void TickPlaybackSeek();
    void CancelPlaybackScrub();
    double PlaybackFraction(const VideoPreview::State& state) const;
    std::wstring PlaybackInfo(const VideoPreview::State& state) const;
    bool PlaybackMouseDown(POINT point);
    void EndPlaybackDrag(bool resume);
    void DrawPlayback(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush);
    // Icon playback bar (kVideoChrome): buttons | time | track | time | volume | speed | chip.
    struct PlaybackGeometry {
        D2D1_RECT_F buttons[3]{};
        int button_count = 0;
        int play_index = 0;
        D2D1_RECT_F time_left{}, track{}, time_right{}, volume{}, speed{}, chip{};
    };
    PlaybackGeometry PlaybackLayout() const;
    void DrawPlaybackChrome(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush);
    // Aspect-fitted, rounded video window inside the content area (plus shadow).
    D2D1_RECT_F VideoFrameRect(const VideoPreview::State& state) const;
    void LayoutVideo(ID2D1DeviceContext* dc, const VideoPreview::State& state, bool show);
    // FFmpeg playback is composed here, clipped like the video child.
    void DrawFfmpegFrame(ID2D1DeviceContext* dc, const D2D1_RECT_F& frame, float radius);
    bool PlaybackHover(POINT client);             // true when hover state changed
    bool PlaybackWheel(POINT client, float steps);  // wheel over the volume button
    // Card shown instead of a black frame when no decoder handles the video track.
    bool CodecCardVisible(const VideoPreview::State& state) const;
    void DrawCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                       const VideoPreview::State& state, ID2D1SolidColorBrush* brush);
    bool CodecCardClick(POINT client);
    bool OverCodecButton(POINT client) const;
    // Codec card wording shared by videos and pictures (HEIF / AVIF).
    struct CodecCardText {
        std::wstring title, lead, name, tail, get, hint;
        const wchar_t* store_id = nullptr;
        bool picture = false;
        bool pack = false;   // offer a preview pack above the Store button
        bool image_pack = false;   // ...the image pack rather than FFmpeg
    };
    void DrawCodecCardText(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                           const CodecCardText& text, ID2D1SolidColorBrush* brush);
    void DrawImageCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                            const std::wstring& codec, ID2D1SolidColorBrush* brush);
    // .ico / .cur: the sizes inside the file as pills under the picture.
    struct IconSize { uint32_t width = 0, height = 0, bits = 0; bool png = false; };
    void ParseIconSizes(const std::wstring& payload);
    void DrawIconSizes(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, ID2D1SolidColorBrush* brush);
    int HitIconSize(POINT client) const;
    void ResetTextState();
    float PeTabsHeight() const;
    D2D1_RECT_F PeTabRect(int index) const;
    int HitPeTab(POINT point) const;
    void SelectPeTab(int index);
    void DrawPeTabs(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush);
    void StylePeDependencies();
    PePreviewTabs pe_tabs_;
    int pe_tab_ = 0;

    // Paged documents (quick_preview_pages.cpp).
    struct PagesLayout {
        D2D1_RECT_F strip{};   // thumbnail strip; empty when hidden
        D2D1_RECT_F view{};    // page area
        float page_w = 0.0f;   // pixel width of every page at the current zoom
        float margin = 0.0f;
        float gap = 0.0f;
    };
    void ResetPages();
    PagesLayout ComputePagesLayout() const;
    float PageAspect(uint32_t page) const noexcept;
    float PageTop(const PagesLayout& layout, uint32_t page) const;
    float PagesDocHeight(const PagesLayout& layout) const;
    uint32_t CurrentPage(const PagesLayout& layout) const;
    void ClampPages(const PagesLayout& layout);
    void ScrollToPage(uint32_t page);
    void ZoomPages(float cursor_x, float cursor_y, float factor);
    void TogglePagesFit();
    void DrawPages(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* text_brush,
                   ID2D1SolidColorBrush* secondary_brush);
    bool PagesWheel(POINT client, float steps, bool ctrl, bool shift);
    bool PagesMouseDown(POINT client);
    bool PagesKey(WPARAM key);

    // Audio files (quick_preview_audio.cpp): cover, tags and waveform above
    // the shared playback bar.
    bool IsAudioPreview() const;
    bool AudioMouseDown(POINT client);
    bool AudioHover(POINT client);               // true when the hover changed
    void DrawAudio(ID2D1DeviceContext* dc, const VideoPreview::State& state,
                   ID2D1SolidColorBrush* text_brush, ID2D1SolidColorBrush* secondary_brush);

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
    Theme CurrentTheme() const;
    void EnsureTextLayout(const std::wstring& text, bool hex, float width);
    bool HitTestText(float x, float y, uint32_t& index);
    void CopyTextSelection(bool require_selection) const;
    void SelectAllText();
    void OpenFind();
    bool OpenListingSelection();
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
    void ShowContextMenu(POINT screen);
    QuickPreviewItem ActionTarget() const;
    void PostAction(QuickPreviewAction action, const QuickPreviewItem* target = nullptr);
    D2D1_RECT_F ChromeButtonRect(ChromeButton button) const;
    // "Rendered | Source" pill left of the chrome buttons; segment -1 = whole pill.
    D2D1_RECT_F MarkdownToggleRect(int segment) const;
    int HitMarkdownToggle(POINT client) const;  // -1 none, 0 rendered, 1 source
    void DrawMarkdownToggle(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* text_brush);
    ChromeButton HitChromeButton(POINT client) const;
    void ActivateChromeButton(ChromeButton button);
    void DrawChromeButtons(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* text_brush);
    static LRESULT CALLBACK FindEditProc(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam, UINT_PTR id, DWORD_PTR data);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    UINT navigate_message_ = 0;
    UINT open_message_ = 0;
    std::wstring open_path_;  // folder listing item for open_message_ (wParam 1)
    UINT command_message_ = 0;
    Compositor compositor_;
    // Room for every visible PDF page at full size plus the thumbnail strip;
    // a smaller budget would evict an on-screen page and re-request it forever.
    ThumbnailCache thumbnails_{48ull * 1024ull * 1024ull, 192};
    PreviewHandlerHost handler_;
    VideoPreview video_;
    AudioWaveform waveform_;
    D2D1_RECT_F audio_wave_rect_{};
    float audio_hover_x_ = -1.0f;                // pointer x over the waveform, or -1
    bool playback_drag_ = false;
    bool playback_resume_ = false;
    bool playback_scrub_pending_ = false;
    bool playback_seek_dirty_ = false;
    double playback_scrub_fraction_ = 0;
    ULONGLONG playback_last_seek_ms_ = 0;
    float playback_hover_x_ = -1.0f;   // pointer x over the seek track, or -1
    int playback_hover_button_ = -1;   // 0..2 transport, 3 volume, 4 speed
    float playback_volume_ = 1.0f;     // session-wide
    bool playback_muted_ = false;      // session-wide
    float playback_rate_ = 1.0f;       // per file
    std::wstring playback_note_;       // transient chip text (volume / speed)
    ULONGLONG playback_note_until_ = 0;
    D2D1_RECT_F codec_store_rect_{};   // codec card buttons (this frame)
    D2D1_RECT_F codec_open_rect_{};
    D2D1_RECT_F codec_pack_rect_{};
    ComPtr<ID2D1Bitmap> ffmpeg_bitmap_;        // reused while the size stays
    ComPtr<ID2D1BitmapBrush> ffmpeg_brush_;
    ComPtr<ID2D1Device> ffmpeg_device_;        // the bitmap's device
    uint64_t ffmpeg_serial_ = 0;
    bool ffmpeg_noted_ = false;               // the "FFmpeg" chip note was shown
    MediaPackOffer pack_offer_;
    MediaPackOffer image_offer_;
    bool codec_pack_image_ = false;   // codec_pack_rect_ installs the image pack
    std::wstring codec_store_id_;
    ComPtr<IDWriteTextFormat> close_format_;
    ComPtr<IDWriteTextFormat> preview_text_format_;
    ComPtr<IDWriteTextLayout> text_layout_;
    QuickPreviewItem item_;
    QuickPreviewCommands commands_;
    std::wstring preview_notice_;
    float preview_notice_height_ = 0;
    uint64_t generation_ = 1;
    bool dark_ = false;
    WindowEffect effect_ = WindowEffect::MicaAlt;
    bool safe_mode_ = false;
    bool handler_immediate_ = false;
    // Open/close zoom (Compositor::PlayZoom).
    bool zoom_enabled_ = false;
    bool zoom_pending_ = false;
    bool closing_ = false;
    POINT zoom_origin_{};
    void FinishClose(bool restore_focus);
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
    ArchivePreview archive_;
    MarkdownView markdown_;
    bool markdown_source_ = false;  // session-wide: Markdown shown as source
    bool markdown_shown_ = false;   // this frame drew a Markdown item (toggle visible)
    // Labels of the title-bar pill while markdown_shown_ is set.
    enum class ToggleKind { Markdown, TableSource, TableHandler, TreeSource, NotebookSource, DocHandler };
    ToggleKind toggle_kind_ = ToggleKind::Markdown;
    TableView table_;
    uint32_t sheet_request_ = 0;
    bool table_source_ = false;   // session-wide: CSV shown as source
    bool table_handler_ = false;  // session-wide: XLSX in the system preview handler
    TreeView tree_;
    bool tree_source_ = false;      // session-wide: JSON/XML shown as source
    bool notebook_source_ = false;  // session-wide: notebooks shown as source
    bool doc_handler_ = false;      // session-wide: DOCX in the system preview handler
    uint32_t icon_request_ = 0;     // frame_index for .ico/.cur: 0 largest, k entry k-1
    std::vector<IconSize> icon_sizes_;
    int icon_selected_ = -1;
    std::vector<D2D1_RECT_F> icon_pill_rects_;
    bool& ToggleSecond() noexcept;  // the flag the title-bar pill switches
    bool panning_ = false;
    POINT pan_anchor_{};
    float pan_start_x_ = 0.0f;
    float pan_start_y_ = 0.0f;
    bool close_hover_ = false;
    ChromeButton chrome_hover_ = ChromeButton::None;
    bool mouse_tracking_ = false;
    uint32_t frame_index_ = 0;
    uint32_t requested_frame_ = 0;
    uint32_t frame_count_ = 1;
    uint32_t frame_delay_ms_ = 0;
    uint32_t loop_count_ = 0;
    uint32_t completed_loops_ = 0;
    bool animation_active_ = false;
    bool waiting_for_frame_ = false;
    bool animation_started_ = false;
    // Paged document state; pages_count_ > 1 switches Render to DrawPages.
    uint32_t pages_count_ = 0;
    std::vector<float> page_aspect_;   // height / width per page, 0 = not yet known
    float pages_scroll_ = 0.0f;        // pixels from the top of the document
    float pages_pan_x_ = 0.0f;         // horizontal offset when zoomed wider than the view
    float pages_zoom_ = 1.0f;          // 1 = fit width
    float pages_strip_scroll_ = 0.0f;
    uint32_t pages_follow_ = UINT32_MAX; // strip auto-follows when the current page changes
    bool pages_strip_ = true;

    std::wstring preview_text_;
    float text_layout_width_ = 0.0f;
    float text_layout_scale_ = 0.0f;
    bool text_layout_hex_ = false;
    // Syntax colouring + line-number gutter (syntax_highlight.h).
    bool text_layout_dark_ = false;
    bool line_numbers_ = true;          // context-menu toggle, per session
    float text_gutter_ = 0.0f;          // pixels reserved left of the text
    uint32_t text_line_count_ = 0;
    std::vector<uint32_t> line_starts_; // filled only while the gutter is shown
    std::wstring syntax_language_;
    ComPtr<IDWriteTextFormat> gutter_format_;
    ComPtr<ID2D1SolidColorBrush> syntax_brushes_[16];
    float TextOriginX() const noexcept;
    void DrawTextStatus(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, bool hex,
                        uint32_t bytes_read, bool truncated, uint32_t encoding,
                        ID2D1SolidColorBrush* text_brush);
    // Status pill centred at the bottom of content; parts joined by " | ".
    void DrawStatusPill(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                        const std::vector<std::wstring>& parts, ID2D1SolidColorBrush* text_brush);
    void DrawLineNumbers(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, float origin_y);
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
