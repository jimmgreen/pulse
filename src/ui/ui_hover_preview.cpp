#include "../common/volume_path.h"
// ui_hover_preview.cpp - The hover preview card (HoverPreviewView): an
// acrylic flyout beside the hovered file with its preview, an icon tile with
// the name and type, and a strip of three key facts.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "ffmpeg_playback.h"
#include "snack_style.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace pulse::ui {
namespace {

// 别名: the card names an item the way the list does (empty: none shown).
std::wstring ShownAlias(const std::wstring& path) {
    return path.empty() ? std::wstring() : app::AliasStore::Instance().Shown(path);
}
bool AliasWithRealName() { return app::AliasStore::Instance().Display() == app::AliasDisplay::Both; }

// Card geometry in DIPs.
constexpr float kRadius = 10.0f;
constexpr float kInset = 6.0f;        // around the preview well
constexpr float kTile = 40.0f;        // icon tile
constexpr float kGap = 10.0f;
constexpr float kStatsHeight = 46.0f;
constexpr float kBottom = 12.0f;
constexpr float kInfoHeight = kGap + kTile + kGap + kStatsHeight + kBottom;  // under the well
constexpr float kBareTop = 14.0f;     // no preview: the tile starts the card
constexpr float kMinCardWidth = 320.0f;  // the name, the type line and the three facts
constexpr float kMinWellHeight = 96.0f;
// 卡片尺寸 小 / 中 / 大: the base edge the shape is measured from.
constexpr float kBaseEdge[4] = {280.0f, 380.0f, 500.0f, 660.0f};  // 小 / 中 / 大 / 特大
constexpr float kZoom = 1.75f;        // Ctrl held

// What the card shows, and so its shape.
enum HoverShape : int {
    kShapeGuess = 0,    // nothing decoded yet: a shape from the extension
    kShapePicture = 1,  // a bitmap (photo, page sketch, poster, video)
    kShapeText = 2,     // the first lines of a text file
    kShapeBare = 3,     // no preview: the icon tile, name and facts only
};

struct Fact {
    std::wstring label, value;
};

FILETIME ToFileTime(uint64_t value) {
    FILETIME time{};
    time.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFull);
    time.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return time;
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towupper(c));
    return ext;
}

// Up to two facts the preview host read from the file (尺寸, 时长, 页数 ...),
// then its size; the modified date and the format fill a short list.
std::vector<Fact> Facts(const HoverPreviewView& view, const std::vector<PreviewProperty>& properties) {
    std::vector<Fact> facts;
    for (const auto& property : properties) {
        if (facts.size() >= 2) break;
        if (property.label.empty() || property.value.empty() || property.value.size() > 24) continue;
        facts.push_back({property.label, property.value});
    }
    facts.push_back({l10n::Get(l10n::StringId::GroupBySize).c_str(), format::ByteSize(view.size)});
    if (facts.size() < 3 && view.modified) {
        std::wstring date = format::LocalFileTime(ToFileTime(view.modified));
        if (date.size() > 10) date.resize(10);  // the day; the type line has the time
        if (!date.empty()) facts.push_back({l10n::Get(l10n::StringId::Modified).c_str(), date});
    }
    if (facts.size() < 3) {
        const std::wstring ext = Extension(view.name);
        facts.push_back({l10n::Get(l10n::StringId::Format).c_str(), ext.empty() ? std::wstring(L"-") : ext});
    }
    return facts;
}

// Word / Excel / PowerPoint / WPS / OpenDocument, by extension.
bool OfficeDocument(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* known : {L".doc", L".docx", L".docm", L".dotx", L".rtf", L".wps", L".xls", L".xlsx",
                                 L".xlsm", L".et", L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".dps",
                                 L".odt", L".ods", L".odp", L".odg"})
        if (ext == known) return true;
    return false;
}

// HEIF photos carry a small thumbnail the host returns in about 60 ms; the
// card asks for it first (kHeifQuickEdge) and for the photo once it is in.
constexpr uint32_t kHeifQuickEdge = 320;
bool HeifPhoto(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".heic" || ext == L".heif" || ext == L".hif" || ext == L".avif";
}

// The first guess at a file's proportions, before its preview is decoded:
// pages are A4 portrait, slides and clips 16:9, sheets 4:3.
float GuessAspect(const std::wstring& name, bool video) {
    if (video) return 16.0f / 9.0f;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* page : {L".doc", L".docx", L".docm", L".dotx", L".rtf", L".wps", L".odt", L".pdf",
                                L".epub", L".xps", L".oxps", L".djvu"})
        if (ext == page) return 210.0f / 297.0f;
    for (const wchar_t* slide : {L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".dps", L".odp"})
        if (ext == slide) return 16.0f / 9.0f;
    for (const wchar_t* sheet : {L".xls", L".xlsx", L".xlsm", L".et", L".ods"})
        if (ext == sheet) return 4.0f / 3.0f;
    return 16.0f / 10.0f;
}

// Files whose preview is only ever a hex dump or a listing (programs,
// archives) or only sometimes a picture (audio cover art, which grows the
// card when it arrives): the compact card from the first frame, rather than a picture-shaped card that shrinks once the
// answer arrives (browsing opens cards before the host has answered).
bool GuessBare(const std::wstring& path, const std::wstring& name) {
    if (VideoPreview::IsAudio(path)) return true;
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* bare : {L".exe", L".dll", L".msi", L".sys", L".zip", L".rar", L".7z", L".cab",
                                L".iso", L".tar", L".gz", L".tgz", L".bz2", L".xz", L".zst"})
        if (ext == bare) return true;
    return false;
}

// The first lines of a text preview, tabs as four spaces: enough to fill
// the well, never a whole file through DirectWrite every frame.
std::wstring Excerpt(const std::wstring& text, int max_lines) {
    std::wstring out;
    int lines = 0;
    for (size_t i = 0; i < text.size() && out.size() < 3000; ++i) {
        const wchar_t c = text[i];
        if (c == L'\n' && ++lines >= max_lines) break;
        if (c == L'\t') out += L"    ";
        else out += c;
    }
    return out;
}

// ---- Folder cards ------------------------------------------------------

std::wstring Thousands(uint64_t n) {
    const std::wstring digits = std::to_wstring(n);
    std::wstring out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

std::wstring ItemCount(uint64_t n) {
    return Thousands(n) + (n == 1 ? l10n::Get(l10n::StringId::Item).c_str() : l10n::Get(l10n::StringId::Items).c_str());
}

// "位于 …": the parent folder's own name ("C:" for a drive root), without
// the \\?\ long-path prefix.
std::wstring ParentName(const std::wstring& path) {
    std::wstring parent = fs::ParentPath(path);
    if (parent.empty() || parent == path) return {};
    if (parent.starts_with(L"\\\\?\\UNC\\")) parent = L"\\\\" + parent.substr(8);
    else if (parent.starts_with(L"\\\\?\\")) parent = parent.substr(4);
    while (parent.size() > 1 && parent.back() == L'\\') parent.pop_back();
    const size_t slash = parent.find_last_of(L'\\');
    return slash == std::wstring::npos || slash + 1 >= parent.size() ? parent : parent.substr(slash + 1);
}

std::wstring ChildPath(const std::wstring& parent, const std::wstring& name) {
    if (parent.empty() || parent.back() == L'\\') return parent + name;
    return parent + L'\\' + name;
}

const wchar_t* KindName(int kind) {
    switch (kind) {
    case kHoverKindImage: return l10n::Get(l10n::StringId::Images).c_str();
    case kHoverKindVideo: return l10n::Get(l10n::StringId::Videos).c_str();
    case kHoverKindAudio: return l10n::Get(l10n::StringId::KindAudio).c_str();
    case kHoverKindDocument: return l10n::Get(l10n::StringId::PickerDocuments).c_str();
    case kHoverKindCode: return l10n::Get(l10n::StringId::KindCode).c_str();
    case kHoverKindArchive: return l10n::Get(l10n::StringId::ArcFamArchive).c_str();
    default: return l10n::Get(l10n::StringId::GroupNameOther).c_str();
    }
}

D2D1_COLOR_F KindColor(int kind) {
    switch (kind) {
    case kHoverKindImage: return D2D1::ColorF(0x2FA37A);
    case kHoverKindVideo: return D2D1::ColorF(0xC8507E);
    case kHoverKindAudio: return D2D1::ColorF(0xE0A33A);
    case kHoverKindDocument: return D2D1::ColorF(0x3F7BE0);
    case kHoverKindCode: return D2D1::ColorF(0x8A63D2);
    case kHoverKindArchive: return D2D1::ColorF(0xA8742A);
    default: return D2D1::ColorF(0xA0A4AA);
    }
}

}  // namespace

VideoPreview::State MainRenderer::HoverVideoState() const {
    return hover_video_ ? hover_video_->Snapshot() : VideoPreview::State{};
}

void MainRenderer::DrawHoverPreview(const HoverPreviewView& view, const D2D1_RECT_F& client,
                                    const Theme& theme, bool high_contrast) {
    hover_frame_ms_ = 0;
    const bool suspended = path::OnAnyVolume(view.path, suspended_volumes_);
    hover_image_animation_.Bind(view.path, view.modified, view.size,
        view.visible && !view.folder && !view.video && !view.instant && !suspended &&
        (!view.owner || !IsIconic(view.owner)));
    if (suspended) return;
    const auto stop_video = [&] {
        if (hover_video_ && hover_video_->active()) hover_video_->Reset();
        hover_video_path_.clear();
        hover_video_failed_ = false;
        hover_frame_serial_ = ~0ull;
    };
    if (!compositor_ || !compositor_->Dc()) {
        stop_video();
        return;
    }
    if (hover_bench_) return;
    // Office cards use budgeted thumbnail uploads. Renew their allowance on
    // every paint, including timer-only paints while the pointer stays still.
    hover_cache_.BeginFrame();
    hover_card_rect_ = D2D1_RECT_F{};
    hover_list_rect_ = D2D1_RECT_F{};
    hover_scroll_max_ = 0.0f;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    if (!view.visible) {
        stop_video();
        hover_card_w_ = hover_well_h_ = 0.0f;  // the next card opens at its size
        // The dwell on a slow format: an accent line fills the row's foot
        // until the card opens.
        if (view.dwell && view.dwell_row.right - view.dwell_row.left > 24.0f * s) {
            const SnackPalette palette = MakeSnackPalette(theme, high_contrast);
            ComPtr<ID2D1SolidColorBrush> line;
            if (SUCCEEDED(dc->CreateSolidColorBrush(palette.accent, &line)) && line.get()) {
                const D2D1_RECT_F row = view.dwell_row;
                const float left = row.left + 6.0f * s, right = row.right - 6.0f * s;
                const float p = std::clamp(view.dwell_progress, 0.0f, 1.0f);
                if (p > 0.0f) {
                    line->SetOpacity(0.9f);
                    const D2D1_RECT_F r = D2D1::RectF(left, row.bottom - 2.5f * s, left + (right - left) * p,
                                                      row.bottom - 0.5f * s);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 1.0f * s, 1.0f * s), line.get());
                }
            }
            hover_frame_ms_ = 16;
        }
        return;
    }
    if (view.folder) {
        stop_video();
        hover_shape_path_.clear();
        hover_card_w_ = hover_well_h_ = 0.0f;
        DrawHoverFolderCard(view, client, theme, high_contrast);
        return;
    }
    const float margin = 8.0f * s;
    const float client_w = client.right - client.left, client_h = client.bottom - client.top;

    // The shape: what earlier frames learned of this file's preview, else a
    // guess from its extension.
    if (hover_shape_path_ != view.path) {
        hover_shape_path_ = view.path;
        hover_shape_kind_ = !view.video && GuessBare(view.path, view.name) ? kShapeBare : kShapeGuess;
        hover_shape_aspect_ = GuessAspect(view.name, view.video);
        hover_card_w_ = hover_well_h_ = 0.0f;
    }
    if (view.video && hover_shape_kind_ == kShapeBare) hover_shape_kind_ = kShapeGuess;  // the loop still plays
    const bool bare = hover_shape_kind_ == kShapeBare;
    const bool textual_shape = hover_shape_kind_ == kShapeText;
    const int laid_kind = hover_shape_kind_;
    const float laid_aspect = hover_shape_aspect_;

    // Its size: the 小/中/大/特大 base edge (much larger while Ctrl is held),
    // never more than 45% of the window's width and 80% of its height (特大:
    // 60% and 88%).
    const bool xlarge = view.size_step >= 3;
    float base = kBaseEdge[std::clamp(view.size_step, 0, 3)] * s;
    float max_w = client_w * (xlarge ? 0.6f : 0.45f), max_h = client_h * (xlarge ? 0.88f : 0.8f);
    if (view.zoom) {
        base *= kZoom;
        max_w = client_w * 0.7f;
        max_h = client_h * 0.92f;
    }
    const float min_w = (std::min)(kMinCardWidth * s, client_w - 2.0f * margin);
    max_w = (std::max)(max_w, min_w);
    const float pad = 2.0f * kInset * s;
    const float max_well_h = max_h - (kInset + kInfoHeight) * s;
    float target_w = min_w, target_h = 0.0f;
    if (!bare) {
        float ww = 0.0f, wh = 0.0f;
        const float a = (std::max)(hover_shape_aspect_, 0.2f);
        if (textual_shape) {
            ww = (std::min)(base * 1.1f, max_w) - pad;
            wh = (std::min)(base * 0.66f, max_well_h);
        } else if (a >= 1.15f) {          // landscape: as wide as the step allows
            ww = (std::min)(base * 1.25f, max_w) - pad;
            wh = (std::min)(ww / a, max_well_h);
        } else if (a <= 0.87f) {          // portrait: as tall as the step allows
            wh = (std::min)(base * 0.95f, max_well_h);
            ww = wh * a;
            if (ww > max_w - pad) {
                ww = max_w - pad;
                wh = ww / a;
            }
        } else {                          // about square
            ww = (std::min)(base, max_w) - pad;
            wh = (std::min)(ww / a, max_well_h);
        }
        target_w = std::clamp(ww + pad, min_w, max_w);
        target_h = (std::max)(wh, kMinWellHeight * s);
    }

    // Eased toward a new shape (the preview arrived, Ctrl went down or up);
    // a new card opens at its size.
    const unsigned long long now = GetTickCount64();
    if (view.instant || hover_card_w_ <= 0.0f) {
        hover_card_w_ = target_w;
        hover_well_h_ = target_h;
    } else {
        const float dt = static_cast<float>((std::min)(now - hover_ease_ms_, 100ull));
        const float k = 1.0f - std::exp(-dt / 45.0f);
        hover_card_w_ += (target_w - hover_card_w_) * k;
        hover_well_h_ += (target_h - hover_well_h_) * k;
        if (std::fabs(target_w - hover_card_w_) < 0.5f) hover_card_w_ = target_w;
        if (std::fabs(target_h - hover_well_h_) < 0.5f) hover_well_h_ = target_h;
    }
    hover_ease_ms_ = now;
    const bool easing = hover_card_w_ != target_w || hover_well_h_ != target_h;
    const float width = std::round(hover_card_w_);
    const float well_h = std::round(hover_well_h_);
    const bool show_well = well_h >= 1.0f;
    const float height = show_well ? kInset * s + well_h + kInfoHeight * s
                                   : (kBareTop + kTile + kGap + kStatsHeight + kBottom) * s;
    if (width < 240.0f * s || height > client_h - 2.0f * margin) {
        stop_video();
        return;
    }

    // Beside the pointer on wide rows (details, list), beside the cell in
    // icon grids; flipped to the other side when it would leave the window.
    const float anchor_w = view.anchor.right - view.anchor.left;
    const float anchor_h = view.anchor.bottom - view.anchor.top;
    float x = 0.0f, y = 0.0f;
    if (anchor_w > anchor_h * 3.0f) {
        x = view.pointer.x + 20.0f * s;
        if (x + width > client.right - margin) x = view.pointer.x - 20.0f * s - width;
        y = (view.anchor.top + view.anchor.bottom - height) * 0.5f;
    } else {
        x = view.anchor.right + 8.0f * s;
        if (x + width > client.right - margin) x = view.anchor.left - 8.0f * s - width;
        y = view.anchor.top;
    }
    x = std::round(std::clamp(x, client.left + margin, client.right - margin - width));
    y = std::round(std::clamp(y, client.top + margin, client.bottom - margin - height));
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + width, y + height);
    hover_card_rect_ = card;

    const SnackPalette palette = MakeSnackPalette(theme, high_contrast);
    DrawSnackPlate(dc, card, kRadius * s, palette, s, high_contrast);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.hover, &brush)) || !brush.get()) return;
    const auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get());
    };
    const auto text = [&](std::wstring_view value, const D2D1_RECT_F& r, IDWriteTextFormat* format,
                          const D2D1_COLOR_F& color) {
        painter_.DrawText(value, r, format, color, fluent::HorizontalAlignment::Left, palette.plate);
    };

    // The preview well: the picture, the first lines of text, or the icon.
    // A file without a preview has none; the cache is still asked (unseen)
    // so a late picture can grow the card.
    const D2D1_RECT_F well = show_well
        ? D2D1::RectF(card.left + kInset * s, card.top + kInset * s, card.right - kInset * s,
                      card.top + kInset * s + well_h)
        : D2D1::RectF(card.left + kInset * s, card.top, card.right - kInset * s, card.top);
    if (show_well) fill(well, (kRadius - kInset * 0.5f) * s, palette.hover);
    const D2D1_RECT_F content = show_well
        ? D2D1::RectF(well.left + 4.0f * s, well.top + 4.0f * s, well.right - 4.0f * s, well.bottom - 4.0f * s)
        : D2D1::RectF(card.left, card.top, card.left + 1.0f, card.top + 1.0f);
    // A playing clip replaces the poster. Take one snapshot for this frame,
    // and keep the poster path only until an actual video image is available.
    VideoPreview::State video;
    uint64_t serial = 0;
    std::shared_ptr<const FfmpegFrame> picture;
    if (view.video && view.owner && hover_video_ && hover_video_path_ == view.path) {
        video = hover_video_->Snapshot();
        if (video.ffmpeg) picture = hover_video_->Frame(serial);
    }
    const bool video_showing = !hover_video_failed_ && video.ready && SUCCEEDED(video.error) &&
        (!video.missing_decoder || video.ffmpeg) &&
        (video.ffmpeg ? picture && picture->width && picture->height : video.position > 0);
    uint32_t decoded_w = 0, decoded_h = 0;
    std::wstring preview_text;
    // Office documents ask for the thumbnail kind of preview: the embedded
    // or shell thumbnail, else the first-page sketch. The details kind only
    // tries the shell provider and leaves the icon when there is none (.doc
    // without Office); Quick Look shows those through the preview handler,
    // too slow and focus-hungry for a card.
    // Audio asks as well: the host reads the cover from the tags in a few ms
    // and, for the card (kPreviewRequestFlagCard), fails at once when there
    // is none instead of trying the shell and a hex dump.
    // HEIF: the embedded thumbnail first, then the photo; while the photo
    // decodes the cache keeps drawing the thumbnail (a stale size).
    const bool heif_wait = !video_showing && HeifPhoto(view.name) &&
        hover_cache_.Draw(dc, content, view.path, view.attrs, kHeifQuickEdge, 1, view.modified, view.size, 0.0f,
            nullptr, nullptr, nullptr, true) == PreviewDrawResult::Pending;
    const auto animation_now = GetTickCount64();
    const auto animation_frame = hover_image_animation_.Request(animation_now);
    uint32_t animation_count = 1, animation_delay = 0;
    PreviewDrawResult result = video_showing ? PreviewDrawResult::Bitmap : heif_wait ? PreviewDrawResult::Pending :
        hover_cache_.Draw(dc, content, view.path, view.attrs, 1024u, 1,
        view.modified, view.size, show_well ? 1.0f : 0.0f, &preview_text, nullptr, nullptr,
        !OfficeDocument(view.name), nullptr, nullptr, nullptr, nullptr, nullptr,
        animation_frame, &animation_count, &animation_delay, nullptr,
        &decoded_w, &decoded_h);
    if (!view.video && !view.instant) {
        if (result == PreviewDrawResult::Bitmap)
            hover_image_animation_.Commit(animation_count, animation_delay, animation_now);
        else if (hover_image_animation_.Waiting()) {
            const auto fallback = hover_cache_.Draw(dc, content, view.path, view.attrs, 1024u, 1,
                view.modified, view.size, show_well ? 1.0f : 0.0f, nullptr, nullptr, nullptr, true,
                nullptr, nullptr, nullptr, nullptr, nullptr, hover_image_animation_.Current(),
                nullptr, nullptr, nullptr, &decoded_w, &decoded_h);
            if (result != PreviewDrawResult::Pending) hover_image_animation_.Stop();
            if (fallback == PreviewDrawResult::Bitmap) result = fallback;
        }
        if (const auto delay = hover_image_animation_.NextDelay(animation_now)) hover_frame_ms_ = delay;
    }
    const bool textual = result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex ||
                         result == PreviewDrawResult::Markdown;
    // The facts after the preview: the host works one request at a time, and
    // a slow property read (video duration, HEIC metadata) asked for first
    // used to hold the picture back.
    std::vector<PreviewProperty> properties;
    hover_cache_.Properties(view.path, view.attrs, 1, view.modified, view.size, properties);
    // What this frame learned of the shape; the next frame eases to it.
    if (result == PreviewDrawResult::Bitmap && decoded_w && decoded_h) {
        hover_shape_kind_ = kShapePicture;
        if (!view.video || hover_shape_aspect_ <= 0.0f)
            hover_shape_aspect_ = static_cast<float>(decoded_w) / static_cast<float>(decoded_h);
    } else if (textual && result != PreviewDrawResult::Hex) {
        hover_shape_kind_ = kShapeText;
    } else if (result != PreviewDrawResult::Pending && !view.video) {
        // A hex dump (exe, dll, unknown binaries) tells nothing at a glance:
        // the compact card, like files with no preview at all.
        hover_shape_kind_ = kShapeBare;
    }
    if (!show_well) {
        // nothing to draw in a well that is not there
    } else if (textual) {
        if (!preview_mono_format_.get() && compositor_->DwriteFactory()) {
            typography::CreateTextFormat(compositor_->DwriteFactory(),
                {typography::FontRole::Monospace, 11.0f * s}, &preview_mono_format_);
            if (preview_mono_format_.get()) {
                preview_mono_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                preview_mono_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }
        const D2D1_RECT_F lines = D2D1::RectF(content.left + 8.0f * s, content.top + 6.0f * s,
                                              content.right - 4.0f * s, content.bottom - 2.0f * s);
        if (preview_text.empty()) {
            text(l10n::Get(l10n::StringId::EmptyFile), lines, compositor_->SmallFormat(), palette.text_secondary);
        } else if (preview_mono_format_.get()) {
            const float line_h = 11.0f * s * 1.34f;
            const int max_lines = std::clamp(static_cast<int>((lines.bottom - lines.top) / line_h) + 1, 4, 80);
            const std::wstring excerpt = Excerpt(preview_text, max_lines);
            brush->SetColor(palette.text);
            dc->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->DrawText(excerpt.data(), static_cast<UINT32>(excerpt.size()), preview_mono_format_.get(),
                         lines, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->PopAxisAlignedClip();
        }
    } else if (result != PreviewDrawResult::Bitmap) {
        // No picture of this file (yet): its icon, dimmed while the host works.
        const float icon = 64.0f * s;
        const float cx = (well.left + well.right) * 0.5f, cy = (well.top + well.bottom) * 0.5f;
        icon_cache_.Draw(dc, D2D1::RectF(cx - icon * 0.5f, cy - icon * 0.5f, cx + icon * 0.5f, cy + icon * 0.5f),
                         view.path, view.name, false, view.attrs,
                         result == PreviewDrawResult::Pending ? 0.45f : 1.0f);
    }

    // Videos: the poster above stays until the silent loop has a picture.
    bool loading = view.slow && result == PreviewDrawResult::Pending;
    const D2D1_RECT_F track = D2D1::RectF(well.left + 12.0f * s, well.bottom - 3.0f * s,
                                          well.right - 12.0f * s, well.bottom - 1.0f * s);
    if (view.video && view.owner) {
        // Opened only once the card has settled: browsing down a column of
        // clips must not spin up a player per row.
        const bool settled = GetTickCount64() - view.shown_ms >= 220;
        if (hover_video_path_ != view.path) {
            if (hover_video_ && hover_video_->active()) hover_video_->Reset();
            hover_video_path_.clear();
            hover_video_failed_ = false;
            hover_frame_serial_ = ~0ull;
            if (settled) {
                if (!hover_video_) hover_video_ = std::make_shared<VideoPreview>();
                hover_video_->Open(view.owner, view.path, true);
                hover_video_path_ = view.path;
            }
        }
        // No picture to show: an error, or Media Foundation would only play
        // the (muted) sound because the video decoder is missing.
        if (!hover_video_failed_ && hover_video_path_ == view.path &&
            (FAILED(video.error) || (video.ready && video.missing_decoder && !video.ffmpeg))) {
            hover_video_failed_ = true;
            hover_video_->Reset();
        }
        if (!hover_video_failed_ && video.ended) hover_video_->Play(true);  // loop
        if (video.width && video.height) {
            const float a = static_cast<float>(video.width) / static_cast<float>(video.height);
            if (std::fabs(a - hover_shape_aspect_) > 0.01f || hover_shape_kind_ != kShapePicture) {
                hover_shape_aspect_ = a;
                hover_shape_kind_ = kShapePicture;
            }
        }
        D2D1_RECT_F frame = content;
        if (video.width && video.height) {
            const float bw = content.right - content.left, bh = content.bottom - content.top;
            const float k = (std::min)(bw / static_cast<float>(video.width), bh / static_cast<float>(video.height));
            const float w = static_cast<float>(video.width) * k, h = static_cast<float>(video.height) * k;
            const float cx = (content.left + content.right) * 0.5f, cy = (content.top + content.bottom) * 0.5f;
            frame = D2D1::RectF(std::round(cx - w * 0.5f), std::round(cy - h * 0.5f),
                                std::round(cx + w * 0.5f), std::round(cy + h * 0.5f));
        }
        const bool showing = video_showing;
        if (hover_video_ && hover_video_->active()) {
            const RECT bounds{std::lround(frame.left), std::lround(frame.top), std::lround(frame.right),
                              std::lround(frame.bottom)};
            hover_video_->Layout(bounds, showing && !video.ffmpeg, static_cast<int>(std::lround(5.0f * s)));
        }
        if (showing && video.ffmpeg) {
            ComPtr<ID2D1Device> device;
            dc->GetDevice(&device);
            const D2D1_SIZE_U size = D2D1::SizeU(picture->width, picture->height);
            bool upload = serial != hover_frame_serial_;
            if (!hover_frame_bitmap_.get() || hover_frame_device_.get() != device.get() ||
                hover_frame_bitmap_->GetPixelSize().width != size.width ||
                hover_frame_bitmap_->GetPixelSize().height != size.height) {
                hover_frame_bitmap_.reset();
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                if (SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, props, &hover_frame_bitmap_)))
                    hover_frame_device_ = std::move(device);
                upload = true;
            }
            if (hover_frame_bitmap_.get()) {
                if (upload) {
                    hover_frame_bitmap_->CopyFromMemory(nullptr, picture->pixels.data(), picture->width * 4);
                    hover_frame_serial_ = serial;
                }
                dc->DrawBitmap(hover_frame_bitmap_.get(), frame, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
        if (showing) {
            // Where the loop is, in a hairline under the picture.
            const double fraction = video.duration > 0
                ? std::clamp(static_cast<double>(video.position) / static_cast<double>(video.duration), 0.0, 1.0)
                : 0.0;
            fill(track, 1.0f * s, palette.divider);
            if (fraction > 0.0) {
                fill(D2D1::RectF(track.left, track.top,
                                 track.left + (track.right - track.left) * static_cast<float>(fraction), track.bottom),
                     1.0f * s, palette.accent);
            }
            hover_frame_ms_ = 200;  // progress, and the loop's restart
        } else if (!hover_video_failed_) {
            loading = true;
        }
    } else if (hover_video_path_.size() || (hover_video_ && hover_video_->active())) {
        stop_video();
    }
    if (loading) {
        // Still decoding: a short accent segment sweeps the track.
        fill(track, 1.0f * s, palette.divider);
        const float span = track.right - track.left, seg = span * 0.3f;
        const float t = static_cast<float>(GetTickCount64() % 1200ull) / 1200.0f;
        const float head = track.left - seg + (span + seg) * t;
        dc->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fill(D2D1::RectF(head, track.top, head + seg, track.bottom), 1.0f * s, palette.accent);
        dc->PopAxisAlignedClip();
        hover_frame_ms_ = 33;
    }

    // Still easing, or this frame learned a new shape: another frame soon.
    if (easing || laid_kind != hover_shape_kind_ || std::fabs(laid_aspect - hover_shape_aspect_) > 0.01f)
        hover_frame_ms_ = hover_frame_ms_ ? (std::min)(hover_frame_ms_, 16u) : 16u;

    // Icon tile, name and type line.
    const float info_top = show_well ? well.bottom + kGap * s : card.top + kBareTop * s;
    const D2D1_RECT_F tile = D2D1::RectF(card.left + 14.0f * s, info_top,
                                         card.left + (14.0f + kTile) * s, info_top + kTile * s);
    fill(tile, 9.0f * s, palette.hover);
    brush->SetColor(palette.stroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f,
        tile.right - 0.5f, tile.bottom - 0.5f), 9.0f * s, 9.0f * s), brush.get(), 1.0f);
    icon_cache_.Draw(dc, D2D1::RectF(tile.left + 6.0f * s, tile.top + 6.0f * s,
                                     tile.right - 6.0f * s, tile.bottom - 6.0f * s),
                     view.path, view.name, false, view.attrs);
    const float text_left = tile.right + 12.0f * s, text_right = card.right - 14.0f * s;
    IDWriteTextFormat* name_format = compositor_->FileNameFormat() ? compositor_->FileNameFormat()
                                                                   : compositor_->TextFormat();
    const std::wstring file_alias = ShownAlias(view.path);
    text(file_alias.empty() ? view.name : file_alias,
         D2D1::RectF(text_left, tile.top - 3.0f * s, text_right, tile.top + 21.0f * s), name_format, palette.text);
    std::wstring type_line = view.type_text;
    if (!file_alias.empty() && AliasWithRealName())
        type_line = view.name + (type_line.empty() ? L"" : L" \u00B7 ") + type_line;
    if (view.modified) {
        const std::wstring when = format::LocalFileTime(ToFileTime(view.modified));
        if (!when.empty()) type_line += (type_line.empty() ? L"" : L" · ") + when;
    }
    text(type_line, D2D1::RectF(text_left, tile.top + 21.0f * s, text_right, tile.bottom + 3.0f * s),
         compositor_->SmallFormat(), palette.text_secondary);

    // Three facts in a quiet strip: label above value.
    const D2D1_RECT_F strip = D2D1::RectF(card.left + 12.0f * s, tile.bottom + kGap * s,
                                          card.right - 12.0f * s, tile.bottom + (kGap + kStatsHeight) * s);
    fill(strip, 8.0f * s, palette.hover);
    const std::vector<Fact> facts = Facts(view, properties);
    const float cell = (strip.right - strip.left) / 3.0f;
    for (size_t i = 0; i < 3 && i < facts.size(); ++i) {
        const float left = strip.left + cell * static_cast<float>(i);
        if (i) {
            brush->SetColor(palette.divider);
            dc->FillRectangle(D2D1::RectF(left, strip.top + 10.0f * s, left + 1.0f, strip.bottom - 10.0f * s),
                              brush.get());
        }
        text(facts[i].label, D2D1::RectF(left + 10.0f * s, strip.top + 5.0f * s, left + cell - 6.0f * s,
                                         strip.top + 22.0f * s),
             compositor_->SmallFormat(), palette.text_secondary);
        text(facts[i].value, D2D1::RectF(left + 10.0f * s, strip.top + 21.0f * s, left + cell - 6.0f * s,
                                         strip.bottom - 2.0f * s),
             compositor_->TextFormat(), palette.text);
    }
}

// A folder's card: what is inside (a scrolling list, or a picture mosaic
// when most of its files are images and clips; a drive adds its capacity),
// the mix of file kinds, then the icon tile, name and the three facts.
void MainRenderer::DrawHoverFolderCard(const HoverPreviewView& view, const D2D1_RECT_F& client,
                                       const Theme& theme, bool high_contrast) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    const float margin = 8.0f * s;
    const float client_w = client.right - client.left, client_h = client.bottom - client.top;
    using State = HoverFolderInfo::State;
    const HoverFolderInfo* info = view.listing.get();
    const bool reading = !info || info->state == State::Reading;
    const bool ready = info && info->state == State::Ready;
    const bool denied = info && info->state == State::Denied;
    const bool unreadable = denied || (info && info->state == State::Failed) || view.drive_locked;
    const uint64_t total = ready ? info->folders + info->files : 0;
    const size_t listed = ready ? info->items.size() : 0;

    // Width: the 小/中/大/特大 base edge (Ctrl: much larger), like a file's card.
    const bool xlarge = view.size_step >= 3;
    float base = kBaseEdge[std::clamp(view.size_step, 0, 3)] * s;
    float max_w = client_w * (xlarge ? 0.6f : 0.45f), max_h = client_h * (xlarge ? 0.88f : 0.8f);
    int rows_cap = view.size_step <= 0 ? 6 : view.size_step == 1 ? 8 : view.size_step == 2 ? 11 : 14;
    if (view.zoom) {
        base *= kZoom;
        max_w = client_w * 0.7f;
        max_h = client_h * 0.92f;
        rows_cap *= 2;
    }
    const float min_w = (std::min)(kMinCardWidth * s, client_w - 2.0f * margin);
    const float width = std::round(std::clamp(base, min_w, (std::max)(max_w, min_w)));

    // Pictures when most of the direct files are images or clips.
    std::vector<size_t> media;
    uint32_t pictures = 0;
    if (ready && !view.drive) {
        pictures = info->kinds[kHoverKindImage] + info->kinds[kHoverKindVideo];
        if (pictures >= 4 && pictures * 10 >= info->files * 6 && info->folders * 2 <= info->files) {
            for (size_t i = 0; i < listed && media.size() < 9; ++i) {
                const HoverFolderItem& item = info->items[i];
                if (!item.is_dir && (item.kind == kHoverKindImage || item.kind == kHoverKindVideo))
                    media.push_back(i);
            }
        }
    }
    const bool mosaic = media.size() >= 4;
    const bool truncated = ready && total > listed;

    // The well: a drive's capacity on top, then the list (or the mosaic).
    const float row_h = 26.0f * s;
    const float list_pad = 4.0f * s;
    const float well_w = width - 2.0f * kInset * s;
    const float meter_h = view.drive ? 50.0f * s : 0.0f;
    const float tile_gap = 3.0f * s;
    const float tile_w = std::floor((well_w - 8.0f * s - 2.0f * tile_gap) / 3.0f);
    const float tile_h = std::round(tile_w * 0.75f);
    float content_h = 0.0f;  // the list's full height
    float body_h = 0.0f;     // what of it the well shows
    if (reading) {
        content_h = body_h = 3.0f * row_h + 2.0f * list_pad;
    } else if (mosaic) {
        const int mosaic_rows = static_cast<int>((media.size() + 2) / 3);
        content_h = body_h = static_cast<float>(mosaic_rows) * tile_h + static_cast<float>(mosaic_rows - 1) * tile_gap + 8.0f * s;
    } else if (ready && (listed || truncated)) {
        const size_t rows = listed + (truncated ? 1 : 0);
        content_h = static_cast<float>(rows) * row_h + 2.0f * list_pad;
        body_h = (std::min)(content_h, static_cast<float>(rows_cap) * row_h + 2.0f * list_pad);
    }
    const bool composition = ready && !view.drive && info->files >= 3;
    const float composition_h = composition ? (kGap + 6.0f + 5.0f + 16.0f) * s : 0.0f;
    const float note_h = unreadable ? (kGap + 30.0f) * s : 0.0f;
    const bool hint = !view.drive && !unreadable;
    const float hint_h = hint ? 16.0f * s : 0.0f;
    const auto card_height = [&](float body) {
        const float well = meter_h + body;
        const float top = well >= 1.0f ? kInset * s + well + kGap * s : kBareTop * s;
        return top + composition_h + kTile * s + note_h + (kGap + kStatsHeight) * s + hint_h + kBottom * s;
    };
    // A short window: fewer list rows, never fewer than two.
    float height = card_height(body_h);
    const float room = (std::min)(max_h, client_h - 2.0f * margin);
    if (height > room && !mosaic && body_h > 0.0f) {
        body_h = (std::max)(2.0f * row_h + 2.0f * list_pad, body_h - (height - room));
        height = card_height(body_h);
    }
    if (width < 240.0f * s || height > client_h - 2.0f * margin) return;
    const float well_h = meter_h + body_h;
    const bool show_well = well_h >= 1.0f;

    // Beside the pointer on wide rows, beside the cell in icon grids (as a
    // file's card).
    const float anchor_w = view.anchor.right - view.anchor.left;
    const float anchor_h = view.anchor.bottom - view.anchor.top;
    float x = 0.0f, y = 0.0f;
    if (anchor_w > anchor_h * 3.0f) {
        x = view.pointer.x + 20.0f * s;
        if (x + width > client.right - margin) x = view.pointer.x - 20.0f * s - width;
        y = (view.anchor.top + view.anchor.bottom - height) * 0.5f;
    } else {
        x = view.anchor.right + 8.0f * s;
        if (x + width > client.right - margin) x = view.anchor.left - 8.0f * s - width;
        y = view.anchor.top;
    }
    x = std::round(std::clamp(x, client.left + margin, client.right - margin - width));
    y = std::round(std::clamp(y, client.top + margin, client.bottom - margin - height));
    const D2D1_RECT_F card = D2D1::RectF(x, y, x + width, y + height);
    hover_card_rect_ = card;

    const SnackPalette palette = MakeSnackPalette(theme, high_contrast);
    DrawSnackPlate(dc, card, kRadius * s, palette, s, high_contrast);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.hover, &brush)) || !brush.get()) return;
    const auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color) {
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.get());
    };
    const auto text = [&](std::wstring_view value, const D2D1_RECT_F& r, IDWriteTextFormat* format,
                          const D2D1_COLOR_F& color,
                          fluent::HorizontalAlignment align = fluent::HorizontalAlignment::Left) {
        painter_.DrawText(value, r, format, color, align, palette.plate);
    };

    const D2D1_RECT_F well = show_well
        ? D2D1::RectF(card.left + kInset * s, card.top + kInset * s, card.right - kInset * s,
                      card.top + kInset * s + well_h)
        : D2D1::RectF(card.left + kInset * s, card.top, card.right - kInset * s, card.top);
    if (show_well) fill(well, (kRadius - kInset * 0.5f) * s, palette.hover);

    // A drive: how full it is.
    if (view.drive && show_well) {
        const D2D1_RECT_F head = D2D1::RectF(well.left + 12.0f * s, well.top + 7.0f * s, well.right - 12.0f * s,
                                             well.top + 27.0f * s);
        if (view.drive_total) {
            const uint64_t used = view.drive_total > view.drive_free ? view.drive_total - view.drive_free : 0;
            text(format::ByteSize(used) + l10n::Get(l10n::StringId::Used).c_str(), head, compositor_->TextFormat(),
                 palette.text);
            text(l10n::Get(l10n::StringId::Of).c_str() + format::ByteSize(view.drive_total), head, compositor_->SmallFormat(),
                 palette.text_secondary, fluent::HorizontalAlignment::Right);
            const D2D1_RECT_F bar = D2D1::RectF(head.left, well.top + 33.0f * s, head.right, well.top + 41.0f * s);
            const float fraction = static_cast<float>(static_cast<double>(used) / static_cast<double>(view.drive_total));
            painter_.DrawCapacityBar(bar, fraction, palette.accent, 1.0f, view.drive_free);
        } else {
            text(l10n::Get(l10n::StringId::CapacityUnknown).c_str(), head, compositor_->SmallFormat(), palette.text_secondary);
        }
    }

    // The list (or the mosaic) under it.
    const D2D1_RECT_F list = D2D1::RectF(well.left + 4.0f * s, well.top + meter_h, well.right - 4.0f * s,
                                         well.bottom);
    if (show_well && body_h > 0.0f) {
        if (reading) {
            text(l10n::Get(l10n::StringId::Reading).c_str(),
                 D2D1::RectF(list.left + 10.0f * s, list.top + list_pad, list.right - 10.0f * s,
                             list.top + list_pad + row_h),
                 compositor_->SmallFormat(), palette.text_secondary);
            const D2D1_RECT_F track = D2D1::RectF(well.left + 12.0f * s, well.bottom - 3.0f * s,
                                                  well.right - 12.0f * s, well.bottom - 1.0f * s);
            fill(track, 1.0f * s, palette.divider);
            const float span = track.right - track.left, seg = span * 0.3f;
            const float t = static_cast<float>(GetTickCount64() % 1200ull) / 1200.0f;
            const float head = track.left - seg + (span + seg) * t;
            dc->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            fill(D2D1::RectF(head, track.top, head + seg, track.bottom), 1.0f * s, palette.accent);
            dc->PopAxisAlignedClip();
            hover_frame_ms_ = 33;
        } else if (mosaic) {
            const uint32_t pixels = tile_w <= 120.0f ? 128u : 256u;
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            for (size_t k = 0; k < media.size(); ++k) {
                const HoverFolderItem& item = info->items[media[k]];
                const float tx = list.left + static_cast<float>(k % 3) * (tile_w + tile_gap);
                const float ty = list.top + 4.0f * s + static_cast<float>(k / 3) * (tile_h + tile_gap);
                const D2D1_RECT_F tile = D2D1::RectF(tx, ty, tx + tile_w, ty + tile_h);
                fill(tile, 4.0f * s, palette.divider);
                const std::wstring path = ChildPath(view.path, item.name);
                dc->PushAxisAlignedClip(tile, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                D2D1_RECT_F art{};
                const PreviewDrawResult result = thumbnail_cache_.DrawGridThumbnail(
                    dc, tile, path, item.attrs, pixels, 1, item.modified, item.size, 1.0f, false, &art);
                const float art_w = art.right - art.left, art_h = art.bottom - art.top;
                if (result == PreviewDrawResult::Bitmap && art_w > 1.0f && art_h > 1.0f) {
                    // Fill the tile (cover, cropped by the clip) instead of
                    // leaving bars around a letterboxed picture.
                    const float grow = (std::max)(tile_w / art_w, tile_h / art_h);
                    if (grow > 1.01f) {
                        const float w = tile_w * grow, h = tile_h * grow;
                        const float mx = (tile.left + tile.right) * 0.5f, my = (tile.top + tile.bottom) * 0.5f;
                        thumbnail_cache_.DrawGridThumbnail(dc, D2D1::RectF(mx - w * 0.5f, my - h * 0.5f,
                            mx + w * 0.5f, my + h * 0.5f), path, item.attrs, pixels, 1, item.modified,
                            item.size, 1.0f, false, nullptr);
                    }
                }
                dc->PopAxisAlignedClip();
                const float cx = (tile.left + tile.right) * 0.5f, cy = (tile.top + tile.bottom) * 0.5f;
                // The last tile says how many more there are: no fallback
                // icon or play mark under the count.
                const bool more_tile = k + 1 == media.size() && pictures > media.size();
                if (result != PreviewDrawResult::Bitmap && !more_tile) {
                    const float icon = 28.0f * s;
                    icon_cache_.Draw(dc, D2D1::RectF(cx - icon * 0.5f, cy - icon * 0.5f, cx + icon * 0.5f,
                                                     cy + icon * 0.5f),
                                     path, item.name, false, item.attrs,
                                     result == PreviewDrawResult::Pending ? 0.45f : 1.0f);
                    if (result == PreviewDrawResult::Pending) hover_frame_ms_ = hover_frame_ms_ ? hover_frame_ms_ : 100;
                }
                if (more_tile && result == PreviewDrawResult::Pending)
                    hover_frame_ms_ = hover_frame_ms_ ? hover_frame_ms_ : 100;
                if (item.kind == kHoverKindVideo && factory.get() && !more_tile) {
                    // A clip: a play mark on its poster.
                    brush->SetColor(D2D1::ColorF(0x000000, 0.42f));
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 11.0f * s, 11.0f * s), brush.get());
                    ComPtr<ID2D1PathGeometry> play;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(&play)) && SUCCEEDED(play->Open(&sink))) {
                        sink->BeginFigure(D2D1::Point2F(cx - 3.5f * s, cy - 5.5f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(cx + 5.5f * s, cy));
                        sink->AddLine(D2D1::Point2F(cx - 3.5f * s, cy + 5.5f * s));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        brush->SetColor(D2D1::ColorF(0xFFFFFF, 0.95f));
                        dc->FillGeometry(play.get(), brush.get());
                    }
                }
                if (more_tile) {
                    fill(tile, 4.0f * s, D2D1::ColorF(0x000000, 0.45f));
                    text(L"+" + Thousands(pictures - static_cast<uint32_t>(media.size()) + 1), tile,
                         compositor_->TextFormat(), D2D1::ColorF(0xFFFFFF), fluent::HorizontalAlignment::Center);
                }
            }
        } else {
            // The children in the pane's order; the wheel scrolls them.
            const float view_h = list.bottom - list.top;
            hover_scroll_max_ = (std::max)(0.0f, content_h - view_h);
            const float scroll = std::clamp(view.folder_scroll, 0.0f, hover_scroll_max_);
            hover_list_rect_ = list;
            dc->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const size_t rows = listed + (truncated ? 1 : 0);
            const size_t first = static_cast<size_t>((std::max)(0.0f, (scroll - list_pad) / row_h));
            const float icon = 18.0f * s;
            for (size_t i = first; i < rows; ++i) {
                const float top = list.top + list_pad + static_cast<float>(i) * row_h - scroll;
                if (top >= list.bottom) break;
                const D2D1_RECT_F row = D2D1::RectF(list.left, top, list.right, top + row_h);
                if (i >= listed) {
                    text(l10n::Get(l10n::StringId::OnlyTheFirst).c_str() + Thousands(listed) +
                             l10n::Get(l10n::StringId::Listed).c_str() + ItemCount(total),
                         D2D1::RectF(row.left + 34.0f * s, row.top, row.right - 10.0f * s, row.bottom),
                         compositor_->SmallFormat(), palette.text_secondary);
                    break;
                }
                const HoverFolderItem& item = info->items[i];
                const float cy = (row.top + row.bottom) * 0.5f;
                icon_cache_.Draw(dc, D2D1::RectF(row.left + 8.0f * s, cy - icon * 0.5f, row.left + 8.0f * s + icon,
                                                 cy + icon * 0.5f),
                                 ChildPath(view.path, item.name), item.name, item.is_dir, item.attrs,
                                 (item.attrs & FILE_ATTRIBUTE_HIDDEN) ? 0.55f : 1.0f);
                std::wstring side;
                if (item.is_dir) {
                    if (item.children >= 0) side = ItemCount(static_cast<uint64_t>(item.children));
                } else {
                    side = format::ByteSize(item.size);
                }
                const float side_w = side.empty() ? 0.0f : 78.0f * s;
                const std::wstring item_alias = ShownAlias(ChildPath(view.path, item.name));
                text(item_alias.empty() ? item.name : item_alias,
                     D2D1::RectF(row.left + 34.0f * s, row.top, row.right - 12.0f * s - side_w, row.bottom),
                     compositor_->TextFormat(), palette.text);
                if (!side.empty())
                    text(side, D2D1::RectF(row.right - 10.0f * s - side_w, row.top, row.right - 10.0f * s, row.bottom),
                         compositor_->SmallFormat(), palette.text_secondary, fluent::HorizontalAlignment::Right);
            }
            dc->PopAxisAlignedClip();
            if (hover_scroll_max_ > 0.0f) {
                // Where in the list the well is.
                const float thumb_h = (std::max)(20.0f * s, view_h * view_h / content_h);
                const float thumb_top = list.top + (view_h - thumb_h) * (scroll / hover_scroll_max_);
                fill(D2D1::RectF(list.right - 3.0f * s, thumb_top + 2.0f * s, list.right - 0.5f * s,
                                 thumb_top + thumb_h - 2.0f * s),
                     1.25f * s, SnackAlpha(palette.text_secondary, 0.45f));
            }
        }
    }

    // The mix of file kinds: the four largest, the rest as 其他.
    float cursor = show_well ? well.bottom : card.top;
    if (composition) {
        std::vector<std::pair<int, uint32_t>> kinds;
        for (int k = 0; k < kHoverKindCount; ++k)
            if (info->kinds[static_cast<size_t>(k)]) kinds.push_back({k, info->kinds[static_cast<size_t>(k)]});
        std::stable_sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) {
            if ((a.first == kHoverKindOther) != (b.first == kHoverKindOther)) return b.first == kHoverKindOther;
            return a.second > b.second;
        });
        if (kinds.size() > 5) {
            uint32_t rest = 0;
            for (size_t k = 4; k < kinds.size(); ++k) rest += kinds[k].second;
            kinds.resize(4);
            kinds.push_back({kHoverKindOther, rest});
        }
        const D2D1_RECT_F bar = D2D1::RectF(card.left + 14.0f * s, cursor + kGap * s, card.right - 14.0f * s,
                                            cursor + (kGap + 6.0f) * s);
        const float span = bar.right - bar.left;
        const float files = static_cast<float>((std::max)(1ull, info->files));
        // Widest extent first: each kind is drawn from the bar's start, so
        // the rounded ends of the later (shorter) ones mark the joins.
        std::vector<float> ends;
        float acc = 0.0f;
        for (const auto& kind : kinds) {
            acc += static_cast<float>(kind.second) / files;
            ends.push_back(bar.left + span * (std::min)(1.0f, acc));
        }
        for (size_t k = kinds.size(); k-- > 0;)
            fill(D2D1::RectF(bar.left, bar.top, ends[k], bar.bottom), 3.0f * s, KindColor(kinds[k].first));
        const float column = span / static_cast<float>(kinds.size());
        for (size_t k = 0; k < kinds.size(); ++k) {
            const float left = bar.left + column * static_cast<float>(k);
            const float top = bar.bottom + 5.0f * s;
            fill(D2D1::RectF(left, top + 5.0f * s, left + 7.0f * s, top + 12.0f * s), 2.0f * s,
                 KindColor(kinds[k].first));
            const int percent = (std::max)(1, static_cast<int>(std::lround(100.0f * static_cast<float>(kinds[k].second) / files)));
            text(std::wstring(KindName(kinds[k].first)) + L" " + std::to_wstring(percent) + L"%",
                 D2D1::RectF(left + 11.0f * s, top, left + column - 2.0f * s, top + 16.0f * s),
                 compositor_->SmallFormat(), palette.text_secondary);
        }
        cursor = bar.bottom + (5.0f + 16.0f) * s;
    }

    // Icon tile, name and the type line.
    const float info_top = show_well ? cursor + kGap * s : card.top + kBareTop * s + composition_h;
    const D2D1_RECT_F tile = D2D1::RectF(card.left + 14.0f * s, info_top, card.left + (14.0f + kTile) * s,
                                         info_top + kTile * s);
    fill(tile, 9.0f * s, palette.hover);
    brush->SetColor(palette.stroke);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f,
        tile.right - 0.5f, tile.bottom - 0.5f), 9.0f * s, 9.0f * s), brush.get(), 1.0f);
    icon_cache_.Draw(dc, D2D1::RectF(tile.left + 6.0f * s, tile.top + 6.0f * s, tile.right - 6.0f * s,
                                     tile.bottom - 6.0f * s),
                     view.path, view.name, true, view.attrs);
    const float text_left = tile.right + 12.0f * s, text_right = card.right - 14.0f * s;
    IDWriteTextFormat* name_format = compositor_->FileNameFormat() ? compositor_->FileNameFormat()
                                                                   : compositor_->TextFormat();
    const std::wstring folder_alias = ShownAlias(view.path);
    text(folder_alias.empty() ? view.name : folder_alias,
         D2D1::RectF(text_left, tile.top - 3.0f * s, text_right, tile.top + 21.0f * s), name_format, palette.text);
    std::wstring type_line;
    const std::wstring dot = L" \u00B7 ";
    if (view.drive) {
        type_line = view.type_text;
        if (view.drive_total) type_line += dot + l10n::Get(l10n::StringId::Text3350).c_str() + format::ByteSize(view.drive_total);
    } else {
        if (ready && total == 0) {
            type_line = l10n::Get(l10n::StringId::EmptyFolder).c_str();
        } else if (ready) {
            if (info->folders)
                type_line = Thousands(info->folders) +
                    (info->folders == 1 ? l10n::Get(l10n::StringId::Folder_2).c_str() : l10n::Get(l10n::StringId::Folders).c_str());
            if (info->files)
                type_line += (type_line.empty() ? L"" : dot) + Thousands(info->files) +
                    (info->files == 1 ? l10n::Get(l10n::StringId::File_2).c_str() : l10n::Get(l10n::StringId::Files_3).c_str());
        } else {
            type_line = view.type_text;
        }
        const std::wstring parent = ParentName(view.path);
        if (!parent.empty()) type_line += dot + l10n::Get(l10n::StringId::In).c_str() + parent;
    }
    if (!folder_alias.empty() && AliasWithRealName())
        type_line = view.name + (type_line.empty() ? L"" : dot) + type_line;
    text(type_line, D2D1::RectF(text_left, tile.top + 21.0f * s, text_right, tile.bottom + 3.0f * s),
         compositor_->SmallFormat(), palette.text_secondary);
    float below = tile.bottom;

    // Why there is nothing to list.
    if (unreadable) {
        const D2D1_RECT_F note = D2D1::RectF(card.left + 12.0f * s, below + kGap * s, card.right - 12.0f * s,
                                             below + (kGap + 30.0f) * s);
        const D2D1_COLOR_F amber = palette.dark ? D2D1::ColorF(0xE8B04A) : D2D1::ColorF(0x97620C);
        fill(note, 7.0f * s, SnackAlpha(amber, palette.dark ? 0.14f : 0.10f));
        const wchar_t* why = view.drive_locked ? l10n::Get(l10n::StringId::LockedByBitLockerUnlockItToSeeInside).c_str()
                           : denied ? l10n::Get(l10n::StringId::CanTReadItsContentsAccessDenied).c_str()
                                    : l10n::Get(l10n::StringId::CanTReadItsContents).c_str();
        text(why, D2D1::RectF(note.left + 10.0f * s, note.top, note.right - 8.0f * s, note.bottom),
             compositor_->SmallFormat(), amber);
        below = note.bottom;
    }

    // Three facts in a quiet strip: label above value.
    std::vector<Fact> facts;
    bool size_fact_running = false;
    if (view.drive) {
        const int percent = view.drive_total
            ? static_cast<int>(std::lround(100.0 * static_cast<double>(view.drive_total - (std::min)(view.drive_free, view.drive_total)) /
                                           static_cast<double>(view.drive_total)))
            : 0;
        facts.push_back({l10n::Get(l10n::StringId::Free).c_str(), view.drive_total ? format::ByteSize(view.drive_free) : L"\u2014"});
        facts.push_back({l10n::Get(l10n::StringId::Used_2).c_str(), view.drive_total ? std::to_wstring(percent) + L"%" : L"\u2014"});
        facts.push_back({l10n::Get(l10n::StringId::FileSystem).c_str(),
                         info && !info->file_system.empty() ? info->file_system : std::wstring(L"\u2014")});
    } else {
        facts.push_back({l10n::Get(l10n::StringId::AdvSearchContains).c_str(),
                         ready ? ItemCount(total) : reading ? std::wstring(L"\u2026") : std::wstring(L"\u2014")});
        size_fact_running = view.size_running;
        facts.push_back({view.size_running ? l10n::Get(l10n::StringId::SizeCounting).c_str() : l10n::Get(l10n::StringId::GroupBySize).c_str(),
                         view.size_text.empty() ? std::wstring(L"\u2014") : view.size_text});
        std::wstring date = view.modified ? format::LocalFileTime(ToFileTime(view.modified)) : std::wstring();
        if (date.size() > 10) date.resize(10);
        facts.push_back({l10n::Get(l10n::StringId::Modified).c_str(), date.empty() ? std::wstring(L"\u2014") : date});
    }
    const D2D1_RECT_F strip = D2D1::RectF(card.left + 12.0f * s, below + kGap * s, card.right - 12.0f * s,
                                          below + (kGap + kStatsHeight) * s);
    fill(strip, 8.0f * s, palette.hover);
    const float cell = (strip.right - strip.left) / 3.0f;
    for (size_t i = 0; i < 3 && i < facts.size(); ++i) {
        const float left = strip.left + cell * static_cast<float>(i);
        if (i) {
            brush->SetColor(palette.divider);
            dc->FillRectangle(D2D1::RectF(left, strip.top + 10.0f * s, left + 1.0f, strip.bottom - 10.0f * s),
                              brush.get());
        }
        text(facts[i].label, D2D1::RectF(left + 10.0f * s, strip.top + 5.0f * s, left + cell - 6.0f * s,
                                         strip.top + 22.0f * s),
             compositor_->SmallFormat(), palette.text_secondary);
        text(facts[i].value, D2D1::RectF(left + 10.0f * s, strip.top + 21.0f * s, left + cell - 6.0f * s,
                                         strip.bottom - 2.0f * s),
             compositor_->TextFormat(), i == 1 && size_fact_running ? palette.accent : palette.text);
    }

    // What the card answers to.
    if (hint) {
        const std::wstring keys = hover_scroll_max_ > 0.0f
            ? l10n::Get(l10n::StringId::WheelToScrollSpaceForQuickLook).c_str()
            : l10n::Get(l10n::StringId::SpaceForQuickLook).c_str();
        text(keys, D2D1::RectF(card.left + 14.0f * s, strip.bottom + 3.0f * s, card.right - 14.0f * s,
                               strip.bottom + 3.0f * s + hint_h),
             compositor_->SmallFormat(), palette.text_secondary, fluent::HorizontalAlignment::Right);
    }
    if (view.size_running && !hover_frame_ms_) hover_frame_ms_ = 250;
}

namespace {
struct BenchClock {
    LARGE_INTEGER frequency{}, start{};
    BenchClock() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    double Ms() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }
};
}  // namespace

MainRenderer::HoverBenchSample MainRenderer::BenchHoverPreview(const std::wstring& path, DWORD attrs,
        uint64_t size, uint64_t modified, uint32_t pixel_size, uint32_t timeout_ms, bool properties_first) {
    HoverBenchSample sample;
    if (!compositor_ || !compositor_->Dc()) return sample;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ComPtr<ID2D1Bitmap1> target;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(512, 512), nullptr, 0, props, &target)) || !target.get()) return sample;
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const bool direct = !OfficeDocument(name);
    // The card asks at 1024 px; any other size is the warm run: the cache
    // is dropped first so the file is decoded again by the running host.
    const bool warm_run = pixel_size != 1024u;
    if (warm_run) hover_cache_.Evict();
    std::vector<PreviewProperty> properties;
    const BenchClock clock;
    // HEIF, as the card: the embedded thumbnail, timed on its own.
    if (!warm_run && HeifPhoto(name)) {
        for (;;) {
            hover_cache_.BeginFrame();
            dc->SetTarget(target.get());
            dc->BeginDraw();
            const PreviewDrawResult quick = hover_cache_.Draw(dc, D2D1::RectF(4.0f, 4.0f, 380.0f, 380.0f), path,
                attrs, kHeifQuickEdge, 1, modified, size, 1.0f, nullptr, nullptr, nullptr, true);
            dc->EndDraw();
            dc->SetTarget(previous.get());
            if (quick != PreviewDrawResult::Pending) {
                if (quick == PreviewDrawResult::Bitmap) sample.first_ms = clock.Ms();
                break;
            }
            if (clock.Ms() > static_cast<double>(timeout_ms)) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
        }
    }
    for (;;) {
        hover_cache_.BeginFrame();
        if (properties_first && sample.properties_ms < 0.0 &&
            hover_cache_.Properties(path, attrs, 1, modified, size, properties))
            sample.properties_ms = clock.Ms();
        dc->SetTarget(target.get());
        dc->BeginDraw();
        std::wstring text;
        uint32_t w = 0, h = 0;
        const double before = clock.Ms();
        const PreviewDrawResult result = hover_cache_.Draw(dc, D2D1::RectF(4.0f, 4.0f, 380.0f, 380.0f), path, attrs,
            pixel_size, 1, modified, size, 1.0f, &text, nullptr, nullptr, direct, nullptr, nullptr, nullptr, nullptr,
            nullptr, 0, nullptr, nullptr, nullptr, &w, &h);
        dc->EndDraw();
        dc->SetTarget(previous.get());
        // The thumbnail drawn in the photo's place is not the photo.
        const bool stand_in = sample.first_ms >= 0.0 && result == PreviewDrawResult::Bitmap &&
                              (std::max)(w, h) <= kHeifQuickEdge && pixel_size > kHeifQuickEdge;
        if (!properties_first && sample.properties_ms < 0.0 &&
            hover_cache_.Properties(path, attrs, 1, modified, size, properties))
            sample.properties_ms = clock.Ms();
        if (result != PreviewDrawResult::Pending && !stand_in) {
            sample.content_ms = clock.Ms();
            sample.upload_ms = sample.content_ms - before;
            sample.result = static_cast<int>(result);
            sample.width = w;
            sample.height = h;
            break;
        }
        if (clock.Ms() > static_cast<double>(timeout_ms)) {
            sample.content_ms = clock.Ms();
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
    }
    // The facts strip may fill after the picture: how much later (2 s cap).
    while (!warm_run && sample.properties_ms < 0.0 && clock.Ms() < sample.content_ms + 2000.0) {
        if (hover_cache_.Properties(path, attrs, 1, modified, size, properties)) sample.properties_ms = clock.Ms();
        else MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
    return sample;
}

int MainRenderer::PrefetchHoverPreview(const std::wstring& path, DWORD attrs, uint64_t size, uint64_t modified,
                                       bool photo_first) {
    if (path.empty() || !compositor_ || !compositor_->Dc()) return -1;
    ID2D1DeviceContext* dc = compositor_->Dc();
    // The same request the card makes (size, kind), queued through a draw
    // into a throwaway target; a cached picture is drawn there unseen.
    ComPtr<ID2D1Bitmap1> target;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, props, &target)) || !target.get()) return -2;
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    hover_cache_.BeginFrame();
    dc->SetTarget(target.get());
    dc->BeginDraw();
    // HEIF: the thumbnail first, as the card does; the photo follows once
    // the card (or a second prefetch) sees the thumbnail in.
    PreviewDrawResult result = photo_first && HeifPhoto(name)
        ? hover_cache_.Draw(dc, D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f), path, attrs, kHeifQuickEdge,
            1, modified, size, 0.0f, nullptr, nullptr, nullptr, true)
        : PreviewDrawResult::Failed;
    if (result != PreviewDrawResult::Pending)
        result = hover_cache_.Draw(dc, D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f), path, attrs, 1024u,
            1, modified, size, 0.0f, nullptr, nullptr, nullptr, !OfficeDocument(name));
    dc->EndDraw();
    dc->SetTarget(previous.get());
    return static_cast<int>(result);
}

double MainRenderer::BenchHoverVideo(HWND owner, const std::wstring& path, uint32_t timeout_ms, double* ready_ms) {
    if (ready_ms) *ready_ms = -1.0;
    if (!hover_video_) hover_video_ = std::make_shared<VideoPreview>();
    if (hover_video_->active()) hover_video_->Reset();
    const BenchClock clock;
    hover_video_->Open(owner, path, true);
    double first = -1.0;
    bool laid_out = false;
    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const VideoPreview::State state = hover_video_->Snapshot();
        if (ready_ms && *ready_ms < 0.0 && state.ready) *ready_ms = clock.Ms();
        if (!laid_out && hover_video_->active()) {
            hover_video_->Layout(RECT{16, 16, 656, 376}, false, 5);
            laid_out = true;
        }
        if (FAILED(state.error) || (state.ready && state.missing_decoder && !state.ffmpeg)) break;
        bool picture = false;
        if (state.ffmpeg) {
            uint64_t serial = 0;
            const auto frame = hover_video_->Frame(serial);
            picture = frame && frame->width && frame->height;
        } else {
            picture = state.ready && state.position > 0;
        }
        if (picture) {
            first = clock.Ms();
            break;
        }
        if (clock.Ms() > static_cast<double>(timeout_ms)) break;
        Sleep(2);
    }
    hover_video_->Reset();
    hover_video_path_.clear();
    return first;
}

}  // namespace pulse::ui
