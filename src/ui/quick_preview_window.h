#include <future>
#pragma once
#include "plain_highlight_lines.h"
#include "preview_text_identity.h"

#include "ui_compositor.h"
#include "thumbnail_cache.h"
#include "native_preview_view.h"
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
#include "quick_preview_size_probe.h"
#include "open_with_icons.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <deque>

namespace pulse::ui {

class QuickPreviewWindow {
public:
    std::vector<std::shared_future<void>> SuspendVolume(const std::wstring& root);
    void ResumeVolume(const std::wstring& root);
    // An app popup over the pane hides a shell preview handler's topmost
    // overlay (kept loaded); false shows it again.
    void SetHandlerCovered(bool covered);

    QuickPreviewWindow() = default;
    ~QuickPreviewWindow();
    QuickPreviewWindow(const QuickPreviewWindow&) = delete;
    QuickPreviewWindow& operator=(const QuickPreviewWindow&) = delete;

    bool Initialize(HWND owner, UINT navigate_message, UINT open_message,
                    UINT command_message = 0);
    // zoom_from: screen point of the previewed item's icon; the window grows
    // out of it and shrinks back into it on Close (ui_motion.h rules).
    // activate false: the press-and-hold peek; focus and the mouse stay with
    // the owner and media start muted until Activate.
    void Show(const QuickPreviewItem& item, bool dark, WindowEffect effect, bool safe_mode,
              const POINT* zoom_from = nullptr, bool activate = true);
    bool InitializeEmbedded(HWND owner, UINT navigate_message, UINT open_message, UINT command_message);
    void ShowEmbedded(const QuickPreviewItem& item, RECT bounds, bool dark, bool safe_mode);
    void SetEmbeddedBounds(RECT bounds);
    // Subtract a parent-rendered flyout from the child without closing or
    // reloading its preview. Empty bounds restore the complete child.
    void SetEmbeddedOcclusion(const D2D1_RECT_F& parent_bounds);
    bool embedded() const noexcept { return embedded_; }
    void Activate();
    void Update(const QuickPreviewItem& item);
    void SetStarred(bool starred);
    // Content-search highlights without opening the interactive find bar.
    void SetSearchQuery(const std::wstring& query);
    // The app's accent colour (open button); call before Show.
    void SetAccent(const D2D1_COLOR_F& accent);
    // Settings > 快速预览 > 预览窗口; call before Show. What the window changes
    // itself is reported with QuickPreviewAction::SaveOptions.
    void SetOptions(const QuickPreviewOptions& options) { options_ = options; }
    const QuickPreviewOptions& options() const noexcept { return options_; }
    // The FFmpeg preview pack offer on the codec cards; repaints on change.
    void SetMediaPackOffer(const MediaPackOffer& offer);
    // A pack was installed: a file it can play now is opened again.
    void OnMediaPackInstalled();
    // The image preview pack offer on the picture cards, and its install.
    void SetImagePackOffer(const MediaPackOffer& offer);
    void SetCloudDownloadOffer(const CloudDownloadOffer& offer);
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
    bool embedded_ = false;
    bool embedded_open_ = false;
    bool embedded_occluded_ = false;
    bool handler_covered_ = false;
    RECT embedded_occlusion_{}, embedded_region_bounds_{};
    std::vector<std::wstring> suspended_volumes_;

    friend struct DetailsPreviewProbe;
    friend struct NativePreviewProbe;
    friend struct QuickPreviewAuditTest;
    friend struct QuickPreviewPlaybackProbe;
    friend struct PreviewDeadlineProbe;
    // Pages: multi-page PDF / AI read as a continuous scroll (quick_preview_pages.cpp).
    enum class NativeKind { None, Bitmap, Text, Hex, Archive, Pages, Markdown, Table, Tree, Model };
    enum class ChromeButton { None, Prev, Next, More, Open, Size };

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
    void PostNavigate(int dir);
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
    // Timeline row; media add a second row for volume, speed and track menus.
    struct PlaybackGeometry {
        D2D1_RECT_F buttons[3]{};
        int button_count = 0;
        int play_index = 0;
        D2D1_RECT_F time_left{}, track{}, time_right{}, volume{}, volume_track{}, volume_value{}, audio_tracks{}, subtitles{}, speed{}, chip{};
    };
    PlaybackGeometry PlaybackLayout() const;
    void DrawPlaybackChrome(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush);
    // Aspect-fitted, rounded video window inside the content area (plus shadow).
    D2D1_RECT_F VideoFrameRect(const VideoPreview::State& state) const;
    void LayoutVideo(ID2D1DeviceContext* dc, const VideoPreview::State& state, bool show);
    // FFmpeg playback is composed here, clipped like the video child.
    void DrawFfmpegFrame(ID2D1DeviceContext* dc, const D2D1_RECT_F& frame, float radius);
    bool PlaybackHover(POINT client);             // true when hover state changed
    void SetPlaybackVolume(float value);
    void DragPlaybackVolume(float x);
    std::vector<FluentMenuItem> PlaybackTrackItems(bool subtitles, const VideoPreview::State& state) const;
    void ShowPlaybackTracks(bool subtitles, POINT point);
    bool PlaybackWheel(POINT client, float steps);  // wheel over the volume controls
    // Card shown instead of a black frame when no decoder handles the video track.
    bool CodecCardVisible(const VideoPreview::State& state) const;
    void DrawCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                       const VideoPreview::State& state, ID2D1SolidColorBrush* brush);
    bool CodecCardClick(POINT client);
    // Cloud-only file: explanation + 下载并预览 (or its progress). False when
    // the window is too small for the card.
    bool DrawCloudCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, ID2D1SolidColorBrush* brush);
    CloudDownloadOffer cloud_offer_;
    D2D1_RECT_F cloud_download_rect_{};
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
    void DrawPages(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* secondary_brush);
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
    // Image facts and the zoom control in the footer.
    void SetImageFooter(const D2D1_RECT_F& content);
    // Image info drawer (EXIF rows and a histogram) on the right of the picture.
    float InfoDrawerWidth() const noexcept;
    // The backdrop behind a picture; false when the mode draws nothing.
    bool ImageBackdropWanted();
    bool DrawImageBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, const D2D1_RECT_F& draw);
    void DrawFindBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar);
    Theme CurrentTheme() const;
    void EnsureTextLayout(const std::wstring& text, bool hex, float width, bool reading = false, bool same_text = false);
    bool PlainTextCandidate() const;
    bool HitTestText(float x, float y, uint32_t& index);
    void CopyTextSelection(bool require_selection) const;
    void SelectAllText();
    void OpenFind();
    bool OpenListingSelection();
    void CloseFind();
    void UpdateFindMatches();
    void FindNext(int direction);
    void ScrollMatchIntoView(uint32_t start);
    void ResetTextDrawingCache();
    void DrawCachedText(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* text_brush,
                        float origin_x, float origin_y);
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
    // Window size presets (Settings > 快速预览 > 预览窗口, the header's size
    // button, Ctrl+1/2/3, F).
    void ShowSizeMenu(POINT screen);
    void ChooseSizeMode(int mode);           // menu / Ctrl+1..3: also the new default
    // The rect for the current item: the remembered size, else the preset's
    // (适应内容 sets fit_pending_ when the size is only known after decoding).
    RECT PlanRect(int mode, POINT centre, bool use_remembered);
    void ApplyRect(const RECT& rect);
    POINT WindowCentre() const;
    void EnterFullScreen();
    void LeaveFullScreen(bool restore_rect);
    bool ContentPixelSize(uint32_t& width, uint32_t& height) const;  // decoded / probed
    void BeginContentSizeProbe();
    QuickPreviewSizeProbe size_probe_;
    uint32_t probe_w_ = 0, probe_h_ = 0;
    int SizingChromeHeight() const;
    void FitToContent(uint32_t width, uint32_t height);              // 适应内容, keeps the centre
    void ReportOptions();
    void ToggleLoop();
    bool AnimationLoops() const noexcept { return options_.loop != 2; }
    bool VideoLoops() const noexcept { return options_.loop == 1; }
    QuickPreviewOptions options_;
    bool fullscreen_ = false;
    RECT pre_fullscreen_{};
    bool user_sized_ = false;     // resized by hand since Show: presets leave the size alone
    bool fit_pending_ = false;    // 适应内容 waits for the decoded picture / video size
    RECT size_move_start_{};
    ULONGLONG video_loop_at_ = 0;
    uint32_t fit_w_ = 0, fit_h_ = 0;   // 适应内容 from the first frame (kFitTimer)
    // "Space closes · K pauses · L loops" in a video's footer, for the first
    // few videos after Space started closing them.
    ULONGLONG key_hint_until_ = 0;
    void StartKeyHint();
    QuickPreviewItem ActionTarget() const;
    void PostAction(QuickPreviewAction action, const QuickPreviewItem* target = nullptr);
    D2D1_RECT_F ChromeButtonRect(ChromeButton button) const;
    // "Rendered | Source" pill left of the chrome buttons; segment -1 = whole pill.
    D2D1_RECT_F MarkdownToggleRect(int segment) const;
    int HitMarkdownToggle(POINT client) const;  // -1 none, 0 rendered, 1 source
    void DrawMarkdownToggle(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* text_brush);
    ChromeButton HitChromeButton(POINT client) const;
    void ActivateChromeButton(ChromeButton button);

    // Unified header and footer (quick_preview_chrome.cpp).
    struct HeaderLayout {
        D2D1_RECT_F badge{}, name{}, meta{}, tools{}, separator{}, prev{}, counter{}, next{},
            open{}, more{}, size{}, close{};
        bool meta_visible = true;     // narrow windows drop the meta line first,
        bool tools_visible = true;    // then fold the type tools into the more menu,
        bool open_text = true;        // then show only the open button's icon,
        bool counter_visible = true;  // then hide the position
        std::wstring counter_text, open_label;
    };
    struct ChromePalette {
        D2D1_COLOR_F bg{}, ink{}, ink2{}, ink3{}, line{}, hover{}, on_hover{}, accent{}, on_accent{};
        bool high_contrast = false;
    };
    static constexpr int kFooterZoom = 1;
    static constexpr int kFooterToc = 2;        // Markdown/document contents sidebar
    static constexpr int kFooterReadWidth = 3;  // .txt reading column width
    static constexpr int kFooterCopyPath = 4;   // JSON/XML: copy the current node's path
    static constexpr int kFooterFoldAll = 5;    // JSON/XML: expand all / collapse all
    static constexpr int kFooterZoomOut = 6;    // pages: zoom out one step
    static constexpr int kFooterZoomIn = 7;     // pages and images: zoom in one step
    static constexpr int kFooterImageBg = 8;    // images: backdrop auto / light / dark / checkerboard
    static constexpr int kFooterInfo = 9;       // images: the info drawer
    static constexpr int kFooterRotate = 10;    // images: a quarter turn clockwise
    ChromePalette Palette() const;
    float HeaderHeight() const noexcept;
    float FooterHeight() const noexcept;
    D2D1_RECT_F FooterRect() const;
    void RefreshItemFacts();
    std::wstring OpenLabel() const;
    std::wstring MetaLine() const;
    void LayoutHeader();
    bool HeaderInteractive(POINT client) const;
    void DrawHeader(ID2D1DeviceContext* dc, const ChromePalette& palette);
    void DrawFooter(ID2D1DeviceContext* dc, const ChromePalette& palette);
    void DrawInfoDrawer(ID2D1DeviceContext* dc, const ChromePalette& palette);
    void SetFooterParts(std::vector<std::wstring> parts);
    void AddFooterControl(int id, std::wstring text);
    int HitFooterControl(POINT client) const;   // control id, 0 = none
    void ActivateFooterControl(int id);
    void ToggleLabels(const wchar_t* (&labels)[2]) const;
    float ToggleSegmentWidth(int segment) const;
    ComPtr<IDWriteTextLayout> UiLayout(std::wstring_view text, IDWriteTextFormat* format,
                                       float width, float height,
                                       DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) const;
    struct UiLayoutEntry {
        std::wstring text;
        ComPtr<IDWriteTextFormat> format;
        float width = 0, height = 0;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        ComPtr<IDWriteTextLayout> layout;
    };
    mutable std::deque<UiLayoutEntry> ui_layout_cache_;
    mutable size_t ui_layout_text_bytes_ = 0;
    mutable uint64_t ui_layout_generation_ = 0;
    mutable uint64_t ui_layout_creations_ = 0, ui_layout_hits_ = 0, ui_sign_creations_ = 0;
    uint64_t ui_brush_creations_ = 0;
    struct UiTrimmingEntry {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteInlineObject> sign;
    };
    mutable std::deque<UiTrimmingEntry> ui_trimming_cache_;
    struct UiBrushEntry {
        D2D1_COLOR_F color{};
        ComPtr<ID2D1SolidColorBrush> brush;
    };
    ComPtr<ID2D1Device> ui_brush_device_;
    std::deque<UiBrushEntry> ui_brush_cache_;
    ComPtr<ID2D1Device> backdrop_device_;
    ComPtr<ID2D1BitmapBrush> backdrop_brush_;
    bool backdrop_dark_ = false;
    HRESULT CreateUiBrush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& color,
                          ID2D1SolidColorBrush** result);
    float UiTextWidth(std::wstring_view text, IDWriteTextFormat* format) const;
    void DrawUiText(ID2D1DeviceContext* dc, std::wstring_view text, IDWriteTextFormat* format,
                    const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    HeaderLayout header_;
    D2D1_COLOR_F accent_ = D2D1::ColorF(0x0078D4);
    std::wstring title_;       // PreviewTitle(item_)
    std::wstring type_name_;   // shell type name for the meta line
    std::wstring open_app_;    // default program's name; empty = none
    std::unordered_map<std::wstring, std::wstring> open_app_cache_;  // extension -> name, per Show
    std::vector<std::wstring> footer_parts_;   // this frame's facts (left)
    struct FooterControl { int id = 0; std::wstring text; D2D1_RECT_F rect{}; bool on = false; };
    std::vector<FooterControl> footer_controls_;   // this frame's controls (right)
    std::wstring footer_hint_;                     // this frame's keyboard hint (right, passive)
    int footer_hover_ = 0;
    int footer_focus_ = 0;   // keyboard focus (Tab) on a footer control; 0 none
    // Tab / Shift+Tab through this frame's footer controls; false when there are none.
    bool CycleFooterFocus(bool backwards);
    ComPtr<IDWriteTextFormat> badge_format_, badge_small_format_, open_format_;
    OpenWithIconCache open_icons_;
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
    // Room for a few full-window pictures (a 4K window's frame is ~32 MB).
    NativePreviewView native_preview_;
    ThumbnailCache thumbnails_{160ull * 1024ull * 1024ull, 192};
    PreviewHandlerHost handler_;
    VideoPreview video_;
    AudioWaveform waveform_;
    D2D1_RECT_F audio_wave_rect_{};
    float audio_hover_x_ = -1.0f;                // pointer x over the waveform, or -1
    bool playback_volume_drag_ = false;
    bool playback_drag_ = false;
    bool playback_resume_ = false;
    bool playback_scrub_pending_ = false;
    bool playback_seek_dirty_ = false;
    double playback_scrub_fraction_ = 0;
    ULONGLONG playback_last_seek_ms_ = 0;
    float playback_hover_x_ = -1.0f;   // pointer x over the seek track, or -1
    int playback_hover_button_ = -1;   // 0..2 transport, 3 volume, 4 speed, 5 audio, 6 subtitles
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
    ComPtr<IDWriteTextFormat> reading_text_format_;  // .txt reading mode: UI font, 1.75 line height
    ComPtr<IDWriteTextLayout> text_layout_;
    ComPtr<ID2D1CommandList> text_commands_;
    ComPtr<ID2D1Device> text_commands_device_;
    ComPtr<IDWriteRenderingParams> text_commands_params_;
    D2D1_COLOR_F text_commands_color_{};
    D2D1_POINT_2F text_commands_origin_{};
    float text_commands_dpi_x_ = 0.0f;
    float text_commands_dpi_y_ = 0.0f;
    D2D1_TEXT_ANTIALIAS_MODE text_commands_aa_ = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT;
    PlainHighlightLines plain_highlight_lines_;
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
    double last_render_ms_ = 0.0;
    double last_text_ms_ = 0.0;    // of which DrawTextPreview  // CPU time spent building the last frame
    float pan_x_ = 0.0f;
    float pan_y_ = 0.0f;
    bool image_fit_ = true;
    float image_zoom_ = 1.0f;
    uint32_t preview_pixels_ = 0;
    bool image_item_ = false;            // the item's extension is a picture (RefreshItemFacts)
    int image_turns_ = 0;                // quarter turns clockwise (R); reset per item
    // The decoded picture's size as shown, i.e. after image_turns_.
    float TurnedW() const noexcept { return static_cast<float>((image_turns_ & 1) ? decoded_h_ : decoded_w_); }
    float TurnedH() const noexcept { return static_cast<float>((image_turns_ & 1) ? decoded_w_ : decoded_h_); }
    // Video: the transport bar fades out after the pointer rests for 2 s.
    uint64_t pointer_tick_ = 0;
    POINT pointer_last_{};
    float PlaybackBarOpacity();
    // Shared by every Quick Look window of this session.
    static inline int image_backdrop_ = 0;   // 0 auto (checkerboard when translucent), 1 light, 2 dark, 3 checkerboard
    static inline bool info_open_ = false;
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
    enum class ToggleKind { Markdown, TableSource, TableHandler, TreeSource, NotebookSource, DocHandler,
                            TextReading };
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
    // Prev / next slide (motion phase 6): direction asked for, and the slide
    // of the item that answered it.
    int pending_nav_dir_ = 0;
    uint64_t pending_nav_at_ = 0;   // motion::NowMs
    int nav_dir_ = 0;
    uint64_t nav_start_ = 0;        // motion::NowMs
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
    PreviewTextIdentity preview_text_identity_;
    float text_layout_width_ = 0.0f;
    float text_layout_scale_ = 0.0f;
    bool text_layout_hex_ = false;
    bool text_layout_reading_ = false;
    float text_reading_x_ = 0.0f;   // left edge of the centred reading column
    bool text_mono_txt_ = false;    // session-wide: .txt shown monospaced instead of reading
    bool text_mono_other_ = true;   // session-wide: other plain text (.log, no extension)
    int reading_width_ = 1;         // session-wide: 0 narrow, 1 medium, 2 wide column
    int hex_hover_ = -1;            // byte under the pointer in the hex view
    int hex_selected_ = -1;         // byte last clicked in the hex view
    int HexFocus() const noexcept { return hex_hover_ >= 0 ? hex_hover_ : hex_selected_; }
    float HexRulerHeight() const noexcept { return text_layout_hex_ ? 22.0f * scale_ : 0.0f; }
    int HexByteAt(uint32_t index) const noexcept;
    void DrawHexRuler(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, float origin_x);
    bool toc_hidden_ = false;       // session-wide: contents sidebar hidden
    // Syntax colouring + line-number gutter (syntax_highlight.h).
    bool text_layout_dark_ = false;
    bool line_numbers_ = true;          // context-menu toggle, per session
    float text_gutter_ = 0.0f;          // pixels reserved left of the text
    uint32_t text_line_count_ = 0;
    uint8_t text_eol_ = 0;   // 0 none, 1 LF, 2 CRLF, 3 CR, 4 mixed
    std::vector<uint32_t> line_starts_; // filled only while the gutter is shown
    std::wstring syntax_language_;
    ComPtr<IDWriteTextFormat> gutter_format_;
    ComPtr<ID2D1SolidColorBrush> syntax_brushes_[16];
    float TextOriginX() const noexcept;
    // Language | encoding | lines and size, as footer facts.
    // show_eol: only raw text keeps its line endings (structured payloads drop CR).
    void SetTextFooter(bool hex, uint32_t bytes_read, bool truncated, uint32_t encoding,
                       bool show_eol = false);
    void SetTableFooter(uint32_t encoding);
    // Format, picture size (video), duration, extra facts, file size.
    void SetMediaFooter(const VideoPreview::State& state, std::vector<std::wstring> extra);
    void DrawLineNumbers(ID2D1DeviceContext* dc, const D2D1_RECT_F& content, float origin_y);
    uint32_t sel_anchor_ = 0;
    uint32_t sel_focus_ = 0;
    bool selecting_ = false;
    bool find_open_ = false;
    std::wstring find_query_;
    std::wstring search_query_;
    bool pending_search_reveal_ = false;
    std::vector<uint32_t> find_matches_;
    uint32_t find_index_ = 0;
    HWND find_edit_ = nullptr;
    HFONT find_edit_font_ = nullptr;
    HBRUSH find_edit_brush_ = nullptr;
    FluentMenu text_menu_;
    fluent::Painter find_painter_;
};

} // namespace pulse::ui
