// Quick Look's immersive photo viewer (pictures and videos):
//
//   header   badge | name / meta | [ 全部 21 | 图片 18 | 视频 3 ] | ↻ ⓘ | ‹ 17 / 21 › | Open | ⋯ | ×
//   content  the picture centred with rounded corners and a soft shadow on an
//            ambient backdrop (the picture itself, blurred and darkened);
//            round ‹ › buttons at the edges
//   panel    a floating glass filmstrip: "name  W × H" / "size · RAW · DNG",
//            the current tile larger with a white ring, ▶ 0:27 on videos,
//            both ends fading out
//
// The filter (全部 / 图片 / 视频) drives the filmstrip and ←/→ alike. The arrows
// and the filmstrip fade after the pointer rests for 2.5 s; any movement brings
// them back. While a video plays the filmstrip folds away so it never sits
// over the transport bar. Embedded previews keep the classic layout, and ⋯
// switches back to it (QuickPreviewOptions::immersive, saved by the app).
#include "quick_preview_window.h"
#include "FluentTokens.h"
#include "text_contrast_scope.h"
#include "command_icons.h"
#include "legacy_icons.h"
#include "typography.h"
#include "ui_motion.h"
#include "../common/localization.h"
#include "../common/preview_extensions.h"
#include "../common/text_format.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {

constexpr float kPanelPadX = 14.0f;
constexpr float kPanelPadTop = 9.0f;
constexpr float kPanelInfo = 18.0f;
constexpr float kPanelGap = 7.0f;
constexpr float kPanelPadBottom = 10.0f;
constexpr float kPanelBottom = 14.0f;     // panel to the window's bottom edge
constexpr float kPanelAbove = 10.0f;      // picture to the panel
constexpr float kPanelMinWidth = 520.0f;
constexpr float kPanelMaxWidth = 1180.0f;
constexpr float kPanelShare = 0.64f;       // of the window's width
// Landscape tiles, the current one larger (the reference filmstrip).
constexpr float kTileSmallW = 54.0f;
constexpr float kTileSmallH = 38.0f;
constexpr float kTileBigW = 68.0f;
constexpr float kTileBigH = 46.0f;
constexpr float kTileGap = 6.0f;
constexpr float kEdgeButton = 40.0f;
constexpr float kEdgeInset = 18.0f;
constexpr float kSideMargin = 72.0f;      // room for the edge buttons beside the picture
constexpr float kPictureRadius = 10.0f;
constexpr uint64_t kIdleMs = 2500;
constexpr UINT kPollMs = 100;
constexpr UINT kAnimMs = 15;
constexpr uint64_t kAmbientFadeMs = 280;
constexpr uint64_t kJumpPatienceMs = 1500;

// GalleryHit codes.
constexpr int kHitPrev = 1;
constexpr int kHitNext = 2;
constexpr int kHitPanel = 3;
constexpr int kHitFilter = 10;   // + visible segment
constexpr int kHitTile = 100;    // + view row

D2D1_COLOR_F White(float a) { return D2D1::ColorF(1.0f, 1.0f, 1.0f, a); }
D2D1_COLOR_F Black(float a) { return D2D1::ColorF(0.0f, 0.0f, 0.0f, a); }

bool GalleryInside(const D2D1_RECT_F& r, POINT p) {
    return r.right > r.left && r.bottom > r.top && static_cast<float>(p.x) >= r.left &&
           static_cast<float>(p.x) < r.right && static_cast<float>(p.y) >= r.top &&
           static_cast<float>(p.y) < r.bottom;
}

std::wstring LowerDotExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

std::wstring UpperExtension(const std::wstring& path) {
    std::wstring ext = LowerDotExtension(path);
    if (!ext.empty()) ext.erase(0, 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    return ext;
}

bool RawFile(const std::wstring& path) {
    const std::wstring ext = LowerDotExtension(path);
    return !ext.empty() && pulse::preview::IsFamilyExtension(pulse::preview::PreviewFamily::Raw, ext);
}

std::wstring Duration(uint32_t ms) {
    const uint32_t total = (ms + 500) / 1000;
    const uint32_t h = total / 3600, m = (total / 60) % 60, sec = total % 60;
    wchar_t text[32]{};
    if (h) swprintf_s(text, L"%u:%02u:%02u", h, m, sec);
    else swprintf_s(text, L"%u:%02u", m, sec);
    return text;
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) {
    return D2D1::RectF(r.left - d, r.top - d, r.right + d, r.bottom + d);
}

float Approach(float value, float target, float step) {
    if (value < target) return (std::min)(target, value + step);
    return (std::max)(target, value - step);
}

}  // namespace

// ---- State ------------------------------------------------------------------

bool QuickPreviewWindow::Immersive() const noexcept {
    return options_.immersive && !embedded_ && media_kind_ != 0 && !TextHighContrast();
}

bool QuickPreviewWindow::GalleryActive() const noexcept {
    return Immersive() && HasNavigation() && item_.gallery && gallery_cursor_ >= 0 &&
           gallery_cursor_ < static_cast<int>(item_.gallery->kinds.size());
}

bool QuickPreviewWindow::GalleryMatches(int row) const noexcept {
    if (!item_.gallery || row < 0 || row >= static_cast<int>(item_.gallery->kinds.size())) return false;
    const int kind = item_.gallery->kinds[static_cast<size_t>(row)];
    return gallery_filter_ == 0 || kind == gallery_filter_;
}

int QuickPreviewWindow::GalleryTarget(int dir) const noexcept {
    if (dir == 0 || !GalleryActive()) return -1;
    const int count = static_cast<int>(item_.gallery->kinds.size());
    const int step = dir > 0 ? 1 : -1;
    int remaining = std::abs(dir);
    int found = -1;
    for (int row = gallery_cursor_ + step; row >= 0 && row < count; row += step) {
        if (!GalleryMatches(row)) continue;
        found = row;
        if (--remaining == 0) break;
    }
    return found;
}

// 1-based place of `row` among the rows the filter keeps (0 when it is not kept).
int QuickPreviewWindow::GalleryRank(int row) const noexcept {
    if (!GalleryMatches(row)) return 0;
    if (gallery_filter_ == 0) return row + 1;
    int rank = 0;
    for (int r = 0; r <= row; ++r)
        if (item_.gallery->kinds[static_cast<size_t>(r)] == gallery_filter_) ++rank;
    return rank;
}

bool QuickPreviewWindow::GalleryNavEnd(int dir, bool fallback) const noexcept {
    if (!GalleryActive()) return fallback;
    return GalleryTarget(dir) < 0;
}

// Show / Update (through RefreshItemFacts): the listing that came with the item.
void QuickPreviewWindow::OnGalleryItem() {
    const auto& gallery = item_.gallery;
    int cursor = -1;
    gallery_counts_[0] = gallery_counts_[1] = gallery_counts_[2] = 0;
    if (gallery && gallery->current >= 0 && gallery->current < static_cast<int>(gallery->kinds.size())) {
        cursor = gallery->current;
        gallery_counts_[0] = static_cast<int>(gallery->kinds.size());
        for (const uint8_t kind : gallery->kinds) {
            if (kind == 1) ++gallery_counts_[1];
            else if (kind == 2) ++gallery_counts_[2];
        }
    }
    if (gallery_target_ >= 0) {
        // A later jump is still on its way: keep heading there.
        if (cursor == gallery_target_ || cursor < 0 || GetTickCount64() - gallery_target_at_ > kJumpPatienceMs)
            gallery_target_ = -1;
        else
            cursor = gallery_target_;
    }
    gallery_cursor_ = cursor;
    // An item the filter would skip (picked in the list behind): show everything.
    if (cursor >= 0 && gallery_filter_ != 0 && gallery->kinds[static_cast<size_t>(cursor)] != gallery_filter_) {
        gallery_filter_ = 0;
        gallery_scroll_init_ = false;
    }
    if (hwnd_ && Immersive()) StartGalleryTimer(kAnimMs);
}

void QuickPreviewWindow::PostGalleryJump(int row) {
    if (!HasNavigation() || !message_owner_ || row < 0 || row == gallery_cursor_) return;
    const int dir = row > gallery_cursor_ ? 1 : -1;
    gallery_cursor_ = row;
    gallery_target_ = row;
    gallery_target_at_ = GetTickCount64();
    gallery_activity_ = gallery_target_at_;   // stepping (keys too) brings the filmstrip back
    pending_nav_dir_ = dir;
    pending_nav_at_ = motion::NowMs();
    // lParam: the view row + 1 (absolute); wParam keeps the direction.
    PostMessageW(message_owner_, navigate_message_, static_cast<WPARAM>(static_cast<INT_PTR>(dir)),
                 static_cast<LPARAM>(row + 1));
    StartGalleryTimer(kAnimMs);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::SetGalleryFilter(int filter) {
    filter = std::clamp(filter, 0, 2);
    if (filter == gallery_filter_ || !GalleryActive()) return;
    gallery_filter_ = filter;
    gallery_scroll_init_ = false;
    if (!GalleryMatches(gallery_cursor_)) {
        // The shown item leaves the filter: go to the nearest one it keeps.
        const int count = static_cast<int>(item_.gallery->kinds.size());
        int best = -1;
        for (int d = 1; d < count && best < 0; ++d) {
            if (GalleryMatches(gallery_cursor_ + d)) best = gallery_cursor_ + d;
            else if (GalleryMatches(gallery_cursor_ - d)) best = gallery_cursor_ - d;
        }
        if (best >= 0) PostGalleryJump(best);
        else gallery_filter_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::ToggleImmersive() {
    options_.immersive = !options_.immersive;
    ReportOptions();
    gallery_scroll_init_ = false;
    gallery_alpha_ = 1.0f;
    gallery_activity_ = GetTickCount64();
    if (Immersive()) StartGalleryTimer(kAnimMs);
    const D2D1_RECT_F content = ContentRect();
    ClampPan(content.right - content.left, content.bottom - content.top);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- Geometry ---------------------------------------------------------------

float QuickPreviewWindow::GalleryPanelHeight() const noexcept {
    return std::round((kPanelPadTop + kPanelInfo + kPanelGap + kTileBigH + kPanelPadBottom) * scale_);
}

bool QuickPreviewWindow::GalleryStripWanted() const noexcept {
    return GalleryActive() && static_cast<float>(compositor_.Height()) >= 440.0f * scale_ &&
           static_cast<float>(compositor_.Width()) >= 360.0f * scale_;
}

float QuickPreviewWindow::GalleryReserve() const noexcept {
    if (!Immersive() || fullscreen_) return 0.0f;
    if (!GalleryStripWanted()) return std::round(12.0f * scale_);
    return std::round((GalleryPanelHeight() + (kPanelBottom + kPanelAbove) * scale_) * gallery_open_);
}

float QuickPreviewWindow::ImmersiveSide() const noexcept {
    if (!Immersive()) return 0.0f;
    const bool arrows = HasNavigation() && message_owner_;
    if (!arrows) return std::round(20.0f * scale_);
    return std::round((static_cast<float>(compositor_.Width()) >= 640.0f * scale_ ? kSideMargin : 20.0f) * scale_);
}

D2D1_RECT_F QuickPreviewWindow::ImmersiveContentRect(const D2D1_RECT_F& content) const noexcept {
    const float s = scale_;
    const float side = ImmersiveSide();
    D2D1_RECT_F r = D2D1::RectF(content.left + side, content.top + (fullscreen_ ? 0.0f : std::round(6.0f * s)),
                                content.right - side, content.bottom - GalleryReserve());
    if (r.right - r.left < 120.0f * s) { r.left = content.left; r.right = content.right; }
    if (r.bottom - r.top < 120.0f * s) { r.top = content.top; r.bottom = (std::max)(content.top, content.bottom); }
    return r;
}

D2D1_RECT_F QuickPreviewWindow::GalleryPanelRect() const noexcept {
    const float s = scale_;
    const float width = static_cast<float>(compositor_.Width()) - InfoDrawerWidth();
    const float height = static_cast<float>(compositor_.Height());
    // About the picture's width, as in the reference; never wider than the window.
    const float wanted = std::clamp(width * kPanelShare, kPanelMinWidth * s, kPanelMaxWidth * s);
    const float panel_w = std::round((std::min)(wanted, width - 48.0f * s));
    const float panel_h = GalleryPanelHeight();
    // Folding away (a playing video) slides the panel down as it fades.
    const float drop = (1.0f - gallery_open_) * 24.0f * s;
    const float bottom = height - kPanelBottom * s + drop;
    const float left = std::round((width - panel_w) * 0.5f);
    return D2D1::RectF(left, std::round(bottom - panel_h), left + panel_w, std::round(bottom));
}

// ---- Backdrop and picture -------------------------------------------------------

void QuickPreviewWindow::DrawImmersiveBackdrop(ID2D1DeviceContext* dc) {
    if (!dc) return;
    const float w = static_cast<float>(compositor_.Width());
    const float h = static_cast<float>(compositor_.Height());
    ComPtr<ID2D1SolidColorBrush> fill;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x0E0E10), &fill);
    if (!fill.get()) return;
    dc->FillRectangle(D2D1::RectF(0, 0, w, h), fill.get());

    ComPtr<ID2D1Device> device;
    dc->GetDevice(&device);
    if (device.get() != ambient_device_.get()) {
        ambient_.reset();
        ambient_prev_.reset();
        ambient_key_.clear();
        ambient_device_ = std::move(device);
    }
    const std::wstring key = item_.path + L"|" + std::to_wstring(item_.modified);
    if (key != ambient_key_) {
        if (ambient_.get()) {
            ambient_prev_ = std::move(ambient_);
            ambient_fade_at_ = GetTickCount64();
        }
        ambient_.reset();
        ambient_key_ = key;
    }
    if (!ambient_.get() && !item_.path.empty()) {
        // The picture squeezed to 12 x 12 and drawn back up with a cubic
        // filter is a soft blur of its colours, without an effect graph.
        ComPtr<ID2D1Bitmap1> big_bitmap, tiny_bitmap;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(48, 48), nullptr, 0, &props, &big_bitmap)) && big_bitmap.get() &&
            SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(12, 12), nullptr, 0, &props, &tiny_bitmap)) && tiny_bitmap.get()) {
            ComPtr<ID2D1Image> target;
            dc->GetTarget(&target);
            D2D1_MATRIX_3X2_F transform{};
            dc->GetTransform(&transform);
            float dpi_x = 96.0f, dpi_y = 96.0f;
            dc->GetDpi(&dpi_x, &dpi_y);
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTransform(D2D1::IdentityMatrix());
            dc->SetTarget(big_bitmap.get());
            dc->Clear(D2D1::ColorF(0x0E0E10));
            const PreviewDrawResult drawn = tile_thumbs_.DrawWallThumbnail(dc, D2D1::RectF(0, 0, 48, 48),
                item_.path, item_.attrs, 96, generation_, item_.modified, item_.size, 1.0f, nullptr);
            if (drawn == PreviewDrawResult::Bitmap) {
                dc->SetTarget(tiny_bitmap.get());
                dc->Clear(D2D1::ColorF(0x0E0E10));
                const D2D1_RECT_F dst = D2D1::RectF(0, 0, 12, 12);
                dc->DrawBitmap(big_bitmap.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
            dc->SetTarget(target.get());
            dc->SetDpi(dpi_x, dpi_y);
            dc->SetTransform(transform);
            if (drawn == PreviewDrawResult::Bitmap) ambient_ = std::move(tiny_bitmap);
        }
    }
    const D2D1_RECT_F cover = D2D1::RectF(-w * 0.1f, -h * 0.1f, w * 1.1f, h * 1.1f);
    const uint64_t since = GetTickCount64() - ambient_fade_at_;
    const float t = ambient_prev_.get() ? (std::min)(1.0f, static_cast<float>(since) / kAmbientFadeMs) : 1.0f;
    if (ambient_prev_.get()) {
        if (t >= 1.0f) ambient_prev_.reset();
        else dc->DrawBitmap(ambient_prev_.get(), &cover, 0.9f * (1.0f - t), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
    if (ambient_.get())
        dc->DrawBitmap(ambient_.get(), &cover, 0.9f * motion::EaseOutCubic(t), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    // Darken, more at the top (header) and the bottom (filmstrip).
    fill->SetColor(Black(0.36f));
    dc->FillRectangle(D2D1::RectF(0, 0, w, h), fill.get());
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, Black(0.38f)}, {0.22f, Black(0.0f)}, {0.68f, Black(0.0f)}, {1.0f, Black(0.48f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> shade;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) && collection.get() &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
            D2D1::Point2F(0, h)), collection.get(), &shade)) && shade.get())
        dc->FillRectangle(D2D1::RectF(0, 0, w, h), shade.get());
}

D2D1_RECT_F QuickPreviewWindow::BeginImmersivePicture(ID2D1DeviceContext* dc, const D2D1_RECT_F& draw,
                                                      bool allowed) {
    picture_layer_ = false;
    if (!dc || !allowed || !Immersive() || media_kind_ != 1 || decoded_w_ == 0 || decoded_h_ == 0 ||
        frame_count_ > 1)
        return D2D1_RECT_F{};
    D2D1_RECT_F shown = draw;
    if (image_fit_) {
        const float aspect = TurnedW() / TurnedH();
        const float dw = draw.right - draw.left, dh = draw.bottom - draw.top;
        if (dw <= 0.0f || dh <= 0.0f || !(aspect > 0.0f)) return D2D1_RECT_F{};
        if (aspect > dw / dh) {
            const float ph = dw / aspect, cy = (draw.top + draw.bottom) * 0.5f;
            shown.top = cy - ph * 0.5f;
            shown.bottom = cy + ph * 0.5f;
        } else {
            const float pw = dh * aspect, cx = (draw.left + draw.right) * 0.5f;
            shown.left = cx - pw * 0.5f;
            shown.right = cx + pw * 0.5f;
        }
    }
    const float s = scale_;
    const float radius = kPictureRadius * s;
    ImageStats stats;
    const bool translucent = thumbnails_.Stats(item_.path, preview_pixels_, item_.modified, item_.size, stats) &&
                             stats.has_alpha;
    if (!translucent && shown.right - shown.left < 20000.0f && shown.bottom - shown.top < 20000.0f) {
        // A soft drop shadow as rings around the picture (the picture covers
        // the inside, so nothing is filled under it): seven 2.5 px steps whose
        // alpha falls the way seven stacked 6 % fills would.
        ComPtr<ID2D1SolidColorBrush> shadow;
        dc->CreateSolidColorBrush(Black(0.0f), &shadow);
        if (shadow.get()) {
            const float step = 2.5f * s, drop = 5.0f * s;
            for (int i = 0; i < 7; ++i) {
                const float grow = (static_cast<float>(i) + 0.5f) * step;
                D2D1_RECT_F r = Inflate(shown, grow);
                r.top += drop;
                r.bottom += drop;
                shadow->SetColor(Black(1.0f - std::pow(0.94f, static_cast<float>(7 - i))));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius + grow, radius + grow), shadow.get(), step);
            }
            // The strip the drop uncovers below the picture.
            shadow->SetColor(Black(1.0f - std::pow(0.94f, 7.0f)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(shown.left, shown.bottom - radius - drop,
                                     shown.right, shown.bottom + drop), radius, radius), shadow.get());
        }
    }
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (!factory.get() ||
        FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(shown, radius, radius), &geometry)) ||
        !geometry.get())
        return shown;
    // Bounds = the picture: the layer is no larger than what it clips.
    dc->PushLayer(D2D1::LayerParameters1(shown, geometry.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                  nullptr);
    picture_layer_ = true;
    return shown;
}

void QuickPreviewWindow::EndImmersivePicture(ID2D1DeviceContext* dc) {
    if (picture_layer_ && dc) dc->PopLayer();
    picture_layer_ = false;
}

// ---- Header filter pill -----------------------------------------------------------

float QuickPreviewWindow::LayoutGalleryFilter(float left, float right, float cy) {
    gallery_filter_count_ = 0;
    gallery_filter_pill_ = D2D1_RECT_F{};
    if (!GalleryActive() || HeaderHeight() <= 0.0f) return right;
    const int all = gallery_counts_[0], pictures = gallery_counts_[1], videos = gallery_counts_[2];
    // One kind only: nothing to filter.
    if (all <= 0 || pictures == all || videos == all || (pictures == 0 && videos == 0)) return right;
    int ids[3]{};
    int n = 0;
    ids[n++] = 0;
    if (pictures > 0) ids[n++] = 1;
    if (videos > 0) ids[n++] = 2;
    if (n < 2) return right;
    const float s = scale_;
    IDWriteTextFormat* format = compositor_.SmallFormat();
    using pulse::l10n::StringId;
    static constexpr StringId kLabels[] = {StringId::QuickLookFilterAll, StringId::QuickLookFilterPhotos,
                                           StringId::QuickLookFilterVideos};
    float widths[3]{};
    float total = 0.0f;
    const float inner = 3.0f * s;
    for (int i = 0; i < n; ++i) {
        const std::wstring label = pulse::l10n::Get(kLabels[ids[i]]);
        const std::wstring count = std::to_wstring(gallery_counts_[ids[i]]);
        widths[i] = std::round(UiTextWidth(label, format) + 5.0f * s + UiTextWidth(count, format) + 22.0f * s);
        total += widths[i];
    }
    const float pill_w = total + inner * 2.0f;
    const float pill_h = std::round(28.0f * s);
    const float width = static_cast<float>(compositor_.Width());
    float x = std::round(width * 0.5f - pill_w * 0.5f);
    const float title_min = 120.0f * s;
    if (x < left + title_min) x = left + title_min;
    if (x + pill_w > right) return right;
    gallery_filter_pill_ = D2D1::RectF(x, std::round(cy - pill_h * 0.5f), x + pill_w, std::round(cy + pill_h * 0.5f));
    float sx = x + inner;
    for (int i = 0; i < n; ++i) {
        gallery_filter_rects_[i] = D2D1::RectF(sx, gallery_filter_pill_.top + inner, sx + widths[i],
                                               gallery_filter_pill_.bottom - inner);
        gallery_filter_ids_[i] = ids[i];
        sx += widths[i];
    }
    gallery_filter_count_ = n;
    return (std::max)(left, x - 12.0f * s);
}

void QuickPreviewWindow::DrawGalleryFilter(ID2D1DeviceContext* dc) {
    if (!dc || gallery_filter_count_ < 2) return;
    const float s = scale_;
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(White(0.08f), &brush);
    if (!brush.get()) return;
    const D2D1_RECT_F pill = gallery_filter_pill_;
    const float r = (pill.bottom - pill.top) * 0.5f;
    dc->FillRoundedRectangle(D2D1::RoundedRect(pill, r, r), brush.get());
    brush->SetColor(White(0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(pill, -0.5f), r, r), brush.get(), 1.0f);
    IDWriteTextFormat* format = compositor_.SmallFormat();
    using pulse::l10n::StringId;
    static constexpr StringId kLabels[] = {StringId::QuickLookFilterAll, StringId::QuickLookFilterPhotos,
                                           StringId::QuickLookFilterVideos};
    for (int i = 0; i < gallery_filter_count_; ++i) {
        const D2D1_RECT_F seg = gallery_filter_rects_[i];
        const bool selected = gallery_filter_ids_[i] == gallery_filter_;
        const bool hot = gallery_hover_ == kHitFilter + i;
        const float sr = (seg.bottom - seg.top) * 0.5f;
        if (selected || hot) {
            brush->SetColor(White(selected ? 0.20f : 0.08f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(seg, sr, sr), brush.get());
        }
        const std::wstring label = pulse::l10n::Get(kLabels[gallery_filter_ids_[i]]);
        const std::wstring count = std::to_wstring(gallery_counts_[gallery_filter_ids_[i]]);
        const float lw = UiTextWidth(label, format), cw = UiTextWidth(count, format);
        const float x = std::round((seg.left + seg.right - (lw + 5.0f * s + cw)) * 0.5f);
        const auto text = [&](const std::wstring& value, float left, float w, const D2D1_COLOR_F& color) {
            const ComPtr<IDWriteTextLayout> layout = UiLayout(value, format, w + 2.0f * s, seg.bottom - seg.top);
            if (!layout.get()) return;
            brush->SetColor(color);
            compositor_.DrawTextLayout(D2D1::Point2F(left, seg.top), layout.get(), brush.get(),
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        text(label, x, lw, White(selected ? 0.98f : 0.78f));
        text(count, x + lw + 5.0f * s, cw, White(selected ? 0.70f : 0.48f));
    }
}

void QuickPreviewWindow::GalleryCounterText(std::wstring& text) const {
    if (!GalleryActive()) return;
    const int rank = GalleryRank(gallery_cursor_);
    const int count = gallery_counts_[gallery_filter_];
    if (rank > 0 && count > 0) text = std::to_wstring(rank) + L" / " + std::to_wstring(count);
}

// ---- Edge buttons and the filmstrip -----------------------------------------------

void QuickPreviewWindow::DrawImmersiveChrome(ID2D1DeviceContext* dc) {
    gallery_tiles_.clear();
    gallery_edges_[0] = gallery_edges_[1] = D2D1_RECT_F{};
    gallery_panel_ = D2D1_RECT_F{};
    if (!dc || !Immersive()) return;
    if (!gallery_timer_ms_) StartGalleryTimer(kPollMs);
    const float alpha = gallery_alpha_;
    if (alpha <= 0.01f) return;
    const float s = scale_;
    const D2D1_RECT_F content = ContentRect();
    const float width = static_cast<float>(compositor_.Width()) - InfoDrawerWidth();
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(Black(0.3f), &brush);
    if (!brush.get()) return;
    if (HasNavigation() && message_owner_ && content.bottom - content.top > kEdgeButton * 2.0f * s &&
        width >= 360.0f * s) {
        const float d = std::round(kEdgeButton * s);
        const float cy = std::round((content.top + content.bottom) * 0.5f);
        for (int side = 0; side < 2; ++side) {
            const int dir = side ? 1 : -1;
            const bool end = GalleryNavEnd(dir, dir < 0 ? item_.count > 0 && item_.position <= 1
                                                        : item_.count > 0 && item_.position >= item_.count);
            if (end) continue;
            const float left = side ? width - kEdgeInset * s - d : kEdgeInset * s;
            const D2D1_RECT_F rect = D2D1::RectF(std::round(left), cy - d * 0.5f, std::round(left) + d, cy + d * 0.5f);
            const bool hot = gallery_hover_ == (side ? kHitNext : kHitPrev);
            const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F((rect.left + rect.right) * 0.5f, cy), d * 0.5f, d * 0.5f);
            brush->SetColor(Black((hot ? 0.52f : 0.34f) * alpha));
            dc->FillEllipse(circle, brush.get());
            brush->SetColor(White((hot ? 0.22f : 0.13f) * alpha));
            dc->DrawEllipse(D2D1::Ellipse(circle.point, circle.radiusX - 0.5f, circle.radiusY - 0.5f), brush.get(), 1.0f);
            brush->SetColor(White(0.94f * alpha));
            const wchar_t* glyph = side ? L"\xE76C" : L"\xE76B";
            if (!command_icons::DrawGlyph(dc, brush.get(), glyph, rect, 14.0f * s))
                DrawLegacyIcon(dc, compositor_.DwriteFactory(), glyph, rect, brush.get());
            gallery_edges_[side] = rect;
        }
    }
    if (GalleryStripWanted() && gallery_open_ > 0.01f) DrawGalleryPanel(dc, alpha * gallery_open_);
}

void QuickPreviewWindow::DrawGalleryPanel(ID2D1DeviceContext* dc, float alpha) {
    const float s = scale_;
    const D2D1_RECT_F panel = GalleryPanelRect();
    if (panel.right - panel.left < 200.0f * s || alpha <= 0.01f) return;
    const auto& gallery = *item_.gallery;
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(Black(0.4f * alpha), &brush);
    if (!brush.get()) return;
    gallery_panel_ = panel;

    // Glass: a dark veil, a faint white tint, a hairline and a top highlight.
    const float radius = 16.0f * s;
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(panel, radius, radius);
    dc->FillRoundedRectangle(shape, brush.get());
    brush->SetColor(White(0.055f * alpha));
    dc->FillRoundedRectangle(shape, brush.get());
    brush->SetColor(White(0.13f * alpha));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(panel, -0.5f), radius, radius), brush.get(), 1.0f);
    brush->SetColor(White(0.07f * alpha));
    dc->DrawLine(D2D1::Point2F(panel.left + radius, panel.top + 1.5f), D2D1::Point2F(panel.right - radius, panel.top + 1.5f),
                 brush.get(), 1.0f);

    IDWriteTextFormat* fmt_small = compositor_.SmallFormat();
    const auto text = [&](const std::wstring& value, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                          const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align, bool bold = false) {
        if (value.empty() || rect.right <= rect.left) return;
        const ComPtr<IDWriteTextLayout> layout = UiLayout(value, format, rect.right - rect.left, rect.bottom - rect.top, align);
        if (!layout.get()) return;
        if (bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(value.size())});
        brush->SetColor(color);
        compositor_.DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), brush.get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Info line: "name  W × H" on the left, "size · RAW · DNG" on the right.
    const D2D1_RECT_F info = D2D1::RectF(panel.left + kPanelPadX * s, panel.top + kPanelPadTop * s,
                                         panel.right - kPanelPadX * s, panel.top + (kPanelPadTop + kPanelInfo) * s);
    uint32_t pw = 0, ph = 0;
    if (media_kind_ == 2 && video_.active()) {
        const auto state = video_.Snapshot();
        pw = static_cast<uint32_t>(state.width);
        ph = static_cast<uint32_t>(state.height);
    } else {
        pw = source_w_ ? source_w_ : decoded_w_;
        ph = source_w_ ? source_h_ : decoded_h_;
        if (image_turns_ & 1) std::swap(pw, ph);
    }
    const std::wstring dims = pw && ph ? std::to_wstring(pw) + L" \x00D7 " + std::to_wstring(ph) : std::wstring();
    std::wstring facts;
    const auto add = [&facts](const std::wstring& part) {
        if (part.empty()) return;
        if (!facts.empty()) facts += L" \x00B7 ";
        facts += part;
    };
    if (!(item_.attrs & FILE_ATTRIBUTE_DIRECTORY)) add(pulse::format::ByteSize(item_.size, true));
    if (RawFile(item_.path)) add(L"RAW");
    add(UpperExtension(item_.path));
    const float facts_w = (std::min)((info.right - info.left) * 0.45f, UiTextWidth(facts, fmt_small));
    text(facts, fmt_small, D2D1::RectF(info.right - facts_w, info.top, info.right, info.bottom), White(0.60f * alpha),
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float dims_w = dims.empty() ? 0.0f : UiTextWidth(dims, fmt_small);
    const float name_room = (std::max)(0.0f, info.right - facts_w - 16.0f * s - info.left - (dims_w ? dims_w + 10.0f * s : 0.0f));
    // The name is semibold: a little wider than the regular measure.
    const float name_w = (std::min)(name_room, std::ceil(UiTextWidth(item_.name, fmt_small) * 1.07f + 2.0f * s));
    text(item_.name, fmt_small, D2D1::RectF(info.left, info.top, info.left + name_w, info.bottom), White(0.96f * alpha),
         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    if (!dims.empty())
        text(dims, fmt_small, D2D1::RectF(info.left + name_w + 10.0f * s, info.top, info.left + name_w + 10.0f * s + dims_w,
                                      info.bottom), White(0.55f * alpha), DWRITE_TEXT_ALIGNMENT_LEADING);

    // The strip.
    const float big_w = std::round(kTileBigW * s), big_h = std::round(kTileBigH * s);
    const float little_w = std::round(kTileSmallW * s), little_h = std::round(kTileSmallH * s);
    const float gap = std::round(kTileGap * s);
    const D2D1_RECT_F strip = D2D1::RectF(info.left, info.bottom + kPanelGap * s, info.right, info.bottom + kPanelGap * s + big_h);
    std::vector<int> rows;
    const int first = gallery.first;
    const int last = first + static_cast<int>(gallery.entries.size()) - 1;
    rows.reserve(gallery.entries.size());
    int current_index = -1;
    for (int row = first; row <= last; ++row) {
        if (!GalleryMatches(row)) continue;
        if (row == gallery_cursor_) current_index = static_cast<int>(rows.size());
        rows.push_back(row);
    }
    if (rows.empty() || current_index < 0) return;
    const float cursor_rank = static_cast<float>(GalleryRank(gallery_cursor_) - 1);
    if (!gallery_scroll_init_) {
        gallery_scroll_ = cursor_rank;
        gallery_scroll_init_ = true;
    }
    if (!gallery_badge_format_.get() || gallery_badge_scale_ != s) {
        gallery_badge_format_.reset();
        if (compositor_.DwriteFactory())
            compositor_.DwriteFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.5f * s, L"", &gallery_badge_format_);
        gallery_badge_scale_ = s;
    }
    // Lay the tiles out first: one layer then clips every rounded tile and
    // fades both ends (a layer per tile cost more than the rest of the panel).
    const float cx = std::round((strip.left + strip.right) * 0.5f);
    const float cy = (strip.top + strip.bottom) * 0.5f;
    const float step = little_w + gap;
    const float bump = (big_w - little_w) * 0.5f;
    const uint32_t pixels = big_w > 128.0f ? 256u : 128u;
    const int base_rank = static_cast<int>(cursor_rank) - current_index;
    const float span = strip.right - strip.left;
    const float edge_px = (std::min)(span * 0.12f, 36.0f * s);
    struct Placed { D2D1_RECT_F tile; int row; int kind; bool current, hot; float radius, fade; uint32_t duration; };
    std::vector<Placed> placed;
    placed.reserve(24);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const float offset = static_cast<float>(base_rank + i) - gallery_scroll_;
        if (std::abs(offset) * step > span) continue;
        const float near_ = (std::max)(0.0f, 1.0f - std::abs(offset));
        const float tw = little_w + (big_w - little_w) * near_;
        const float th = little_h + (big_h - little_h) * near_;
        const float x = cx + offset * step + (offset > 0 ? 1.0f : -1.0f) * (std::min)(1.0f, std::abs(offset)) * bump;
        const D2D1_RECT_F tile = D2D1::RectF(std::round(x - tw * 0.5f), std::round(cy - th * 0.5f),
                                             std::round(x + tw * 0.5f), std::round(cy + th * 0.5f));
        if (tile.right < strip.left || tile.left > strip.right) continue;
        Placed t{};
        t.tile = tile;
        t.row = rows[static_cast<size_t>(i)];
        t.kind = gallery.kinds[static_cast<size_t>(t.row)];
        t.current = t.row == gallery_cursor_;
        t.hot = gallery_hover_ == kHitTile + t.row;
        t.radius = (t.current ? 8.0f : 6.0f) * s;
        t.fade = edge_px > 0.0f ? std::clamp((std::min)(x - strip.left, strip.right - x) / edge_px, 0.0f, 1.0f) : 1.0f;
        placed.push_back(t);
    }
    if (placed.empty()) return;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    std::vector<ComPtr<ID2D1RoundedRectangleGeometry>> shapes(placed.size());
    std::vector<ID2D1Geometry*> raw;
    raw.reserve(placed.size());
    for (size_t i = 0; i < placed.size(); ++i) {
        if (factory.get()) factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(placed[i].tile, placed[i].radius, placed[i].radius), &shapes[i]);
        if (shapes[i].get()) raw.push_back(shapes[i].get());
    }
    ComPtr<ID2D1GeometryGroup> group;
    if (factory.get() && raw.size() == placed.size())
        factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()), &group);
    ComPtr<ID2D1GradientStopCollection> stops;
    ComPtr<ID2D1LinearGradientBrush> fade;
    const float edge = (std::min)(0.12f, 36.0f * s / (std::max)(1.0f, span));
    const D2D1_GRADIENT_STOP fade_stops[] = {
        {0.0f, White(0.0f)}, {edge, White(1.0f)}, {1.0f - edge, White(1.0f)}, {1.0f, White(0.0f)}};
    if (SUCCEEDED(dc->CreateGradientStopCollection(fade_stops, 4, &stops)) && stops.get())
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(strip.left, 0),
            D2D1::Point2F(strip.right, 0)), stops.get(), &fade);
    const D2D1_RECT_F strip_clip = D2D1::RectF(strip.left, strip.top - 2.0f * s, strip.right, strip.bottom + 2.0f * s);
    dc->PushLayer(D2D1::LayerParameters1(strip_clip, group.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                         alpha, fade.get(), D2D1_LAYER_OPTIONS1_NONE), nullptr);
    for (Placed& t : placed) {
        const QuickPreviewGalleryEntry& entry = gallery.entries[static_cast<size_t>(t.row - first)];
        brush->SetColor(White(0.08f));
        dc->FillRectangle(t.tile, brush.get());
        if (!entry.path.empty()) {
            if (t.kind != 0) {
                float aspect = 0.0f;
                const PreviewDrawResult drawn = tile_thumbs_.DrawWallThumbnail(dc, t.tile, entry.path, entry.attrs, pixels,
                    generation_, entry.modified, entry.size, 1.0f, &aspect, &t.duration);
                if (drawn != PreviewDrawResult::Bitmap) {
                    // Still decoding (videos take longest): a quiet glyph, not a hole.
                    brush->SetColor(White(0.42f));
                    command_icons::DrawGlyph(dc, brush.get(), t.kind == 2 ? L"\xE714" : L"\xE91B", t.tile, 14.0f * s);
                }
            } else {
                brush->SetColor(White(0.30f));
                command_icons::DrawGlyph(dc, brush.get(), L"\xE7C3", t.tile, 14.0f * s);   // under the icon until it loads
                tile_thumbs_.DrawGridThumbnail(dc, Inflate(t.tile, -6.0f * s), entry.path, entry.attrs, pixels,
                                               generation_, entry.modified, entry.size, 1.0f, false, nullptr);
            }
        }
        if (!t.current) {
            brush->SetColor(Black(t.hot ? 0.05f : 0.22f));
            dc->FillRectangle(t.tile, brush.get());
        }
    }
    dc->PopLayer();
    // Rings and badges sit on top, faded like the tiles under them.
    for (const Placed& t : placed) {
        const float a = alpha * t.fade;
        const D2D1_RECT_F& tile = t.tile;
        const float r = t.radius;
        if (a > 0.01f) {
            if (t.current) {
                brush->SetColor(White(0.96f * a));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(tile, -1.0f * s), r - 1.0f * s, r - 1.0f * s),
                                         brush.get(), 2.0f * s);
            } else if (t.hot) {
                brush->SetColor(White(0.5f * a));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(tile, -0.5f), r, r), brush.get(), 1.0f);
            }
            const auto badge = [&](const std::wstring& label, bool right) {
                IDWriteTextFormat* format = gallery_badge_format_.get() ? gallery_badge_format_.get() : fmt_small;
                const float bw = UiTextWidth(label, format) + 8.0f * s;
                const float bh = std::round(14.0f * s);
                const float inset = 4.0f * s;
                if (bw > tile.right - tile.left - inset * 2.0f) return;
                const float left = right ? tile.right - inset - bw : tile.left + inset;
                const D2D1_RECT_F rect = D2D1::RectF(left, tile.bottom - inset - bh, left + bw, tile.bottom - inset);
                brush->SetColor(Black(0.58f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f * s, 4.0f * s), brush.get());
                text(label, format, rect, White(0.96f * a), DWRITE_TEXT_ALIGNMENT_CENTER);
            };
            if (t.kind == 2) badge(t.duration ? L"\x25B6 " + Duration(t.duration) : std::wstring(L"\x25B6"), true);
            if (t.current && RawFile(gallery.entries[static_cast<size_t>(t.row - first)].path)) badge(L"RAW", false);
        }
        GalleryTile hit;
        hit.rect = D2D1::RectF((std::max)(tile.left, strip.left), tile.top, (std::min)(tile.right, strip.right), tile.bottom);
        hit.row = t.row;
        gallery_tiles_.push_back(hit);
    }
}

// ---- Input ------------------------------------------------------------------

int QuickPreviewWindow::GalleryHit(POINT client) const noexcept {
    if (!Immersive()) return 0;
    for (int i = 0; i < gallery_filter_count_; ++i)
        if (GalleryInside(gallery_filter_rects_[i], client)) return kHitFilter + i;
    if (gallery_alpha_ <= 0.02f) return 0;
    if (GalleryInside(gallery_edges_[0], client)) return kHitPrev;
    if (GalleryInside(gallery_edges_[1], client)) return kHitNext;
    if (!GalleryInside(gallery_panel_, client)) return 0;
    for (const GalleryTile& tile : gallery_tiles_)
        if (GalleryInside(tile.rect, client)) return kHitTile + tile.row;
    return kHitPanel;
}

bool QuickPreviewWindow::GalleryMouseDown(POINT client) {
    const int hit = GalleryHit(client);
    if (hit == 0) return false;
    gallery_activity_ = GetTickCount64();
    if (GetFocus() != hwnd_) SetFocus(hwnd_);
    if (hit == kHitPrev || hit == kHitNext) {
        PostNavigate(hit == kHitPrev ? -1 : 1);
    } else if (hit >= kHitFilter && hit < kHitFilter + 3) {
        SetGalleryFilter(gallery_filter_ids_[hit - kHitFilter]);
    } else if (hit >= kHitTile) {
        PostGalleryJump(hit - kHitTile);
    }
    return true;   // the panel's background swallows the click (no pan)
}

void QuickPreviewWindow::GalleryMouseMove(POINT client) {
    if (!Immersive()) return;
    if (client.x != gallery_move_.x || client.y != gallery_move_.y) {
        gallery_move_ = client;
        gallery_activity_ = GetTickCount64();
        if (gallery_alpha_ < 1.0f) StartGalleryTimer(kAnimMs);
    }
    const int hover = GalleryHit(client);
    if (hover != gallery_hover_) {
        gallery_hover_ = hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void QuickPreviewWindow::GalleryMouseLeave() {
    gallery_move_ = POINT{-1, -1};
    if (gallery_hover_ != 0) {
        gallery_hover_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool QuickPreviewWindow::GalleryWheel(POINT client, float steps) {
    if (!Immersive() || steps == 0.0f || gallery_alpha_ <= 0.02f || !GalleryInside(gallery_panel_, client))
        return false;
    gallery_activity_ = GetTickCount64();
    // The wheel over the filmstrip steps through it (one item per notch).
    const uint64_t now = GetTickCount64();
    if (now - gallery_wheel_at_ >= 60) {
        gallery_wheel_at_ = now;
        PostNavigate(steps > 0.0f ? -1 : 1);
    }
    return true;
}

void QuickPreviewWindow::StartGalleryTimer(UINT ms) {
    if (!hwnd_) return;
    if (gallery_timer_ms_ == ms) return;
    gallery_timer_ms_ = ms;
    if (!gallery_tick_) gallery_tick_ = GetTickCount64();
    SetTimer(hwnd_, kGalleryTimer, ms, nullptr);
}

void QuickPreviewWindow::TickGallery() {
    if (!hwnd_ || closing_ || !visible() || !Immersive()) {
        KillTimer(hwnd_, kGalleryTimer);
        gallery_timer_ms_ = 0;
        gallery_tick_ = 0;
        return;
    }
    const uint64_t now = GetTickCount64();
    const float dt = static_cast<float>((std::min<uint64_t>)(100, now - (gallery_tick_ ? gallery_tick_ : now)));
    gallery_tick_ = now;
    // The pointer is polled as well: over a video it belongs to the player's window.
    POINT cursor{};
    RECT window{};
    if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &window) && PtInRect(&window, cursor) &&
        (cursor.x != gallery_pointer_.x || cursor.y != gallery_pointer_.y)) {
        if (gallery_pointer_.x != 0 || gallery_pointer_.y != 0) gallery_activity_ = now;
        gallery_pointer_ = cursor;
    }
    bool changed = false;
    bool animating = false;
    // Arrows and filmstrip: shown while the pointer moves or rests on them.
    const bool held = gallery_hover_ != 0 || GetCapture() == hwnd_;
    const float alpha_target = held || now - gallery_activity_ < kIdleMs ? 1.0f : 0.0f;
    if (gallery_alpha_ != alpha_target) {
        gallery_alpha_ = Approach(gallery_alpha_, alpha_target, dt / (alpha_target > gallery_alpha_ ? 150.0f : 280.0f));
        changed = animating = true;
    }
    // A playing video folds the filmstrip away.
    const bool playing = media_kind_ == 2 && video_.active() && video_.Snapshot().playing;
    const float open_target = playing ? 0.0f : 1.0f;
    if (gallery_open_ != open_target) {
        gallery_open_ = Approach(gallery_open_, open_target, dt / 200.0f);
        changed = animating = true;
    }
    // The strip glides to the current tile.
    if (GalleryActive() && gallery_scroll_init_) {
        const float target = static_cast<float>(GalleryRank(gallery_cursor_) - 1);
        const float diff = target - gallery_scroll_;
        if (std::abs(diff) > 0.002f) {
            if (std::abs(diff) > 12.0f) gallery_scroll_ = target - (diff > 0 ? 12.0f : -12.0f);
            gallery_scroll_ += (target - gallery_scroll_) * (1.0f - std::exp(-dt / 70.0f));
            if (std::abs(target - gallery_scroll_) <= 0.002f) gallery_scroll_ = target;
            changed = animating = true;
        }
    }
    if (ambient_prev_.get()) {
        changed = true;
        animating = animating || now - ambient_fade_at_ < kAmbientFadeMs + 40;
    }
    if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
    const UINT next = animating ? kAnimMs : kPollMs;
    if (next != gallery_timer_ms_) {
        gallery_timer_ms_ = next;
        SetTimer(hwnd_, kGalleryTimer, next, nullptr);
    }
}

}  // namespace pulse::ui
