#include "quick_preview_window.h"
#include "playback_timeline.h"
#include "typography.h"
#include "ffmpeg_playback.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/image_pack_protocol.h"
#include "../common/preview_extensions.h"
#include <d2d1helper.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kVideoTimer = 8;
constexpr UINT_PTR kScrubTimer = 9;
constexpr ULONGLONG kSeekIntervalMs = 80;
constexpr float kPlaybackHeight = 76.0f;
constexpr int kEnableTrackPlayback = 0x40000000;
// Rollback switch: false restores the text-button bar and full-bleed video.
constexpr bool kVideoChrome = true;
constexpr float kChromeBarHeight = 56.0f;
constexpr float kWidePlaybackWidth = 900.0f;
// Rollback switch: false leaves an undecodable video track as a black frame.
constexpr bool kCodecHint = true;
constexpr float kRates[] = {1.0f, 1.25f, 1.5f, 2.0f, 0.5f};
const wchar_t* Label(const wchar_t* chinese, const wchar_t* english) {
    return l10n::Pick(chinese, english);
}
bool Contains(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}
std::wstring Clock(int64_t ticks) {
    const int64_t ms = (std::max)(int64_t{0}, ticks / 10000);
    wchar_t value[64];
    swprintf_s(value, L"%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return value;
}
// m:ss, or h:mm:ss when the media is an hour or longer.
std::wstring ShortClock(int64_t ticks, bool hours) {
    const int64_t total = (std::max)(int64_t{0}, ticks / 10000000);
    wchar_t value[32];
    if (hours) swprintf_s(value, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else swprintf_s(value, L"%lld:%02lld", total / 60, total % 60);
    return value;
}
std::wstring RateLabel(float rate) {
    wchar_t value[16];
    swprintf_s(value, L"%g\x00D7", static_cast<double>(rate));
    return value;
}
std::wstring ResolutionLabel(uint32_t width, uint32_t height) {
    const uint32_t lines = (std::min)(width, height);
    if (lines >= 2160) return L"4K";
    if (lines >= 1440) return L"1440p";
    if (lines >= 1080) return L"1080p";
    if (lines >= 720) return L"720p";
    if (!width || !height) return {};
    return std::to_wstring(width) + L"\x00D7" + std::to_wstring(height);
}
// Icons are outlines in a 16-unit box centred in `box`, so they do not depend
// on the installed icon font.
struct IconPen {
    ID2D1DeviceContext* dc;
    ID2D1Factory* factory;
    D2D1_RECT_F box;
    float unit;
    D2D1_POINT_2F P(float x, float y) const {
        const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f;
        return D2D1::Point2F(cx + (x - 8.0f) * unit, cy + (y - 8.0f) * unit);
    }
    void Poly(std::initializer_list<D2D1_POINT_2F> points, ID2D1Brush* brush) const {
        if (!factory || points.size() < 3) return;
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory->CreatePathGeometry(&path)) || !path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink.get()) return;
        auto it = points.begin();
        sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
        for (++it; it != points.end(); ++it) sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) dc->FillGeometry(path.get(), brush);
    }
    void Bar(float l, float t, float r, float b, ID2D1Brush* brush) const {
        const auto a = P(l, t), c = P(r, b);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a.x, a.y, c.x, c.y), unit, unit), brush);
    }
    void Arc(float x0, float y0, float x1, float y1, float radius, ID2D1Brush* brush) const {
        if (!factory) return;
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory->CreatePathGeometry(&path)) || !path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink)) || !sink.get()) return;
        sink->BeginFigure(P(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(P(x1, y1), D2D1::SizeF(radius * unit, radius * unit), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        if (SUCCEEDED(sink->Close())) dc->DrawGeometry(path.get(), brush, 1.35f * unit);
    }
    void Line(float x0, float y0, float x1, float y1, ID2D1Brush* brush) const {
        dc->DrawLine(P(x0, y0), P(x1, y1), brush, 1.35f * unit);
    }
};
}

bool QuickPreviewWindow::HasPlayback() const noexcept {
    return frame_count_ > 1 || video_.active();
}
float QuickPreviewWindow::PlaybackHeight() const noexcept {
    if (embedded_ && HasPlayback() && kVideoChrome) return (std::min)((video_.active() ? 80.0f : 40.0f) * scale_, (std::max)(0.0f, static_cast<float>(compositor_.Height()) - FooterHeight()));
    return HasPlayback() ? (kVideoChrome ? (video_.active() && compositor_.Width() < kWidePlaybackWidth * scale_ ? 96.0f : kChromeBarHeight) : kPlaybackHeight) * scale_ : 0.0f;
}
D2D1_RECT_F QuickPreviewWindow::PlaybackRect() const {
    // Directly above the footer.
    const float bottom = static_cast<float>(compositor_.Height()) - FooterHeight();
    return D2D1::RectF(0, bottom - PlaybackHeight(),
        static_cast<float>(compositor_.Width()), bottom);
}
QuickPreviewWindow::PlaybackGeometry QuickPreviewWindow::PlaybackLayout() const {
    PlaybackGeometry g;
    const auto bar = PlaybackRect();
    const float s = scale_;
    const float cy = bar.top + 28.0f * s;
    const bool video = video_.active();
    g.button_count = video ? 2 : 3;
    g.play_index = video ? 0 : 1;
    float x = bar.left + 14.0f * s;
    for (int i = 0; i < g.button_count; ++i) {
        const float size = (i == g.play_index ? 40.0f : 36.0f) * s;
        g.buttons[i] = D2D1::RectF(x, cy - size * 0.5f, x + size, cy + size * 0.5f);
        x += size + 6.0f * s;
    }
    float right = bar.right - 14.0f * s;
    const auto slot = [&](float width) {
        const D2D1_RECT_F rect = D2D1::RectF(right - width, cy - 18.0f * s, right, cy + 18.0f * s);
        right -= width + 4.0f * s;
        return rect;
    };
    g.chip = slot(70.0f * s);
    if (video && bar.right >= kWidePlaybackWidth * s) {
        g.subtitles = slot(80.0f * s);
        g.audio_tracks = slot(90.0f * s);
        g.speed = slot(42.0f * s);
        g.volume_value = slot(44.0f * s);
        g.volume_track = slot(72.0f * s);
        g.volume = slot(36.0f * s);
    } else if (video) {
        const float y = bar.top + 72.0f * s;
        g.volume = D2D1::RectF(14 * s, y - 18 * s, 50 * s, y + 18 * s);
        g.volume_track = D2D1::RectF(58 * s, y - 14 * s, 130 * s, y + 14 * s);
        g.volume_value = D2D1::RectF(138 * s, y - 14 * s, 182 * s, y + 14 * s);
        g.speed = D2D1::RectF(188 * s, y - 18 * s, 230 * s, y + 18 * s);
        const float tracks_width = (std::max)(0.0f, (bar.right - 250 * s) * 0.5f);
        g.audio_tracks = D2D1::RectF(238 * s, y - 18 * s, 238 * s + tracks_width, y + 18 * s);
        g.subtitles = D2D1::RectF(g.audio_tracks.right + 4 * s, y - 18 * s, bar.right - 8 * s, y + 18 * s);
    }
    if (embedded_) {
        // Sidebar uses two compact rows; actions and hit testing stay shared.
        g = {};
        g.button_count = video ? 2 : 3;
        g.play_index = video ? 0 : 1;
        const float edge = 8*s, end = bar.right - edge;
        float left = edge;
        for (int i = 0; i < g.button_count; ++i) {
            const float size = (i == g.play_index ? 28.0f : 24.0f)*s;
            g.buttons[i] = D2D1::RectF(left, bar.top + (40*s-size)*0.5f,
                                      left+size, bar.top+(40*s+size)*0.5f);
            left += size + 2*s;
        }
        g.time_left = D2D1::RectF(left+2*s, bar.top+8*s, left+42*s, bar.top+32*s);
        g.time_right = D2D1::RectF(end-40*s, bar.top+8*s, end, bar.top+32*s);
        g.track = D2D1::RectF(left+46*s, bar.top+8*s, end-44*s, bar.top+32*s);
        if (video && bar.bottom-bar.top >= 80*s) {
            const float top = bar.top+44*s, bottom = bar.top+72*s;
            g.volume = D2D1::RectF(edge, top, edge+24*s, bottom);
            g.volume_track = D2D1::RectF(edge+30*s, top, end-188*s, bottom);
            g.speed = D2D1::RectF(end-180*s, top, end-150*s, bottom);
            g.audio_tracks = D2D1::RectF(end-146*s, top, end-68*s, bottom);
            g.subtitles = D2D1::RectF(end-64*s, top, end, bottom);
        }
        const auto fit = [&](D2D1_RECT_F& rect) {
            if (rect.left < 0 || rect.top < 0 || rect.right > bar.right ||
                rect.bottom > bar.bottom || rect.right <= rect.left) rect = {};
        };
        for (int i = 0; i < g.button_count; ++i) fit(g.buttons[i]);
        fit(g.time_left); fit(g.time_right); fit(g.track); fit(g.volume);
        fit(g.volume_track); fit(g.speed); fit(g.audio_tracks); fit(g.subtitles);
        return g;
    }
    const bool hours = video && video_.Snapshot().duration >= int64_t{36000000000};
    const float time_width = (hours ? 56.0f : 44.0f) * s;
    g.time_right = D2D1::RectF(right - time_width, cy - 12.0f * s, right, cy + 12.0f * s);
    right -= time_width + 4.0f * s;
    x += 4.0f * s;
    g.time_left = D2D1::RectF(x, cy - 12.0f * s, x + time_width, cy + 12.0f * s);
    x += time_width + 10.0f * s;
    g.track = D2D1::RectF(x, cy - 12.0f * s, (std::max)(x + s, right - 6.0f * s), cy + 12.0f * s);
    return g;
}
D2D1_RECT_F QuickPreviewWindow::VideoFrameRect(const VideoPreview::State& state) const {
    const auto content = ContentRect();
    if (!kVideoChrome) return content;
    const float s = scale_;
    // Side/top margins frame the picture; the bottom one leaves room for the
    // seek bar's time bubble, which cannot draw over the video window.
    const D2D1_RECT_F box = D2D1::RectF(content.left + (embedded_ ? 4.0f : 16.0f) * s, content.top + (embedded_ ? 4.0f : 12.0f) * s,
                                        content.right - (embedded_ ? 4.0f : 16.0f) * s, content.bottom - (embedded_ ? 4.0f : 28.0f) * s);
    const float bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw < 48.0f * s || bh < 48.0f * s) return content;
    if (!state.width || !state.height) return box;
    const float k = (std::min)(bw / static_cast<float>(state.width), bh / static_cast<float>(state.height));
    const float w = static_cast<float>(state.width) * k, h = static_cast<float>(state.height) * k;
    const float cx = (box.left + box.right) * 0.5f, cy = (box.top + box.bottom) * 0.5f;
    return D2D1::RectF(std::round(cx - w * 0.5f), std::round(cy - h * 0.5f),
                       std::round(cx + w * 0.5f), std::round(cy + h * 0.5f));
}
void QuickPreviewWindow::LayoutVideo(ID2D1DeviceContext* dc, const VideoPreview::State& state,
                                     bool show) {
    const D2D1_RECT_F frame = VideoFrameRect(state);
    const RECT bounds{std::lround(frame.left), std::lround(frame.top), std::lround(frame.right),
                      std::lround(frame.bottom)};
    const int radius = kVideoChrome ? static_cast<int>(std::lround(10.0f * scale_)) : 0;
    if (kVideoChrome && show && dc) {
        ComPtr<ID2D1SolidColorBrush> shadow;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, dark_ ? 0.07f : 0.035f), &shadow);
        if (shadow.get()) {
            for (int i = 6; i >= 1; --i) {
                const float grow = static_cast<float>(i) * 2.0f * scale_;
                const D2D1_RECT_F r = D2D1::RectF(frame.left - grow, frame.top - grow + 3.0f * scale_,
                                                  frame.right + grow, frame.bottom + grow + 3.0f * scale_);
                const float rr = static_cast<float>(radius) + grow;
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), shadow.get());
            }
        }
    }
    video_.Layout(bounds, show && !state.ffmpeg, radius);
    if (show && state.ffmpeg && dc) DrawFfmpegFrame(dc, frame, static_cast<float>(radius));
}
void QuickPreviewWindow::DrawFfmpegFrame(ID2D1DeviceContext* dc, const D2D1_RECT_F& frame, float radius) {
    uint64_t serial = 0;
    const auto picture = video_.Frame(serial);
    if (!picture || !picture->width || !picture->height) return;
    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    const D2D1_SIZE_U size = D2D1::SizeU(picture->width, picture->height);
    bool upload = serial != ffmpeg_serial_;
    if (!ffmpeg_bitmap_.get() || ffmpeg_device_.get() != device.get() ||
        ffmpeg_bitmap_->GetPixelSize().width != size.width || ffmpeg_bitmap_->GetPixelSize().height != size.height) {
        ffmpeg_bitmap_.reset();
        ffmpeg_brush_.reset();
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &ffmpeg_bitmap_))) return;
        ffmpeg_device_ = std::move(device);
        upload = true;
    }
    if (upload) {
        ffmpeg_bitmap_->CopyFromMemory(nullptr, picture->pixels.data(), picture->width * 4);
        ffmpeg_serial_ = serial;
    }
    if (!ffmpeg_brush_.get() && FAILED(dc->CreateBitmapBrush(ffmpeg_bitmap_.get(), &ffmpeg_brush_))) return;
    // The frame was scaled to the viewport by FFmpeg; this only fits it.
    const float sx = (frame.right - frame.left) / static_cast<float>(size.width);
    const float sy = (frame.bottom - frame.top) / static_cast<float>(size.height);
    ffmpeg_brush_->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    ffmpeg_brush_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(frame.left, frame.top));
    if (radius > 0.0f) dc->FillRoundedRectangle(D2D1::RoundedRect(frame, radius, radius), ffmpeg_brush_.get());
    else dc->FillRectangle(frame, ffmpeg_brush_.get());
}
bool QuickPreviewWindow::PlaybackHover(POINT point) {
    float hover_x = -1.0f;
    int button = -1;
    if (kVideoChrome && HasPlayback() && Contains(PlaybackRect(), point)) {
        const auto g = PlaybackLayout();
        for (int i = 0; i < g.button_count; ++i)
            if (Contains(g.buttons[i], point)) button = i;
        if (Contains(g.volume, point) || Contains(g.volume_track, point)) button = 3;
        if (Contains(g.audio_tracks, point)) button = 5;
        if (Contains(g.subtitles, point)) button = 6;
        if (Contains(g.speed, point)) button = 4;
        if (Contains(g.track, point)) hover_x = static_cast<float>(point.x);
    }
    const bool changed = button != playback_hover_button_ ||
        (hover_x < 0) != (playback_hover_x_ < 0) || std::abs(hover_x - playback_hover_x_) >= 1.0f;
    playback_hover_x_ = hover_x;
    playback_hover_button_ = button;
    return AudioHover(point) || changed;
}
void QuickPreviewWindow::SetPlaybackVolume(float value) {
    playback_volume_ = std::clamp(value, 0.0f, 1.0f);
    playback_muted_ = false;
    video_.SetMuted(false);
    video_.SetVolume(playback_volume_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::DragPlaybackVolume(float x) {
    const auto track = PlaybackLayout().volume_track;
    SetPlaybackVolume(static_cast<float>(playback::Fraction(x, track.left, track.right)));
}
bool QuickPreviewWindow::PlaybackWheel(POINT point, float steps) {
    if (!kVideoChrome || !video_.active()) return false;
    const auto g = PlaybackLayout();
    if (!Contains(g.volume, point) && !Contains(g.volume_track, point) &&
        !Contains(g.volume_value, point)) return false;
    SetPlaybackVolume(playback_volume_ + steps * 0.05f);
    return true;
}
std::vector<FluentMenuItem> QuickPreviewWindow::PlaybackTrackItems(
    bool subtitles, const VideoPreview::State& state) const {
    const auto& tracks = subtitles ? state.subtitle_tracks : state.audio_tracks;
    const bool available = state.ready && SUCCEEDED(state.error) &&
        (subtitles ? state.can_select_subtitle : state.can_select_audio);
    const int selected = subtitles ? state.subtitle_track : state.audio_track;
    std::vector<FluentMenuItem> items;
    if (!state.ffmpeg && (subtitles ? state.ffmpeg_subtitles_available : state.ffmpeg_available)) {
        FluentMenuItem item;
        item.command = kEnableTrackPlayback;
        item.text = l10n::Get(subtitles ? l10n::StringId::PreviewEnableSubtitles
                                        : l10n::StringId::PreviewEnableAudioTracks);
        item.enabled = state.ready && SUCCEEDED(state.error);
        item.separator_after = true;
        items.push_back(std::move(item));
    }
    if (!available) {
        FluentMenuItem item;
        item.text = l10n::Get(subtitles && !state.ffmpeg_subtitles_available
            ? (state.ffmpeg_available ? l10n::StringId::PreviewSubtitlePackUnsupported
                                      : l10n::StringId::PreviewSubtitlePackHint)
            : l10n::StringId::PreviewTrackUnavailable);
        item.enabled = false;
        items.push_back(std::move(item));
    }
    if (subtitles) {
        FluentMenuItem item;
        item.command = 1;
        item.text = l10n::Get(l10n::StringId::PreviewSubtitlesOff);
        item.enabled = available;
        item.radio_group = true;
        item.radio = selected < 0;
        items.push_back(std::move(item));
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        FluentMenuItem item;
        item.command = static_cast<int>(i) + 2;
        item.text = tracks[i].label;
        item.enabled = available && tracks[i].supported;
        item.radio_group = true;
        item.radio = selected == tracks[i].id;
        items.push_back(std::move(item));
    }
    return items;
}
void QuickPreviewWindow::ShowPlaybackTracks(bool subtitles, POINT point) {
    const auto state = video_.Snapshot();
    const auto& tracks = subtitles ? state.subtitle_tracks : state.audio_tracks;
    const bool available = state.ready && SUCCEEDED(state.error) &&
        (subtitles ? state.can_select_subtitle : state.can_select_audio);
    const uint64_t generation = generation_;
    auto items = PlaybackTrackItems(subtitles, state);
    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, accent_);
    text_menu_.SetTopmost(true);
    ClientToScreen(hwnd_, &point);
    const int command = text_menu_.TrackPopup(point, std::move(items));
    if (generation != generation_ || !video_.active()) return;
    if (command == kEnableTrackPlayback && !state.ffmpeg &&
        (subtitles ? state.ffmpeg_subtitles_available : state.ffmpeg_available)) {
        video_.EnableTrackPlayback();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!available) return;
    if (subtitles && command == 1) video_.SelectSubtitleTrack(-1);
    else if (command >= 2 && static_cast<size_t>(command - 2) < tracks.size()) {
        const auto& track = tracks[command - 2];
        if (track.supported) {
            if (subtitles) video_.SelectSubtitleTrack(track.id);
            else video_.SelectAudioTrack(track.id);
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
D2D1_RECT_F QuickPreviewWindow::PlaybackButtonRect(int button) const {
    if (kVideoChrome) {
        const auto g = PlaybackLayout();
        return g.buttons[std::clamp(button, 0, g.button_count - 1)];
    }
    const auto bar = PlaybackRect();
    const float x = (12.0f + 70.0f * static_cast<float>(button)) * scale_;
    return D2D1::RectF(x, bar.top + 6.0f * scale_, x + 66.0f * scale_,
        bar.top + 38.0f * scale_);
}
D2D1_RECT_F QuickPreviewWindow::PlaybackTrackRect() const {
    if (kVideoChrome) return PlaybackLayout().track;
    const auto bar = PlaybackRect();
    const float left = (video_.active() ? 164.0f : 234.0f) * scale_;
    return D2D1::RectF(left, bar.top + 6.0f * scale_,
        (std::max)(left + scale_, bar.right - 18.0f * scale_), bar.top + 38.0f * scale_);
}
void QuickPreviewWindow::BeginVideo() {
    if (safe_mode_ || OfflinePlaceholder() || (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        !VideoPreview::Supports(item_.path) || native_preview_.Available(owner_, item_.path)) return;
    handler_.Reset();
    video_.Open(hwnd_, item_.path);
    ffmpeg_noted_ = false;
    playback_rate_ = 1.0f;
    playback_note_until_ = 0;
    playback_hover_x_ = -1.0f;
    playback_hover_button_ = -1;
    video_.SetVolume(playback_volume_);
    video_.SetMuted(playback_muted_);
    if (VideoPreview::IsAudio(item_.path)) waveform_.Start(item_.path);
    SetTimer(hwnd_, kVideoTimer, 50, nullptr);
}
void QuickPreviewWindow::CancelPlaybackScrub() {
    const bool was_dragging = playback_drag_ || playback_volume_drag_;
    playback_volume_drag_ = false;
    playback_drag_ = false;
    playback_resume_ = false;
    playback_scrub_pending_ = false;
    playback_seek_dirty_ = false;
    playback_scrub_fraction_ = 0;
    playback_last_seek_ms_ = 0;
    if (hwnd_) {
        KillTimer(hwnd_, kScrubTimer);
        if (was_dragging && GetCapture() == hwnd_) ReleaseCapture();
    }
}
void QuickPreviewWindow::ResetPlayback() {
    CancelPlaybackScrub();
    if (hwnd_) {
        KillTimer(hwnd_, kVideoTimer);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    video_.Reset();
}
void QuickPreviewWindow::TogglePlayback() {
    if (playback_drag_) {
        EndPlaybackDrag(false);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (state.ready && SUCCEEDED(state.error)) video_.Play(!state.playing);
    } else if (frame_count_ > 1) {
        animation_active_ = !animation_active_;
        KillTimer(hwnd_, kAnimationTimer);
        if (animation_active_) {
            completed_loops_ = 0;
            if (!playback_scrub_pending_ && !waiting_for_frame_ && frame_index_ + 1 >= frame_count_) {
                requested_frame_ = 0;
                waiting_for_frame_ = true;
            }
            if (!playback_scrub_pending_ && !waiting_for_frame_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (waiting_for_frame_ && !playback_scrub_pending_) {
            // Pause means retain the frame actually on screen, not an outstanding auto tick.
            waiting_for_frame_ = false;
            requested_frame_ = frame_index_;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::StepPlayback(int direction) {
    const uint32_t base_frame = playback_scrub_pending_
        ? playback::FrameAt(playback_scrub_fraction_, frame_count_)
        : waiting_for_frame_ ? requested_frame_ : frame_index_;
    CancelPlaybackScrub();
    if (video_.active()) {
        if (direction > 0) video_.Step(); // MFPlay's exact frame step is forward-only.
    } else if (frame_count_ > 1) {
        animation_active_ = false;
        KillTimer(hwnd_, kAnimationTimer);
        requested_frame_ = playback::StepFrame(
            base_frame, direction, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SeekPlayback(float x) {
    const auto track = PlaybackTrackRect();
    // Pointer position is continuous, independent of both frame quantization and
    // asynchronous decoding. Moving the thumb never waits for a thumbnail.
    playback_scrub_fraction_ = playback::Fraction(x, track.left, track.right);
    playback_scrub_pending_ = true;
    playback_seek_dirty_ = true;
    SubmitPlaybackSeek(!playback_drag_);
    SetTimer(hwnd_, kScrubTimer, 16, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SubmitPlaybackSeek(bool immediate) {
    if (!playback_seek_dirty_) return;
    const auto now = GetTickCount64();
    if (!immediate && playback_last_seek_ms_ && now - playback_last_seek_ms_ < kSeekIntervalMs)
        return;
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (!state.ready || !state.can_seek || state.duration <= 0 || FAILED(state.error)) {
            playback_seek_dirty_ = false;
            return;
        }
        if (!immediate && state.busy) return;
        video_.Seek(playback_scrub_fraction_);
    } else if (frame_count_ > 1) {
        // Do not replace an in-flight frame with every mouse move. Keep only the
        // newest desired fraction, then submit it when this decode has finished.
        if (waiting_for_frame_) return;
        requested_frame_ = playback::FrameAt(playback_scrub_fraction_, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    playback_last_seek_ms_ = now;
    playback_seek_dirty_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::TickPlaybackSeek() {
    SubmitPlaybackSeek(!playback_drag_);
    const bool settled = video_.active() ? !playback_seek_dirty_
        : !playback_seek_dirty_ && !waiting_for_frame_;
    if (!playback_drag_ && settled) {
        playback_scrub_pending_ = false;
        KillTimer(hwnd_, kScrubTimer);
        if (!video_.active() && animation_active_ && frame_count_ > 1)
            SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
double QuickPreviewWindow::PlaybackFraction(const VideoPreview::State& state) const {
    if (playback_drag_ || playback_scrub_pending_) return playback_scrub_fraction_;
    if (video_.active() && state.duration > 0)
        return std::clamp(static_cast<double>(state.position) / state.duration, 0.0, 1.0);
    return frame_count_ > 1 ? static_cast<double>(frame_index_) / (frame_count_ - 1) : 0;
}
bool QuickPreviewWindow::PlaybackMouseDown(POINT point) {
    if (!HasPlayback() || !Contains(PlaybackRect(), point)) return false;
    SetFocus(hwnd_);
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    if (kVideoChrome && video) {
        const auto g = PlaybackLayout();
        if (Contains(g.volume_track, point)) {
            playback_volume_drag_ = true;
            SetCapture(hwnd_);
            DragPlaybackVolume(static_cast<float>(point.x));
            return true;
        }
        if (Contains(g.audio_tracks, point) || Contains(g.subtitles, point)) {
            ShowPlaybackTracks(Contains(g.subtitles, point), point);
            return true;
        }
        if (Contains(g.volume, point)) {
            playback_muted_ = !playback_muted_;
            video_.SetMuted(playback_muted_);
            playback_note_ = playback_muted_ ? std::wstring(Label(L"\x5DF2\x9759\x97F3", L"Muted"))
                : Label(L"\x97F3\x91CF ", L"Volume ") +
                  std::to_wstring(std::lround(playback_volume_ * 100.0f)) + L"%";
            playback_note_until_ = GetTickCount64() + 1200;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (Contains(g.speed, point)) {
            size_t next = 0;
            for (size_t i = 0; i < std::size(kRates); ++i)
                if (std::abs(kRates[i] - playback_rate_) < 0.01f) next = (i + 1) % std::size(kRates);
            playback_rate_ = kRates[next];
            video_.SetRate(playback_rate_);
            playback_note_ = Label(L"\x500D\x901F ", L"Speed ") + RateLabel(playback_rate_);
            playback_note_until_ = GetTickCount64() + 1200;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }
    if (video && (!state.ready || FAILED(state.error))) return true;
    const int play = video ? 0 : 1;
    const int next = video ? 1 : 2;
    if (Contains(PlaybackButtonRect(play), point)) TogglePlayback();
    else if (Contains(PlaybackButtonRect(next), point)) StepPlayback(1);
    else if (!video && Contains(PlaybackButtonRect(0), point)) StepPlayback(-1);
    else if (Contains(PlaybackTrackRect(), point) && (!video || state.can_seek)) {
        const bool was_playing = video ? state.playing : animation_active_;
        CancelPlaybackScrub();
        playback_resume_ = was_playing;
        playback_drag_ = true;
        if (video) video_.Play(false);
        else {
            animation_active_ = false;
            KillTimer(hwnd_, kAnimationTimer);
        }
        SetCapture(hwnd_);
        SeekPlayback(static_cast<float>(point.x));
    }
    return true;
}
void QuickPreviewWindow::EndPlaybackDrag(bool resume) {
    if (!playback_drag_) return;
    playback_drag_ = false;
    const bool play = resume && playback_resume_;
    playback_resume_ = false;
    SubmitPlaybackSeek(true); // release bypasses throttling, but not a GIF decode in flight
    if (video_.active()) video_.Play(play);
    else animation_active_ = play;
    TickPlaybackSeek();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

float QuickPreviewWindow::PlaybackBarOpacity() {
    if (embedded_) return 1.0f;
    // The pointer is polled: over the video it belongs to the player's window.
    const uint64_t now = GetTickCount64();
    POINT cursor{};
    RECT window{};
    if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &window) && PtInRect(&window, cursor) &&
        (cursor.x != pointer_last_.x || cursor.y != pointer_last_.y)) {
        pointer_last_ = cursor;
        pointer_tick_ = now;
    }
    const auto state = video_.Snapshot();
    if (!state.playing || playback_drag_ || playback_volume_drag_ || playback_hover_x_ >= 0.0f || playback_hover_button_ >= 0 ||
        VideoPreview::IsAudio(item_.path))
        pointer_tick_ = now;
    const uint64_t idle = now - pointer_tick_;
    if (idle <= 2000) return 1.0f;
    return (std::max)(0.0f, 1.0f - static_cast<float>(idle - 2000) / 300.0f);
}

void QuickPreviewWindow::DrawPlayback(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    if (!HasPlayback() || !dc || !brush) return;
    if (kVideoChrome) {
        const auto state = video_.Snapshot();
        const bool picture = video_.active() && !VideoPreview::IsAudio(item_.path) && state.ready &&
            SUCCEEDED(state.error) && !CodecCardVisible(state) && !Palette().high_contrast;
        if (!picture) {
            DrawPlaybackChrome(dc, brush);
            return;
        }
        // On the video's black: light ink whatever the theme, fading while the pointer rests.
        const float opacity = PlaybackBarOpacity();
        if (opacity <= 0.01f) return;
        const bool saved_dark = dark_;
        const D2D1_COLOR_F saved_color = brush->GetColor();
        dark_ = true;
        brush->SetColor(D2D1::ColorF(0xF4F4F4));
        const bool layer = opacity < 0.99f;
        if (layer)
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), opacity), nullptr);
        DrawPlaybackChrome(dc, brush);
        if (layer) dc->PopLayer();
        dark_ = saved_dark;
        brush->SetColor(saved_color);
        return;
    }
    const auto bar = PlaybackRect();
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    const auto original = brush->GetColor();
    auto color = original;
    color.a = 0.12f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(bar.left, bar.top), D2D1::Point2F(bar.right, bar.top), brush);
    auto* format = compositor_.SmallFormat();
    if (!format) { brush->SetColor(original); return; }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < (video ? 2 : 3); ++i) {
        const bool is_play = i == (video ? 0 : 1);
        const wchar_t* text = is_play ? (playing ? Label(L"暂停", L"Pause") : Label(L"播放", L"Play"))
            : i == 0 ? Label(L"上一帧", L"Prev frame") : Label(L"下一帧", L"Next frame");
        const bool available = enabled && (is_play || !video || !state.ended);
        const auto rect = PlaybackButtonRect(i);
        color = original; color.a = available ? 0.09f : 0.03f;
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4 * scale_, 4 * scale_), brush);
        color = original; color.a = available ? 1.0f : 0.4f;
        brush->SetColor(color);
        dc->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, rect, brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    const auto track = PlaybackTrackRect();
    const float y = (track.top + track.bottom) * 0.5f;
    const double fraction = PlaybackFraction(state);
    color = original; color.a = enabled && (!video || state.can_seek) ? 0.3f : 0.1f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(track.right, y), brush, 3 * scale_);
    const float thumb = track.left + static_cast<float>(fraction) * (track.right - track.left);
    brush->SetColor(original);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(thumb, y), brush, 3 * scale_);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y), 5 * scale_, 5 * scale_), brush);
    const std::wstring info = PlaybackInfo(state);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    dc->DrawTextW(info.data(), static_cast<UINT32>(info.size()), format,
        D2D1::RectF(12 * scale_, bar.top + 40 * scale_, bar.right - 12 * scale_, bar.bottom - 8 * scale_),
        brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    brush->SetColor(original);
}
std::wstring QuickPreviewWindow::PlaybackInfo(const VideoPreview::State& state) const {
    const bool video = video_.active();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    std::wstring info;
    if (video) {
        info = Clock(state.position) + L" / " + Clock(state.duration);
        if (state.width && state.height)
            info += L" · " + std::to_wstring(state.width) + L"×" + std::to_wstring(state.height);
        if (state.stepped_frames)
            info += Label(L" · 逐帧 +", L" · Stepped +") + std::to_wstring(state.stepped_frames);
        if (FAILED(state.error)) info += Label(L" · 无法播放，请用默认应用打开", L" · Cannot play; open externally");
        else if (FAILED(state.control_error)) info += Label(L" · 此媒体不支持该操作", L" · Control unavailable for this media");
        else if (!state.ready) info += Label(L" · 正在加载", L" · Loading");
        else if (playback_drag_) info += Label(L" · 拖动定位", L" · Scrubbing");
        else if (state.busy) info += Label(L" · 正在定位", L" · Seeking/stepping");
        else if (state.ended) info += Label(L" · 已结束", L" · Ended");
    } else {
        info = Label(L"帧 ", L"Frame ") + std::to_wstring(frame_index_ + 1) + L" / " +
            std::to_wstring(frame_count_) + L" · " + std::to_wstring(frame_delay_ms_) + L" ms";
        // Waiting is normal between every pair of GIF frames. Never insert a
        // transient "Seeking" label here: it shifts/flashes the entire suffix.
        if (playback_drag_) info += Label(L" · 拖动定位", L" · Scrubbing");
    }
    if (enabled) info += playing ? Label(L" · 播放中", L" · Playing") : Label(L" · 已暂停", L" · Paused");
    return info;
}
void QuickPreviewWindow::DrawPlaybackChrome(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    auto* format = compositor_.SmallFormat();
    if (!format) return;
    const auto g = PlaybackLayout();
    const auto bar = PlaybackRect();
    const float s = scale_;
    const bool video = video_.active();
    const bool audio = video && VideoPreview::IsAudio(item_.path);
    const auto state = video_.Snapshot();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    const bool seekable = enabled && (!video || state.can_seek);
    const auto original = brush->GetColor();
    const auto tint = [&](float alpha) {
        auto color = original;
        color.a = original.a * alpha;
        brush->SetColor(color);
        return brush;
    };
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1SolidColorBrush> ink, white, shade;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x111111) : D2D1::ColorF(0xFFFFFF), &ink);
    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &white);
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.35f), &shade);
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const auto text = [&](const std::wstring& value, const D2D1_RECT_F& rect, ID2D1Brush* b) {
        dc->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format, rect, b,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto hover_fill = [&](const D2D1_RECT_F& rect, int id) {
        if (playback_hover_button_ != id) return;
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f * s, 6.0f * s), tint(dark_ ? 0.08f : 0.06f));
    };

    // Transport buttons: a filled round play/pause plus frame steps.
    for (int i = 0; i < g.button_count; ++i) {
        const auto rect = g.buttons[i];
        if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
        if (i == g.play_index) {
            const float r = (rect.right - rect.left) * 0.5f;
            const auto centre = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
            dc->FillEllipse(D2D1::Ellipse(centre, r, r),
                            tint(enabled ? (playback_hover_button_ == i ? 0.86f : 1.0f) : 0.35f));
            if (!ink.get()) continue;
            const IconPen pen{dc, factory.get(), rect, 14.0f * s / 16.0f};
            if (playing) {
                pen.Bar(3.4f, 2.2f, 6.9f, 13.8f, ink.get());
                pen.Bar(9.1f, 2.2f, 12.6f, 13.8f, ink.get());
            } else {
                pen.Poly({pen.P(4.6f, 2.0f), pen.P(4.6f, 14.0f), pen.P(14.0f, 8.0f)}, ink.get());
            }
            continue;
        }
        hover_fill(rect, i);
        const bool forward = i > g.play_index;
        const bool available = enabled && (!video || (forward && !state.ended && !CodecCardVisible(state)));
        const IconPen pen{dc, factory.get(), rect, s};
        ID2D1Brush* b = tint(available ? 0.95f : 0.35f);
        if (forward) {
            pen.Poly({pen.P(3.0f, 2.5f), pen.P(3.0f, 13.5f), pen.P(10.5f, 8.0f)}, b);
            pen.Bar(11.5f, 2.5f, 13.5f, 13.5f, b);
        } else {
            pen.Poly({pen.P(13.0f, 2.5f), pen.P(13.0f, 13.5f), pen.P(5.5f, 8.0f)}, b);
            pen.Bar(2.5f, 2.5f, 4.5f, 13.5f, b);
        }
    }

    // Times either side of the track.
    const double fraction = PlaybackFraction(state);
    const bool hours = video && state.duration >= int64_t{36000000000};
    std::wstring left, right;
    if (video) {
        const int64_t shown = (playback_drag_ || playback_scrub_pending_)
            ? static_cast<int64_t>(fraction * static_cast<double>(state.duration)) : state.position;
        left = ShortClock(shown, hours);
        right = ShortClock(state.duration, hours);
    } else {
        const uint32_t frame = (playback_drag_ || playback_scrub_pending_)
            ? playback::FrameAt(playback_scrub_fraction_, frame_count_) : frame_index_;
        left = std::to_wstring(frame + 1);
        right = std::to_wstring(frame_count_);
    }
    text(left, g.time_left, tint(enabled ? 0.85f : 0.4f));
    text(right, g.time_right, tint(enabled ? 0.85f : 0.4f));

    // Seek track: thicker under the pointer or while dragging.
    const auto track = g.track;
    const float y = (track.top + track.bottom) * 0.5f;
    const bool has_track = track.right > track.left && track.bottom > track.top;
    const bool active = has_track && seekable && (playback_hover_x_ >= 0.0f || playback_drag_);
    if (has_track) {
        const float thick = (active ? 6.0f : 4.0f) * s;
        const auto rail = D2D1::RectF(track.left, y - thick * 0.5f, track.right, y + thick * 0.5f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rail, thick * 0.5f, thick * 0.5f),
                                 tint(seekable ? (dark_ ? 0.18f : 0.14f) : 0.07f));
        const float thumb = track.left + static_cast<float>(fraction) * (track.right - track.left);
        if (thumb > track.left + 0.5f) {
            const auto filled = D2D1::RectF(track.left, rail.top, thumb, rail.bottom);
            dc->FillRoundedRectangle(D2D1::RoundedRect(filled, thick * 0.5f, thick * 0.5f),
                                     tint(enabled ? 1.0f : 0.35f));
        }
        if (active && !playback_drag_ && playback_hover_x_ >= 0.0f) {
            const float gx = std::clamp(playback_hover_x_, track.left, track.right);
            dc->DrawLine(D2D1::Point2F(gx, y - 6.0f * s), D2D1::Point2F(gx, y + 6.0f * s),
                         tint(0.6f), 2.0f * s);
        }
        if (enabled && white.get() && shade.get()) {
            const float r = (playback_drag_ ? 8.0f : 7.0f) * s;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y + 1.0f * s), r + 1.0f * s, r + 1.0f * s), shade.get());
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y), r, r), white.get());
        }

    }

    // Volume and speed (video / audio only).
    if (video && g.volume.right > g.volume.left) {
        hover_fill(g.volume, 3);
        const IconPen pen{dc, factory.get(), g.volume, s};
        ID2D1Brush* b = tint(0.95f);
        pen.Poly({pen.P(1.5f, 6.0f), pen.P(4.2f, 6.0f), pen.P(7.8f, 2.8f), pen.P(7.8f, 13.2f),
                  pen.P(4.2f, 10.0f), pen.P(1.5f, 10.0f)}, b);
        if (playback_muted_ || playback_volume_ <= 0.0f) {
            pen.Line(10.2f, 5.8f, 14.4f, 10.2f, b);
            pen.Line(14.4f, 5.8f, 10.2f, 10.2f, b);
        } else {
            pen.Arc(10.3f, 5.3f, 10.3f, 10.7f, 3.9f, b);
            if (playback_volume_ > 0.5f) pen.Arc(12.1f, 3.4f, 12.1f, 12.6f, 6.4f, b);
        }
        const float volume_y = (g.volume_track.top + g.volume_track.bottom) * 0.5f;
        const float volume_x = g.volume_track.left + playback_volume_ * (g.volume_track.right - g.volume_track.left);
        dc->DrawLine(D2D1::Point2F(g.volume_track.left, volume_y), D2D1::Point2F(g.volume_track.right, volume_y), tint(0.2f), 4 * s);
        dc->DrawLine(D2D1::Point2F(g.volume_track.left, volume_y), D2D1::Point2F(volume_x, volume_y), tint(playback_muted_ ? 0.35f : 1.0f), 4 * s);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(volume_x, volume_y), 5 * s, 5 * s), tint(1.0f));
        text(std::to_wstring(std::lround(playback_volume_ * 100)) + L"%", g.volume_value, tint(0.85f));
        const auto track_button = [&](bool subtitle, const D2D1_RECT_F& rect, int id) {
            const bool selectable = enabled && (subtitle
                ? (state.can_select_subtitle || (!state.ffmpeg && state.ffmpeg_subtitles_available))
                : (state.can_select_audio || (!state.ffmpeg && state.ffmpeg_available)));
            hover_fill(rect, id);
            DrawUiText(dc, l10n::Get(subtitle ? l10n::StringId::PreviewSubtitles : l10n::StringId::PreviewAudioTracks),
                format, rect, tint(selectable ? 0.95f : 0.35f)->GetColor(), DWRITE_TEXT_ALIGNMENT_CENTER);
        };
        track_button(false, g.audio_tracks, 5);
        track_button(true, g.subtitles, 6);
        hover_fill(g.speed, 4);
        text(RateLabel(playback_rate_), g.speed, tint(std::abs(playback_rate_ - 1.0f) < 0.01f ? 0.9f : 1.0f));
    }

    // Chip: a transient note, otherwise state / resolution / exact time.
    if (video && state.ffmpeg && state.ready && !ffmpeg_noted_) {
        // Say once which engine plays it; the chip has no room to keep it.
        ffmpeg_noted_ = true;
        playback_note_ = L"FFmpeg";
        playback_note_until_ = GetTickCount64() + 2500;
    }
    std::wstring chip;
    if (GetTickCount64() < playback_note_until_) chip = playback_note_;
    else if (video && CodecCardVisible(state)) chip = state.codec;
    else if (video && state.opening) chip = l10n::Get(l10n::StringId::PreviewLoading);
    else if (video && FAILED(state.error)) chip = Label(L"\x65E0\x6CD5\x64AD\x653E", L"Can't play");
    else if (video && FAILED(state.control_error)) chip = Label(L"\x64CD\x4F5C\x4E0D\x53EF\x7528", L"Unavailable");
    else if (video && !state.ready) chip = Label(L"\x52A0\x8F7D\x4E2D", L"Loading");
    else if (video && !audio && (playback_drag_ || (!state.playing && state.stepped_frames)))
        chip = Clock(playback_drag_ ? static_cast<int64_t>(fraction * static_cast<double>(state.duration))
                                    : state.position);
    else if (video && !audio) chip = ResolutionLabel(state.width, state.height);
    else if (!video) chip = std::to_wstring(frame_delay_ms_) + L" ms";
    if (!chip.empty() && g.chip.right > g.chip.left && g.chip.bottom > g.chip.top) {
        const float h = 22.0f * s;
        const float cy = (g.chip.top + g.chip.bottom) * 0.5f;
        const auto pill = D2D1::RectF(g.chip.left, cy - h * 0.5f, g.chip.right, cy + h * 0.5f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, h * 0.5f, h * 0.5f), tint(dark_ ? 0.08f : 0.06f));
        text(chip, pill, tint(0.9f));
    }

    // Time bubble above the pointer on the track.
    if (active && !playback_drag_ && playback_hover_x_ >= 0.0f && compositor_.DwriteFactory()) {
        const double at = playback::Fraction(playback_hover_x_, track.left, track.right);
        const std::wstring tip = video
            ? ShortClock(static_cast<int64_t>(at * static_cast<double>(state.duration)), hours)
            : std::to_wstring(playback::FrameAt(at, frame_count_) + 1);
        const float tw = typography::MeasureAdvance(compositor_.DwriteFactory(), format, tip) + 16.0f * s;
        const float th = 22.0f * s;
        const float cx = std::clamp(playback_hover_x_, bar.left + tw * 0.5f + 4.0f * s,
                                    bar.right - tw * 0.5f - 4.0f * s);
        const auto bubble = D2D1::RectF(cx - tw * 0.5f, bar.top - th - 4.0f * s,
                                        cx + tw * 0.5f, bar.top - 4.0f * s);
        ComPtr<ID2D1SolidColorBrush> fill;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x2B2B2B, 0.96f), &fill);
        if (fill.get() && white.get()) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(bubble, 5.0f * s, 5.0f * s), fill.get());
            text(tip, bubble, white.get());
        }
    }
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
    brush->SetColor(original);
}
bool QuickPreviewWindow::CodecCardVisible(const VideoPreview::State& state) const {
    if (state.opening) return false;
    if (state.unsupported_audio) {
        const auto content = ContentRect();
        // Fall back to the status text when the existing card cannot fit.
        const bool pack = pack_offer_.installable || pack_offer_.installing;
        if (content.right - content.left < 360.0f * scale_ ||
            content.bottom - content.top < (pack ? 292.0f : 226.0f) * scale_) return false;
    }
    return kCodecHint && (state.unsupported_audio ||
        (state.missing_decoder && !VideoPreview::IsAudio(item_.path)));
}
bool QuickPreviewWindow::OverCodecButton(POINT point) const {
    return Contains(codec_store_rect_, point) || Contains(codec_open_rect_, point) ||
           Contains(codec_pack_rect_, point) || Contains(cloud_download_rect_, point);
}
bool QuickPreviewWindow::CodecCardClick(POINT point) {
    if (Contains(cloud_download_rect_, point)) {
        PostAction(QuickPreviewAction::DownloadCloud);
        return true;
    }
    if (Contains(codec_pack_rect_, point)) {
        // The app starts the download, or cancels the running one.
        PostAction(codec_pack_image_ ? QuickPreviewAction::InstallImagePack : QuickPreviewAction::InstallMediaPack);
        return true;
    }
    if (Contains(codec_store_rect_, point) && !codec_store_id_.empty()) {
        const std::wstring url = L"ms-windows-store://pdp/?ProductId=" + codec_store_id_;
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (Contains(codec_open_rect_, point)) {
        PostAction(QuickPreviewAction::Open);
        return true;
    }
    return false;
}
void QuickPreviewWindow::SetMediaPackOffer(const MediaPackOffer& offer) {
    if (offer == pack_offer_) return;
    pack_offer_ = offer;
    if (hwnd_ && visible() && video_.active()) InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SetCloudDownloadOffer(const CloudDownloadOffer& offer) {
    if (offer == cloud_offer_) return;
    cloud_offer_ = offer;
    if (hwnd_ && visible() && OfflinePlaceholder()) InvalidateRect(hwnd_, nullptr, FALSE);
}

// 下载并预览 (C4b): same card as the codec hint - what is missing, why Pulse
// waits (it never downloads just to preview), one accent button; a progress
// bar takes the button's place while the file comes down.
bool QuickPreviewWindow::DrawCloudCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                       ID2D1SolidColorBrush* brush) {
    cloud_download_rect_ = {};
    auto* factory = compositor_.DwriteFactory();
    auto* caption = compositor_.SmallFormat();
    auto* icons = compositor_.IconFormat();
    if (!dc || !brush || !factory || !caption) return false;
    const float s = scale_;
    const bool mine = cloud_offer_.path == item_.path;
    const bool downloading = mine && cloud_offer_.percent >= 0;
    const std::wstring provider = mine && !cloud_offer_.provider.empty()
        ? cloud_offer_.provider : l10n::Get(l10n::StringId::CloudProviderGeneric);
    const auto fill = [](std::wstring str, std::wstring_view key, const std::wstring& value) {
        if (const size_t at = str.find(key); at != std::wstring::npos) str.replace(at, key.size(), value);
        return str;
    };
    const std::wstring note = downloading
        ? fill(fill(l10n::Get(l10n::StringId::CloudDownloadingFrom), L"{provider}", provider),
               L"{n}", std::to_wstring(cloud_offer_.percent))
        : fill(fill(l10n::Get(l10n::StringId::CloudOnlyNote), L"{size}",
                    pulse::format::ByteSize(item_.size, true)), L"{provider}", provider);
    const float width = (std::min)(440.0f * s, content.right - content.left - 32.0f * s);
    if (width < 160.0f * s) return false;
    const float inner = width - 40.0f * s;
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(note.data(), static_cast<UINT32>(note.size()), caption, inner, 400.0f * s, &layout);
    float text_h = 18.0f * s;
    if (layout.get()) {
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) text_h = metrics.height;
    }
    const float action_h = downloading ? 4.0f * s : 32.0f * s;
    const float height = 24.0f * s + 40.0f * s + 12.0f * s + text_h + 20.0f * s + action_h + 24.0f * s;
    if (content.bottom - content.top < height) return false;
    const float cx = (content.left + content.right) * 0.5f;
    const float top = std::round((content.top + content.bottom - height) * 0.5f);
    const auto card = D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);
    const auto original = brush->GetColor();
    const auto tint = [&](float alpha) {
        auto color = original;
        color.a = original.a * alpha;
        brush->SetColor(color);
        return brush;
    };
    ComPtr<ID2D1SolidColorBrush> card_fill, line, accent, on_accent;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.05f) : D2D1::ColorF(0xFFFFFF, 1.0f), &card_fill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.07f), &line);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x062030) : D2D1::ColorF(0xFFFFFF), &on_accent);
    const auto rounded = D2D1::RoundedRect(card, 12.0f * s, 12.0f * s);
    if (card_fill.get()) dc->FillRoundedRectangle(rounded, card_fill.get());
    if (line.get()) dc->DrawRoundedRectangle(rounded, line.get(), 1.0f);
    const auto glyph = [&](const wchar_t* code, float size, const D2D1_RECT_F& box, ID2D1Brush* ink) {
        if (!icons || !ink) return;
        ComPtr<IDWriteTextLayout> g;
        factory->CreateTextLayout(code, 1, icons, box.right - box.left, box.bottom - box.top, &g);
        if (!g.get()) return;
        g->SetFontSize(size, DWRITE_TEXT_RANGE{0, 1});
        g->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextLayout(D2D1::Point2F(box.left, box.top), g.get(), ink);
    };
    float y = top + 24.0f * s;
    glyph(downloading ? L"\xE896" : L"\xE753", 32.0f * s, D2D1::RectF(cx - 24.0f * s, y, cx + 24.0f * s, y + 40.0f * s),
          tint(0.85f));
    y += 40.0f * s + 12.0f * s;
    if (layout.get()) dc->DrawTextLayout(D2D1::Point2F(cx - inner * 0.5f, y), layout.get(), tint(0.72f));
    y += text_h + 20.0f * s;
    if (downloading) {
        const float bar_w = (std::min)(260.0f * s, inner);
        const float r = 2.0f * s;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - bar_w * 0.5f, y, cx + bar_w * 0.5f, y + 4.0f * s), r, r),
                                 tint(0.12f));
        const float done = bar_w * std::clamp(static_cast<float>(cloud_offer_.percent) / 100.0f, 0.0f, 1.0f);
        if (accent.get() && done >= 4.0f * s)
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - bar_w * 0.5f, y, cx - bar_w * 0.5f + done, y + 4.0f * s),
                                                       r, r), accent.get());
    } else if (accent.get() && on_accent.get()) {
        const std::wstring label = l10n::Get(l10n::StringId::CloudDownloadPreview);
        const float label_w = typography::MeasureAdvance(factory, caption, label);
        const float icon_w = 16.0f * s, gap = 6.0f * s;
        const float bw = (std::min)(inner, label_w + icon_w + gap + 32.0f * s);
        const auto button = D2D1::RectF(std::round(cx - bw * 0.5f), y, std::round(cx + bw * 0.5f), y + 32.0f * s);
        dc->FillRoundedRectangle(D2D1::RoundedRect(button, 5.0f * s, 5.0f * s), accent.get());
        const float start = (std::max)(button.left + 8.0f * s, cx - (icon_w + gap + label_w) * 0.5f);
        glyph(L"\xE896", 14.0f * s, D2D1::RectF(start, button.top, start + icon_w, button.bottom), on_accent.get());
        ComPtr<IDWriteTextLayout> text;
        const float box = (std::max)(1.0f, button.right - 8.0f * s - (start + icon_w + gap));
        factory->CreateTextLayout(label.data(), static_cast<UINT32>(label.size()), caption, box,
                                  button.bottom - button.top, &text);
        if (text.get()) {
            text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            dc->DrawTextLayout(D2D1::Point2F(start + icon_w + gap, button.top), text.get(), on_accent.get());
        }
        cloud_download_rect_ = button;
    }
    brush->SetColor(original);
    return true;
}
void QuickPreviewWindow::SetImagePackOffer(const MediaPackOffer& offer) {
    if (offer == image_offer_) return;
    image_offer_ = offer;
    if (hwnd_ && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::OnImagePackInstalled() {
    thumbnails_.Evict();
    if (!hwnd_ || !visible()) return;
    // The picture card (or a text / hex view of an EXR) becomes the picture.
    const size_t dot = item_.path.find_last_of(L'.');
    if (dot == std::wstring::npos || !imgpack::IsImagePackExtension(std::wstring_view(item_.path).substr(dot))) return;
    const QuickPreviewItem item = item_;
    Update(item);
}
void QuickPreviewWindow::OnExtraPackInstalled() {
    thumbnails_.Evict();
    if (!hwnd_ || !visible()) return;
    const size_t dot = item_.path.find_last_of(L'.');
    if (dot == std::wstring::npos) return;
    std::wstring extension = item_.path.substr(dot);
    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
    if (!preview::IsFamilyExtension(preview::PreviewFamily::Raw, extension) &&
        !preview::IsArchiveExtension(extension)) return;
    const QuickPreviewItem item = item_;
    Update(item);
}
void QuickPreviewWindow::OnMediaPackInstalled() {
    thumbnails_.Evict();
    if (!hwnd_ || !visible()) return;
    // Reopen what the pack can play now: a card that could not decode it, or
    // a format Quick Look did not play at all before.
    const bool reopen = video_.active() ? CodecCardVisible(video_.Snapshot())
                                        : VideoPreview::Supports(item_.path);
    if (!reopen) return;
    const QuickPreviewItem item = item_;
    Update(item);
}
void QuickPreviewWindow::DrawCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                       const VideoPreview::State& state,
                                       ID2D1SolidColorBrush* brush) {
    const bool zh = l10n::IsChinese();
    CodecCardText t;
    if (state.unsupported_audio) {
        t.title = l10n::HantText(l10n::Get(l10n::StringId::AudioPreviewUnavailable).c_str());
        t.lead = l10n::HantText(l10n::Get(l10n::StringId::WindowsCannotDecodeThisAudioTryOpeningItInTheDefaultApp).c_str());
        t.pack = pack_offer_.installable || pack_offer_.installing;
        DrawCodecCardText(dc, content, t, brush);
        return;
    }
    t.title = l10n::Get(l10n::StringId::CanTShowThisVideoOnThisPC).c_str();
    t.lead = l10n::Get(l10n::StringId::ItSEncodedAs).c_str();
    t.name = state.codec_name;
    t.tail = l10n::Get(l10n::StringId::AndNoDecoderForItIsInstalledTheSoundStillPlays).c_str();
    t.store_id = state.store_id;
    if (state.store_id)
        t.get = zh ? l10n::Cn(L"获取 ") + state.codec + l10n::Cn(L" 视频扩展") : L"Get " + state.codec + L" Video Extension";
    t.hint = l10n::Get(l10n::StringId::ReopenThePreviewAfterInstalling).c_str();
    t.pack = pack_offer_.installable || pack_offer_.installing;
    DrawCodecCardText(dc, content, t, brush);
}

// HEIF / HEIC / AVIF without the Store extension that decodes them (mockup ⑥),
// and ("pack") pictures Windows never reads: OpenEXR, Radiance HDR, QOI and
// JPEG XL without its extension. Both offer the image preview pack.
void QuickPreviewWindow::DrawImageCodecCard(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                            const std::wstring& codec, ID2D1SolidColorBrush* brush) {
    CodecCardText t;
    t.picture = true;
    t.image_pack = true;
    t.pack = image_offer_.installable || image_offer_.installing;
    t.title = l10n::Get(l10n::StringId::CanTShowThisPictureOnThisPC).c_str();
    t.lead = l10n::Get(l10n::StringId::ItUses).c_str();
    t.tail = l10n::Get(l10n::StringId::AndNoDecoderForItIsInstalled).c_str();
    if (codec == L"pack") {
        const size_t dot = item_.path.find_last_of(L'.');
        std::wstring extension = dot == std::wstring::npos ? std::wstring{} : item_.path.substr(dot);
        for (wchar_t& c : extension) c = static_cast<wchar_t>(towlower(c));
        t.name = extension == L".exr" ? L"OpenEXR" : extension == L".hdr" ? L"Radiance HDR"
               : extension == L".qoi" ? L"QOI" : extension == L".jxl" ? L"JPEG XL" : L"";
        if (t.name.empty()) t.name = extension.size() > 1 ? extension.substr(1) : extension;
        t.tail = l10n::Get(l10n::StringId::WhichWindowsCannotReadByItself).c_str();
    } else if (codec == L"av1") {
        t.name = L"AVIF";
        t.store_id = L"9MVZQVXJBQ9V";
        t.get = l10n::Get(l10n::StringId::GetAV1VideoExtension).c_str();
        t.hint = l10n::Get(l10n::StringId::ReopenThePreviewAfterInstalling_2).c_str();
    } else if (codec == L"hevc") {
        t.name = L"HEIF / HEIC";
        t.tail = l10n::Get(l10n::StringId::ItsPhotoIsHEVCEncodedAndNeedsTheHEVCVideoExtension).c_str();
        t.store_id = L"9NMZLZ57R3T7";
        t.get = l10n::Get(l10n::StringId::GetHEVCVideoExtension).c_str();
        t.hint = l10n::Get(l10n::StringId::ReopenThePreviewAfterInstalling_2).c_str();
    } else {
        t.name = L"HEIF / HEIC";
        t.store_id = L"9PMMSR1CGPWG";
        t.get = l10n::Get(l10n::StringId::GetHEIFImageExtensions).c_str();
        t.hint = l10n::Get(l10n::StringId::CommonForIPhonePhotosSomeHEICAlsoNeedTheHEVCVideoExtension).c_str();
    }
    DrawCodecCardText(dc, content, t, brush);
}

void QuickPreviewWindow::DrawCodecCardText(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                           const CodecCardText& text, ID2D1SolidColorBrush* brush) {
    auto* factory = compositor_.DwriteFactory();
    auto* title_format = compositor_.HeaderFormat();
    auto* caption = compositor_.SmallFormat();
    if (!dc || !brush || !factory || !title_format || !caption) return;
    const float s = scale_;
    const auto original = brush->GetColor();
    const auto tint = [&](float alpha) {
        auto color = original;
        color.a = original.a * alpha;
        brush->SetColor(color);
        return brush;
    };
    const std::wstring& title = text.title;
    const std::wstring& lead = text.lead;
    const std::wstring body = text.lead + text.name + text.tail;
    const std::wstring& get = text.get;
    const std::wstring open = l10n::Get(l10n::StringId::OpenInDefaultApp).c_str();
    const std::wstring& hint = text.hint;

    const float width = (std::min)(440.0f * s, content.right - content.left - 32.0f * s);
    const float height = (text.pack ? 292.0f : text.store_id ? 250.0f : 226.0f) * s;
    if (width < 160.0f * s || content.bottom - content.top < height) return;
    const float cx = (content.left + content.right) * 0.5f;
    const float top = std::round((content.top + content.bottom - height) * 0.5f);
    const auto card = D2D1::RectF(cx - width * 0.5f, top, cx + width * 0.5f, top + height);
    ComPtr<ID2D1SolidColorBrush> fill, line;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.05f) : D2D1::ColorF(0xFFFFFF, 1.0f), &fill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.07f), &line);
    const auto rounded = D2D1::RoundedRect(card, 12.0f * s, 12.0f * s);
    if (fill.get()) dc->FillRoundedRectangle(rounded, fill.get());
    if (line.get()) dc->DrawRoundedRectangle(rounded, line.get(), 1.0f);

    // Film strip (video) or picture (image) with a slash (48-unit box).
    float y = top + 24.0f * s;
    if (text.picture) {
        const float u = 44.0f * s / 48.0f;
        const float ox = cx - 24.0f * u, oy = y;
        const auto P = [&](float x, float yy) { return D2D1::Point2F(ox + x * u, oy + yy * u); };
        ID2D1Brush* ink = tint(0.85f);
        const float w = 2.2f * u;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(P(6, 9).x, P(6, 9).y, P(42, 39).x, P(42, 39).y),
                                                   4.0f * u, 4.0f * u), ink, w);
        dc->DrawEllipse(D2D1::Ellipse(P(16, 18), 3.2f * u, 3.2f * u), ink, w);
        dc->DrawLine(P(8, 34), P(19, 24), ink, w);
        dc->DrawLine(P(19, 24), P(26, 30), ink, w);
        dc->DrawLine(P(26, 30), P(32, 23), ink, w);
        dc->DrawLine(P(32, 23), P(40, 32), ink, w);
        dc->DrawLine(P(8, 42), P(40, 6), ink, 2.6f * u);
        y += 44.0f * s + 10.0f * s;
    } else {
        const float u = 44.0f * s / 48.0f;
        const float ox = cx - 24.0f * u, oy = y;
        const auto P = [&](float x, float yy) { return D2D1::Point2F(ox + x * u, oy + yy * u); };
        ID2D1Brush* ink = tint(0.85f);
        const float w = 2.2f * u;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(P(6, 11).x, P(6, 11).y, P(42, 37).x, P(42, 37).y),
                                                   4.0f * u, 4.0f * u), ink, w);
        dc->DrawLine(P(6, 18), P(42, 18), ink, w);
        for (float x : {13.0f, 22.0f, 31.0f, 40.0f}) dc->DrawLine(P(x, 11), P(x - 3.0f, 18), ink, w);
        dc->DrawLine(P(8, 42), P(40, 6), ink, 2.6f * u);
        y += 44.0f * s + 10.0f * s;
    }
    const auto ta = title_format->GetTextAlignment();
    const auto tp = title_format->GetParagraphAlignment();
    const auto sa = caption->GetTextAlignment();
    const auto sp = caption->GetParagraphAlignment();
    title_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    caption->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    caption->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float inner_l = card.left + 20.0f * s, inner_r = card.right - 20.0f * s;
    dc->DrawTextW(title.data(), static_cast<UINT32>(title.size()), title_format,
                  D2D1::RectF(inner_l, y, inner_r, y + 24.0f * s), tint(1.0f), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    y += 28.0f * s;
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(body.data(), static_cast<UINT32>(body.size()), caption,
                              inner_r - inner_l, 44.0f * s, &layout);
    if (layout.get()) {
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_RANGE{static_cast<UINT32>(lead.size()), static_cast<UINT32>(text.name.size())});
        dc->DrawTextLayout(D2D1::Point2F(inner_l, y), layout.get(), tint(0.72f));
    }
    y += 44.0f * s + 16.0f * s;

    // Buttons: the FFmpeg preview pack (when offered) on its own row, then
    // "Get extension" (when the Store has one) + "Open". Only one is accent.
    const float bh = 32.0f * s;
    const float get_w = get.empty() ? 0.0f : typography::MeasureAdvance(factory, caption, get) + 32.0f * s;
    const float open_w = typography::MeasureAdvance(factory, caption, open) + 32.0f * s;
    const float gap = get.empty() ? 0.0f : 10.0f * s;
    const auto neutral = [&](const D2D1_RECT_F& r, const std::wstring& label) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f * s, 5.0f * s), tint(dark_ ? 0.08f : 0.04f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 5.0f * s, 5.0f * s), tint(dark_ ? 0.10f : 0.12f), 1.0f);
        dc->DrawTextW(label.data(), static_cast<UINT32>(label.size()), caption, r, tint(1.0f),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    ComPtr<ID2D1SolidColorBrush> accent, on_accent;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x062030) : D2D1::ColorF(0xFFFFFF), &on_accent);
    const auto primary = [&](const D2D1_RECT_F& r, const std::wstring& label) {
        if (!accent.get() || !on_accent.get()) return neutral(r, label);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5.0f * s, 5.0f * s), accent.get());
        dc->DrawTextW(label.data(), static_cast<UINT32>(label.size()), caption, r, on_accent.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    codec_pack_rect_ = {};
    codec_pack_image_ = text.image_pack;
    const MediaPackOffer& offer = text.image_pack ? image_offer_ : pack_offer_;
    if (text.pack) {
        std::wstring label;
        if (offer.installing) {
            const int percent = static_cast<int>(std::clamp(offer.progress, 0.0f, 1.0f) * 100.0f + 0.5f);
            label = (text.image_pack
                ? l10n::Get(l10n::StringId::DownloadingTheImagePreviewPack).c_str()
                : l10n::Get(l10n::StringId::DownloadingTheFFmpegPreviewPack).c_str()) +
                    std::to_wstring(percent) + l10n::Get(l10n::StringId::ClickToCancel).c_str();
        } else {
            label = text.image_pack
                ? l10n::Get(l10n::StringId::InstallTheImagePreviewPack).c_str()
                : l10n::Get(l10n::StringId::InstallTheFFmpegPreviewPack).c_str();
            if (offer.download_bytes) {
                const uint64_t mb = (std::max)(uint64_t{1}, (offer.download_bytes + 512 * 1024) / (1024 * 1024));
                label += L" \x00B7 " + std::to_wstring(mb) + L" MB";
            }
        }
        const float row_w = get_w + gap + open_w;
        const float pack_w = (std::min)(inner_r - inner_l,
            (std::max)(row_w, typography::MeasureAdvance(factory, caption, label) + 32.0f * s));
        codec_pack_rect_ = D2D1::RectF(std::round(cx - pack_w * 0.5f), y, std::round(cx + pack_w * 0.5f), y + bh);
        if (offer.installing) {
            neutral(codec_pack_rect_, label);
            // Progress along the bottom edge, inside the rounded corners.
            const float inset = 5.0f * s, track = 3.0f * s;
            const float left = codec_pack_rect_.left + inset, right = codec_pack_rect_.right - inset;
            const float bottom = codec_pack_rect_.bottom - 4.0f * s;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, bottom - track, right, bottom),
                                                       track * 0.5f, track * 0.5f), tint(dark_ ? 0.12f : 0.10f));
            const float done = left + (right - left) * std::clamp(offer.progress, 0.0f, 1.0f);
            if (accent.get() && done > left + track)
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, bottom - track, done, bottom),
                                                           track * 0.5f, track * 0.5f), accent.get());
        } else {
            primary(codec_pack_rect_, label);
        }
        y += bh + 10.0f * s;
    }
    float bx = cx - (get_w + gap + open_w) * 0.5f;
    codec_store_id_ = text.store_id ? text.store_id : L"";
    codec_store_rect_ = {};
    if (!get.empty()) {
        codec_store_rect_ = D2D1::RectF(bx, y, bx + get_w, y + bh);
        if (text.pack) neutral(codec_store_rect_, get);
        else primary(codec_store_rect_, get);
        bx += get_w + gap;
    }
    codec_open_rect_ = D2D1::RectF(bx, y, bx + open_w, y + bh);
    neutral(codec_open_rect_, open);
    y += bh + 10.0f * s;
    std::wstring footnote;
    if (text.pack) {
        if (!offer.notice.empty()) footnote = offer.notice;
        else if (offer.installing)
            footnote = l10n::Get(l10n::StringId::ThePreviewReopensByItselfOnceInstalled).c_str();
        else footnote = l10n::Get(l10n::StringId::RemoveItAnyTimeInSettingsPreviewPacks).c_str();
    } else if (text.store_id) {
        footnote = hint;
    }
    if (!footnote.empty())
        dc->DrawTextW(footnote.data(), static_cast<UINT32>(footnote.size()), caption,
                      D2D1::RectF(inner_l, y, inner_r, y + 20.0f * s), tint(0.5f), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    title_format->SetTextAlignment(ta);
    title_format->SetParagraphAlignment(tp);
    caption->SetTextAlignment(sa);
    caption->SetParagraphAlignment(sp);
    brush->SetColor(original);
}
void QuickPreviewWindow::SetMediaFooter(const VideoPreview::State& state, std::vector<std::wstring> extra) {
    std::vector<std::wstring> parts;
    std::wstring ext;
    if (const size_t dot = item_.name.find_last_of(L'.'); dot != std::wstring::npos && dot + 1 < item_.name.size()) {
        ext = item_.name.substr(dot + 1);
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    }
    if (!ext.empty()) parts.push_back(std::move(ext));
    if (!state.codec_name.empty()) parts.push_back(state.codec_name);
    if (state.width && state.height) {
        wchar_t dims[48];
        swprintf_s(dims, L"%u \x00D7 %u", state.width, state.height);
        parts.push_back(dims);
    }
    if (state.duration > 0) parts.push_back(ShortClock(state.duration, state.duration >= 36000000000LL));
    for (std::wstring& part : extra)
        if (!part.empty()) parts.push_back(std::move(part));
    if (item_.size) parts.push_back(pulse::format::ByteSize(item_.size, true));
    SetFooterParts(std::move(parts));
}

} // namespace pulse::ui
