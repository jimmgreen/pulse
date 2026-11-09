#include <chrono>
#include "file_item_selection.h"
// ui_pane_list.cpp — Pane, list, empty states, columns, and icons.
#include "ui_renderer.h"
#include "lumatext_renderer.h"
#include "ai_list_overlay.h"
#include "file_item_selection.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "details_column_widths.h"
#include "pane_header_icons.h"
#include "icon_size_slider.h"
#include "toolbar_layout.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../app/search_query.h"
#include "../common/text_format.h"
#include "../common/preview_extensions.h"
#include "../common/file_placeholder.h"
#include "wall_aspects.h"
#include "wall_layout_source.h"
#include "wall_layout_job.h"
#include "file_name_breaks.h"
#include "../fs/cloud_state.h"
#include "../app/cloud_sync_probe.h"
#include "cloud_swap.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace pulse::ui {

namespace {
// Attribute wall time and Luma fallback deltas to the current row phase.
class RowPerfScope {
public:
    RowPerfScope(uint64_t& time, uint64_t& fallbacks, Compositor* compositor)
        : time_(&time), fallbacks_(&fallbacks), compositor_(compositor), start_(Clock::now()), before_(Fallbacks()) {}
    ~RowPerfScope() { Finish(); }
    void Next(uint64_t& time, uint64_t& fallbacks) {
        Finish(); time_ = &time; fallbacks_ = &fallbacks;
        start_ = Clock::now(); before_ = Fallbacks();
    }
private:
    using Clock = std::chrono::steady_clock;
    uint64_t Fallbacks() const { const auto* s = compositor_->GetLumaTextStats(); return s ? s->fallback_draws : 0; }
    void Finish() {
        *time_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_).count());
        const auto after = Fallbacks();
        if (after >= before_) *fallbacks_ += after - before_;
    }
    uint64_t* time_; uint64_t* fallbacks_; Compositor* compositor_;
    Clock::time_point start_; uint64_t before_;
};
bool IsSizePlaceholder(const std::wstring& text) {
    return text == L"\u2014" || text == L"-" ||
        text == l10n::Get(l10n::StringId::GroupSizeEmpty) ||
        text == l10n::Get(l10n::StringId::SizeEmpty);
}

// Pulse 助手's list overlay of the pane DrawSinglePane is drawing (it owns
// the painter: the spark and the title pill are drawn outside the list's clip).
AiListPainter* g_ai_list_painter = nullptr;
int g_ai_list_pane = -1;
// How a row looked when its card started to fly (the row is gone soon after).
std::unordered_map<std::wstring, ListEntryView>& AiGhostEntries() {
    static std::unordered_map<std::wstring, ListEntryView> entries;
    return entries;
}
}  // namespace

// Cloud-file state glyph (Segoe Fluent) and ink, shared by the Details
// 状态 column and the grid badge.
static const wchar_t* CloudStateGlyph(fs::CloudState state) {
    switch (state) {
    case fs::CloudState::Online: return L"\xE753";  // Cloud
    case fs::CloudState::Pinned: return L"\xEC61";  // CompletedSolid
    default: return L"\xE930";                            // Completed
    }
}

static D2D1_COLOR_F CloudStateInk(fs::CloudState state, const Theme& theme) {
    const bool dark = theme.bg.r < 0.5f;
    return state == fs::CloudState::Online ? HexColor(dark ? 0x6CB4F0 : 0x3B82C4)
                                                  : HexColor(dark ? 0x6CC28F : 0x3F8A5F);
}

// Details 状态 column text for a cloud-file state.
static std::wstring CloudStateLabel(fs::CloudState state) {
    using l10n::StringId;
    switch (state) {
    case fs::CloudState::Online: return l10n::Get(StringId::CloudStateOnline);
    case fs::CloudState::Pinned: return l10n::Get(StringId::CloudStatePinned);
    default: return l10n::Get(StringId::CloudStateLocal);
    }
}

// What a drawn cloud row shows: a transfer or problem the sync engine reports
// (asked only for rows on screen) wins over where the content lives.
struct CloudLook {
    const wchar_t* glyph;
    D2D1_COLOR_F ink;
    std::wstring label;
};

static CloudLook CloudLookFor(const ListEntryView& entry, const Theme& theme) {
    const bool dark = theme.bg.r < 0.5f;
    // The probe caches by a stamp; folding the attributes in makes 始终保留 /
    // 释放空间 (attributes only, mtime unchanged) ask the sync engine again.
    const uint64_t stamp = entry.modified_value ^ (static_cast<uint64_t>(entry.attrs) * 0x9E3779B97F4A7C15ull);
    switch (app::CloudSyncProbe::Instance().Lookup(entry.path, stamp)) {
    case fs::CloudSync::Syncing:
        return {L"\xE895", HexColor(dark ? 0x6CB4F0 : 0x3B82C4), l10n::Get(l10n::StringId::CloudStateSyncing)};
    case fs::CloudSync::Error:
        return {L"\xE7BA", HexColor(dark ? 0xF5A06B : 0xC2410C), l10n::Get(l10n::StringId::CloudStateError)};
    default: break;
    }
    const fs::CloudState state = fs::CloudStateFromAttrs(entry.attrs);
    return {CloudStateGlyph(state), CloudStateInk(state, theme), CloudStateLabel(state)};
}

// One swap tracker for every window; keys mix in the renderer so two windows
// showing the same row each animate their own icon (ui/cloud_swap.h).
static CloudSwapPose CloudSwapFor(const void* owner, const std::wstring& path, int slot, wchar_t glyph,
                                  uint64_t now) {
    static CloudSwapTracker tracker;
    const uint64_t key = std::hash<std::wstring>{}(path) ^
                         (reinterpret_cast<uintptr_t>(owner) * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64_t>(slot);
    return tracker.Pose(key, glyph, now, motion::SystemAnimationsEnabled());
}


namespace {
// This PC tiles: the capacity bar grows in over this long, once per visit.
constexpr float kCapacityRevealMs = 700.0f;

// This PC tiles: the bar follows the text column, up to 360 DIP, and stops
// short of the tile's edge (and the scrollbar next to the right column).
float DriveTileBarWidth(const D2D1_RECT_F& name_rect, float scale) {
    return std::max(0.0f, std::min(name_rect.right - name_rect.left - 12.0f * scale, 360.0f * scale));
}
}  // namespace

float MainRenderer::ListRowHeightDip(const PaneViewModel& vm) const {
    if (DetailsShowsSnippet(vm)) return std::max(row_height_dip_, kDetailsSnippetMinRowDip);
    return row_height_dip_;
}

bool MainRenderer::EnsureEmptyStateSvg() {
    if (empty_state_svg_.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;

    if (FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_))))
        return false;

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_EMPTY_FOLDER_SVG), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byteCount == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byteCount);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(512.0f, 360.0f), &empty_state_svg_))) {
        empty_state_svg_.reset();
        return false;
    }

    ComPtr<ID2D1SvgElement> background;
    if (SUCCEEDED(empty_state_svg_->FindElementById(L"background", &background)) &&
        background.get()) {
        background->SetAttributeValue(L"display", D2D1_SVG_DISPLAY_NONE);
    }
    return true;
}

bool MainRenderer::DrawEmptyStateSvg(const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureEmptyStateSvg()) return false;
    ComPtr<ID2D1SvgElement> root;
    empty_state_svg_->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float availableWidth = std::max(0.0f, bounds.right - bounds.left);
    const float artWidth = std::min(availableWidth, 280.0f * scale_);
    if (artWidth <= 1.0f) return false;
    const float artHeight = artWidth * 360.0f / 512.0f;
    const float left = (bounds.left + bounds.right - artWidth) * 0.5f;
    const float top = (bounds.top + bounds.bottom - artHeight) * 0.5f;

    empty_state_svg_->SetViewportSize(D2D1::SizeF(512.0f, 360.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(artWidth / 512.0f, artHeight / 360.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(empty_state_svg_.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

bool MainRenderer::EnsureNoSelectionSvg() {
    if (no_selection_svg_.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;

    if (!empty_state_svg_dc_.get() &&
        FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_))))
        return false;

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_NO_SELECTION_SVG), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byteCount == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byteCount);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(320.0f, 240.0f), &no_selection_svg_))) {
        no_selection_svg_.reset();
        return false;
    }
    return true;
}

bool MainRenderer::DrawNoSelectionSvg(const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureNoSelectionSvg()) return false;
    ComPtr<ID2D1SvgElement> root;
    no_selection_svg_->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float availableWidth = std::max(0.0f, bounds.right - bounds.left);
    const float availableHeight = std::max(0.0f, bounds.bottom - bounds.top);
    if (availableWidth <= 1.0f || availableHeight <= 1.0f) return false;
    // Fit the 320x240 art into the caller layout rect (details or pane empty).
    const float artWidth = std::min(availableWidth, availableHeight * 320.0f / 240.0f);
    const float artHeight = artWidth * 240.0f / 320.0f;
    const float left = (bounds.left + bounds.right - artWidth) * 0.5f;
    const float top = (bounds.top + bounds.bottom - artHeight) * 0.5f;

    no_selection_svg_->SetViewportSize(D2D1::SizeF(320.0f, 240.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(artWidth / 320.0f, artHeight / 240.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(no_selection_svg_.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

bool MainRenderer::EnsureCuratedEmptyStateSvg(bool starred) {
    ComPtr<ID2D1SvgDocument>& document = starred ? starred_empty_svg_ : recent_empty_svg_;
    if (document.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;
    if (!empty_state_svg_dc_.get() &&
        FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_)))) {
        return false;
    }

    const int resource_id = starred ? IDR_STARRED_EMPTY_SVG : IDR_RECENT_EMPTY_SVG;
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byte_count = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byte_count == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byte_count);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(512.0f, 360.0f), &document))) {
        document.reset();
        return false;
    }
    return true;
}

bool MainRenderer::DrawCuratedEmptyStateSvg(bool starred, const D2D1_RECT_F& bounds,
                                             float opacity) {
    if (!EnsureCuratedEmptyStateSvg(starred)) return false;
    ComPtr<ID2D1SvgDocument>& document = starred ? starred_empty_svg_ : recent_empty_svg_;
    ComPtr<ID2D1SvgElement> root;
    document->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float available_width = std::max(0.0f, bounds.right - bounds.left);
    const float art_width = std::min(available_width, 280.0f * scale_);
    if (art_width <= 1.0f) return false;
    const float art_height = art_width * 360.0f / 512.0f;
    const float left = (bounds.left + bounds.right - art_width) * 0.5f;
    const float top = (bounds.top + bounds.bottom - art_height) * 0.5f;

    document->SetViewportSize(D2D1::SizeF(512.0f, 360.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(art_width / 512.0f, art_height / 360.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(document.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

bool MainRenderer::EnsureExcludeEmptySvg() {
    if (exclude_empty_svg_.get() && empty_state_svg_dc_.get()) return true;
    if (!compositor_ || !compositor_->Dc()) return false;
    if (!empty_state_svg_dc_.get() &&
        FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_)))) {
        return false;
    }

    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(IDR_EXCLUDE_EMPTY_SVG), RT_RCDATA);
    if (!resource) return false;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byte_count = SizeofResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || byte_count == 0) return false;

    ComPtr<IStream> stream;
    stream.p = SHCreateMemStream(static_cast<const BYTE*>(bytes), byte_count);
    if (!stream.get()) return false;
    if (FAILED(empty_state_svg_dc_->CreateSvgDocument(
            stream.get(), D2D1::SizeF(512.0f, 360.0f), &exclude_empty_svg_))) {
        exclude_empty_svg_.reset();
        return false;
    }
    return true;
}

bool MainRenderer::DrawExcludeEmptySvg(const D2D1_RECT_F& bounds, float opacity) {
    if (!EnsureExcludeEmptySvg()) return false;
    ComPtr<ID2D1SvgElement> root;
    exclude_empty_svg_->GetRoot(&root);
    if (root.get())
        root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
    const float available_width = std::max(0.0f, bounds.right - bounds.left);
    const float available_height = std::max(0.0f, bounds.bottom - bounds.top);
    if (available_width <= 1.0f || available_height <= 1.0f) return false;
    float art_width = available_width;
    float art_height = art_width * 360.0f / 512.0f;
    if (art_height > available_height) {
        art_height = available_height;
        art_width = art_height * 512.0f / 360.0f;
    }
    const float left = (bounds.left + bounds.right - art_width) * 0.5f;
    const float top = (bounds.top + bounds.bottom - art_height) * 0.5f;

    exclude_empty_svg_->SetViewportSize(D2D1::SizeF(512.0f, 360.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(
        D2D1::Matrix3x2F::Scale(art_width / 512.0f, art_height / 360.0f) *
        D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(exclude_empty_svg_.get());
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}
D2D1_RECT_F MainRenderer::PaneListRect(const D2D1_RECT_F& pane_bounds, float extra_top,
                                       ViewMode mode) const {
    D2D1_RECT_F list = pane_bounds;
    list.top += pane_header_height_ + extra_top +
                (ShowsColumnHeader(mode) ? column_header_height_ : 0.0f);
    if (list.top > list.bottom) list.top = list.bottom;
    return list;
}

D2D1_RECT_F MainRenderer::PaneListRect(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    return PaneListRect(pane_bounds,
        PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_), vm.view_mode);
}

D2D1_RECT_F MainRenderer::FilterBoxRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    const float width = pane_bounds.right;
    const float left = EffectiveSidebarWidth(width);
    return ToolbarLayoutAt(width, NewButtonWidthPx(width-left < 600*scale_), expand).filter;
}

namespace {
// Pane header size slider (P1e).
constexpr float kSizeSliderDip = kIconSizeSliderWidth;
constexpr float kSizeSliderMinPaneDip = 440.0f;
bool ShowsSizeControl(const PaneViewModel& vm) noexcept {
    return vm.size_control && IsZoomIconMode(vm.view_mode);
}
} // namespace

D2D1_RECT_F MainRenderer::PaneZoomChipRect(const PaneViewModel& pane, const D2D1_RECT_F& bounds) const {
    const float limit = PaneHeaderTitleRight(pane,bounds);
    const float width = (bounds.right-bounds.left > 600*scale_ ? 258.0f : 170.0f)*scale_;
    const float titleWidth = compositor_ ? MeasureTextWidth(compositor_->DwriteFactory(),compositor_->HeaderFormat(),pane.header_text) : 60*scale_;
    const float left = std::max(bounds.left+8*scale_,std::min(bounds.left+16*scale_+titleWidth,limit-width));
    return D2D1::RectF(left,bounds.top+(pane_header_height_-26*scale_)*.5f,
        std::min(limit,left+width),bounds.top+(pane_header_height_+26*scale_)*.5f);
}

D2D1_RECT_F MainRenderer::PaneExpandRect(const D2D1_RECT_F& bounds) const {
    auto rc = PaneViewButtonRect(bounds);
    const float offset = kCommandIconStepDip * scale_;
    rc.left += offset; rc.right += offset;
    return rc;
}

D2D1_RECT_F MainRenderer::PaneCloseRect(const D2D1_RECT_F& bounds) const {
    auto rc = PaneViewButtonRect(bounds);
    rc.left += (1 + (pane_expand_visible_ ? 1 : 0)) * kCommandIconStepDip * scale_;
    rc.right += (1 + (pane_expand_visible_ ? 1 : 0)) * kCommandIconStepDip * scale_;
    return rc;
}

D2D1_RECT_F MainRenderer::PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                              float filter_expand) const {
    (void)filter_expand;
    const float right = pane_bounds.right - (8 + (pane_close_visible_ ? kCommandIconStepDip : 0) + (pane_expand_visible_ ? kCommandIconStepDip : 0))*scale_;
    return D2D1::RectF(right-kCommandIconButtonDip*scale_,pane_bounds.top+4*scale_,
        right,pane_bounds.top+pane_header_height_-4*scale_);
}

D2D1_RECT_F MainRenderer::PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                               float filter_expand, bool wall) const {
    const D2D1_RECT_F view = PaneViewButtonRect(pane_bounds, filter_expand);
    const D2D1_RECT_F rc = D2D1::RectF(view.left - kCommandIconStepDip * scale_, view.top,
                                       view.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_,
                                       view.bottom);
    return wall ? StepLeftHeaderButton(rc, scale_) : rc;
}
D2D1_RECT_F MainRenderer::PaneDetailsRect(const D2D1_RECT_F& pane_bounds,
                                                float filter_expand, bool wall) const {
    return StepLeftHeaderButton(PaneMediumIconsRect(pane_bounds, filter_expand, wall), scale_);
}
D2D1_RECT_F MainRenderer::PaneWallRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    // Details, icons, photo wall, then the view menu, like the prototype.
    return vm.size_control ? PaneMediumIconsRect(pane_bounds) : D2D1_RECT_F{};
}

D2D1_RECT_F MainRenderer::PaneSizeButtonRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    if (!ShowsSizeControl(vm) || pane_bounds.right - pane_bounds.left >= kSizeSliderMinPaneDip * scale_)
        return {};
    const D2D1_RECT_F details = PaneDetailsRect(pane_bounds, 1.0f, vm.size_control);
    const float right = details.left - 6.0f * scale_;  // clear of the segment's groove
    return D2D1::RectF(right - kCommandIconButtonDip * scale_, details.top, right, details.bottom);
}

D2D1_RECT_F MainRenderer::PaneSizeSliderRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    if (!ShowsSizeControl(vm)) return {};
    const D2D1_RECT_F details = PaneDetailsRect(pane_bounds, 1.0f, vm.size_control);
    float right = details.left - 10.0f * scale_;
    if (pane_bounds.right - pane_bounds.left < kSizeSliderMinPaneDip * scale_) {
        if (!vm.size_slider_open) return {};
        right = PaneSizeButtonRect(vm, pane_bounds).left - 4.0f * scale_;
    }
    const float left = std::max(pane_bounds.left + 8.0f * scale_, right - kSizeSliderDip * scale_);
    if (right - left < 48.0f * scale_) return {};
    return D2D1::RectF(left, details.top, right, details.bottom);
}

float MainRenderer::PaneSizeSliderValueAt(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                          float x) const {
    const D2D1_RECT_F rc = PaneSizeSliderRect(vm, pane_bounds);
    if (rc.right <= rc.left) return 0.0f;
    return IconSizeSliderValueAt(rc, x, scale_, scale_);
}

float MainRenderer::PaneHeaderTitleRight(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    if(pane_bounds.right-pane_bounds.left<280*scale_)
        return PaneViewButtonRect(pane_bounds).left-8*scale_;
    float left = PaneDetailsRect(pane_bounds, 1.0f, vm.size_control).left;
    const D2D1_RECT_F button = PaneSizeButtonRect(vm, pane_bounds);
    const D2D1_RECT_F slider = PaneSizeSliderRect(vm, pane_bounds);
    if (button.right > button.left) left = button.left;  // an open slider covers the title instead
    else if (slider.right > slider.left) left = slider.left;
    return left - 8.0f * scale_;
}

void MainRenderer::DrawPaneSizeSlider(const PaneViewModel& pane, int pane_index, const D2D1_RECT_F& slider,
                                      const D2D1_RECT_F& label_at, bool hovered, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return;
    const bool shown = slider.right > slider.left;
    const float dip = IsZoomIconMode(pane.view_mode) ? IconDip(pane.view_mode, pane.icon_dip, scale_) : 0.0f;
    HeaderSizeMotion* motion = pane_index >= 0
        ? &header_size_motion_[static_cast<size_t>(pane_index) % header_size_motion_.size()] : nullptr;
    D2D1_RECT_F full = slider;
    D2D1_RECT_F drawn = slider;
    if (motion) {
        if (shown) motion->slider_rc = slider;
        full = motion->slider_rc;
        // Grows out of the segment's side and folds back into it.
        const D2D1_RECT_F folded = D2D1::RectF(full.right, full.top, full.right, full.bottom);
        drawn = motion->slider.Update(0, shown ? 1 : 0, shown ? slider : folded, motion_frame_,
                                      motion_now_, motion::spring::Snappy);
        if (motion->slider.Active(motion_now_)) RequestMotionFrame();
        if (dip > 0.0f && dip != motion->dip && motion->path == pane.path)
            motion->label_until = motion_now_ + 1100;
        motion->dip = dip;
        motion->path = pane.path;
    }
    const float width = full.right - full.left;
    if (dip > 0.0f && width > 1.0f && drawn.right - drawn.left > 0.5f) {
        const float alpha = std::clamp((drawn.right - drawn.left) / width, 0.0f, 1.0f);
        dc->PushAxisAlignedClip(drawn, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        MakeBrush(dc, theme.text_secondary, brFillHover_);
        DrawIconSizeSlider(dc, brFillHover_.get(), full, dip, scale_, scale_, theme, hovered, alpha);
        dc->PopAxisAlignedClip();
    }
    if (motion && dip > 0.0f && motion_now_ < motion->label_until) {
        RequestMotionFrame();
        const float a = std::clamp(static_cast<float>(motion->label_until - motion_now_) / 250.0f, 0.0f, 1.0f);
        const float cy = (label_at.top + label_at.bottom) * 0.5f;
        const float right = label_at.left - 6.0f * scale_;
        const D2D1_RECT_F pill = D2D1::RectF(right - 40.0f * scale_, cy - 11.0f * scale_, right, cy + 11.0f * scale_);
        painter_.FillRoundedRect(pill, 11.0f * scale_, WithAlpha(theme.surface_card, a));
        painter_.StrokeRoundedRect(pill, 11.0f * scale_, WithAlpha(theme.stroke_card, a));
        painter_.DrawText(std::to_wstring(static_cast<int>(dip + 0.5f)), pill, compositor_->SmallFormat(),
                          WithAlpha(theme.text, a), fluent::HorizontalAlignment::Center);
    }
}

D2D1_RECT_F MainRenderer::FilterClearRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    auto rc = FilterBoxRect(pane_bounds, expand);
    rc.left = rc.right - 30.0f * scale_;
    rc.right -= 3.0f * scale_;
    rc.top += 3.0f * scale_;
    rc.bottom -= 3.0f * scale_;
    return rc;
}

D2D1_RECT_F MainRenderer::FilterEditRect(const D2D1_RECT_F& pane_bounds, float expand, bool has_text) const {
    const D2D1_RECT_F box = FilterBoxRect(pane_bounds, expand);
    D2D1_RECT_F rc = box;
    rc.left += 34.0f * scale_;
    rc.right -= (has_text ? 34.0f : 10.0f) * scale_;
    if (rc.right < rc.left + 24.0f * scale_) {
        // A crowded toolbar can leave the expanded field narrower than its icon,
        // 24 DIP of text and the clear button. The text gives up icon padding
        // first; with text it never grows into the clear button's hit area.
        rc.left = (std::max)(box.left + 26.0f * scale_, rc.right - 24.0f * scale_);
        if (!has_text) rc.right = (std::max)(rc.right, rc.left + 24.0f * scale_);
        rc.left = (std::min)(rc.left, rc.right);
    }
    return rc;
}

MainRenderer::DetailsColumnLayout MainRenderer::ThisPcColumns(
    const D2D1_RECT_F& pane_bounds, const PaneViewModel& vm) const {
    DetailsColumnLayout out;
    const auto content = DetailsContentRect(pane_bounds, scale_);
    out.left = content.left + margin_;
    out.right = std::max(out.left, content.right - margin_ * 3.0f);
    const float total = out.right - out.left;
    out.count = 1;
    out.kinds[0] = ColumnKind::Name;
    out.widths[0] = std::max(0.0f, total);
    if (total <= 0.0f || !compositor_ || !compositor_->FileNameFormat()) return out;
    auto measure = [&](const std::wstring& text, IDWriteTextFormat* format) {
        float luma = 0.0f;
        compositor_->MeasureLumaText(text, format, luma);
        return std::max(luma, MeasureTextWidth(compositor_->DwriteFactory(), format, text));
    };
    // Header text, both cell insets and room for a sort mark.
    auto header = [&](pulse::l10n::StringId id) {
        return measure(pulse::l10n::Get(id), compositor_->HeaderFormat()) + 35.0f * scale_;
    };
    std::wstring natural_key = std::to_wstring(typography::Generation()) + L":" + std::to_wstring(scale_);
    natural_key += L":" + std::to_wstring(reinterpret_cast<uintptr_t>(compositor_->FileNameFormat())) +
        L":" + std::to_wstring(reinterpret_cast<uintptr_t>(compositor_->HeaderFormat()));
    for (auto id : {pulse::l10n::StringId::ColumnFree, pulse::l10n::StringId::ColumnTotalSize,
                    pulse::l10n::StringId::ColumnType, pulse::l10n::StringId::ColumnUsage}) {
        natural_key += L'\x1f'; natural_key += pulse::l10n::Get(id);
    }
    for (size_t row = 0; row < std::min<size_t>(vm.EntryCount(), 128); ++row) {
        const int source = vm.SourceIndex(static_cast<int>(row));
        if (source < 0) continue;
        const auto& e = MakeVisibleEntry(vm, static_cast<size_t>(source));
        natural_key += L'\x1e';
        natural_key += std::to_wstring(e.drive_free) + L":" + std::to_wstring(e.drive_total);
        natural_key += e.device ? L":d:" : L":f:";
        natural_key += std::to_wstring(e.type_text.size()) + L":" + e.type_text;
    }
    float free_w = 0, total_w = 0, type_w = 0, usage_w = 0;
    const auto cached = std::find_if(this_pc_widths_.begin(), this_pc_widths_.end(),
        [&](const auto& entry) { return entry.key == natural_key; });
    if (cached != this_pc_widths_.end()) {
        free_w = cached->free; total_w = cached->total; type_w = cached->type; usage_w = cached->usage;
    } else {
        free_w = header(pulse::l10n::StringId::ColumnFree);
        total_w = header(pulse::l10n::StringId::ColumnTotalSize);
        type_w = header(pulse::l10n::StringId::ColumnType);
        // The bar plus "100%" beside it.
        usage_w = std::max(header(pulse::l10n::StringId::ColumnUsage), 200.0f * scale_);
        const size_t rows = std::min<size_t>(vm.EntryCount(), 128);
        for (size_t row = 0; row < rows; ++row) {
            const int source = vm.SourceIndex(static_cast<int>(row));
            if (source < 0) continue;
            const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(source));
            if (e.drive_total > 0) {
                // The free size is semibold: a little wider than it measures here.
                free_w = std::max(free_w, measure(pulse::format::ByteSize(e.drive_free),
                                                  compositor_->FileNameFormat()) * 1.08f + 20.0f * scale_);
                total_w = std::max(total_w, measure(pulse::format::ByteSize(e.drive_total),
                                                    compositor_->FileNameFormat()) + 20.0f * scale_);
            }
            // A phone's hint runs across the columns; it does not size 类型.
            if (!e.device)
                type_w = std::max(type_w, measure(e.type_text, compositor_->FileNameFormat()) + 20.0f * scale_);
        }
        type_w = std::min(type_w, 240.0f * scale_);
        if (natural_key.size() <= 32768) {
            if (this_pc_widths_.size() >= 4) this_pc_widths_.erase(this_pc_widths_.begin());
            this_pc_widths_.push_back({std::move(natural_key), free_w, total_w, type_w, usage_w});
        }
    }
    // Display order; when the pane is narrow, 类型 goes first, then 总大小,
    // then 使用情况, so the name keeps at least 180 px.
    struct Col { ColumnKind kind; float width; };
    std::array<Col, 4> cols{{{ColumnKind::Date, usage_w}, {ColumnKind::Size, free_w},
                          {ColumnKind::Total, total_w}, {ColumnKind::Type, type_w}}};
    size_t count = cols.size();
    const float name_min = 180.0f * scale_;
    const ColumnKind drop_order[] = {ColumnKind::Type, ColumnKind::Total, ColumnKind::Date, ColumnKind::Size};
    for (const ColumnKind kind : drop_order) {
        float used = 0.0f;
        for (size_t i = 0; i < count; ++i) used += cols[i].width;
        if (total - used >= name_min) break;
        const auto end = std::remove_if(cols.begin(), cols.begin() + count,
            [kind](const Col& c) { return c.kind == kind; });
        count = static_cast<size_t>(end - cols.begin());
    }
    float used = 0.0f;
    for (size_t i = 0; i < count; ++i) used += cols[i].width;
    out.widths[0] = std::max(0.0f, total - used);
    for (size_t i = 0; i < count; ++i) {
        const Col& c = cols[i];
        out.kinds[static_cast<size_t>(out.count)] = c.kind;
        out.widths[static_cast<size_t>(out.count)] = c.width;
        ++out.count;
    }
    return out;
}

SortColumn MainRenderer::MediaSortColumn(ColumnKind kind) noexcept {
    switch (kind) {
    case ColumnKind::MediaTaken: return SortColumn::MediaTaken;
    case ColumnKind::MediaDims: return SortColumn::MediaDims;
    case ColumnKind::MediaCamera: return SortColumn::MediaCamera;
    case ColumnKind::MediaTitle: return SortColumn::MediaTitle;
    case ColumnKind::MediaArtist: return SortColumn::MediaArtist;
    case ColumnKind::MediaAlbum: return SortColumn::MediaAlbum;
    case ColumnKind::MediaDuration: return SortColumn::MediaDuration;
    case ColumnKind::MediaFps: return SortColumn::MediaFps;
    default: return SortColumn::Name;
    }
}

std::wstring MainRenderer::MediaColumnLabel(ColumnKind kind, uint8_t media_columns) {
    using pulse::l10n::StringId;
    switch (kind) {
    case ColumnKind::MediaTaken: return pulse::l10n::Get(StringId::ColumnMediaTaken);
    case ColumnKind::MediaDims:  // a video's size is its resolution
        return pulse::l10n::Get(media_columns == 3 ? StringId::ColumnMediaResolution : StringId::ColumnMediaDims);
    case ColumnKind::MediaCamera: return pulse::l10n::Get(StringId::ColumnMediaCamera);
    case ColumnKind::MediaTitle: return pulse::l10n::Get(StringId::ColumnMediaTitle);
    case ColumnKind::MediaArtist: return pulse::l10n::Get(StringId::ColumnMediaArtist);
    case ColumnKind::MediaAlbum: return pulse::l10n::Get(StringId::ColumnMediaAlbum);
    case ColumnKind::MediaDuration: return pulse::l10n::Get(StringId::ColumnMediaDuration);
    case ColumnKind::MediaFps: return pulse::l10n::Get(StringId::ColumnMediaFps);
    default: return std::wstring();
    }
}

MainRenderer::DetailsColumnLayout MainRenderer::MediaColumns(
    const D2D1_RECT_F& pane_bounds, const PaneViewModel& vm) const {
    auto out = DetailsColumns(pane_bounds, vm.details_column_dividers, false, {}, vm.cloud_status_column);
    if (!compositor_ || !compositor_->HeaderFormat()) return out;
    struct Col { ColumnKind kind; float dip; int drop; };
    using K = ColumnKind;
    std::vector<Col> cols;
    switch (vm.media_columns) {
    case 1: cols = {{K::MediaTaken, 150, 2}, {K::MediaDims, 112, 1}, {K::MediaCamera, 160, 0}}; break;
    case 2: cols = {{K::MediaTitle, 220, 0}, {K::MediaArtist, 160, 2}, {K::MediaAlbum, 180, 1}, {K::MediaDuration, 76, 3}}; break;
    case 3: cols = {{K::MediaDuration, 84, 2}, {K::MediaDims, 120, 1}, {K::MediaFps, 72, 0}}; break;
    default: return out;
    }
    // User-selected columns take precedence; media uses only the spare name width.
    const float total = out.widths[0];
    std::vector<float> widths;
    std::vector<float> default_widths;
    widths.reserve(cols.size());
    for (const Col& c : cols) {
        // Header text, both cell insets and room for a sort mark.
        const std::wstring label = c.kind == K::Size ? pulse::l10n::Get(pulse::l10n::StringId::ColumnSize)
                                                     : MediaColumnLabel(c.kind, vm.media_columns);
        const float header = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->HeaderFormat(), label) +
                             35.0f * scale_;
        default_widths.push_back(std::max(c.dip * scale_, header));
        const int slot = MediaColumnSlot(c.kind, vm.media_columns);
        const float manual = vm.media_column_widths[vm.media_columns - 1][slot];
        widths.push_back(manual > 1.0f ? std::max(48.0f, manual) * scale_
                                      : std::max(c.dip * scale_, header));
    }
    std::vector<bool> shown(cols.size(), true);
    const float name_min = 200.0f * scale_;
    // Narrow-pane visibility follows the default layout, so shrinking a column
    // cannot make a hidden column appear underneath the divider being dragged.
    for (int rank = 0; rank < static_cast<int>(cols.size()); ++rank) {
        float used = 0.0f;
        for (size_t i = 0; i < cols.size(); ++i) if (shown[i]) used += default_widths[i];
        if (total - used >= name_min) break;
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i].drop == rank) shown[i] = false;
    }
    for (int rank = 0; rank < static_cast<int>(cols.size()); ++rank) {
        float used = 0.0f;
        for (size_t i = 0; i < cols.size(); ++i) if (shown[i]) used += widths[i];
        if (total - used >= name_min) break;
        for (size_t i = 0; i < cols.size(); ++i) if (cols[i].drop == rank) shown[i] = false;
    }
    float used = 0.0f;
    for (size_t i = 0; i < cols.size(); ++i) if (shown[i]) used += widths[i];
    out.widths[0] = std::max(0.0f, total - used);
    for (size_t i = 0; i < cols.size() && out.count < static_cast<int>(kMaxDetailsColumns); ++i) {
        if (!shown[i]) continue;
        out.kinds[static_cast<size_t>(out.count)] = cols[i].kind;
        out.widths[static_cast<size_t>(out.count)] = widths[i];
        ++out.count;
    }
    return out;
}

MainRenderer::DetailsColumnLayout MainRenderer::DetailsColumns(
    const D2D1_RECT_F& pane_bounds, const PaneViewModel& vm) const {
    if (vm.this_pc_columns && !vm.is_search && !vm.column_strip.Active())
        return ThisPcColumns(pane_bounds, vm);
    if (vm.media_columns && !vm.is_search && !vm.column_strip.Active())
        return MediaColumns(pane_bounds, vm);
    if (vm.column_strip.Active() && !vm.is_search) {
        // Column view: one name column, like the columns beside it.
        DetailsColumnLayout name_only = DetailsColumns(pane_bounds, DetailsColumnWidths{}, false, {});
        name_only.count = 1;
        name_only.kinds[0] = ColumnKind::Name;
        name_only.widths[0] = std::max(0.0f, name_only.right - name_only.left);
        name_only.two_line = false;
        name_only.reorderable = false;
        return name_only;
    }
    auto columns = DetailsColumns(pane_bounds, vm.details_column_dividers, vm.is_search,
                                  vm.search_column_dividers, vm.cloud_status_column);
    // An AI cleanup list explains each row in the type column (原因). When the
    // pane is too narrow for both, the reason outranks the modified date.
    if (!vm.type_column_label.empty() && !columns.Has(ColumnKind::Type)) {
        for (int i = 0; i < columns.count; ++i) {
            if (columns.kinds[static_cast<size_t>(i)] != ColumnKind::Date) continue;
            columns.kinds[static_cast<size_t>(i)] = ColumnKind::Type;
            break;
        }
    }
    // Explicit divider positions remain a manual override. A drag starts by
    // capturing this measured layout, so the first movement does not jump.
    const bool manual = vm.is_search
        ? std::any_of(vm.search_column_dividers.begin(), vm.search_column_dividers.end(), [](float v) { return v > 1.0f; })
        : std::any_of(vm.details_column_dividers.begin(), vm.details_column_dividers.end(), [](float v) { return v > 1.0f; });
    // Content fitting covers the classic modified / type / size trio; extra
    // date columns keep their measured automatic widths.
    if (manual || !columns.Has(ColumnKind::Date) || !columns.Has(ColumnKind::Type) ||
        !columns.Has(ColumnKind::Size) || columns.Has(ColumnKind::Created) ||
        columns.Has(ColumnKind::Accessed) || columns.Has(ColumnKind::Status) ||
        vm.view_mode != ViewMode::Details ||
        !compositor_ || !compositor_->FileNameFormat()) return columns;
    const std::array<std::wstring, 3> labels{
        vm.date_column_label.empty() ? l10n::Get(l10n::StringId::ColumnModified)
                                     : vm.date_column_label,
        l10n::Get(l10n::StringId::ColumnType), l10n::Get(l10n::StringId::ColumnSize)};
    auto measure = [&](const std::wstring& text, IDWriteTextFormat* format) {
        float luma = 0.0f;
        compositor_->MeasureLumaText(text, format, luma);
        return std::max(luma, MeasureTextWidth(compositor_->DwriteFactory(), format, text));
    };
    const auto automatic = AutoColumnWidths();
    std::array<float, 3> measured{automatic.date * scale_, automatic.type * scale_, automatic.size * scale_};
    for (size_t i = 0; i < measured.size(); ++i) {
        // Include both cell insets, ink overhang and a sort indicator. Reserving
        // the indicator for every header avoids widths jumping when sorting.
        measured[i] = std::max(measured[i], measure(labels[i], compositor_->HeaderFormat()) + 35.0f * scale_);
    }
    // Bound work for very large directories and paged searches. Only resident
    // rows near the viewport are inspected; sizing must never fetch disk data.
    const size_t start = static_cast<size_t>(std::max(0.0f, vm.scroll_y) /
        std::max(1.0f, ListRowHeightDip(vm) * scale_));
    const size_t end = std::min(vm.EntryCount(), start + 128);
    for (size_t row = start; row < end; ++row) {
        const int source = vm.SourceIndex(static_cast<int>(row));
        if (source < 0 || (vm.content_results && !vm.content_results->Ready(source))) continue;
        const auto& entry = MakeVisibleEntry(vm, static_cast<size_t>(source));
        const std::wstring* values[] = { &entry.date_text, &entry.type_text, &entry.size_text };
        for (size_t i = 0; i < measured.size(); ++i)
            measured[i] = std::max(measured[i],
                measure(*values[i], compositor_->FileNameFormat()) + 20.0f * scale_ +
                (i == 0 ? MtimeDateExtraDip() * scale_ : 0.0f));
        measured[1] = std::max(measured[1], measure(entry.type_text, compositor_->FileNameFormat()) +
            (20.0f + TypeChipWidthDip(TypeChipLabel(entry.name, entry.is_dir))) * scale_);
    }
    // FitDetailsMetadata works on the classic modified / type / size tail;
    // a reordered trio gets the fitted widths by kind.
    auto fit = columns.widths;
    FitDetailsMetadata(fit, columns.count, measured, scale_);
    const int tail = columns.count - 3;
    for (int i = 0; i < tail; ++i) columns.widths[static_cast<size_t>(i)] = fit[static_cast<size_t>(i)];
    for (int i = tail; i < columns.count; ++i) {
        const ColumnKind kind = columns.kinds[static_cast<size_t>(i)];
        const int classic = kind == ColumnKind::Date ? 0 : kind == ColumnKind::Type ? 1 : 2;
        columns.widths[static_cast<size_t>(i)] = fit[static_cast<size_t>(tail + classic)];
    }
    return columns;
}

MainRenderer::ColumnAutoWidths MainRenderer::AutoColumnWidths() const {
    using pulse::l10n::StringId;
    const std::wstring language = pulse::l10n::Get(StringId::TypeFolder) +
        pulse::l10n::Get(StringId::DateToday);
    if (auto_widths_scale_ == scale_ && auto_widths_language_ == language)
        return auto_widths_;
    ColumnAutoWidths out;
    IDWriteFactory2* factory = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->FileNameFormat() : nullptr;
    IDWriteTextFormat* header = compositor_ ? compositor_->HeaderFormat() : nullptr;
    if (factory && fmt && header && scale_ > 0.0f) {
        // LumaText and DWrite advances differ slightly; fit the wider one.
        auto measure = [&](IDWriteTextFormat* format, const std::wstring& text) {
            float luma = 0.0f;
            compositor_->MeasureLumaText(text, format, luma);
            return std::max(MeasureTextWidth(factory, format, text), luma) / scale_;
        };
        const float pad = 16.0f + 4.0f;   // 8 DIP inset per side + rounding slack
        const float sort_icon = 15.0f;    // header chevron when the column is sorted
        auto header_w = [&](StringId id) { return measure(header, pulse::l10n::Get(id)) + sort_icon; };
        std::vector<std::wstring> dates;
        if (list_smart_date_) {
            dates = SmartDateSamples();
        } else {
            dates = {L"2026-12-30 23:59"};
        }
        float date = header_w(StringId::ColumnModified);
        float dates_w = 0.0f;
        for (const auto& d : dates) dates_w = std::max(dates_w, measure(fmt, d));
        date = std::max(date, dates_w);
        const float created = std::max(header_w(StringId::ColumnCreated), dates_w);
        const float accessed = std::max(header_w(StringId::ColumnAccessed), dates_w);
        float type = std::max(header_w(StringId::ColumnType), measure(fmt, L"\u2014"));
        const StringId kinds[] = {StringId::TypeFolder, StringId::TypeFile, StringId::TypeTextDocument,
            StringId::TypeImage, StringId::TypeVideo, StringId::TypeAudio, StringId::TypeArchive,
            StringId::TypeApplication, StringId::Unavailable};
        const float chip = TypeChipWidthDip(L"XLSX");
        for (StringId id : kinds) type = std::max(type, measure(fmt, pulse::l10n::Get(id)) + chip);
        type = std::max(type, measure(fmt, L"AutoCAD " + pulse::l10n::Get(StringId::TypeFile)) + chip);
        float size = std::max(header_w(StringId::ColumnSize), measure(fmt, L"1023.9") + kSizeUnitDip);
        // Status: a 16 DIP glyph, a gap, then the longest state label.
        float status = header_w(StringId::ColumnStatus);
        for (StringId id : {StringId::CloudStateOnline, StringId::CloudStateLocal, StringId::CloudStatePinned})
            status = std::max(status, measure(fmt, pulse::l10n::Get(id)) + 22.0f);
        out.date = std::clamp(date + pad + MtimeDateExtraDip(), 72.0f, 176.0f + MtimeDateExtraDip());
        out.created = std::clamp(created + pad, 72.0f, 176.0f);
        out.accessed = std::clamp(accessed + pad, 72.0f, 176.0f);
        out.type = std::clamp(type + pad, 64.0f, 196.0f);
        out.size = std::clamp(size + pad, 60.0f, 112.0f);
        out.status = std::clamp(status + pad, 72.0f, 160.0f);
    }
    auto_widths_ = out;
    auto_widths_scale_ = scale_;
    auto_widths_language_ = language;
    return out;
}

float MainRenderer::TypeChipWidthDip(const std::wstring& chip) const {
    if (chip.empty()) return 0.0f;
    float w = 0.0f;
    if (compositor_ && compositor_->SmallFormat()) {
        // Uniform minimum (the width of "XLSX") so the type text after the chip
        // lines up across DWG / BAK / PDF / DWL2 rows; longer extensions still grow.
        w = std::max(CellTextWidth(chip, true), CellTextWidth(L"XLSX", true));
        w /= std::max(0.01f, scale_);
    } else {
        w = 8.0f * static_cast<float>(std::max<size_t>(chip.size(), 4));
    }
    return w + 2.0f * kTypeChipPadDip + kTypeChipGapDip;
}

void MainRenderer::DrawDriveSpaceLine(ID2D1DeviceContext* dc, const ListEntryView& entry, CapacityLevel level,
                                      const Theme& theme, float x, float y, float width, float height, bool cut) {
    const std::wstring& text = entry.drive_space_text;
    if (!dc || !compositor_ || text.empty() || width <= 1.0f || height <= 0.0f) return;
    const auto ink = [cut](const D2D1_COLOR_F& color) { return cut ? WithAlpha(color, 0.55f) : color; };
    IDWriteTextFormat* format = compositor_->SmallFormat();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    MakeBrush(dc, ink(theme.text_secondary), brTextSecondary_);
    const auto size = static_cast<UINT32>(text.size());
    const UINT32 bold_start = std::min<UINT32>(entry.drive_space_bold_start, size);
    const UINT32 bold_length = std::min<UINT32>(entry.drive_space_bold_length, size - bold_start);
    ComPtr<IDWriteTextLayout> layout;
    if (!format || !factory || bold_length == 0 ||
        FAILED(factory->CreateTextLayout(text.c_str(), size, format, width, height, &layout)) || !layout.get()) {
        DrawTextEndEllipsis(compositor_, format, brTextSecondary_.get(), text, x, y, width, height);
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (MeasureTextWidth(factory, format, text) > width) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    const DWRITE_TEXT_RANGE bold{bold_start, bold_length};
    layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, bold);
    if (level == CapacityLevel::Critical) {
        // "仅剩 14.7 GB" in red, the words before the size included.
        MakeBrush(dc, ink(painter_.CapacityInk(level)), brCapacityStrong_);
        layout->SetDrawingEffect(brCapacityStrong_.get(), DWRITE_TEXT_RANGE{0, bold_start + bold_length});
    } else {
        MakeBrush(dc, ink(theme.text), brCapacityStrong_);
        layout->SetDrawingEffect(brCapacityStrong_.get(), bold);
    }
    compositor_->DrawTextLayout(D2D1::Point2F(x, y), layout.get(), brTextSecondary_.get(),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void MainRenderer::DrawDriveOverview(const PaneViewModel& pane, const D2D1_RECT_F& card, float reveal,
                                     const Theme& theme) {
    // 存储总览: the title and drive count, the free / used / total sizes on
    // the right; a stacked bar, one segment per drive sized by capacity and
    // filled by use; then each drive's letter and percent.
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    const DriveOverviewView& ov = pane.drive_overview;
    if (!dc || !ov.On() || ov.total == 0 || card.right - card.left < 160.0f * scale_) return;
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    IDWriteTextFormat* title_fmt = NameSemiboldFormat();
    if (!factory || !small_fmt || !title_fmt) return;
    const float radius = 8.0f * scale_;
    painter_.FillRoundedRect(card, radius, theme.surface_card);
    painter_.StrokeRoundedRect(card, radius, theme.stroke_card);
    const float left = card.left + 16.0f * scale_;
    const float right = card.right - 16.0f * scale_;
    const float width = right - left;
    using pulse::l10n::StringId;

    // Title row.
    const float row_top = card.top + 12.0f * scale_, row_h = 22.0f * scale_;
    const std::wstring title = pulse::l10n::Get(StringId::ThisPcOverviewTitle);
    const float title_w = std::min(width, MeasureTextWidth(factory, title_fmt, title) + 2.0f * scale_);
    MakeBrush(dc, theme.text, brText_);
    DrawTextEndEllipsis(compositor_, title_fmt, brText_.get(), title, left, row_top, title_w, row_h);
    std::wstring count = pulse::l10n::Get(StringId::ThisPcOverviewDrives);
    capacity_detail::ReplaceToken(count, L"{count}", std::to_wstring(ov.drives.size()));
    const float count_left = left + title_w + 10.0f * scale_;
    const float count_w = MeasureTextWidth(factory, small_fmt, count) + 2.0f * scale_;
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    if (count_left + count_w <= right)
        DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), count, count_left, row_top + scale_,
                            count_w, row_h);
    // "75.0 GB 可用 · 已用 401.6 GB · 共 1.4 TB", the free size semibold.
    std::wstring space = pulse::l10n::Get(StringId::ThisPcOverviewSpace);
    capacity_detail::ReplaceToken(space, L"{used}", pulse::format::ByteSize(ov.total - std::min(ov.free, ov.total)));
    capacity_detail::ReplaceToken(space, L"{total}", pulse::format::ByteSize(ov.total));
    const std::wstring free_text = pulse::format::ByteSize(ov.free);
    size_t free_at = std::wstring::npos;
    capacity_detail::ReplaceToken(space, L"{free}", free_text, &free_at);
    const float space_left = count_left + count_w + 12.0f * scale_;
    ComPtr<IDWriteTextLayout> space_layout;
    if (space_left < right &&
        SUCCEEDED(factory->CreateTextLayout(space.c_str(), static_cast<UINT32>(space.size()), small_fmt,
                                            right - space_left, row_h, &space_layout)) && space_layout.get()) {
        space_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        space_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DWRITE_TEXT_METRICS metrics{};
        // Too narrow for all of it: leave it out rather than cut the numbers.
        if (SUCCEEDED(space_layout->GetMetrics(&metrics)) && metrics.widthIncludingTrailingWhitespace <= right - space_left) {
            if (free_at != std::wstring::npos) {
                const DWRITE_TEXT_RANGE bold{static_cast<UINT32>(free_at), static_cast<UINT32>(free_text.size())};
                space_layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, bold);
                MakeBrush(dc, theme.text, brCapacityStrong_);
                space_layout->SetDrawingEffect(brCapacityStrong_.get(), bold);
            }
            compositor_->DrawTextLayout(D2D1::Point2F(space_left, row_top + scale_), space_layout.get(),
                                        brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // Stacked bar: every drive gets at least 6% of the width, so a small
    // stick next to a large disk is still there to see.
    const float bar_top = row_top + row_h + 10.0f * scale_;
    const float bar_h = 10.0f * scale_, gap = 3.0f * scale_;
    const size_t n = ov.drives.size();
    double weight_sum = 0.0;
    std::vector<double> weights(n);
    for (size_t i = 0; i < n; ++i) {
        weights[i] = std::max(static_cast<double>(ov.drives[i].total), static_cast<double>(ov.total) * 0.06);
        weight_sum += weights[i];
    }
    const float bar_room = std::max(0.0f, width - gap * static_cast<float>(n - 1));
    float seg_left = left;
    for (size_t i = 0; i < n && weight_sum > 0.0; ++i) {
        const float seg_w = i + 1 == n ? right - seg_left
                                       : std::round(bar_room * static_cast<float>(weights[i] / weight_sum));
        const D2D1_RECT_F seg = D2D1::RectF(seg_left, bar_top, seg_left + std::max(0.0f, seg_w), bar_top + bar_h);
        painter_.DrawCapacityBar(seg, ov.drives[i].used, theme.accent, reveal, ov.drives[i].free);
        seg_left += seg_w + gap;
    }

    // Legend: a swatch in the drive's level colour, "C:" and its percent.
    float lx = left;
    const float legend_top = bar_top + bar_h + 9.0f * scale_, legend_h = 18.0f * scale_;
    for (const DriveOverviewView::Drive& drive : ov.drives) {
        const CapacityLevel level = CapacityLevelFor(drive.used, drive.free);
        const std::wstring pct = CapacityPercentText(drive.used);
        const float letter_w = MeasureTextWidth(factory, small_fmt, drive.letter) + 2.0f * scale_;
        const float pct_w = MeasureTextWidth(factory, small_fmt, pct) + 2.0f * scale_;
        const float item_w = 14.0f * scale_ + letter_w + 4.0f * scale_ + pct_w;
        if (lx + item_w > right) break;
        const float sw = 8.0f * scale_;
        const float sy = std::round(legend_top + (legend_h - sw) * 0.5f);
        painter_.FillRoundedRect(D2D1::RectF(lx, sy, lx + sw, sy + sw), 2.0f * scale_,
                                 level == CapacityLevel::Normal ? theme.accent : painter_.CapacityInk(level));
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), drive.letter, lx + 14.0f * scale_,
                            legend_top, letter_w, legend_h);
        MakeBrush(dc, painter_.CapacityInk(level), brTextSecondary_);
        DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), pct,
                            lx + 14.0f * scale_ + letter_w + 4.0f * scale_, legend_top, pct_w, legend_h);
        lx += item_w + 16.0f * scale_;
    }
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
}

void MainRenderer::DrawTypeChipLabel(ID2D1DeviceContext* dc, const std::wstring& chip,
                                     const D2D1_RECT_F& box, const D2D1_COLOR_F& ink,
                                     const D2D1_COLOR_F& background) {
    if (!dc || !compositor_ || chip.empty()) return;
    IDWriteTextFormat* format = compositor_->SmallFormat();
    if (compositor_->DrawLumaText(chip, format, box, ink, background, DWRITE_TEXT_ALIGNMENT_CENTER)) return;
    // DirectWrite path (Text rendering: Sharp / Smooth, or a LumaText fallback).
    // The small format is leading-aligned, so center explicitly; drawing it
    // as-is left the label against the chip's left edge.
    if (!format || !compositor_->DwriteFactory()) return;
    const D2D1_RECT_F rc = typography::SnapVerticalBounds(box);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(chip.c_str(), static_cast<UINT32>(chip.size()),
            format, std::max(1.0f, rc.right - rc.left), std::max(1.0f, rc.bottom - rc.top), &layout)) ||
        !layout.get()) {
        return;
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    MakeBrush(dc, ink, brTagDot_);
    compositor_->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.get(), brTagDot_.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float MainRenderer::CellTextWidth(const std::wstring& text, bool small_text) const {
    if (text.empty() || !compositor_) return 0.0f;
    // Details metadata shares the filename's size so a row reads as one line.
    IDWriteTextFormat* fmt = small_text ? compositor_->SmallFormat() : compositor_->FileNameFormat();
    if (!fmt) return 0.0f;
    if (cell_text_widths_scale_ != scale_ || cell_text_widths_.size() > 8192) {
        cell_text_widths_.clear();
        cell_text_widths_scale_ = scale_;
    }
    std::wstring key;
    key.reserve(text.size() + 1);
    key.push_back(small_text ? L's' : L'n');
    key += text;
    if (const auto it = cell_text_widths_.find(key); it != cell_text_widths_.end()) return it->second;
    float luma = 0.0f;
    compositor_->MeasureLumaText(text, fmt, luma);
    const float width = std::max(luma, MeasureTextWidth(compositor_->DwriteFactory(), fmt, text));
    cell_text_widths_.emplace(std::move(key), width);
    return width;
}

namespace {
// Stored widths are manual DIP widths; 0 (or a legacy ratio <= 1) is automatic.
float ManualWidthDip(float stored) { return stored > 1.0f ? stored : 0.0f; }
}

MainRenderer::DetailsColumnLayout MainRenderer::DetailsColumns(
    const D2D1_RECT_F& pane_bounds,
    const DetailsColumnWidths& dividers,
    bool search_view,
    const std::array<float, 4>& search_dividers,
    bool status_column) const {
    DetailsColumnLayout out;
    const auto content = DetailsContentRect(pane_bounds, scale_);
    out.left = content.left + margin_;
    out.right = std::max(out.left, content.right - margin_ * 3.0f);
    const float total = out.right - out.left;
    out.count = 1;
    out.kinds[0] = ColumnKind::Name;
    out.widths[0] = std::max(0.0f, total);
    if (total <= 0.0f) return out;

    const ColumnAutoWidths fitted = AutoColumnWidths();
    struct Meta { ColumnKind kind; float width; float automatic; };
    std::vector<Meta> meta;
    meta.reserve(6);
    // Display order: details_column_order. Search results carry no creation /
    // access times. Status is drawn only in cloud folders; it has no stored
    // width (always content-fitted).
    out.reorderable = true;
    auto fitted_width = [&](ColumnKind kind) {
        switch (kind) {
        case ColumnKind::Status: return fitted.status;
        case ColumnKind::Date: return fitted.date;
        case ColumnKind::Created: return fitted.created;
        case ColumnKind::Accessed: return fitted.accessed;
        case ColumnKind::Type: return fitted.type;
        default: return fitted.size;
        }
    };
    for (const uint8_t raw : details_order_) {
        const ColumnKind kind = static_cast<ColumnKind>(raw);
        const float automatic = fitted_width(kind);
        if (!(details_columns_ & (1u << static_cast<uint32_t>(kind)))) continue;
        const bool status = kind == ColumnKind::Status;
        if (status && (!status_column || search_view)) continue;
        const int slot = ManualColumnSlot(kind, search_view);
        if (slot < 0 && !status) continue;
        const float manual = slot < 0 ? 0.0f
            : ManualWidthDip(search_view ? search_dividers[static_cast<size_t>(slot)]
                                         : dividers[static_cast<size_t>(slot)]);
        meta.push_back({kind, (manual > 0.0f ? manual : automatic) * scale_, automatic * scale_});
    }
    auto meta_sum = [&] { float sum = 0.0f; for (const auto& m : meta) sum += m.width; return sum; };
    auto automatic_sum = [&] { float sum = 0.0f; for (const auto& m : meta) sum += m.automatic; return sum; };
    auto drop = [&](ColumnKind kind) {
        for (auto it = meta.begin(); it != meta.end(); ++it)
            if (it->kind == kind) { meta.erase(it); return true; }
        return false;
    };
    // Column visibility depends on the viewport, never on a divider drag.
    // Otherwise captured widths can reveal a hidden column on mouse-down,
    // or a growing column can remove the divider currently being dragged.
    const float min_name = kDetailsFitNameDip * scale_;
    const float min_path = kDetailsFitPathDip * scale_;
    bool path_column = search_view;
    if (search_view && total - automatic_sum() < min_name + min_path) {
        path_column = false;
        out.two_line = true;
    }
    // Low-value columns go first when the name would get too narrow.
    while (total - automatic_sum() < min_name + (path_column ? min_path : 0.0f) && meta.size() > 1) {
        if (!drop(ColumnKind::Accessed) && !drop(ColumnKind::Created) && !drop(ColumnKind::Type) &&
            !drop(ColumnKind::Status) && !drop(ColumnKind::Date)) break;
    }
    const float floor_flex = (kDetailsMinNameDip + (path_column ? kDetailsMinPathDip : 0.0f)) * scale_;
    const float available = std::max(0.0f, total - floor_flex);
    if (!meta.empty() && meta_sum() > available) {
        // A narrower window may no longer fit saved widths. Keep visible
        // metadata readable and compress only its surplus above the floor.
        const float floor_meta = std::min(48.0f * scale_, available / static_cast<float>(meta.size()));
        float surplus = 0.0f;
        for (const auto& m : meta) surplus += std::max(0.0f, m.width - floor_meta);
        const float factor = surplus > 0.0f
            ? (available - floor_meta * static_cast<float>(meta.size())) / surplus : 0.0f;
        for (auto& m : meta) m.width = floor_meta + std::max(0.0f, m.width - floor_meta) * factor;
    }
    const float flex = std::max(0.0f, total - meta_sum());
    int n = 0;
    if (path_column) {
        const float manual_name = ManualWidthDip(search_dividers[0]) * scale_;
        float name = 0.0f;
        if (manual_name > 0.0f) {
            const float lo = std::min(kDetailsMinNameDip * scale_, flex * 0.5f);
            name = std::clamp(manual_name, lo, std::max(lo, flex - kDetailsMinPathDip * scale_));
        } else {
            name = std::clamp(flex * 0.42f, std::min(min_name, flex * 0.5f), std::max(min_name, flex - min_path));
        }
        name = std::min(name, flex);
        out.kinds[n] = ColumnKind::Name; out.widths[n++] = name;
        out.kinds[n] = ColumnKind::Path; out.widths[n++] = flex - name;
    } else {
        out.kinds[n] = ColumnKind::Name; out.widths[n++] = flex;
    }
    for (const auto& m : meta) { out.kinds[n] = m.kind; out.widths[n++] = m.width; }
    out.count = n;
    return out;
}

namespace {
int FolderIconNameLines(Compositor* compositor, const PaneViewModel& vm, float scale);
const WallRows* PaneWallRows(const PaneViewModel& vm, float width, float scale, HWND owner);
}

ViewLayoutParams MainRenderer::ListRowHeightDip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    // Icon grids: the folder's name block and the icon size instead (every
    // ViewLayout of a pane comes through here, so paint, hit testing and
    // scrolling agree).
    if (IsZoomIconMode(vm.view_mode)) {
        ViewLayoutParams params(IconGridNameBlockDip(FolderIconNameLines(compositor_, vm, scale_)), vm.icon_dip);
        params.wall = PaneWallRows(vm, std::max(1.0f, pane_bounds.right - pane_bounds.left), scale_, notify_hwnd_);
        return params;
    }
    const float base = ListRowHeightDip(vm);
    if (vm.view_mode != ViewMode::Details || !vm.is_search || vm.content_results) return base;
    return DetailsColumns(pane_bounds, vm).two_line ? std::max(base, kDetailsTwoLineMinRowDip) : base;
}

int MainRenderer::ManualColumnSlot(ColumnKind kind, bool search_view) noexcept {
    using K = ColumnKind;
    switch (kind) {
    case K::Name: return search_view ? 0 : -1;
    case K::Date: return search_view ? 1 : 0;
    case K::Type: return search_view ? 2 : 1;
    case K::Size: return search_view ? 3 : 2;
    case K::Created: return search_view ? -1 : 3;
    case K::Accessed: return search_view ? -1 : 4;
    default: return -1;
    }
}

int MainRenderer::ColumnDropSlot(const DetailsColumnLayout& layout, int from, float x,
                                 float* line_x) noexcept {
    const int first = layout.FirstOrderable();
    if (!layout.reorderable || from < first || from >= layout.count) return -1;
    int best = -1;
    float best_distance = 0.0f;
    float best_x = 0.0f;
    for (int slot = first; slot <= layout.count; ++slot) {
        const float edge = slot >= layout.count ? layout.right : layout.DividerX(slot - 1);
        const float distance = std::abs(x - edge);
        if (best < 0 || distance < best_distance) {
            best = slot;
            best_distance = distance;
            best_x = edge;
        }
    }
    if (best == from || best == from + 1) return -1;
    if (line_x) *line_x = best_x;
    return best;
}

DetailsColumnOrder MainRenderer::ReorderedColumns(const DetailsColumnLayout& layout,
                                                  const DetailsColumnOrder& order, int from, int slot) noexcept {
    const int first = layout.FirstOrderable();
    if (!layout.reorderable || from < first || from >= layout.count || slot < first || slot > layout.count ||
        slot == from || slot == from + 1)
        return NormalizeDetailsColumnOrder(order);
    const auto kind = static_cast<uint8_t>(layout.kinds[static_cast<size_t>(from)]);
    if (slot < layout.count)
        return MoveDetailsColumn(order, kind, static_cast<uint8_t>(layout.kinds[static_cast<size_t>(slot)]), false);
    return MoveDetailsColumn(order, kind, static_cast<uint8_t>(layout.kinds[static_cast<size_t>(layout.count - 1)]),
                             true);
}

void MainRenderer::AutoFitColumnDivider(const D2D1_RECT_F& pane_bounds,
                                        DetailsColumnWidths& dividers, bool search_view,
                                        std::array<float, 4>& search_dividers, int divider_index,
                                        bool status_column) const {
    const DetailsColumnLayout layout =
        DetailsColumns(pane_bounds, dividers, search_view, search_dividers, status_column);
    if (divider_index < 0 || divider_index >= layout.count - 1) return;
    for (int side : {divider_index, divider_index + 1}) {
        const int slot = ManualColumnSlot(layout.kinds[static_cast<size_t>(side)], search_view);
        if (slot < 0) continue;
        if (search_view) search_dividers[static_cast<size_t>(slot)] = 0.0f;
        else dividers[static_cast<size_t>(slot)] = 0.0f;
    }
}

MainRenderer::DetailsColumnLayout MainRenderer::AnimateColumns(DetailsColumnLayout target, int pane_index) {
    if (pane_index < 0 || pane_index >= static_cast<int>(col_fit_.size())) return target;
    auto& f = col_fit_[static_cast<size_t>(pane_index)];
    if (f.start) {
        const float ms = static_cast<float>(motion_now_ - f.start);
        if (f.count != target.count || ms >= static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) {
            f.start = 0;
        } else {
            // Pop: a touch of overshoot, like the prototype's spring auto-fit.
            const float p = motion::SpringProgress(motion::spring::Pop, ms);
            float sum_from = 0.0f, sum_to = 0.0f;
            for (int i = 0; i < target.count; ++i) {
                sum_from += f.from[static_cast<size_t>(i)];
                sum_to += target.widths[static_cast<size_t>(i)];
            }
            // A resized pane since: keep the old shares, on the new total.
            const float k = sum_from > 0.0f ? sum_to / sum_from : 1.0f;
            for (int i = 0; i < target.count; ++i) {
                float& w = target.widths[static_cast<size_t>(i)];
                const float from = f.from[static_cast<size_t>(i)] * k;
                w = (std::max)(0.0f, std::round(from + (w - from) * p));
            }
            RequestMotionFrame();
        }
    }
    f.last = target.widths;
    f.last_count = target.count;
    return target;
}

void MainRenderer::DrawColumnBadge(int pane_index, const Theme& theme) {
    auto& b = col_badge_;
    if (b.pane != pane_index) return;
    float alpha = 1.0f;
    if (!b.on) {
        const uint64_t age = motion_now_ - b.released;
        if (age >= 140) {
            b = {};
            return;
        }
        alpha = 1.0f - static_cast<float>(age) / 140.0f;
        RequestMotionFrame();
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->SmallFormat();
    if (!dc || !factory || !fmt) return;
    // Guide through the list, 60% accent.
    if (b.on) {
        MakeBrush(dc, WithAlpha(theme.accent, 0.6f), brFillInput_);
        dc->FillRectangle(D2D1::RectF(b.guide_x - scale_, b.guide_top, b.guide_x + scale_, b.guide_bottom),
                          brFillInput_.get());
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor) || !ScreenToClient(compositor_->Hwnd(), &cursor)) return;
    const int w_dip = static_cast<int>(std::lround(b.w / scale_));
    const int d_dip = w_dip - static_cast<int>(std::lround(b.w0 / scale_));
    const std::wstring value = L"  " + std::to_wstring(w_dip) + L"px  ";
    const std::wstring delta = (d_dip >= 0 ? L"+" : L"") + std::to_wstring(d_dip);
    const std::wstring text = b.name + value + delta;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 1000.0f * scale_,
                                         26.0f * scale_, &layout)))
        return;
    layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(b.name.size())});
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float h = 26.0f * scale_;
    const float pw = std::ceil(m.widthIncludingTrailingWhitespace) + 18.0f * scale_;
    const float pop = 0.8f + 0.2f * motion::SpringProgress(motion::spring::Pop, static_cast<float>(motion_now_ - b.since));
    if (motion_now_ - b.since < motion::SpringSettleMs(motion::spring::Pop)) RequestMotionFrame();
    const float left = static_cast<float>(cursor.x) + 14.0f * scale_;
    const float top = static_cast<float>(cursor.y) + 16.0f * scale_;
    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(pop, pop, D2D1::Point2F(left, top)) * saved);
    const bool dark = theme.bg.r < 0.5f;
    const D2D1_COLOR_F plate = dark ? D2D1::ColorF(0xF3F4F6, alpha) : D2D1::ColorF(0x111827, alpha);
    const D2D1_COLOR_F ink = dark ? D2D1::ColorF(0x111827, alpha) : D2D1::ColorF(0xFFFFFF, alpha);
    MakeBrush(dc, D2D1::ColorF(0, 0.18f * alpha), brFillInput_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top + 3.0f * scale_, left + pw, top + h + 3.0f * scale_),
                                               h * 0.5f, h * 0.5f), brFillInput_.get());
    MakeBrush(dc, plate, brFillInput_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + pw, top + h), h * 0.5f, h * 0.5f),
                             brFillInput_.get());
    MakeBrush(dc, ink, brFillInput_);
    // The change reads quieter than the value.
    ComPtr<ID2D1SolidColorBrush> quiet;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(ink.r, ink.g, ink.b, ink.a * 0.6f), &quiet)))
        layout->SetDrawingEffect(quiet.get(), DWRITE_TEXT_RANGE{static_cast<UINT32>(b.name.size() + value.size()),
                                                                 static_cast<UINT32>(delta.size())});
    compositor_->DrawTextLayout(D2D1::Point2F(left + 9.0f * scale_, top), layout.get(), brFillInput_.get());
    dc->SetTransform(saved);
}

namespace {
// Move the shared edge while keeping the adjacent pair's outer edges fixed.
// Flexible name/path widths are implicit; metadata widths are stored in DIP.
template <size_t N>
std::array<float, N> ResizeColumns(const MainRenderer::DetailsColumnLayout& layout,
                                   std::array<float, N> stored, bool search_view,
                                   int divider_index, float cursor_x, float scale, int media = 0) {
    using K = MainRenderer::ColumnKind;
    if (divider_index < 0 || divider_index >= layout.count - 1 || scale <= 0.0f) return stored;
    for (float& value : stored) if (value <= 1.0f) value = 0.0f;   // drop legacy ratios
    const K left = layout.kinds[static_cast<size_t>(divider_index)];
    const K right = layout.kinds[static_cast<size_t>(divider_index + 1)];
    const float x0 = divider_index == 0 ? layout.left : layout.DividerX(divider_index - 1);
    const float x1 = layout.DividerX(divider_index);
    const float x2 = x1 + layout.widths[static_cast<size_t>(divider_index + 1)];
    const float min_meta = std::min(48.0f * scale, (x2 - x0) * 0.5f);
    const float min_flex = std::min((left == K::Path ? kDetailsMinPathDip : media ? 200.0f : kDetailsMinNameDip) * scale,
                                   std::max(0.0f, x2 - x0 - min_meta));
    auto put = [&](K kind, float px) {
        if (media && !MainRenderer::IsMediaColumn(kind)) return;
        const int slot = media ? MainRenderer::MediaColumnSlot(kind, media)
                               : MainRenderer::ManualColumnSlot(kind, search_view);
        if (slot >= 0 && static_cast<size_t>(slot) < N) stored[static_cast<size_t>(slot)] = std::max(px / scale, 1.01f);
    };
    if (left == K::Name && right == K::Path) {
        put(K::Name, std::clamp(cursor_x, x0 + min_flex, std::max(x0 + min_flex, x2 - kDetailsMinPathDip * scale)) - x0);
    } else if (left == K::Name || left == K::Path) {
        // Grow/shrink the metadata column on the right; its right edge is fixed.
        put(right, x2 - std::clamp(cursor_x, x0 + min_flex, x2 - min_meta));
    } else {
        const float edge = std::clamp(cursor_x, x0 + min_meta, std::max(x0 + min_meta, x2 - min_meta));
        put(left, edge - x0);
        put(right, x2 - edge);
    }
    return stored;
}
}

int MainRenderer::MediaColumnSlot(ColumnKind kind, int media) noexcept {
    using K = ColumnKind;
    static constexpr K kinds[3][4] = {
        {K::MediaTaken, K::MediaDims, K::MediaCamera, K::Size},
        {K::MediaTitle, K::MediaArtist, K::MediaAlbum, K::MediaDuration},
        {K::MediaDuration, K::MediaDims, K::MediaFps, K::Size}};
    if (media < 1 || media > 3) return -1;
    for (int i = 0; i < 4; ++i) if (kinds[media - 1][i] == kind) return i;
    return -1;
}

std::array<float, 4> MainRenderer::ResizeMediaColumnDivider(
    const D2D1_RECT_F& bounds, const PaneViewModel& view, int divider, float cursor_x) const {
    if (view.media_columns < 1 || view.media_columns > 3) return {};
    return ResizeColumns(MediaColumns(bounds, view), view.media_column_widths[view.media_columns - 1],
                         false, divider, cursor_x, scale_, view.media_columns);
}

DetailsColumnWidths MainRenderer::ResizeMediaDetailsColumnDivider(
    const D2D1_RECT_F& bounds, const PaneViewModel& view, int divider, float cursor_x) const {
    return ResizeColumns(MediaColumns(bounds, view), view.details_column_dividers,
                         false, divider, cursor_x, scale_);
}

DetailsColumnWidths MainRenderer::ResizeDetailsColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const DetailsColumnWidths& dividers,
    int divider_index, float cursor_x, bool status_column) const {
    return ResizeColumns(DetailsColumns(pane_bounds, dividers, false, {}, status_column), dividers, false,
                         divider_index, cursor_x, scale_);
}

std::array<float, 4> MainRenderer::ResizeSearchColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 4>& dividers,
    int divider_index, float cursor_x) const {
    return ResizeColumns(DetailsColumns(pane_bounds, {}, true, dividers), dividers, true,
                         divider_index, cursor_x, scale_);
}

D2D1_RECT_F MainRenderer::NameCellRect(const D2D1_RECT_F& pane_bounds, int view_row, float scroll_y,
                                       float extra_top, ViewMode mode, float scroll_x,
                                       size_t item_count,
                                       const DetailsColumnWidths& column_dividers,
                                       bool search_view,
                                       const std::array<float, 4>& search_dividers,
                                       float row_height_px) const {
    const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, mode);
    if (mode != ViewMode::Details) {
        ViewLayout layout(mode, list, item_count, scroll_x, scroll_y, scale_,
                          IsZoomIconMode(mode) ? row_height_px / scale_ : row_height_dip_);
        return layout.NameRect(view_row);
    }
    const float list_x = list.left;
    const float list_y = list.top;
    const float name_w = DetailsColumns(list, column_dividers, search_view,
                                        search_dividers).widths[0];
    const float icon_size = 16.0f * scale_;
    const float row_h = row_height_px > 0.0f ? row_height_px : row_height_;
    const float row_y = list_y + static_cast<float>(view_row) * row_h - scroll_y;
    const float name_x = list_x + margin_ + icon_size + margin_ + 4.0f * scale_;
    const float name_avail = name_w - icon_size - margin_ * 3;
    const float inset = 1.0f * scale_;
    return D2D1::RectF(name_x, row_y + inset, name_x + std::max(40.0f * scale_, name_avail),
                       row_y + row_h - inset);
}

namespace {
// Details tree: a disclosure chevron (right when closed, down when open).
void DrawTreeChevron(ID2D1DeviceContext* dc, ID2D1Brush* brush, float cx, float cy,
                     float arm, float stroke, bool expanded) {
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (!factory || FAILED(factory->CreatePathGeometry(&path))) return;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return;
    const float half = arm * 0.5f;
    if (expanded) {
        sink->BeginFigure(D2D1::Point2F(cx - arm, cy - half), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(cx, cy + half));
        sink->AddLine(D2D1::Point2F(cx + arm, cy - half));
    } else {
        sink->BeginFigure(D2D1::Point2F(cx - half, cy - arm), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(cx + half, cy));
        sink->AddLine(D2D1::Point2F(cx - half, cy + arm));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    dc->DrawGeometry(path.Get(), brush, stroke);
}

// Details tree guide lines over rows first..last (BuildTreeGuides): long
// vertical runs per level, a tick into each child and a rounded elbow into
// the last one. Two geometries, the plain lines and the highlighted folder's.
template <class CellOf>
void DrawTreeGuides(ID2D1DeviceContext* dc, const DetailsTreeRows& tree, const TreeGuides& g,
                    CellOf cell_of, float base_x, float scale, ID2D1Brush* plain, ID2D1Brush* hi,
                    float plain_w, float hi_w) {
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory || g.rows.empty()) return;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> paths[2];
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sinks[2];
    for (int k = 0; k < 2; ++k)
        if (FAILED(factory->CreatePathGeometry(&paths[k])) || FAILED(paths[k]->Open(&sinks[k]))) return;
    const auto bit = [](int level) { return uint64_t{1} << level; };
    const auto level_x = [&](int level) {
        return std::floor(base_x + (static_cast<float>(level) * kTreeIndentDip + kTreeChevronDip * 0.5f) * scale) + 0.5f;
    };
    std::array<float, kTreeGuideMaxLevels> run_top{};
    std::array<bool, kTreeGuideMaxLevels> run_hi{};
    uint64_t open = 0;
    float prev_bottom = 0.0f;
    const auto line = [&](bool highlighted, D2D1_POINT_2F a, D2D1_POINT_2F b) {
        ID2D1GeometrySink* s = sinks[highlighted ? 1 : 0].Get();
        s->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine(b);
        s->EndFigure(D2D1_FIGURE_END_OPEN);
    };
    const auto close_run = [&](int level, float y_end) {
        const float x = level_x(level);
        if (y_end > run_top[static_cast<size_t>(level)])
            line(run_hi[static_cast<size_t>(level)], D2D1::Point2F(x, run_top[static_cast<size_t>(level)]),
                 D2D1::Point2F(x, y_end));
        open &= ~bit(level);
    };
    for (size_t idx = 0; idx < g.rows.size(); ++idx) {
        const int row = g.first + static_cast<int>(idx);
        const TreeGuideRow& gr = g.rows[idx];
        const D2D1_RECT_F cell = cell_of(row);
        if (cell.bottom - cell.top < 0.5f) continue;
        uint64_t want = gr.through;
        if (gr.join >= 0) want |= bit(gr.join);
        for (int l = 0; l < kTreeGuideMaxLevels && (open >> l); ++l)
            if ((open & bit(l)) && (!(want & bit(l)) || run_hi[static_cast<size_t>(l)] != g.Highlighted(row, l)))
                close_run(l, prev_bottom);
        for (int l = 0; l < kTreeGuideMaxLevels && (want >> l); ++l) {
            if (!(want & bit(l)) || (open & bit(l))) continue;
            run_top[static_cast<size_t>(l)] = cell.top;
            run_hi[static_cast<size_t>(l)] = g.Highlighted(row, l);
            open |= bit(l);
        }
        if (gr.join >= 0) {
            const int l = gr.join;
            const float x = level_x(l);
            const float mid = std::round((cell.top + cell.bottom) * 0.5f) + 0.5f;
            const DetailsTreeRow* tr = tree.At(row);
            const bool folder = tr && tr->kind == TreeRowKind::Entry && tr->expandable;
            // Short of the child's chevron, or of its icon past the empty slot.
            const float x_end = x + (folder ? 8.5f : 13.5f) * scale;
            const bool h = run_hi[static_cast<size_t>(l)];
            if (gr.last) {
                const float top = run_top[static_cast<size_t>(l)];
                const float r = std::min(5.0f * scale, mid - top);
                ID2D1GeometrySink* s = sinks[h ? 1 : 0].Get();
                s->BeginFigure(D2D1::Point2F(x, top), D2D1_FIGURE_BEGIN_HOLLOW);
                if (r >= 0.5f) {
                    s->AddLine(D2D1::Point2F(x, mid - r));
                    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, mid), D2D1::SizeF(r, r), 0.0f,
                                               D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                } else {
                    s->AddLine(D2D1::Point2F(x, mid));
                }
                s->AddLine(D2D1::Point2F(x_end, mid));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                open &= ~bit(l);
            } else {
                line(h, D2D1::Point2F(x, mid), D2D1::Point2F(x_end, mid));
            }
        }
        prev_bottom = cell.bottom;
    }
    for (int l = 0; l < kTreeGuideMaxLevels && (open >> l); ++l)
        if (open & bit(l)) close_run(l, prev_bottom);
    for (int k = 0; k < 2; ++k)
        if (FAILED(sinks[k]->Close())) return;
    dc->DrawGeometry(paths[0].Get(), plain, plain_w);
    dc->DrawGeometry(paths[1].Get(), hi, hi_w);
}

// Details tree "more" row: "showing X of Y", then show more / show all / open
// in a new tab. Shared by DrawList and TreeMoreActionAt so they never disagree.
struct TreeMoreLayout {
    std::wstring label;
    float label_x = 0.0f;
    float label_w = 0.0f;
    std::wstring texts[3];
    int actions[3]{};
    D2D1_RECT_F buttons[3]{};
    int count = 0;
};

TreeMoreLayout LayoutTreeMore(const DetailsTreeRow& row, const D2D1_RECT_F& cell, float x0,
                              float scale, IDWriteFactory2* factory, IDWriteTextFormat* fmt) {
    TreeMoreLayout out;
    wchar_t text[192]{};
    swprintf_s(text, l10n::Get(l10n::StringId::TreeShownOfFormat).c_str(),
               pulse::format::GroupedInt(row.shown).c_str(),
               pulse::format::GroupedInt(row.total).c_str());
    out.label = text;
    out.label_x = x0;
    out.label_w = MeasureTextWidth(factory, fmt, out.label);
    const uint32_t remaining = row.total > row.shown ? row.total - row.shown : 0;
    const uint32_t step = std::min(remaining, kTreeMoreStep);
    std::wstring texts[3];
    int actions[3]{};
    int n = 0;
    if (step > 0) {
        swprintf_s(text, l10n::Get(l10n::StringId::TreeShowMoreFormat).c_str(),
                   pulse::format::GroupedInt(step).c_str());
        texts[n] = text;
        actions[n++] = 0;
    }
    if (remaining > kTreeMoreStep) {
        texts[n] = l10n::Get(l10n::StringId::TreeShowAll);
        actions[n++] = 1;
    }
    texts[n] = l10n::Get(l10n::StringId::OpenNewTab);
    actions[n++] = 2;
    const float pad = 10.0f * scale;
    const float gap = 6.0f * scale;
    const float button_h = 24.0f * scale;
    const float top = std::round((cell.top + cell.bottom - button_h) * 0.5f);
    float bx = std::round(x0 + out.label_w + 12.0f * scale);
    for (int k = 0; k < n; ++k) {
        const float bw = std::round(MeasureTextWidth(factory, fmt, texts[k]) + pad * 2.0f);
        if (bx + bw > cell.right - 6.0f * scale) break;
        out.texts[out.count] = texts[k];
        out.actions[out.count] = actions[k];
        out.buttons[out.count] = D2D1::RectF(bx, top, bx + bw, top + button_h);
        ++out.count;
        bx += bw + gap;
    }
    return out;
}
} // namespace

namespace {
constexpr float kCheckGutterDip = 24.0f;
constexpr float kCheckBoxDip = 16.0f;  // as large as a Details icon, no larger

void EnsureRoundCapStroke(ID2D1DeviceContext* dc, ComPtr<ID2D1StrokeStyle>& stroke) {
    if (stroke.get() || !dc) return;
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;
    D2D1_STROKE_STYLE_PROPERTIES props{};
    props.startCap = D2D1_CAP_STYLE_ROUND;
    props.endCap = D2D1_CAP_STYLE_ROUND;
    factory->CreateStrokeStyle(props, nullptr, 0, &stroke);
    factory->Release();
}

// Item check box (A): a hairline box that stays
// transparent until checked; checked and mixed fill with the accent and carry
// a round-capped tick or dash. Antialiased per primitive with the box edges on
// whole device pixels and the hairline centred on a pixel row, so it stays
// crisp. state: 0 off, 1 checked, 2 mixed. progress draws the tick in (0..1).
void DrawItemCheckBox(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* round,
                      D2D1_RECT_F box, float scale, const Theme& theme, bool high_contrast,
                      int state, bool hot, float progress, float alpha) {
    if (!dc || !brush) return;
    const D2D1_ANTIALIAS_MODE saved = dc->GetAntialiasMode();
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float side = std::round(kCheckBoxDip * scale);
    box.left = std::round(box.left);
    box.top = std::round(box.top);
    box.right = box.left + side;
    box.bottom = box.top + side;
    const float radius = 4.0f * scale;
    const bool dark = theme.bg.r < 0.5f;
    const auto with = [alpha](D2D1_COLOR_F c) { c.a *= alpha; return c; };
    if (state == 0) {
        const float px = std::max(1.0f, std::round(scale));
        const float h = px * 0.5f;
        brush->SetColor(with(high_contrast ? theme.text
            : dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, hot ? 0.66f : 0.42f)
                   : D2D1::ColorF(0.0f, 0.0f, 0.0f, hot ? 0.56f : 0.36f)));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + h, box.top + h, box.right - h, box.bottom - h),
                                                   radius - h, radius - h), brush, px);
        dc->SetAntialiasMode(saved);
        return;
    }
    // Dark backgrounds take a lighter accent with a dark tick (contrast > 4.5:1).
    D2D1_COLOR_F fill = theme.accent;
    if (dark && !high_contrast) {
        const float k = hot ? 0.58f : 0.5f;
        fill = D2D1::ColorF(fill.r + (1.0f - fill.r) * k, fill.g + (1.0f - fill.g) * k,
                            fill.b + (1.0f - fill.b) * k, 1.0f);
    } else if (hot && !high_contrast) {
        fill = D2D1::ColorF(fill.r * 0.9f, fill.g * 0.9f, fill.b * 0.9f, 1.0f);
    }
    brush->SetColor(with(fill));
    dc->FillRoundedRectangle(D2D1::RoundedRect(box, radius, radius), brush);
    const D2D1_COLOR_F ink = high_contrast ? theme.accent_text
        : dark ? D2D1::ColorF(0x1A1C2C) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    brush->SetColor(with(ink));
    const float u = side / 16.0f;
    const float stroke = std::max(1.5f, 1.6f * scale);
    const auto at = [&](float x, float y) { return D2D1::Point2F(box.left + x * u, box.top + y * u); };
    if (state == 2) {
        dc->DrawLine(at(4.8f, 8.0f), at(11.2f, 8.0f), brush, stroke, round);
        dc->SetAntialiasMode(saved);
        return;
    }
    const D2D1_POINT_2F a = at(4.2f, 8.4f), b = at(6.9f, 11.0f), c = at(11.8f, 5.4f);
    const float l1 = std::hypot(b.x - a.x, b.y - a.y), l2 = std::hypot(c.x - b.x, c.y - b.y);
    const float p = std::clamp(progress, 0.0f, 1.0f);
    const float run = (l1 + l2) * (1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p));  // ease-out
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> tick;
    if (factory && run > 0.01f && SUCCEEDED(factory->CreatePathGeometry(&tick)) && tick.get()) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(tick->Open(&sink)) && sink.get()) {
            sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
            if (run <= l1) {
                sink->AddLine(D2D1::Point2F(a.x + (b.x - a.x) * run / l1, a.y + (b.y - a.y) * run / l1));
            } else {
                sink->AddLine(b);
                const float t2 = std::min(1.0f, (run - l1) / l2);
                sink->AddLine(D2D1::Point2F(b.x + (c.x - b.x) * t2, b.y + (c.y - b.y) * t2));
            }
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            dc->DrawGeometry(tick.get(), brush, stroke, round);
        }
    }
    if (factory) factory->Release();
    dc->SetAntialiasMode(saved);
}

}  // namespace

float MainRenderer::CheckGutterDip(const PaneViewModel& vm) const {
    return item_checks_ && vm.view_mode == ViewMode::Details ? kCheckGutterDip : 0.0f;
}

float MainRenderer::DetailsLeadDip(const PaneViewModel& vm, int view_index) const {
    if (vm.view_mode != ViewMode::Details) return 0.0f;
    const DetailsTreeRow* tree_row = vm.tree ? vm.tree->At(view_index) : nullptr;
    return (tree_row ? TreeOffsetDip(*tree_row) : 0.0f) + CheckGutterDip(vm);
}

D2D1_RECT_F MainRenderer::ItemCheckRect(const PaneViewModel& vm, const D2D1_RECT_F& cell,
                                        float details_base_x) const {
    if (!item_checks_) return {};
    const float side = kCheckBoxDip * scale_;
    if (vm.view_mode == ViewMode::Details) {
        const float left = std::round(details_base_x + 2.0f * scale_);  // clear of the selection bar
        const float top = std::round(cell.top + (cell.bottom - cell.top - side) * 0.5f);
        return D2D1::RectF(left, top, left + side, top + side);
    }
    if (vm.view_mode == ViewMode::ExtraLargeIcons || vm.view_mode == ViewMode::LargeIcons ||
        vm.view_mode == ViewMode::MediumIcons) {
        const float left = std::round(cell.left + 6.0f * scale_);
        const float top = std::round(cell.top + 6.0f * scale_);
        return D2D1::RectF(left, top, left + side, top + side);
    }
    return {};
}

D2D1_RECT_F MainRenderer::HeaderCheckRect(float columns_left, float header_top) const {
    const float side = kCheckBoxDip * scale_;
    const float left = std::round(columns_left + 4.0f * scale_);  // over the rows' boxes
    const float top = std::round(header_top + (column_header_height_ - side) * 0.5f);
    return D2D1::RectF(left, top, left + side, top + side);
}

bool MainRenderer::PointInItemName(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                   int source_index, float x, float y) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    if (!compositor_ || !compositor_->DwriteFactory() || source_index < 0) return false;
    const int view_index = vm.ViewIndex(source_index);
    if (view_index < 0) return false;

    const ListEntryView& entry = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
    if (entry.name.empty()) return false;
    const float extra_top = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F name = NameCellRect(
        pane_bounds, view_index, vm.scroll_y, extra_top,
        vm.view_mode, vm.scroll_x, vm.EntryCount(), vm.details_column_dividers,
        vm.is_search, vm.search_column_dividers,
        ListRowHeightDip(vm, PaneListRect(pane_bounds, extra_top, vm.view_mode)).row_height_dip * scale_);
    if (IsZoomIconMode(vm.view_mode)) {
        // NameCellRect has no view model: take the icon size from the pane's layout.
        const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, vm.view_mode);
        name = ViewLayout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_,
                          ListRowHeightDip(vm, list)).NameRect(view_index);
    }
    if (const ListGroups* groups = vm.Groups()) {
        // Grouped rows are offset by headers; take the vertical band from the layout.
        const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, vm.view_mode);
        const ViewLayout grouped(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                                 scale_, ListRowHeightDip(vm, list), groups);
        const D2D1_RECT_F cell = grouped.ItemRect(view_index);
        if (cell.bottom - cell.top < 0.5f) return false;
        if (vm.view_mode == ViewMode::Details || vm.view_mode == ViewMode::Content) {
            const float h = name.bottom - name.top;
            name.top = cell.top + std::max(0.0f, (cell.bottom - cell.top - h) * 0.5f);
            name.bottom = name.top + h;
        } else {
            name = grouped.NameRect(view_index);  // grids: the cell moved in both axes
        }
    }
    if (vm.view_mode == ViewMode::Details) {
        const auto actual = DetailsColumns(pane_bounds, vm);
        const auto defaults = DetailsColumns(pane_bounds, vm.details_column_dividers,
                                            vm.is_search, vm.search_column_dividers);
        name.right += actual.Width(ColumnKind::Name) - defaults.Width(ColumnKind::Name);
        if (const float lead = DetailsLeadDip(vm, view_index); lead > 0.0f)
            name.left = std::min(name.right, name.left + std::round(lead * scale_));
    }
    const bool icon_grid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                           vm.view_mode == ViewMode::LargeIcons ||
                           vm.view_mode == ViewMode::MediumIcons;
    float available = std::max(0.0f, name.right - name.left);
    const std::vector<D2D1_COLOR_F>* tag_dots = nullptr;
    const std::vector<int>* tag_indices = vm.tag_catalog
        ? vm.tag_catalog->TagIndicesForPath(entry.path) : nullptr;
    if (!tag_indices && vm.tag_dots) {
        const auto found = vm.tag_dots->find(source_index);
        if (found != vm.tag_dots->end()) tag_dots = &found->second;
    }
    const size_t tag_count = tag_indices ? tag_indices->size() : (tag_dots ? tag_dots->size() : 0);
    const int visible_dots = static_cast<int>(std::min<size_t>(3, tag_count));
    const float diameter = 8.0f * scale_;
    const float tag_gap = 4.0f * scale_;
    const float overlap_w = OverlapTagsWidth(visible_dots, diameter);
    if (overlap_w > 0.0f)
        available = std::max(24.0f * scale_, available - overlap_w - tag_gap);

    const std::wstring fitted = FitFileName(
        compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(),
        ShownName(entry), available);
    const float text_width = MeasureLayoutText(
        compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(), fitted);
    float text_left = name.left;
    if (icon_grid) {
        const float leftover = std::max(0.0f, (name.right - name.left) - text_width - tag_gap);
        const float dots_width = visible_dots > 0 &&
            SpreadTagsWidth(visible_dots, diameter, tag_gap) <= leftover + 0.5f
            ? SpreadTagsWidth(visible_dots, diameter, tag_gap)
            : overlap_w;
        const float group_width = text_width + (dots_width > 0.0f ? tag_gap + dots_width : 0.0f);
        text_left += std::max(0.0f, (name.right - name.left - group_width) * 0.5f);
    }
    const float slop = 2.0f * scale_;
    return x >= text_left - slop && x < text_left + text_width + slop &&
           y >= name.top && y < name.bottom;
}

namespace {
// Width and height of an icon-grid name laid out as DrawCenteredIconName
// lays it out (defined with FitIconName below).
D2D1_SIZE_F IconGridNameSize(Compositor* compositor, const std::wstring& name, float width, int max_lines);
} // namespace

bool MainRenderer::PointInItemGrip(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                   int source_index, float x, float y, bool /*selected*/) const {
    const int index = vm.ViewIndex(source_index);
    if (index < 0 || !compositor_) return false;
    const auto pane = PaneBodyBounds(vm, full_bounds);
    const auto list = PaneListRect(vm, full_bounds);
    const ViewLayout geometry(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                              scale_, ListRowHeightDip(vm, list), vm.Groups());
    const auto inside = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (!inside(list) || !inside(geometry.ItemRect(index))) return false;
    const bool grid = vm.view_mode == ViewMode::MediumIcons || vm.view_mode == ViewMode::LargeIcons ||
                      vm.view_mode == ViewMode::ExtraLargeIcons;
    if (grid) {
        const auto& entry=MakeVisibleEntry(vm,static_cast<size_t>(source_index));
        const ItemArtworkKey key{vm.view_generation,source_index,static_cast<int>(vm.view_mode),
            entry.path,entry.name,entry.modified_value,entry.size_value,geometry.ItemRect(index)};
        if (item_artwork_hits_.Contains(key,x,y)) return true;
    } else if (inside(geometry.IconRect(index))) return true;
    if (vm.view_mode == ViewMode::Details) {
        const auto columns = DetailsColumns(pane, vm);
        const auto& e = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
        const auto row = geometry.ItemRect(index);
        const auto hit_text = [&](const std::wstring& text, float left, float width, bool trailing = false) {
            if (text.empty() || width <= 1.0f) return false;
            const auto fitted = FitEndEllipsis(text, width, [&](const std::wstring& value) { return CellTextWidth(value); });
            const float ink = std::min(width, CellTextWidth(fitted));
            if (trailing) left += width - ink;
            const float height = std::min(row.bottom-row.top, 20.0f*scale_);
            const float top = (row.top+row.bottom-height)*0.5f;
            return inside(D2D1::RectF(left, top, left+ink, top+height));
        };
        for (int col = 1; col < columns.count; ++col) {
            float left = columns.DividerX(col-1)+8.0f*scale_;
            float width = std::max(0.0f, columns.widths[static_cast<size_t>(col)]-16.0f*scale_);
            if (x < left || x >= left+width) continue;
            const auto kind = columns.kinds[static_cast<size_t>(col)];
            std::wstring text;
            bool trailing = false;
            switch (kind) {
            case ColumnKind::Date:
                text = list_smart_date_ && e.modified_value && !e.date_text.empty() ? SmartListDate(e.modified_value) : e.date_text;
                if (mtime_hint_.style == 1 && !vm.is_changes && !vm.is_recycle) { left += kMtimeDotLeadDip*scale_; width -= kMtimeDotLeadDip*scale_; }
                break;
            case ColumnKind::Created: text = list_smart_date_ && e.created_value && !e.created_text.empty() ? SmartListDate(e.created_value) : e.created_text; break;
            case ColumnKind::Accessed: text = list_smart_date_ && e.accessed_value && !e.accessed_text.empty() ? SmartListDate(e.accessed_value) : e.accessed_text; break;
            case ColumnKind::Status: text = CloudLookFor(e,Theme{}).label; left += 22.0f*scale_; width -= 22.0f*scale_; break;
            case ColumnKind::Type: {
                text = e.type_text;
                const auto chip = vm.is_changes ? std::wstring() : TypeChipLabel(e.name,e.is_dir);
                const float shift = vm.dup_chips ? 8.0f*scale_ : !chip.empty() && TypeChipWidthDip(chip)*scale_ < width*0.6f && !IsHighContrast() ? TypeChipWidthDip(chip)*scale_ : 0.0f;
                if (shift > 0 && hit_text(chip,left,shift)) return true;
                left += shift; width -= shift; break;
            }
            case ColumnKind::Path: {
                text = FolderOf(e.path);
                for (const auto& [root,label] : vm.path_roots) {
                    if (text.size() >= root.size() && _wcsnicmp(text.c_str(),root.c_str(),root.size()) == 0 &&
                        (text.size() == root.size() || text[root.size()] == L'\\')) { text = label+text.substr(root.size()); break; }
                }
                const auto parts = MiddleEllipsisPath(text,width,[&](const std::wstring& value){return CellTextWidth(value);});
                text = parts.first+parts.second; break;
            }
            case ColumnKind::Size: {
                const auto found = vm.folder_size_labels.find(source_index);
                text = e.is_dir && found != vm.folder_size_labels.end() ? found->second : e.size_text;
                const auto space = text.find_last_of(L' ');
                const float unit = kSizeUnitDip*scale_;
                if (space != std::wstring::npos && space > 0 && width > unit*1.8f && CellTextWidth(text.substr(0,space)) <= width-unit)
                    return hit_text(text.substr(0,space),left,width-unit,true) || hit_text(text.substr(space+1),left+width-unit+4.0f*scale_,unit-4.0f*scale_);
                if (IsSizePlaceholder(text) && width > unit*1.8f) width -= unit;
                trailing = true; break;
            }
            default:
                if (IsMediaColumn(kind)) { text = vm.media_cell ? vm.media_cell(source_index,MediaSortColumn(kind)) : std::wstring(); if (text.empty()) text = L"\u2014"; trailing = kind == ColumnKind::MediaDuration || kind == ColumnKind::MediaFps; }
                break;
            }
            if (hit_text(text,left,width,trailing)) return true;
        }
    }
    if (vm.view_mode == ViewMode::MediumIcons || vm.view_mode == ViewMode::LargeIcons ||
        vm.view_mode == ViewMode::ExtraLargeIcons) {
        // The remaining grid hit area is the rendered label, not cell padding.
        const int view_index = index;
        const auto& layout = geometry;
        if (layout.Wall()) return false;  // No label in the photo wall.
        const ListEntryView& entry = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
        const float pad = scale_;
        const D2D1_RECT_F name = layout.NameRect(view_index);
        const std::wstring shown = entry.alias.empty() ? ShownName(entry) : entry.alias;
        if (shown.empty()) return false;
        const std::vector<int>* tag_indices = vm.tag_catalog ? vm.tag_catalog->TagIndicesForPath(entry.path) : nullptr;
        const bool tagged = tag_indices ? !tag_indices->empty()
                                        : vm.tag_dots && vm.tag_dots->contains(source_index);
        if (tagged) return PointInItemName(vm, full_bounds, source_index, x, y);  // one line beside the dots
        const auto size_label = entry.is_dir ? vm.folder_size_labels.find(source_index) : vm.folder_size_labels.end();
        const bool size_line = size_label != vm.folder_size_labels.end();
        const int lines = layout.Metrics().name_lines;
        const float cell_w = std::max(1.0f, name.right - name.left);
        const D2D1_SIZE_F text = IconGridNameSize(compositor_, shown, cell_w, size_line ? std::max(1, lines - 1) : lines);
        float w = text.width;
        float h = text.height;
        if (size_line) {
            w = std::max(w, MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(),
                                              size_label->second));
            h += kIconGridNameLineDip * scale_;
        }
        const float mid = (name.left + name.right) * 0.5f;
        w = std::min(w, cell_w);
        return x >= mid - w * 0.5f - pad && x < mid + w * 0.5f + pad && y >= name.top - pad && y < name.top + h + pad;
    }
    return PointInItemName(vm, full_bounds, source_index, x, y);
}

// 滚动时固定上级: the pins of one details tree list laid out by `layout`.
static MainRenderer::StickyTree StickyFromLayout(const PaneViewModel& vm, const ViewLayout& layout,
                                                 const D2D1_RECT_F& list, bool enabled) {
    MainRenderer::StickyTree out;
    out.top = list.top;
    if (!enabled || !vm.tree || vm.view_mode != ViewMode::Details || vm.Groups() || layout.Grouped())
        return out;
    const auto [first, last] = layout.VisibleRange();
    if (first < 0 || last < first) return out;
    const D2D1_RECT_F first_cell = layout.ItemRect(first);
    out.row_h = first_cell.bottom - first_cell.top;
    if (out.row_h < 1.0f) return out;
    out.pins = PinnedTreeRows(*vm.tree, out.row_h, kTreeStickyMaxRows,
        [&](int row) { return layout.ItemRect(row).top - list.top; },
        [&](float y) {
            for (int i = first; i <= last; ++i)
                if (layout.ItemRect(i).bottom - list.top > y) return i;
            return last;
        });
    return out;
}

MainRenderer::StickyTree MainRenderer::StickyTreeRows(const PaneViewModel& vm, const D2D1_RECT_F& list) const {
    if (!tree_sticky_ || !vm.tree || vm.view_mode != ViewMode::Details || vm.EntryCount() == 0) {
        StickyTree none;
        none.top = list.top;
        return none;
    }
    const ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_,
                            ListRowHeightDip(vm, list), vm.Groups());
    return StickyFromLayout(vm, layout, list, true);
}

int MainRenderer::TreeChevronAt(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                float x, float y) const {
    if (!vm.tree || vm.view_mode != ViewMode::Details) return -1;
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    const D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    const ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_,
                            ListRowHeightDip(vm, list), vm.Groups());
    int row = layout.HitTest(x, y);
    const StickyTree sticky = StickyTreeRows(vm, list);
    for (const TreePin& pin : sticky.pins) {
        const float top = sticky.top + pin.y;
        if (y >= top && y < top + sticky.row_h) {
            row = pin.row;
            break;
        }
    }
    const DetailsTreeRow* tree_row = vm.tree->At(row);
    if (!tree_row || tree_row->kind != TreeRowKind::Entry || !tree_row->expandable) return -1;
    const float left = layout.IconRect(row).left + CheckGutterDip(vm) * scale_ +
                       static_cast<float>(tree_row->depth) * kTreeIndentDip * scale_;
    return x >= left - 3.0f * scale_ && x < left + kTreeChevronDip * scale_ ? row : -1;
}

int MainRenderer::TreeMoreActionAt(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                   int view_row, float x, float y) const {
    const DetailsTreeRow* tree_row = vm.tree ? vm.tree->At(view_row) : nullptr;
    if (!tree_row || tree_row->kind != TreeRowKind::More || !compositor_) return -1;
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    const D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    const ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_,
                            ListRowHeightDip(vm, list), vm.Groups());
    const D2D1_RECT_F cell = layout.ItemRect(view_row);
    const float x0 = layout.IconRect(view_row).left + (TreeOffsetDip(*tree_row) + CheckGutterDip(vm)) * scale_;
    const TreeMoreLayout more = LayoutTreeMore(*tree_row, cell, x0, scale_,
                                               compositor_->DwriteFactory(), compositor_->TextFormat());
    for (int k = 0; k < more.count; ++k) {
        const D2D1_RECT_F& b = more.buttons[k];
        if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) return more.actions[k];
    }
    return -1;
}
void MainRenderer::DrawFolderIcon(float x, float y, float size, const Theme& theme,
                                  float opacity) {
    (void)theme;
    if (compositor_ && compositor_->Dc())
        icon_cache_.Draw(compositor_->Dc(), D2D1::RectF(x, y, x + size, y + size),
                         L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, opacity);
}

void MainRenderer::DrawEmptyFolderIcon(float x, float y, float size, const Theme& theme,
                                       float opacity) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || size <= 0.0f || opacity <= 0.0f) return;
    // Brush and geometry belong to one device context; rebuild after a change.
    if (empty_folder_dc_ != dc) {
        for (auto& geometry : empty_folder_geometry_) geometry.reset();
        brEmptyFolder_.reset();
        empty_folder_dc_ = dc;
    }
    if (!empty_folder_geometry_[0].get()) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        if (!factory.get()) return;
        const auto arc = [](ID2D1GeometrySink* sink, D2D1_POINT_2F to, float r) {
            sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(r, r), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        };
        const auto build = [&](ComPtr<ID2D1PathGeometry>& out, auto&& shape) {
            ComPtr<ID2D1PathGeometry> geometry;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return false;
            shape(sink.get());
            if (FAILED(sink->Close())) return false;
            out = std::move(geometry);
            return true;
        };
        // Back and front leaves of the shell folder silhouette, in a 24-unit box
        // (16-32 px rows) and a 96-unit box with softer corners (icon grids).
        const bool built =
            build(empty_folder_geometry_[0], [&](ID2D1GeometrySink* k) {
                k->BeginFigure({2.0f, 6.6f}, D2D1_FIGURE_BEGIN_FILLED);
                arc(k, {4.6f, 4.0f}, 2.6f);
                k->AddLine({8.8f, 4.0f});
                k->AddBezier(D2D1::BezierSegment({9.5f, 4.0f}, {10.1f, 4.3f}, {10.6f, 4.8f}));
                k->AddLine({12.0f, 6.0f});
                k->AddLine({19.4f, 6.0f});
                arc(k, {22.0f, 8.6f}, 2.6f);
                k->AddLine({22.0f, 17.4f});
                arc(k, {19.4f, 20.0f}, 2.6f);
                k->AddLine({4.6f, 20.0f});
                arc(k, {2.0f, 17.4f}, 2.6f);
                k->EndFigure(D2D1_FIGURE_END_CLOSED);
            }) &&
            build(empty_folder_geometry_[1], [&](ID2D1GeometrySink* k) {
                k->BeginFigure({2.0f, 10.4f}, D2D1_FIGURE_BEGIN_FILLED);
                k->AddBezier(D2D1::BezierSegment({2.0f, 9.4f}, {2.8f, 8.6f}, {3.8f, 8.6f}));
                k->AddLine({20.2f, 8.6f});
                k->AddBezier(D2D1::BezierSegment({21.2f, 8.6f}, {22.0f, 9.4f}, {22.0f, 10.4f}));
                k->AddLine({22.0f, 17.4f});
                arc(k, {19.4f, 20.0f}, 2.6f);
                k->AddLine({4.6f, 20.0f});
                arc(k, {2.0f, 17.4f}, 2.6f);
                k->EndFigure(D2D1_FIGURE_END_CLOSED);
            }) &&
            build(empty_folder_geometry_[2], [&](ID2D1GeometrySink* k) {
                k->BeginFigure({8.0f, 24.0f}, D2D1_FIGURE_BEGIN_FILLED);
                arc(k, {16.0f, 16.0f}, 8.0f);
                k->AddLine({34.0f, 16.0f});
                k->AddBezier(D2D1::BezierSegment({36.6f, 16.0f}, {38.6f, 17.0f}, {40.4f, 18.8f}));
                k->AddLine({46.0f, 24.0f});
                k->AddLine({80.0f, 24.0f});
                arc(k, {88.0f, 32.0f}, 8.0f);
                k->AddLine({88.0f, 72.0f});
                arc(k, {80.0f, 80.0f}, 8.0f);
                k->AddLine({16.0f, 80.0f});
                arc(k, {8.0f, 72.0f}, 8.0f);
                k->EndFigure(D2D1_FIGURE_END_CLOSED);
            }) &&
            build(empty_folder_geometry_[3], [&](ID2D1GeometrySink* k) {
                k->BeginFigure({8.0f, 40.0f}, D2D1_FIGURE_BEGIN_FILLED);
                arc(k, {14.0f, 34.0f}, 6.0f);
                k->AddLine({82.0f, 34.0f});
                arc(k, {88.0f, 40.0f}, 6.0f);
                k->AddLine({88.0f, 72.0f});
                arc(k, {80.0f, 80.0f}, 8.0f);
                k->AddLine({16.0f, 80.0f});
                arc(k, {8.0f, 72.0f}, 8.0f);
                k->EndFigure(D2D1_FIGURE_END_CLOSED);
            });
        if (!built) {
            for (auto& geometry : empty_folder_geometry_) geometry.reset();
            return;
        }
    }
    const bool large = size >= 47.5f * scale_;
    const float unit = large ? 96.0f : 24.0f;
    ID2D1PathGeometry* back = empty_folder_geometry_[large ? 2 : 0].get();
    ID2D1PathGeometry* front = empty_folder_geometry_[large ? 3 : 1].get();
    // Theme has no dark flag: light text means a dark surface.
    const auto& ink = theme.text;
    const bool dark = 0.2126f * ink.r + 0.7152f * ink.g + 0.0722f * ink.b > 0.5f;
    const bool contrast = IsHighContrast();
    D2D1_MATRIX_3X2_F previous;
    dc->GetTransform(&previous);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(size / unit, size / unit) *
                     D2D1::Matrix3x2F::Translation(x, y) * previous);
    // One layer keeps the overlap of the two leaves from showing through when faded.
    const bool layered = opacity < 0.999f;
    if (layered)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(-2.0f, -2.0f, unit + 2.0f, unit + 2.0f), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), opacity, nullptr,
                                             D2D1_LAYER_OPTIONS1_NONE), nullptr);
    if (contrast) {
        // High contrast: system colors only, outline in the text color.
        const float stroke = std::max(1.0f, scale_) * unit / size;
        MakeBrush(dc, ink, brEmptyFolder_);
        dc->DrawGeometry(back, brEmptyFolder_.get(), stroke);
        MakeBrush(dc, theme.bg, brEmptyFolder_);
        dc->FillGeometry(front, brEmptyFolder_.get());
        MakeBrush(dc, ink, brEmptyFolder_);
        dc->DrawGeometry(front, brEmptyFolder_.get(), stroke);
    } else {
        MakeBrush(dc, D2D1::ColorF(dark ? 0x5F6E88 : 0x7F8DA6), brEmptyFolder_);
        dc->FillGeometry(back, brEmptyFolder_.get());
        MakeBrush(dc, D2D1::ColorF(dark ? 0x8090AB : 0xA3AFC3), brEmptyFolder_);
        dc->FillGeometry(front, brEmptyFolder_.get());
        if (large) {
            MakeBrush(dc, D2D1::ColorF(dark ? 0x9AA8C0 : 0xC3CCDA, 0.7f), brEmptyFolder_);
            dc->DrawLine({13.0f, 37.2f}, {83.0f, 37.2f}, brEmptyFolder_.get(), 1.6f);
        }
    }
    if (layered) dc->PopLayer();
    dc->SetTransform(previous);
}

PreviewDrawResult MainRenderer::DrawEntryThumbnail(ID2D1DeviceContext* dc,
    const D2D1_RECT_F& dest, const ListEntryView& entry, ViewMode mode,
    uint64_t generation, uint32_t pixels, float opacity, bool align_bottom,
    D2D1_RECT_F* artwork, uint32_t* duration) {
    if (entry.launcher_rgb || entry.device || entry.net_kind) return PreviewDrawResult::Failed;  // no file behind it: DrawEntryIcon
    if (entry.is_dir) {
        const bool grid = mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons ||
                          mode == ViewMode::ExtraLargeIcons;
        const bool drive_root = (entry.path.size() == 3 && entry.path[1] == L':') ||
            (entry.path.size() == 7 && entry.path.starts_with(L"\\\\?\\") && entry.path[5] == L':');
        if (!folder_thumbnails_enabled_ || !grid || entry.is_link || entry.record_only || entry.cloud_recall || drive_root || IsHighContrast())
            return PreviewDrawResult::Failed;
        // Folder composites need only their on-screen resolution. In particular
        // a 96 px large icon should not consume a 256 px CPU+GPU cache slot.
        const float edge = std::max(dest.right - dest.left, dest.bottom - dest.top);
        const uint32_t folder_pixels = edge <= 64.0f ? 64 : edge <= 128.0f ? 128 :
                                       edge <= 256.0f ? 256 : 512;
        return folder_thumbnail_cache_.Draw(dc, dest, entry.path, entry.attrs, folder_pixels,
            generation, entry.modified_value, entry.size_value, opacity, align_bottom, artwork,
            mode == ViewMode::MediumIcons);
    }
    // A phone's file is asked for by its shell id: the preview host takes
    // the device's own thumbnail (never the file).
    return thumbnail_cache_.DrawGridThumbnail(dc, dest, entry.thumb_path.empty() ? entry.path : entry.thumb_path,
        entry.attrs, pixels, generation, entry.modified_value, entry.size_value, opacity, align_bottom, artwork,
        duration);
}

void MainRenderer::PrefetchNextScreen(const PaneViewModel& vm, int first, int last, uint32_t pixels,
                                      int pane_index) {
    if (!pixels || first < 0 || last < first || pane_index < 0 ||
        pane_index >= static_cast<int>(prefetch_signature_.size())) return;
    const int count = static_cast<int>(vm.EntryCount());
    const int begin = last + 1;
    const int end = std::min(count - 1, last + (last - first + 1));
    if (begin > end) return;
    // Once per screen position: a frame that drew the same range asks nothing.
    const uint64_t signature = (vm.view_generation * 0x9E3779B97F4A7C15ull) ^
        (static_cast<uint64_t>(begin) << 40) ^ (static_cast<uint64_t>(end) << 16) ^ pixels;
    auto& done = prefetch_signature_[static_cast<size_t>(pane_index)];
    if (done == signature) return;
    for (int i = begin; i <= end; ++i) {
        const int src = vm.SourceIndex(i);
        if (src < 0) continue;
        if (vm.content_results && !vm.content_results->Ready(static_cast<size_t>(src))) continue;
        const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));
        // Cloud placeholders would download; folders have their own cache.
        if (e.is_dir || e.is_link || e.record_only || e.cloud_recall) continue;
        if (!thumbnail_cache_.Prefetch(e.path, e.attrs, pixels, vm.view_generation,
                                       e.modified_value, e.size_value)) return;  // busy: a later frame
    }
    done = signature;
}

void MainRenderer::DrawFileIcon(float x, float y, float size, const Theme& theme,
                                float opacity) {
    (void)theme;
    if (compositor_ && compositor_->Dc())
        icon_cache_.Draw(compositor_->Dc(), D2D1::RectF(x, y, x + size, y + size),
                         L"", L"", false, FILE_ATTRIBUTE_NORMAL, opacity);
}

namespace {
// Decode size for a view's thumbnails: big grids share one 256 px decode,
// medium icons 128 px, other views their drawn size. Icon grids round up to
// 128 / 256 / 512 px tiers, so zooming between sizes reuses decodes.
uint32_t ThumbnailRequestPixels(ViewMode mode, float drawn_size) noexcept {
    long pixels = std::lround(drawn_size);
    if (mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons)
        pixels = std::max(256l, pixels);
    else if (mode == ViewMode::MediumIcons)
        pixels = std::max(128l, pixels);
    if (IsZoomIconMode(mode)) pixels = pixels <= 128 ? 128l : pixels <= 256 ? 256l : 512l;
    return static_cast<uint32_t>(std::clamp(pixels, 32l, 512l));
}
} // namespace

void MainRenderer::DrawLinkOverlay(float x, float y, float size, const Theme& theme, float opacity,
    const std::wstring& label, float expansion, float right_limit, const D2D1_RECT_F* artwork) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || size <= 0.0f || opacity <= 0.0f) return;
    if (!link_arrow_geometry_.get()) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1PathGeometry> geometry;
        ComPtr<ID2D1GeometrySink> sink;
        if (!factory.get() || FAILED(factory->CreatePathGeometry(&geometry)) ||
            FAILED(geometry->Open(&sink))) return;
        // A folded shortcut arrow, with a curved tail rather than an external-link diagonal.
        sink->BeginFigure({3.2f, 12.4f}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier(D2D1::BezierSegment({3.0f, 8.1f}, {5.1f, 5.7f}, {9.0f, 5.7f}));
        sink->AddLine({9.0f, 3.2f});
        sink->AddLine({13.0f, 7.0f});
        sink->AddLine({9.0f, 10.8f});
        sink->AddLine({9.0f, 8.3f});
        sink->AddBezier(D2D1::BezierSegment({6.2f, 8.3f}, {4.5f, 9.7f}, {3.2f, 12.4f}));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        link_arrow_geometry_ = std::move(geometry);
    }
    // Row icons (16-24 dip) keep most of the app icon visible, like the shell's
    // half-size overlay; grid icons keep the larger pill. The inline pill passes
    // diameter / 0.30, which always lands in the grid branch.
    const bool row_icon = size < 32.0f * scale_;
    const float edge = row_icon
        ? std::min(size, std::round(std::clamp(size * 0.5f, 7.0f * scale_, 11.0f * scale_)))
        : std::min(size, std::clamp(size * 0.30f, 11.0f * scale_, 22.0f * scale_));
    const auto ink = artwork ? *artwork : D2D1::RectF(x, y, x + size, y + size);
    const float label_width = label.empty() ? 0.0f :
        MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->SmallFormat(), label) + 8.0f * scale_;
    const auto bounds = LinkPillRect(ink, edge, std::max(ink.left + edge, right_limit), label_width, expansion);
    const auto background = IsHighContrast() ? theme.bg : theme.accent;
    // Accent palettes can contain pale colors: preserve readable contrast.
    const float luminance = 0.2126f * background.r + 0.7152f * background.g + 0.0722f * background.b;
    const auto foreground = IsHighContrast() ? theme.text :
        D2D1::ColorF(luminance > 0.62f ? 0x15253B : 0xFFFFFF);
    const bool had_fill = brFillInput_.get() != nullptr;
    const auto previous_fill = had_fill ? brFillInput_->GetColor() : theme.fill_input;
    const auto plate = D2D1::RoundedRect(bounds, edge * 0.5f, edge * 0.5f);
    MakeBrush(dc, WithAlpha(theme.bg, opacity), brFillInput_);
    dc->DrawRoundedRectangle(plate, brFillInput_.get(), (row_icon ? 1.0f : 2.0f) * scale_);
    MakeBrush(dc, WithAlpha(background, opacity), brFillInput_);
    dc->FillRoundedRectangle(plate, brFillInput_.get());
    D2D1_MATRIX_3X2_F previous_transform;
    dc->GetTransform(&previous_transform);
    const float arrow_size = edge * 0.80f;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(arrow_size / 16.0f, arrow_size / 16.0f) *
        D2D1::Matrix3x2F::Translation(bounds.left + edge * 0.1f, bounds.top + edge * 0.1f) * previous_transform);
    MakeBrush(dc, WithAlpha(foreground, opacity), brFillInput_);
    dc->FillGeometry(link_arrow_geometry_.get(), brFillInput_.get());
    dc->SetTransform(previous_transform);
    const float available = bounds.right - bounds.left - edge - 4.0f * scale_;
    if (available > 4.0f * scale_ && !label.empty()) {
        dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        auto* format = compositor_->SmallFormat();
        const auto previous_alignment = format->GetParagraphAlignment();
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        MakeBrush(dc, WithAlpha(foreground, opacity * expansion), brFillInput_);
        const auto [head, leaf] = MiddleEllipsisPath(label, available, [&](const std::wstring& text) {
            return MeasureLayoutText(compositor_, compositor_->DwriteFactory(), format, text);
        });
        DrawTextEndEllipsis(compositor_, format, brFillInput_.get(), head + leaf,
            bounds.left + edge, bounds.top, available, edge);
        format->SetParagraphAlignment(previous_alignment);
        dc->PopAxisAlignedClip();
    }
    if (had_fill) brFillInput_->SetColor(previous_fill);
}
namespace {
bool HasExtension(const std::wstring& name, std::initializer_list<const wchar_t*> extensions) {
    const wchar_t* dot = PathFindExtensionW(name.c_str());
    if (!dot || !*dot) return false;
    for (const wchar_t* extension : extensions)
        if (_wcsicmp(dot, extension) == 0) return true;
    return false;
}

bool IsVideoName(const std::wstring& name) {
    return HasExtension(name, {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv",
        L".flv", L".mpg", L".mpeg", L".ts", L".mts", L".m2ts", L".3gp", L".3g2", L".asf", L".vob",
        L".ogv"});
}

// 0:07, 12:34, 1:02:03 (Explorer's style; no leading zero on the first field).
std::wstring FormatPlayingTime(uint32_t duration_ms) {
    const uint32_t total = (duration_ms + 500) / 1000;
    const uint32_t hours = total / 3600, minutes = total / 60 % 60, seconds = total % 60;
    wchar_t text[32]{};
    if (hours) swprintf_s(text, L"%u:%02u:%02u", hours, minutes, seconds);
    else swprintf_s(text, L"%u:%02u", minutes, seconds);
    return text;
}
} // namespace

void MainRenderer::DrawThumbnailBadges(const ListEntryView& entry, const D2D1_RECT_F& artwork,
                                       float icon_size, uint32_t duration_ms, const Theme& theme) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || scale_ <= 0.0f) return;
    const float art_w = artwork.right - artwork.left, art_h = artwork.bottom - artwork.top;
    const float icon_dip = icon_size / scale_;
    if (icon_dip < 40.0f || art_w < 20.0f * scale_ || art_h < 20.0f * scale_) return;
    // Programs themselves (and installers) have no "opened by" program.
    if (HasExtension(entry.name, {L".exe", L".msi", L".scr", L".com", L".appx", L".msix"})) return;
    // Sizes follow Explorer: extra large / large / medium icons.
    const bool large = icon_dip >= 160.0f, medium = !large && icon_dip >= 72.0f;
    const float badge = (large ? 40.0f : medium ? 24.0f : 16.0f) * scale_;
    const float inset = (large ? 8.0f : medium ? 4.0f : -3.0f) * scale_;
    const float ring = (large ? 2.0f : 1.5f) * scale_;

    const bool had_fill = brFillInput_.get() != nullptr;
    const auto previous_fill = had_fill ? brFillInput_->GetColor() : theme.fill_input;
    float badge_left = artwork.right;  // where the left chip has to stop

    const auto icon = open_with_icons_.BadgeFor(entry.name, static_cast<uint32_t>(std::lround(badge)));
    if (icon.bitmap) {
        const float right = std::round(artwork.right - inset), bottom = std::round(artwork.bottom - inset);
        const auto dest = D2D1::RectF(right - badge, bottom - badge, right, bottom);
        const auto plate_rect = D2D1::RectF(dest.left - ring, dest.top - ring, dest.right + ring, dest.bottom + ring);
        const float radius = (badge + ring * 2.0f) * 0.28f;
        // Soft shadow, then a plate in the window color so the icon reads on
        // any photo, then (pale Store logos) a dark inner plate.
        const float drop = std::max(1.0f, scale_);
        MakeBrush(dc, D2D1::ColorF(0, 0.22f), brFillInput_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(plate_rect.left, plate_rect.top + drop,
            plate_rect.right, plate_rect.bottom + drop), radius, radius), brFillInput_.get());
        MakeBrush(dc, IsHighContrast() ? theme.bg : WithAlpha(theme.bg, 0.96f), brFillInput_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(plate_rect, radius, radius), brFillInput_.get());
        if (icon.needs_plate) {
            const float inner = std::max(0.0f, radius - ring);
            MakeBrush(dc, D2D1::ColorF(0x2B2B2B), brFillInput_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(dest, inner, inner), brFillInput_.get());
        }
        dc->DrawBitmap(icon.bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        badge_left = plate_rect.left;
    } else if (icon.format_label) {
        auto* format = compositor_->SmallFormat();
        const float chip_h = std::round((large ? 20.0f : 16.0f) * scale_);
        const float pad = (large ? 6.0f : 4.0f) * scale_;
        const float label_w = format ? MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
            format, icon.format_label) : 0.0f;
        const float chip_w = label_w + pad * 2.0f;
        if (format && chip_w <= art_w - std::max(0.0f, inset) &&
            chip_h < art_h - std::max(0.0f, inset)) {
            const float right = std::round(artwork.right - std::max(0.0f, inset));
            const float bottom = std::round(artwork.bottom - std::max(0.0f, inset));
            const auto chip = D2D1::RectF(right - chip_w, bottom - chip_h, right, bottom);
            MakeBrush(dc, IsHighContrast() ? theme.bg : D2D1::ColorF(0, 0.6f), brFillInput_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(chip, 4.0f * scale_, 4.0f * scale_), brFillInput_.get());
            MakeBrush(dc, IsHighContrast() ? theme.text : D2D1::ColorF(0xFFFFFF), brFillInput_);
            const auto previous_alignment = format->GetParagraphAlignment();
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DrawTextEndEllipsis(compositor_, format, brFillInput_.get(), icon.format_label,
                chip.left + pad, chip.top, label_w + 2.0f * scale_, chip_h);
            format->SetParagraphAlignment(previous_alignment);
            badge_left = chip.left;
        }
    }

    // Small icons: the badge alone (a chip would cover the picture).
    const bool video = duration_ms > 0 || IsVideoName(entry.name);
    const bool gif = !video && HasExtension(entry.name, {L".gif"});
    if ((video || gif) && !(icon_dip < 72.0f)) {
        auto* format = compositor_->SmallFormat();
        const float chip_h = std::round((large ? 20.0f : 16.0f) * scale_);
        const float pad = (large ? 6.0f : 4.0f) * scale_;
        const float chip_inset = (large ? 8.0f : 4.0f) * scale_;
        const float glyph = chip_h * 0.42f;
        const float left = std::round(artwork.left + chip_inset);
        const float bottom = std::round(artwork.bottom - chip_inset);
        const float room = badge_left - 4.0f * scale_ - left;
        std::wstring label = gif ? L"GIF" : (large && duration_ms ? FormatPlayingTime(duration_ms) : std::wstring{});
        float label_w = label.empty() || !format ? 0.0f :
            MeasureLayoutText(compositor_, compositor_->DwriteFactory(), format, label);
        const float glyph_w = video ? glyph + (label.empty() ? 0.0f : 4.0f * scale_) : 0.0f;
        if (video && pad * 2.0f + glyph_w + label_w > room) { label.clear(); label_w = 0.0f; }
        const float chip_w = video && label.empty() ? chip_h : pad * 2.0f + glyph_w + label_w;
        if (chip_w <= room && chip_h < art_h * 0.5f) {
            const auto chip = D2D1::RectF(left, bottom - chip_h, left + chip_w, bottom);
            MakeBrush(dc, D2D1::ColorF(0, 0.6f), brFillInput_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(chip, 4.0f * scale_, 4.0f * scale_), brFillInput_.get());
            MakeBrush(dc, D2D1::ColorF(0xFFFFFF), brFillInput_);
            float text_x = chip.left + pad;
            if (video) {
                if (!play_triangle_geometry_.get()) {
                    ComPtr<ID2D1Factory> factory;
                    dc->GetFactory(&factory);
                    ComPtr<ID2D1PathGeometry> geometry;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (factory.get() && SUCCEEDED(factory->CreatePathGeometry(&geometry)) &&
                        SUCCEEDED(geometry->Open(&sink))) {
                        // Unit play triangle, optically centred in its box.
                        sink->BeginFigure({0.12f, 0.0f}, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine({1.0f, 0.5f});
                        sink->AddLine({0.12f, 1.0f});
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        if (SUCCEEDED(sink->Close())) play_triangle_geometry_ = std::move(geometry);
                    }
                }
                if (play_triangle_geometry_.get()) {
                    const float gx = label.empty() ? chip.left + (chip_w - glyph) * 0.5f + glyph * 0.06f : text_x;
                    const float gy = chip.top + (chip_h - glyph) * 0.5f;
                    D2D1_MATRIX_3X2_F previous_transform;
                    dc->GetTransform(&previous_transform);
                    dc->SetTransform(D2D1::Matrix3x2F::Scale(glyph, glyph) *
                        D2D1::Matrix3x2F::Translation(gx, gy) * previous_transform);
                    dc->FillGeometry(play_triangle_geometry_.get(), brFillInput_.get());
                    dc->SetTransform(previous_transform);
                }
                text_x += glyph_w;
            }
            if (!label.empty() && format) {
                const auto previous_alignment = format->GetParagraphAlignment();
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DrawTextEndEllipsis(compositor_, format, brFillInput_.get(), label,
                    text_x, chip.top, label_w + 2.0f * scale_, chip_h);
                format->SetParagraphAlignment(previous_alignment);
            }
        }
    }
    if (had_fill) brFillInput_->SetColor(previous_fill);
}

void MainRenderer::DrawCloudBadge(const ListEntryView& entry, const D2D1_RECT_F& artwork, bool picture,
                                  const Theme& theme, float opacity) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    const float art = std::min(artwork.right - artwork.left, artwork.bottom - artwork.top);
    if (!dc || art < 24.0f * scale_ || opacity <= 0.0f) return;
    const CloudLook look = CloudLookFor(entry, theme);
    const CloudSwapPose pose = CloudSwapFor(this, entry.path, 0, look.glyph[0], motion_now_);
    if (pose.running) RequestMotionFrame();
    opacity *= pose.alpha;
    const float settled = std::round(std::clamp(art * 0.34f, 16.0f * scale_, 22.0f * scale_));
    // Shell icons: hang off the bottom-left like the prototype's badge.
    // Pictures fill their slot, so the badge sits inside the top-left corner,
    // clear of the open-with and duration badges along the bottom.
    const float inset = 6.0f * scale_;
    const float settledLeft = picture ? artwork.left + inset : artwork.left - settled * 0.3f;
    const float settledTop = picture ? artwork.top + inset : artwork.bottom - settled * 0.75f;
    // Scaled about its own centre while it pops.
    const float d = settled * pose.scale;
    const float left = settledLeft + (settled - d) * 0.5f;
    const float top = settledTop + (settled - d) * 0.5f;
    const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(left + d * 0.5f, top + d * 0.5f), d * 0.5f, d * 0.5f);
    D2D1_COLOR_F plate = theme.surface_flyout;
    plate.a = opacity;
    if (picture) {
        MakeBrush(dc, D2D1::ColorF(0, 0.22f * opacity), brFillInput_);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(circle.point.x, circle.point.y + std::max(1.0f, scale_)),
                                      circle.radiusX, circle.radiusY), brFillInput_.get());
    }
    MakeBrush(dc, plate, brFillInput_);
    dc->FillEllipse(circle, brFillInput_.get());
    MakeBrush(dc, WithAlpha(theme.text, 0.16f * opacity), brFillInput_);
    dc->DrawEllipse(circle, brFillInput_.get(), std::max(1.0f, scale_));
    const D2D1_COLOR_F ink = IsHighContrast() ? theme.text : look.ink;
    DrawIconText(left, top, d, d, look.glyph, L"", WithAlpha(ink, opacity), 0.62f);
}

void MainRenderer::DrawEntryIcon(const ListEntryView& entry, float x, float y, float size,
                                 const Theme& theme, float opacity) {
    const auto dest = D2D1::RectF(x, y, x + size, y + size);
    if (entry.record_only && compositor_ && compositor_->Dc()) {
        auto* dc = compositor_->Dc();
        MakeBrush(dc, WithAlpha(theme.text_secondary, 0.65f * opacity), brTextSecondary_);
        const float pad = size * 0.2f;
        const auto shape = D2D1::RoundedRect(D2D1::RectF(x + pad, y + pad,
            x + size - pad, y + size - pad), size * 0.04f, size * 0.04f);
        dc->DrawRoundedRectangle(shape, brTextSecondary_.get(), std::max(1.0f, size * 0.045f));
        dc->DrawLine({x + size * 0.36f, y + size * 0.5f},
            {x + size * 0.64f, y + size * 0.5f}, brTextSecondary_.get(), std::max(1.0f, size * 0.045f));
        return;
    }
    if (entry.device && compositor_ && compositor_->Dc()) {
        auto* dc = compositor_->Dc();
        // Phones: the phone glyph on a soft plate; an amber lock while the
        // phone waits for Allow / Trust.
        const bool locked = (entry.device & fs::kPcDeviceLocked) != 0;
        const bool none = (entry.device & fs::kPcDevicePlaceholder) != 0;  // "no phone connected": faint plug
        const D2D1_COLOR_F tint = none ? WithAlpha(theme.text_secondary, 0.75f)
            : locked ? HexColor(theme.bg.r < 0.5f ? 0xE3A846u : 0xB7791Fu) : theme.accent;
        const float pad = size * 0.08f;
        MakeBrush(dc, WithAlpha(tint, 0.13f * opacity), brFillInput_);
        FillRoundedRect(dc, brFillInput_.get(), x + pad, y + pad, size - 2.0f * pad, size - 2.0f * pad, size * 0.22f);
        const float quarter = std::round(size * 0.5f / (16.0f * scale_) * 4.0f) / 4.0f;
        DrawIconText(x, y, size, size, none ? L"\xE88E" : locked ? L"\xE72E" : L"\xE8EA", L"", WithAlpha(tint, opacity),
                     std::max(0.5f, quarter));
        return;
    }
    if (entry.net_kind && compositor_ && compositor_->Dc()) {
        auto* dc = compositor_->Dc();
        // 局域网设备: PC, NAS or Mac on the
        // same soft plate as phones; NAS in teal so storage stands apart.
        const bool dark = theme.bg.r < 0.5f;
        const D2D1_COLOR_F tint = entry.net_kind == 2 ? HexColor(dark ? 0x4CC2A4u : 0x1F8A70u)
            : entry.net_kind == 3 ? WithAlpha(theme.text, 0.82f) : theme.accent;
        const float pad = size * 0.08f;
        MakeBrush(dc, WithAlpha(tint, 0.13f * opacity), brFillInput_);
        FillRoundedRect(dc, brFillInput_.get(), x + pad, y + pad, size - 2.0f * pad, size - 2.0f * pad, size * 0.22f);
        const float quarter = std::round(size * 0.5f / (16.0f * scale_) * 4.0f) / 4.0f;
        DrawIconText(x, y, size, size, entry.net_kind == 2 ? L"\xEDA2" : entry.net_kind == 3 ? L"\xE7F8" : L"\xE977", L"",
                     WithAlpha(tint, opacity), std::max(0.5f, quarter));
        return;
    }
    if (entry.launcher_rgb && compositor_ && compositor_->Dc()) {
        auto* dc = compositor_->Dc();
        // 网盘程序入口: the icon the program registered (as Explorer shows it).
        if (ID2D1Bitmap* own = icon_cache_.ExactBitmapFor(entry.path, size)) {
            dc->DrawBitmap(own, &dest, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                           nullptr, nullptr);
            return;
        }
        // Not registered (or still resolving): the brand-tinted cloud on a
        // soft plate, plus the "opens another program" mark once there is room.
        const D2D1_COLOR_F tint = HexColor(entry.launcher_rgb);
        const float pad = size * 0.08f;
        MakeBrush(dc, WithAlpha(tint, 0.13f * opacity), brFillInput_);
        FillRoundedRect(dc, brFillInput_.get(), x + pad, y + pad, size - 2.0f * pad, size - 2.0f * pad, size * 0.22f);
        const float quarter = std::round(size * 0.5f / (16.0f * scale_) * 4.0f) / 4.0f;
        DrawIconText(x, y, size, size, L"\xE753", L"", WithAlpha(tint, opacity), std::max(0.5f, quarter));
        if (size >= 40.0f * scale_) {
            const float mark = std::round(size * 0.3f);
            const float mf = std::max(0.5f, std::round(mark * 0.55f / (16.0f * scale_) * 4.0f) / 4.0f);
            DrawIconText(x + size - mark, y + size - mark, mark, mark, L"\xE8A7", L"",
                         WithAlpha(theme.text_secondary, opacity), mf);
        }
        return;
    }
    if (compositor_ && compositor_->Dc() &&
        icon_cache_.Draw(compositor_->Dc(), dest, entry.path, entry.name, entry.is_dir,
                         entry.attrs, opacity)) {
        return;
    }
    if (entry.is_dir) DrawFolderIcon(x, y, size, theme, opacity);
    else DrawFileIcon(x, y, size, theme, opacity);
}

void MainRenderer::DrawMorphIcon(const ListEntryView& entry, const PaneViewModel& vm,
                                 const motion::ViewMorphMotion::Sample& sample, float dx, float dy,
                                 const D2D1_RECT_F& target_icon, ViewMode from_mode,
                                 const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return;
    const auto side = [](const D2D1_RECT_F& r) {
        return std::max(1.0f, std::min(r.right - r.left, r.bottom - r.top));
    };
    // Sub-pixel rect in the row's translated space: rounding would make the
    // icon wobble by a pixel every frame while it scales.
    const D2D1_RECT_F icon = D2D1::RectF(sample.icon.left - dx, sample.icon.top - dy,
                                         sample.icon.right - dx, sample.icon.bottom - dy);
    const float size = side(icon);
    const float target_size = side(target_icon);
    const float from_size = side(sample.from_icon);

    // Thumbnail <-> shell icon: grid thumbnails stay until the item is almost
    // row-sized, row icons give way to thumbnails early when growing.
    const bool thumb_from = !entry.record_only && !sample.entering && UsesThumbnails(from_mode);
    const bool thumb_to = !entry.record_only && UsesThumbnails(vm.view_mode);
    const float hidden_alpha = HiddenEntryAlpha(entry.attrs);
    float thumb_weight = 0.0f;
    if (thumb_from && thumb_to) thumb_weight = 1.0f;
    else if (thumb_from) thumb_weight = 1.0f - motion::SmoothStep(0.55f, 0.95f, sample.progress);
    else if (thumb_to) thumb_weight = motion::SmoothStep(0.05f, 0.45f, sample.progress);
    // Keep the crossfade weight independent of the item's overall opacity.
    const float thumb_alpha = thumb_weight * sample.opacity * hidden_alpha;
    bool thumb = false;
    D2D1_RECT_F thumbnail_artwork = icon;
    if (thumb_alpha > 0.01f) {
        // Shrinking files reuse only artwork already uploaded by the old view.
        // Pending decodes must not compete with this animation or stay wanted.
        const bool use_from = thumb_from && !thumb_to;
        const uint32_t pixels = ThumbnailRequestPixels(use_from ? from_mode : vm.view_mode,
                                                       use_from ? from_size : target_size);
        thumb = (use_from && !entry.is_dir
            ? thumbnail_cache_.DrawCachedGridThumbnail(dc, icon,
                entry.thumb_path.empty() ? entry.path : entry.thumb_path, entry.attrs, pixels,
                entry.modified_value, entry.size_value, thumb_alpha, true, &thumbnail_artwork)
            : DrawEntryThumbnail(dc, icon, entry, use_from ? from_mode : vm.view_mode,
                vm.view_generation, pixels, thumb_alpha, true, &thumbnail_artwork)) == PreviewDrawResult::Bitmap;
    }
    // The settled view's icon is converted on the icon worker meanwhile, so
    // the first static frame after the glide only finds it cached.
    if (!entry.record_only)
        icon_cache_.Prefetch(entry.path, entry.name, entry.is_dir, entry.attrs, target_size);
    const float icon_alpha = (thumb ? 1.0f - thumb_weight : 1.0f) * sample.opacity * hidden_alpha;
    if (icon_alpha <= 0.01f) {
        if (!entry.record_only && entry.is_link)
            DrawLinkOverlay(icon.left, icon.top, size, theme, sample.opacity * hidden_alpha,
                            {}, 0.0f, 0.0f, &thumbnail_artwork);
        return;
    }
    if (entry.record_only || entry.launcher_rgb || entry.device || entry.net_kind) {
        DrawEntryIcon(entry, icon.left, icon.top, size, theme, hidden_alpha);
        return;
    }
    // One image-list size for the whole morph: a size that crossed SHIL
    // buckets frame by frame would convert every visible icon on the UI
    // thread several times; the GPU scales the bitmap instead. Normally the
    // larger end. Shrinking out of a thumbnail view the icon only shows near
    // row size (the thumbnail covers the rest), so the row size is used -
    // asking every item for a large icon at the hand-over converted dozens of
    // per-file icons in one frame (a 56 ms stall mid-glide).
    const float locked = thumb_from && !thumb_to ? target_size : std::max(from_size, target_size);
    // Never convert on the UI thread just to draw a frame: use whatever size
    // is already converted (the GPU scales it); the worker converts the rest.
    ID2D1Bitmap* bitmap = icon_cache_.CachedBitmapFor(entry.path, entry.name, entry.is_dir,
                                                      entry.attrs, locked);
    if (!bitmap) icon_cache_.Prefetch(entry.path, entry.name, entry.is_dir, entry.attrs, locked);
    const auto is_grid = [](ViewMode mode) {
        return mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons || mode == ViewMode::ExtraLargeIcons;
    };
    const auto ink = icon_cache_.CachedArtworkBounds(entry.path, entry.name, entry.is_dir, entry.attrs, locked);
    const float alignment = (is_grid(from_mode) ? 1.0f - sample.progress : 0.0f) +
                            (is_grid(vm.view_mode) ? sample.progress : 0.0f);
    // An icon with only a small image: at most twice its size, centered
    // across, resting on the baseline in grids and centered in other views.
    const float icon_side = bitmap ? IconDrawSide(size,
        icon_cache_.CachedNativeSide(entry.path, entry.name, entry.is_dir, entry.attrs, locked)) : size;
    const float icon_inset = std::round((size - icon_side) * 0.5f);
    const float aligned_y = icon.top + (size - icon_side * ink.bottom) * alignment +
                            (size - icon_side) * 0.5f * (1.0f - alignment);
    if (bitmap) {
        const D2D1_RECT_F dest = D2D1::RectF(icon.left + icon_inset, aligned_y,
                                             icon.left + icon_inset + icon_side, aligned_y + icon_side);
        dc->DrawBitmap(bitmap, &dest, icon_alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                       nullptr, nullptr);
    } else if (sample.entering) {
        // Vector placeholders cannot fade; a fading-in item waits for its
        // icon (or thumbnail) instead of popping in at full strength.
    } else if (entry.is_dir) {
        DrawFolderIcon(icon.left, aligned_y, size, theme, hidden_alpha);
    } else {
        DrawFileIcon(icon.left, aligned_y, size, theme, hidden_alpha);
    }
    if (entry.is_link) {
        const auto artwork = D2D1::RectF(icon.left + icon_inset + ink.left * icon_side, aligned_y + ink.top * icon_side,
            icon.left + icon_inset + ink.right * icon_side, aligned_y + ink.bottom * icon_side);
        DrawLinkOverlay(icon.left, icon.top, size, theme, sample.opacity * hidden_alpha,
                        {}, 0.0f, 0.0f, &artwork);
    }
}

void MainRenderer::DrawMorphFrom(const PaneViewModel& vm, const motion::ViewMorphMotion& morph,
                                 const D2D1_RECT_F& viewport, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    const float alpha = morph.FromOpacity();
    const auto& from = morph.FromLayout();
    if (!dc || alpha <= 0.01f || !from.valid || from.count != vm.EntryCount()) return;
    const ViewMode mode = static_cast<ViewMode>(morph.FromMode());
    const bool grid = mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons ||
                      mode == ViewMode::MediumIcons;
    // Same inputs as the previous frame's DrawList: identical rects.
    const ViewLayout layout(mode, from.viewport, from.count, from.scroll_x, from.scroll_y,
                            scale_, ViewLayoutParams(from.row_height_dip, from.icon_dip));
    const auto [first, last] = layout.VisibleRange();
    static const std::vector<NameMatchRange> kNoMatches;
    // Details: the metadata columns fade out too, instead of vanishing.
    const bool details = mode == ViewMode::Details;
    const DetailsColumnLayout columns = details ? DetailsColumns(from.viewport, vm)
                                                : DetailsColumnLayout{};
    auto* meta_format = compositor_->FileNameFormat();
    const auto meta_align = meta_format->GetTextAlignment();
    const auto meta_paragraph = meta_format->GetParagraphAlignment();
    if (details) {
        meta_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->PushLayer(D2D1::LayerParameters1(viewport, nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
                      D2D1::IdentityMatrix(), alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE), nullptr);
    for (int i = first; i >= 0 && i <= last; ++i) {
        const int src = vm.SourceIndex(i);
        if (src < 0) continue;
        if (vm.content_results && !vm.content_results->Ready(static_cast<size_t>(src))) continue;
        const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));
        const uint64_t key = std::hash<std::wstring_view>{}(std::wstring_view(e.name));
        motion::ViewMorphMotion::Rects was{}, now{};
        if (!morph.FromRects(key, was)) continue;  // was not on screen
        const bool staying = morph.CurrentRects(key, now);
        // Labels ride along with their item's icon while they fade.
        const float dx = staying ? std::round(now.icon.left - was.icon.left) : 0.0f;
        const float dy = staying ? std::round(now.icon.top - was.icon.top) : 0.0f;
        const D2D1_RECT_F name = layout.NameRect(i);
        const D2D1_RECT_F moved = D2D1::RectF(name.left + dx, name.top + dy,
                                              name.right + dx, name.bottom + dy);
        if (grid) {
            DrawCenteredIconName(ShownName(e), moved, theme.text, theme, kNoMatches, nullptr,
                                 layout.Metrics().name_lines);
        } else {
            DrawTruncatedName(ShownName(e), moved.left, moved.top, std::max(1.0f, moved.right - moved.left),
                              std::max(1.0f, moved.bottom - moved.top), theme, false, kNoMatches);
        }
        if (details) {
            const D2D1_RECT_F row = layout.ItemRect(i);
            const float inset = 8.0f * scale_;
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            for (int col = 1; col < columns.count; ++col) {
                const float left = columns.DividerX(col - 1) + inset + dx;
                const float avail = columns.widths[static_cast<size_t>(col)] - inset * 2.0f;
                if (avail <= 1.0f) continue;
                const float top = row.top + scale_ + dy;
                const float height = std::max(1.0f, row.bottom - row.top - 2.0f * scale_);
                // Same text and placement as DrawList's details columns, so
                // nothing jumps on the first frame of the fade.
                const auto draw = [&](const std::wstring& text, float x, float width,
                                      DWRITE_TEXT_ALIGNMENT alignment) {
                    if (text.empty() || width <= 1.0f) return;
                    meta_format->SetTextAlignment(alignment);
                    DrawTextEndEllipsis(compositor_, meta_format,
                                        brTextSecondary_.get(), text, x, top, width, height);
                };
                switch (columns.kinds[static_cast<size_t>(col)]) {
                case ColumnKind::Date: {
                    const float lead = mtime_hint_.style == 1 ? kMtimeDotLeadDip * scale_ : 0.0f;
                    draw(list_smart_date_ && e.modified_value && !e.date_text.empty()
                             ? SmartListDate(e.modified_value) : e.date_text,
                         left + lead, avail - lead, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                }
                case ColumnKind::Created:
                    draw(list_smart_date_ && e.created_value && !e.created_text.empty()
                             ? SmartListDate(e.created_value) : e.created_text,
                         left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                case ColumnKind::Accessed:
                    draw(list_smart_date_ && e.accessed_value && !e.accessed_text.empty()
                             ? SmartListDate(e.accessed_value) : e.accessed_text,
                         left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                case ColumnKind::Type: {
                    const std::wstring chip = vm.is_changes ? std::wstring() : TypeChipLabel(e.name, e.is_dir);
                    const float chipW = TypeChipWidthDip(chip) * scale_;
                    const float shift = !chip.empty() && chipW < avail * 0.6f && !IsHighContrast() ? chipW : 0.0f;
                    draw(e.type_text, left + shift, avail - shift, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                }
                case ColumnKind::Size: {
                    const size_t space = e.size_text.find_last_of(L' ');
                    const float unitW = kSizeUnitDip * scale_;
                    if (space != std::wstring::npos && space > 0 && avail > unitW * 1.8f) {
                        draw(e.size_text.substr(0, space), left, avail - unitW, DWRITE_TEXT_ALIGNMENT_TRAILING);
                        draw(e.size_text.substr(space + 1), left + avail - unitW + 4.0f * scale_,
                             unitW - 4.0f * scale_, DWRITE_TEXT_ALIGNMENT_LEADING);
                    } else {
                        const float value_width = IsSizePlaceholder(e.size_text) && avail > unitW * 1.8f
                            ? avail - unitW : avail;
                        draw(e.size_text, left, value_width, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                    break;
                }
                default: {
                    const ColumnKind media_kind = columns.kinds[static_cast<size_t>(col)];
                    if (!IsMediaColumn(media_kind) || !vm.media_cell) break;
                    const std::wstring media_text = vm.media_cell(src, MediaSortColumn(media_kind));
                    draw(media_text.empty() ? std::wstring(L"\u2014") : media_text, left, avail,
                         media_kind == ColumnKind::MediaDuration || media_kind == ColumnKind::MediaFps
                             ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                }
                }
            }
        }
        if (staying) continue;
        // No place in the new view: the icon fades out where it was.
        const float size = std::max(1.0f, std::min(was.icon.right - was.icon.left,
                                                   was.icon.bottom - was.icon.top));
        const D2D1_RECT_F dest = D2D1::RectF(was.icon.left, was.icon.top,
                                             was.icon.left + size, was.icon.top + size);
        const float hidden_alpha = HiddenEntryAlpha(e.attrs);
        const bool thumb = !e.record_only && UsesThumbnails(mode) &&
            (e.is_dir
                ? DrawEntryThumbnail(dc, was.icon, e, mode, vm.view_generation,
                    ThumbnailRequestPixels(mode, size), hidden_alpha, grid, nullptr)
                : thumbnail_cache_.DrawCachedGridThumbnail(dc, was.icon,
                    e.thumb_path.empty() ? e.path : e.thumb_path, e.attrs, ThumbnailRequestPixels(mode, size),
                    e.modified_value, e.size_value, hidden_alpha, grid, nullptr)) == PreviewDrawResult::Bitmap;
        if (thumb) continue;
        if (e.launcher_rgb || e.device || e.net_kind) {  // 网盘程序入口, phones and LAN devices draw their own icon
            DrawEntryIcon(e, dest.left, dest.top, size, theme, hidden_alpha);
            continue;
        }
        if (ID2D1Bitmap* bitmap = e.record_only ? nullptr
                : icon_cache_.CachedBitmapFor(e.path, e.name, e.is_dir, e.attrs, size)) {
            dc->DrawBitmap(bitmap, &dest, hidden_alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                           nullptr, nullptr);
        } else if (e.is_dir) {
            // Vector placeholder: DrawEntryIcon would convert synchronously.
            DrawFolderIcon(dest.left, dest.top, size, theme, hidden_alpha);
        } else {
            DrawFileIcon(dest.left, dest.top, size, theme, hidden_alpha);
        }
    }
    dc->PopLayer();
    dc->PopAxisAlignedClip();
    meta_format->SetTextAlignment(meta_align);
    meta_format->SetParagraphAlignment(meta_paragraph);
}

void MainRenderer::DrawSpringRing(ID2D1DeviceContext* dc, const D2D1_RECT_F& icon, float progress,
                                  const Theme& theme) {
    const float cx = (icon.left + icon.right) * 0.5f;
    const float cy = (icon.top + icon.bottom) * 0.5f;
    const float r = (std::max)(icon.right - icon.left, icon.bottom - icon.top) * 0.5f + 3.0f * scale_;
    const float stroke = 2.0f * scale_;
    MakeBrush(dc, WithAlpha(theme.accent, 0.18f), brFillInput_);
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brFillInput_.get(), stroke);
    const float p = (std::clamp)(progress, 0.0f, 1.0f);
    if (p <= 0.0f) return;
    MakeBrush(dc, theme.accent, brFillInput_);
    if (p >= 0.999f) {
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brFillInput_.get(), stroke);
        return;
    }
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (!factory) return;
    ComPtr<ID2D1PathGeometry> arc;
    if (SUCCEEDED(factory->CreatePathGeometry(&arc)) && arc.get()) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(arc->Open(&sink)) && sink.get()) {
            const float a = p * 6.2831853f;
            sink->BeginFigure(D2D1::Point2F(cx, cy - r), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx + r * std::sin(a), cy - r * std::cos(a)),
                                          D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                          p > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            if (!roundCapStroke_.get()) {
                D2D1_STROKE_STYLE_PROPERTIES props{};
                props.startCap = D2D1_CAP_STYLE_ROUND;
                props.endCap = D2D1_CAP_STYLE_ROUND;
                factory->CreateStrokeStyle(props, nullptr, 0, &roundCapStroke_);
            }
            dc->DrawGeometry(arc.get(), brFillInput_.get(), stroke, roundCapStroke_.get());
        }
    }
    factory->Release();
}

// Motion 4b: hover grows an accent knob out of
// the divider (spring Pop), dragging stretches it and shows the split as a
// badge, a magnet snap flashes a guide along the divider.
void MainRenderer::DrawSplitterKnob(int index, const SplitterView& sp, bool hovered, bool pressed,
                                    bool dark, const Theme& theme) {
    if (index < 0 || index >= static_cast<int>(splitter_knobs_.size()) || !compositor_ ||
        !compositor_->Dc() || IsHighContrast())
        return;
    auto* dc = compositor_->Dc();
    SplitterKnobMotion& m = splitter_knobs_[static_cast<size_t>(index)];
    const uint64_t now = motion_now_;
    auto value = [&](float from, float to, uint64_t start, motion::SpringSpec spec) {
        return from + (to - from) * motion::SpringProgress(spec, static_cast<float>(now - start));
    };
    const auto showSpec = [&] { return m.show_to > 0.5f ? motion::spring::Pop : motion::spring::Snappy; };
    const float showTarget = hovered || pressed ? 1.0f : 0.0f;
    if (showTarget != m.show_to) {
        const bool animate = motion::SystemAnimationsEnabled();
        m.show_from = animate ? value(m.show_from, m.show_to, m.show_start, showSpec()) : showTarget;
        m.show_to = showTarget;
        m.show_start = now;
    }
    const float grabTarget = pressed ? 1.0f : 0.0f;
    if (grabTarget != m.grab_to) {
        m.grab_from = value(m.grab_from, m.grab_to, m.grab_start, motion::spring::Pop);
        m.grab_to = grabTarget;
        m.grab_start = now;
    }
    if (sp.snapped && !m.snapped) m.snap_at = now;
    m.snapped = sp.snapped;
    const uint32_t settle = motion::SpringSettleMs(motion::spring::Pop);
    const bool moving = now - m.show_start < settle || now - m.grab_start < settle ||
                        (m.snap_at && now - m.snap_at < 380);
    if (moving) splitter_motion_until_ = now + 60;
    const float show = value(m.show_from, m.show_to, m.show_start, showSpec());
    const float grab = value(m.grab_from, m.grab_to, m.grab_start, motion::spring::Pop);
    const float cx = (sp.hit_rect.left + sp.hit_rect.right) * 0.5f;
    const float cy = (sp.hit_rect.top + sp.hit_rect.bottom) * 0.5f;
    // Snap guide: the whole divider lights up and fades (380 ms).
    if (m.snap_at && now - m.snap_at < 380) {
        const float a = 0.9f * (1.0f - static_cast<float>(now - m.snap_at) / 380.0f);
        MakeBrush(dc, WithAlpha(theme.accent, a), brAccent_);
        const float half = 1.0f * scale_;
        const D2D1_RECT_F line = sp.vertical
            ? D2D1::RectF(cx - half, sp.hit_rect.top, cx + half, sp.hit_rect.bottom)
            : D2D1::RectF(sp.hit_rect.left, cy - half, sp.hit_rect.right, cy + half);
        dc->FillRectangle(line, brAccent_.get());
    }
    if (show <= 0.01f) return;
    // Knob: 6 x 36 DIP along the divider, out of the swap button's way.
    float kx = cx, ky = cy;
    const float alongLen = 36.0f * scale_ * (0.6f + 0.4f * show) * (1.0f + 0.25f * grab);
    const float acrossLen = 6.0f * scale_ * (0.4f + 0.6f * show);
    if (sp.swap_rect.right > sp.swap_rect.left) {
        const bool overlaps = sp.vertical
            ? (ky + alongLen * 0.5f > sp.swap_rect.top && ky - alongLen * 0.5f < sp.swap_rect.bottom)
            : (kx + alongLen * 0.5f > sp.swap_rect.left && kx - alongLen * 0.5f < sp.swap_rect.right);
        if (overlaps) {
            if (sp.vertical) ky = sp.swap_rect.bottom + 12.0f * scale_ + alongLen * 0.5f;
            else kx = sp.swap_rect.right + 12.0f * scale_ + alongLen * 0.5f;
        }
    }
    const D2D1_RECT_F knob = sp.vertical
        ? D2D1::RectF(kx - acrossLen * 0.5f, ky - alongLen * 0.5f, kx + acrossLen * 0.5f, ky + alongLen * 0.5f)
        : D2D1::RectF(kx - alongLen * 0.5f, ky - acrossLen * 0.5f, kx + alongLen * 0.5f, ky + acrossLen * 0.5f);
    MakeBrush(dc, WithAlpha(theme.accent, (std::min)(1.0f, show)), brAccent_);
    const float kr = 4.0f * scale_;
    dc->FillRoundedRectangle(D2D1::RoundedRect(knob, kr, kr), brAccent_.get());
    // Drag badge: "58% · 42%" next to the knob; accent while snapped.
    if (grab <= 0.05f) return;
    const int first = static_cast<int>(std::lround(sp.ratio * 100.0f));
    const std::wstring text = std::to_wstring(first) + L"% \u00B7 " + std::to_wstring(100 - first) + L"%";
    IDWriteTextFormat* fmt = compositor_->SmallFormat();
    const float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt, text);
    const float bw = tw + 14.0f * scale_, bh = 22.0f * scale_;
    const float bx = sp.vertical ? kx - bw * 0.5f : knob.right + 10.0f * scale_;
    const float by = sp.vertical ? knob.bottom + 10.0f * scale_ : ky - bh * 0.5f;
    const float alpha = (std::min)(1.0f, grab);
    const D2D1_COLOR_F badgeBg = sp.snapped ? theme.accent
                                            : (dark ? D2D1::ColorF(0x000000) : D2D1::ColorF(0x111827));
    MakeBrush(dc, WithAlpha(badgeBg, alpha), brFillPressed_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), 6.0f * scale_, 6.0f * scale_),
                             brFillPressed_.get());
    MakeBrush(dc, WithAlpha(sp.snapped ? theme.accent_text : D2D1::ColorF(0xFFFFFF), alpha), brText_);
    DrawTextRect(dc, fmt, brText_.get(), text, bx + 7.0f * scale_, by, tw + 1.0f * scale_, bh);
}

void MainRenderer::DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    if (!vm.pane_slots.empty()) {
        for (int i = 0; i < static_cast<int>(vm.pane_slots.size()); ++i) {
            const auto& slot = vm.pane_slots[static_cast<size_t>(i)];
            const auto& zoomMotion = pane_zoom_motion_;
            if (!(zoomMotion.start && zoomMotion.restoring && i != zoomMotion.excluded))
                DrawSinglePane(vm, slot.pane, slot.rect, i, slot.focused, slot.target, theme);
        }
        for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
            const auto& sp = vm.splitters[static_cast<size_t>(i)];
            fluent::SplitterSpec spec;
            spec.bounds = sp.hit_rect;
            spec.vertical = sp.vertical;
            const bool on_swap = vm.hover_region == static_cast<int>(HitTestResult::Splitter) &&
                                 vm.hover_control_index == kSplitterSwapIndex + i;
            spec.state.hovered = (vm.hover_region == static_cast<int>(HitTestResult::Splitter) &&
                                  vm.hover_control_index == i) || on_swap;
            spec.state.pressed = vm.splitter_pressed && spec.state.hovered;
            painter_.DrawSplitter(spec);
            DrawSplitterKnob(i, sp, spec.state.hovered && !on_swap, spec.state.pressed, vm.dark, theme);
            if (spec.state.hovered && !vm.splitter_pressed && sp.swap_rect.right > sp.swap_rect.left &&
                compositor_ && compositor_->Dc()) {
                auto* dc = compositor_->Dc();
                const float r = (sp.swap_rect.right - sp.swap_rect.left) * 0.5f;
                const auto disc = D2D1::Ellipse(D2D1::Point2F(sp.swap_rect.left + r, sp.swap_rect.top + r), r, r);
                MakeBrush(dc, on_swap ? theme.fill_input_hover : theme.surface_card, brTextSecondary_);
                dc->FillEllipse(disc, brTextSecondary_.get());
                MakeBrush(dc, on_swap ? theme.accent : theme.stroke_card, brTextSecondary_);
                dc->DrawEllipse(disc, brTextSecondary_.get(), std::max(1.0f, scale_));
                DrawPaneHeaderIcon(sp.swap_rect, sp.vertical ? PaneHeaderIcon::SwapHorizontal
                                                             : PaneHeaderIcon::SwapVertical,
                                   on_swap ? theme.accent : theme.text_secondary);
            }
        }
        return;
    }
    DrawSinglePane(vm, vm.pane, ContentRect(rect.right, rect.bottom), 0, true, false, theme);
}
void MainRenderer::DrawPaneEmptyState(const WindowViewModel& vm, const PaneViewModel& pane,
                                      const D2D1_RECT_F& bounds, int pane_index,
                                      const Theme& theme) {
    if (pane.dup_funnel.on) return;   // 重复文件 tab: the pass cards above say how far it is
    if (pane.is_changes) {
        const std::wstring& message = !pane.change_empty_text.empty() ? pane.change_empty_text :
            pulse::l10n::Get(pulse::l10n::StringId::NoMatches);
        ComPtr<IDWriteTextLayout> explanation;
        const float width = std::max(1.0f, bounds.right - bounds.left - 32 * scale_);
        if (SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(message.c_str(),
            static_cast<UINT32>(message.size()), compositor_->TextFormat(), width, 10000 * scale_, &explanation))) {
            explanation->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            explanation->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            explanation->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TEXT_METRICS metrics{};
            explanation->GetMetrics(&metrics);
            MakeBrush(compositor_->Dc(), theme.text_secondary, brTextSecondary_);
            compositor_->DrawTextLayout({bounds.left + 16 * scale_,
                bounds.top + std::max(0.0f, (bounds.bottom - bounds.top - metrics.height) * 0.5f)},
                explanation.get(), brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }
    if (!pane.filter_text.empty()) {
        // Filter matched nothing: offer the subfolder search (real folders) and a reset.
        const int first = pane.is_file_system ? 0 : 1;
        const int count = 2 - first;
        const SearchEmptyLayout layout = MakeSearchEmptyLayout(bounds, scale_, count);
        if (layout.show_art) DrawNoSelectionSvg(layout.art, theme.bg.r > 0.5f ? 0.68f : 1.0f);
        painter_.DrawText(pulse::l10n::Get(pane.is_file_system ? pulse::l10n::StringId::EmptyFilterTry
                                                               : pulse::l10n::StringId::NoMatches),
                          layout.title, compositor_->HeaderFormat(), theme.text,
                          fluent::HorizontalAlignment::Center);
        for (int i = 0; i < count; ++i) {
            const D2D1_RECT_F& rc = layout.buttons[i];
            if (rc.right - rc.left < 40.0f * scale_) break;
            const int action = first + i;
            const bool hovered = IsHovered(vm, HitTestResult::FilterEmptyAction, action) &&
                                 vm.hover_pane_index == pane_index;
            painter_.FillRoundedRect(rc, 6.0f * scale_,
                                     hovered ? theme.fill_input_hover : theme.fill_input);
            painter_.StrokeRoundedRect(rc, 6.0f * scale_, theme.stroke_card);
            painter_.DrawText(pulse::l10n::Get(action == 0 ? pulse::l10n::StringId::HintActSearchSub
                                                           : pulse::l10n::StringId::EmptyActClearFilter),
                              rc, compositor_->SmallFormat(), theme.accent,   // accent_text is for accent fills
                              fluent::HorizontalAlignment::Center);
        }
        return;
    }
    if (pane.is_query_search) {
        app::SearchEmptyAction actions[3];
        const int count = app::SearchEmptyActions(pane.search_query, actions);
        const SearchEmptyLayout layout = MakeSearchEmptyLayout(bounds, scale_, count);
        if (layout.show_art) DrawNoSelectionSvg(layout.art, theme.bg.r > 0.5f ? 0.68f : 1.0f);
        painter_.DrawText(pulse::l10n::Get(count > 0 ? pulse::l10n::StringId::EmptySearchTry
                                                     : pulse::l10n::StringId::NoMatches),
                          layout.title, compositor_->HeaderFormat(), theme.text,
                          fluent::HorizontalAlignment::Center);
        for (int i = 0; i < count; ++i) {
            const D2D1_RECT_F& rc = layout.buttons[i];
            if (rc.right - rc.left < 40.0f * scale_) break;
            const bool hovered = IsHovered(vm, HitTestResult::SearchEmptyAction, i) &&
                                 vm.hover_pane_index == pane_index;
            painter_.FillRoundedRect(rc, 6.0f * scale_,
                                     hovered ? theme.fill_input_hover : theme.fill_input);
            painter_.StrokeRoundedRect(rc, 6.0f * scale_, theme.stroke_card);
            const pulse::l10n::StringId label =
                actions[i] == app::SearchEmptyAction::ClearFilters ? pulse::l10n::StringId::EmptyActClearFilters :
                actions[i] == app::SearchEmptyAction::SearchContent ? pulse::l10n::StringId::EmptyActContent :
                pulse::l10n::StringId::EmptyActEverywhere;
            painter_.DrawText(pulse::l10n::Get(label), rc, compositor_->SmallFormat(), theme.accent,
                              fluent::HorizontalAlignment::Center);
        }
        return;
    }
    if (!pane.is_file_system) {
        if (pane.is_recycle) {
            fluent::EmptyStateSpec empty;
            empty.bounds = bounds;
            empty.glyph = L"\xE75C";
            empty.title = pulse::l10n::Get(pulse::l10n::StringId::RecycleEmpty);
            empty.message = pulse::l10n::Get(pulse::l10n::StringId::RecycleEmptyMessage);
            painter_.DrawEmptyState(empty);
            return;
        }
        if (pane.is_starred || pane.is_recent) {
            const PaneEmptyLayout layout = MakePaneEmptyLayout(bounds, scale_, false);
            const float svg_opacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
            if (!DrawCuratedEmptyStateSvg(pane.is_starred, layout.art, svg_opacity)) {
                fluent::EmptyStateSpec fallback;
                fallback.bounds = bounds;
                fallback.glyph = pane.is_starred ? L"\xE735" : L"\xE823";
                fallback.title = pulse::l10n::Get(pane.is_starred
                    ? pulse::l10n::StringId::NoStarred : pulse::l10n::StringId::NoRecent);
                painter_.DrawEmptyState(fallback);
                return;
            }
            const std::wstring title = pulse::l10n::Get(pane.is_starred
                ? pulse::l10n::StringId::NoStarred
                : pane.recent_filter != 0 ? pulse::l10n::StringId::NoFilteredResults
                                          : pulse::l10n::StringId::NoRecent);
            const std::wstring message = pulse::l10n::Get(pane.is_starred
                ? pulse::l10n::StringId::NoStarredMessage
                : pane.recent_filter != 0 ? pulse::l10n::StringId::TryOtherType
                                          : pulse::l10n::StringId::NoRecentMessage);
            painter_.DrawText(title, layout.title, compositor_->HeaderFormat(), theme.text,
                              fluent::HorizontalAlignment::Center);
            if (layout.show_message) {
                painter_.DrawText(message, layout.message, compositor_->SmallFormat(),
                                  theme.text_secondary,
                                  fluent::HorizontalAlignment::Center);
            }
            return;
        }
        // Virtual / non-filesystem panes: document illustration from
        // assets/no-selection-state.svg (same style as other empty states).
        const PaneEmptyLayout layout =
            MakePaneEmptyLayout(bounds, scale_, false, 320.0f / 240.0f);
        const float svg_opacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
        if (!DrawNoSelectionSvg(layout.art, svg_opacity)) {
            fluent::EmptyStateSpec fallback;
            fallback.bounds = bounds;
            fallback.glyph = kIconFile;
            fallback.title = pulse::l10n::Get(pulse::l10n::StringId::NoContent);
            painter_.DrawEmptyState(fallback);
            return;
        }
        painter_.DrawText(pane.dup_empty_title.empty() ? pulse::l10n::Get(pulse::l10n::StringId::NoContent)
                                                       : pane.dup_empty_title,
                          layout.title, compositor_->HeaderFormat(), theme.text,
                          fluent::HorizontalAlignment::Center);
        return;
    }

    // No listing ever arrived (the folder could not be opened): the banner
    // says why; "this folder is empty" would be wrong.
    if (!pane.snapshot && !pane.banner_title.empty()) return;
    const PaneEmptyLayout layout = MakePaneEmptyLayout(bounds, scale_, pane.can_create);
    const float svgOpacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
    if (!DrawEmptyStateSvg(layout.art, svgOpacity)) {
        const float icon = std::min(72.0f * scale_, layout.art.bottom - layout.art.top);
        DrawFolderIcon((layout.art.left + layout.art.right - icon) * 0.5f,
                       (layout.art.top + layout.art.bottom - icon) * 0.5f, icon, theme);
    }
    painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::FolderEmpty), layout.title,
                      compositor_->HeaderFormat(), theme.text,
                      fluent::HorizontalAlignment::Center);
    if (layout.show_message) {
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::FolderEmptyMessage), layout.message,
                          compositor_->SmallFormat(), theme.text_secondary,
                          fluent::HorizontalAlignment::Center);
    }
    if (layout.show_action) {
        const bool hovered = vm.hover_region == static_cast<int>(HitTestResult::PaneEmptyNewFolder) &&
                             vm.hover_control_index == pane_index;
        painter_.FillRoundedRect(layout.action, 6.0f * scale_,
                                 hovered ? theme.fill_input_hover : theme.fill_input);
        painter_.StrokeRoundedRect(layout.action, 6.0f * scale_, theme.stroke_card);
        const float iconW = 32.0f * scale_;
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::NewFolder),
            D2D1::RectF(layout.action.left + 8.0f * scale_, layout.action.top,
                        layout.action.right - iconW, layout.action.bottom),
            compositor_->SmallFormat(), theme.text, fluent::HorizontalAlignment::Center);
        MakeBrush(compositor_->Dc(), theme.stroke_divider, brStrokeDivider_);
        compositor_->Dc()->DrawLine(
            D2D1::Point2F(layout.action.right - iconW, layout.action.top + 6.0f * scale_),
            D2D1::Point2F(layout.action.right - iconW, layout.action.bottom - 6.0f * scale_),
            brStrokeDivider_.get(), 1.0f);
        DrawIconText(layout.action.right - iconW, layout.action.top, iconW,
                     layout.action.bottom - layout.action.top,
                     kIconAdd, L"+", theme.text, 0.72f);
    }
}

void MainRenderer::DrawSinglePane(const WindowViewModel& vm, const PaneViewModel& pane,
                                  const D2D1_RECT_F& pane_rect, int pane_index, bool focused, bool target,
                                  const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    // The header spans the whole pane; in the column view everything below
    // it uses the narrowed body (see PaneBodyBounds).
    D2D1_RECT_F bounds = pane_rect;
    float x = bounds.left;
    const float y0 = bounds.top;
    float w = bounds.right - bounds.left;
    const float bottom = bounds.bottom;
    if (w <= 1.0f || bottom - y0 <= 1.0f) return;

    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Each pane is a card on the shared sheet; the frame below outlines it.
    // Over DWM material it replaces the title tint and sheet under it, so
    // its opacity is the interface transparency's whatever the sidebar's.
    const bool pane_cut = pane_alpha_ >= 0.0f;
    MakeBrush(dc, pane_cut ? pane_color_ : WithAlpha(theme.surface_card, card_alpha_), brFillHover_);
    if (pane_cut) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    FillRoundedRect(dc, brFillHover_.get(), pane_rect.left, pane_rect.top,
                    pane_rect.right - pane_rect.left, pane_rect.bottom - pane_rect.top,
                    theme.radius_control * scale_);
    if (pane_cut) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);

    float y = y0;
    const std::wstring& title = pane.header_text;
    pane_close_visible_ = vm.pane_slots.size() > 1;
    pane_expand_visible_ = vm.layout_controls.pane_names.size() > 1 && vm.layout_controls.zoom_index < 0;
    const D2D1_RECT_F mediumRc = PaneMediumIconsRect(bounds, pane.filter_expand, pane.size_control);
    const D2D1_RECT_F viewRc = PaneViewButtonRect(bounds, pane.filter_expand);
    const D2D1_RECT_F detailsRc = PaneDetailsRect(bounds, 1.0f, pane.size_control);
    const D2D1_RECT_F wallRc = PaneWallRect(pane, bounds);
    if (pane.header_drop) {
        MakeBrush(dc, WithAlpha(theme.accent, 0.18f), brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bounds.left + scale_, bounds.top + scale_,
            std::max(0.0f, w - 2.0f * scale_), pane_header_height_ - 2.0f * scale_,
            theme.radius_control * scale_);
    }
    // Keep the title stationary while the focused pane's controls crossfade.
    const bool split = vm.layout_controls.pane_names.size() > 1;
    const D2D1_RECT_F sizeButtonRc = PaneSizeButtonRect(pane, bounds);
    const D2D1_RECT_F sizeSliderRc = PaneSizeSliderRect(pane, bounds);
    // A narrow pane's opened size slider takes the title's place.
    const bool sizePopup = sizeButtonRc.right > sizeButtonRc.left && sizeSliderRc.right > sizeSliderRc.left &&
                           pane.header_controls_opacity > 0.5f;
    const float textLeft = x + 8.0f * scale_;
    const bool zoomed = vm.layout_controls.zoom_index >= 0;
    const auto zoomChip = PaneZoomChipRect(pane,bounds);
    const float textRight = std::max(textLeft, zoomed ? zoomChip.left-8*scale_ : PaneHeaderTitleRight(pane, bounds));
    const auto titleBadge = ChangeTitleRect(bounds, textRight, pane_header_height_, pane.title_change_badge, scale_, compositor_, pane.header_text);
    const float textWidth = (titleBadge.right > titleBadge.left ? titleBadge.left - 4 * scale_ : textRight) - textLeft;
    MakeBrush(dc, split && !focused ? theme.text_secondary : theme.text, brText_);
    if (!sizePopup) {
        DrawTextEndEllipsis(compositor_, compositor_->HeaderFormat(), brText_.get(), title,
            textLeft, y, textWidth, pane_header_height_);
        DrawChangeBadge(compositor_, painter_, pane.title_change_badge, titleBadge, theme, scale_);
    }

    const float controlsOpacity = split ? 1.0f : std::clamp(pane.header_controls_opacity, 0.0f, 1.0f);
    if (controlsOpacity > 0.0f) {
        const bool fading = controlsOpacity < 1.0f;
        if (fading) dc->PushLayer(D2D1::LayerParameters(D2D1::RectF(bounds.left, bounds.top,
            bounds.right, bounds.top + pane_header_height_), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), controlsOpacity), nullptr);
        // plate = false: the active button's plate is drawn separately (the view segment).
        auto headerButton = [&](const D2D1_RECT_F& rc, HitTestResult::Region region,
                                PaneHeaderIcon icon, bool active, bool enabled = true, bool plate = true) {
            const bool hovered = focused && enabled && vm.hover_region == static_cast<int>(region) &&
                                 vm.hover_control_index == pane_index;
            const D2D1_COLOR_F fill = active
                ? (plate ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f) : kTransparent)
                : (hovered ? theme.fill_hover : kTransparent);
            if (fill.a > 0.0f) {
                MakeBrush(dc, fill, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                rc.right - rc.left, rc.bottom - rc.top,
                                theme.radius_control * scale_);
            }
            DrawPaneHeaderIcon(rc, icon, !enabled ? theme.text_disabled
                : (active ? theme.accent : theme.text));
        };
        // Details, icons and the photo wall share a groove; the selected block
        // springs between them.
        const bool detailsView = pane.view_mode == ViewMode::Details;
        const bool hasWall = wallRc.right > wallRc.left;
        const bool wallView = hasWall && pane.wall && IsZoomIconMode(pane.view_mode);
        const bool iconView = IsZoomIconMode(pane.view_mode) && !wallView;
        if(bounds.right-bounds.left>=280*scale_) {
        MakeBrush(dc, WithAlpha(theme.text, 0.05f), brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), detailsRc.left - 2.0f * scale_, detailsRc.top,
                        (hasWall ? wallRc.right : mediumRc.right) - detailsRc.left + 4.0f * scale_,
                        detailsRc.bottom - detailsRc.top, theme.radius_control * scale_);
        HeaderSizeMotion* headerMotion = pane_index >= 0
            ? &header_size_motion_[static_cast<size_t>(pane_index) % header_size_motion_.size()] : nullptr;
        if (detailsView || iconView || wallView) {
            D2D1_RECT_F block = detailsView ? detailsRc : wallView ? wallRc : mediumRc;
            if (headerMotion) {
                block = headerMotion->segment.Update(0, detailsView ? 0 : wallView ? 2 : 1, block, motion_frame_,
                                                     motion_now_, motion::spring::Snappy);
                if (headerMotion->segment.Active(motion_now_)) RequestMotionFrame();
            }
            const HitTestResult::Region selected = detailsView ? HitTestResult::PaneDetails
                                                 : wallView ? HitTestResult::PaneWall
                                                            : HitTestResult::PaneMediumIcons;
            const bool hot = focused && vm.hover_region == static_cast<int>(selected) &&
                             vm.hover_control_index == pane_index;
            MakeBrush(dc, WithAlpha(theme.accent, hot ? 0.24f : 0.14f), brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), block.left, block.top, block.right - block.left,
                            block.bottom - block.top, theme.radius_control * scale_);
        } else if (headerMotion) {
            headerMotion->segment.Hide();
        }
        headerButton(detailsRc, HitTestResult::PaneDetails, PaneHeaderIcon::View, detailsView, true, false);
        headerButton(mediumRc, HitTestResult::PaneMediumIcons, PaneHeaderIcon::MediumIcons, iconView, true, false);
        if (hasWall) headerButton(wallRc, HitTestResult::PaneWall, PaneHeaderIcon::Wall, wallView, true, false);
        }
        headerButton(viewRc, HitTestResult::PaneViewButton, PaneHeaderIcon::More, false);
        const bool sizeButton = sizeButtonRc.right > sizeButtonRc.left;
        if (sizeButton)
            headerButton(sizeButtonRc, HitTestResult::PaneSizeButton, PaneHeaderIcon::Size, pane.size_slider_open);
        DrawPaneSizeSlider(pane, pane_index, sizeSliderRc,
            sizePopup || !sizeButton ? (sizeSliderRc.right > sizeSliderRc.left ? sizeSliderRc : detailsRc)
                                     : sizeButtonRc,
            focused && vm.hover_region == static_cast<int>(HitTestResult::PaneSizeSlider) &&
                vm.hover_control_index == pane_index,
            theme);
        if (fading) dc->PopLayer();
    }
    if (zoomed && !sizePopup) {
        MakeBrush(dc,WithAlpha(theme.accent,.12f),brFillHover_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(zoomChip,13*scale_,13*scale_),brFillHover_.get());
        auto icon=zoomChip; icon.left+=6*scale_;icon.right=icon.left+18*scale_;
        DrawPaneHeaderIcon(icon,PaneHeaderIcon::Expand,theme.accent);
        auto restore=zoomChip;restore.left=std::max(zoomChip.left,zoomChip.right-72*scale_);restore.top+=2*scale_;restore.bottom-=2*scale_;restore.right-=2*scale_;
        MakeBrush(dc,theme.surface_card,brFillHover_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(restore,11*scale_,11*scale_),brFillHover_.get());
        MakeBrush(dc,theme.text,brText_);
        DrawTextEndEllipsis(compositor_,compositor_->SmallFormat(),brText_.get(),l10n::Get(l10n::StringId::PaneZoomRestore),
            restore.left+8*scale_,restore.top,restore.right-restore.left-16*scale_,restore.bottom-restore.top);
        MakeBrush(dc,theme.accent,brText_);
        DrawTextEndEllipsis(compositor_,compositor_->SmallFormat(),brText_.get(),l10n::Get(l10n::StringId::PaneZoomBadge),
            icon.right+2*scale_,zoomChip.top,56*scale_,zoomChip.bottom-zoomChip.top);
        if(zoomChip.right-zoomChip.left>240*scale_) {
            wchar_t background[128]{};swprintf_s(background,l10n::Get(l10n::StringId::PaneZoomBackgroundFormat).c_str(),
                static_cast<unsigned>(vm.layout_controls.pane_names.size()-1));
            MakeBrush(dc,theme.text_secondary,brTextSecondary_);
            DrawTextEndEllipsis(compositor_,compositor_->SmallFormat(),brTextSecondary_.get(),background,
                icon.right+62*scale_,zoomChip.top,restore.left-icon.right-66*scale_,zoomChip.bottom-zoomChip.top);
        }
    }
    if (pane_expand_visible_) {
        const auto expandRc = PaneExpandRect(bounds);
        const bool hot = vm.hover_region == static_cast<int>(HitTestResult::PaneExpand) &&
            vm.hover_control_index == pane_index;
        if (hot) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(expandRc,4*scale_,4*scale_),brFillHover_.get());
        }
        DrawPaneHeaderIcon(expandRc, vm.layout_controls.zoom_index >= 0 ? PaneHeaderIcon::Restore : PaneHeaderIcon::Expand,
            hot ? theme.accent : theme.text_secondary);
    }
    if (pane_close_visible_) {
        const auto closeRc = PaneCloseRect(bounds);
        MakeBrush(dc, WithAlpha(theme.text_secondary, 0.25f), brFillHover_);
        const float dividerX = closeRc.left - 2.0f * scale_;
        dc->DrawLine(D2D1::Point2F(dividerX, closeRc.top + 6.0f * scale_),
                     D2D1::Point2F(dividerX, closeRc.bottom - 6.0f * scale_),
                     brFillHover_.get(), scale_);
        const bool closeHot = vm.hover_region == static_cast<int>(HitTestResult::PaneClose) &&
                              vm.hover_control_index == pane_index;
        if (closeHot) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), closeRc.left, closeRc.top,
                            closeRc.right - closeRc.left, closeRc.bottom - closeRc.top,
                            theme.radius_control * scale_);
        }
        DrawPaneHeaderIcon(closeRc, PaneHeaderIcon::Close, closeHot ? theme.text : theme.text_secondary);
    }
    y += pane_header_height_;

    if (const ColumnStripLayout strip = ColumnStripGeometry(pane, pane_rect); strip.active) {
        DrawColumnStrip(vm, pane, pane_rect, pane_index, theme);
        bounds = strip.body;
        x = bounds.left;
        w = bounds.right - bounds.left;
    }

    const float bannerH = PaneBannerHeight(pane, w, scale_, compositor_);
    if (focused) focused_banner_height_ = bannerH;
    if (bannerH > 0.0f) {
        if (pane.is_changes) {
            ComPtr<IDWriteTextLayout> explanation;
            ChangeBannerLayout(pane, w, scale_, compositor_, explanation);
            if (explanation.get()) {
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                compositor_->DrawTextLayout({x + 16 * scale_, y + 8 * scale_}, explanation.get(),
                    brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            PaneBannerParts parts;
            BuildPaneBanner(pane, D2D1::RectF(x + 8 * scale_, y, x + w - 8 * scale_, y + bannerH - 4 * scale_),
                            pane_index, parts);
            fluent::InfoBarSpec& bar = parts.bar;
            const bool here = vm.hover_pane_index == pane_index;
            for (int i = 0; i < 2; ++i)
                if (parts.regions[i] != HitTestResult::None && here && IsHovered(vm, parts.regions[i], parts.indices[i]))
                    bar.hovered_action = i;
            bar.state.hovered = bar.show_close && here &&
                IsHovered(vm, HitTestResult::PaneBannerButton, static_cast<int>(PaneBannerAction::Dismiss));
            painter_.DrawInfoBar(bar);
            if (pane.compare_active && !pane.is_content_search) {
                const fluent::InfoBarLayout lay = painter_.LayoutInfoBar(bar);
                {
                    const bool dark_bg = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
                    IDWriteTextFormat* chip_fmt = compositor_->SmallFormat();
                    const float left = lay.title.left;
                    const float right = lay.actions[0].left - 6.0f * scale_;
                    const float cy = (lay.glyph.top + lay.glyph.bottom) * 0.5f;
                    const float line_h = 18.0f * scale_;
                    static constexpr l10n::StringId kChipLabels[] = {
                        l10n::StringId::CompareChipOnly, l10n::StringId::CompareChipNewer,
                        l10n::StringId::CompareChipOlder, l10n::StringId::CompareChipDiffers,
                    };
                    const auto& counts = pane.compare_counts;
                    const bool identical = counts[1] + counts[2] + counts[3] + counts[4] == 0;
                    if (identical) {
                        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                        DrawTextEndEllipsis(compositor_, chip_fmt, brTextSecondary_.get(),
                            l10n::Get(l10n::StringId::CompareIdentical), left, cy - line_h * 0.5f,
                            std::max(0.0f, right - left), line_h);
                    } else {
                        // Full "● 较新 2" chips when they fit, else compact "● 2".
                        std::wstring texts[2][4];
                        float widths[2] = {0.0f, 0.0f};
                        const float dot = 8.0f * scale_, gap = 12.0f * scale_;
                        for (int k = 0; k < 4; ++k) {
                            const int n = counts[static_cast<size_t>(k + 1)];
                            if (n <= 0) continue;
                            texts[0][k] = l10n::Get(kChipLabels[k]) + L" " + std::to_wstring(n);
                            texts[1][k] = std::to_wstring(n);
                            for (int m = 0; m < 2; ++m)
                                widths[m] += dot + 5.0f * scale_ + gap +
                                    MeasureLayoutText(compositor_, compositor_->DwriteFactory(), chip_fmt, texts[m][k]);
                        }
                        const int mode = widths[0] <= right - left ? 0 : 1;
                        float cx = left;
                        for (int k = 0; k < 4; ++k) {
                            const std::wstring& t = texts[mode][k];
                            if (t.empty()) continue;
                            const float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), chip_fmt, t);
                            if (cx + dot + 5.0f * scale_ + tw > right) break;
                            const D2D1_COLOR_F c = CompareMarkColor(static_cast<uint8_t>(k + 1), dark_bg);
                            MakeBrush(dc, c, brFillInput_);
                            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + dot * 0.5f, cy), dot * 0.5f, dot * 0.5f),
                                            brFillInput_.get());
                            cx += dot + 5.0f * scale_;
                            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                            DrawTextEndEllipsis(compositor_, chip_fmt, brTextSecondary_.get(),
                                t, cx, cy - line_h * 0.5f, tw + 2.0f * scale_, line_h);
                            cx += tw + gap;
                        }
                    }
                }
            }
        }
        y += bannerH;
    }
    if (pane.is_recent) {
        D2D1_RECT_F track = RecentFilterRect(bounds, pane_header_height_ + bannerH, scale_, 0);
        track.right = RecentFilterRect(bounds, pane_header_height_ + bannerH, scale_, 2).right;
        painter_.DrawSegmentedTrack(track);
        static constexpr pulse::l10n::StringId labels[] = {
            pulse::l10n::StringId::FilterAll, pulse::l10n::StringId::FilterFolders,
            pulse::l10n::StringId::FilterFiles,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = RecentFilterRect(bounds, pane_header_height_ + bannerH, scale_, i);
            segment.shared_track = true;
            segment.text = pulse::l10n::Get(labels[i]);
            segment.position = i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = pane.recent_filter == i;
            segment.state.hovered = IsHovered(vm, HitTestResult::RecentFilter, i) &&
                                    vm.hover_pane_index == pane_index;
            painter_.DrawSegmentedItem(segment);
        }
        const D2D1_RECT_F clear = RecentClearRect(bounds, pane_header_height_ + bannerH, scale_);
        fluent::ControlState clear_state;
        clear_state.enabled = pane.recent_total > 0;
        clear_state.hovered = IsHovered(vm, HitTestResult::RecentClear, pane_index);
        painter_.DrawButton({ clear, {}, kIconDelete,
            fluent::ButtonKind::Transparent, clear_state, true });
        y += kRecentControlsDip * scale_;
    }

    if (pane.is_query_search) {
        const app::AdvancedSearchSpec spec = app::ParseSearchQuery(pane.search_query);
        std::wstring labels[5];
        FillSearchFilterChipLabels(pane.search_query, labels);
        float widths[5]{};
        SearchFilterChipWidthsPx(painter_, scale_, labels, widths);
        for (int i = 0; i < 5; ++i) {
            fluent::ButtonSpec chip;
            chip.bounds = SearchFilterRect(bounds, pane_header_height_ + bannerH, scale_, i, widths);
            chip.text = labels[i];
            if (i == 4) chip.glyph = kSearchAdvancedGlyph;
            chip.kind = fluent::ButtonKind::Toggle;
            chip.drop_down = i < 3;
            chip.state.hovered = IsHovered(vm, HitTestResult::SearchFilter, i) &&
                                 vm.hover_pane_index == pane_index;
            const bool active = (i == 0 && spec.kind != pulse::index::SearchKind::Any) ||
                                (i == 1 && spec.date != app::DatePreset::Any) ||
                                (i == 2 && spec.size != app::SizePreset::Any) ||
                                (i == 3 && !spec.content.empty());
            chip.state.selected = active;
            chip.state.checked = active;
            painter_.DrawButton(chip);
        }
        y += kSearchFiltersDip * scale_;
    }

    if (pane.is_tag_query) {
        // Condition bar: 且 chips are tinted with the tag color, 任一 chips
        // are outlined too (or between), 不含 is grey and struck through.
        using Chip = TagQueryChipView;
        const TagQueryBarLayout bar = LayoutTagQueryBar(compositor_, pane, bounds, y, scale_);
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const bool pane_hot = vm.hover_pane_index == pane_index;
        const float line_h = 18.0f * scale_;
        const auto alpha = [](D2D1_COLOR_F c, float a) { c.a *= a; return c; };
        const auto text = [&](const std::wstring& t, float left, float right, float cy,
                              const D2D1_COLOR_F& color) {
            MakeBrush(dc, color, brText_);
            DrawTextEndEllipsis(compositor_, fmt, brText_.get(), t, left,
                                cy - line_h * 0.5f, std::max(0.0f, right - left), line_h);
        };
        const auto dot = [&](float cx, float cy, const D2D1_COLOR_F& color) {
            const float r = 4.0f * scale_;
            painter_.FillRoundedRect(D2D1::RectF(cx - r, cy - r, cx + r, cy + r), r, color);
        };
        const auto glyph = [&](const wchar_t* g, float left, float cy, float size, const D2D1_COLOR_F& color) {
            painter_.DrawGlyph(g, D2D1::RectF(left, cy - size * 0.5f, left + size, cy + size * 0.5f), color);
        };
        for (size_t i = 0; i < pane.tag_query_chips.size(); ++i) {
            const D2D1_RECT_F rc = bar.chips[i];
            if (rc.right <= rc.left) continue;
            const Chip& chip = pane.tag_query_chips[i];
            const bool hot = pane_hot && IsHovered(vm, HitTestResult::TagQueryChip, static_cast<int>(i));
            const bool x_hot = pane_hot && IsHovered(vm, HitTestResult::TagQueryChipRemove, static_cast<int>(i));
            const float radius = (rc.bottom - rc.top) * 0.5f;
            const float cy = (rc.top + rc.bottom) * 0.5f;
            float left = rc.left + 10.0f * scale_;
            if (chip.kind >= Chip::Add) {
                if (hot) painter_.FillRoundedRect(rc, radius, theme.fill_hover);
                painter_.StrokeRoundedRect(rc, radius, theme.stroke_card);
                glyph(chip.kind == Chip::Add ? kIconAdd : L"\xE74E", left, cy, 12.0f * scale_, theme.text_secondary);
                text(chip.label, left + 18.0f * scale_, rc.right - 8.0f * scale_, cy, theme.text_secondary);
                continue;
            }
            const bool tinted = chip.kind == Chip::All || chip.kind == Chip::Any;
            painter_.FillRoundedRect(rc, radius, tinted ? alpha(chip.color, hot || x_hot ? 0.30f : 0.18f)
                                                        : (hot || x_hot ? theme.fill_hover : theme.fill_input));
            if (chip.kind == Chip::Any) painter_.StrokeRoundedRect(rc, radius, alpha(chip.color, 0.75f), scale_);
            const float text_right = bar.removes[i].left - 2.0f * scale_;
            if (chip.kind == Chip::Location) {
                glyph(kIconFolder, left, cy, 14.0f * scale_, theme.text_secondary);
                left += 20.0f * scale_;
                text(chip.label, left, text_right, cy, theme.text);
            } else {
                dot(left + 4.0f * scale_, cy, chip.kind == Chip::Not ? alpha(chip.color, 0.45f) : chip.color);
                left += 14.0f * scale_;
                text(chip.label, left, text_right, cy, chip.kind == Chip::Not ? theme.text_secondary : theme.text);
                if (chip.kind == Chip::Not) {
                    const float strike_w = std::min(text_right - left,
                                             MeasureTextWidth(compositor_->DwriteFactory(), fmt, chip.label));
                    painter_.FillRoundedRect(D2D1::RectF(left, cy - 0.5f * scale_, left + strike_w, cy + 0.5f * scale_),
                                             0.0f, theme.text_secondary);
                }
            }
            const D2D1_RECT_F xr = bar.removes[i];
            if (x_hot) painter_.FillRoundedRect(xr, (xr.bottom - xr.top) * 0.5f, theme.fill_pressed);
            const float xs = 9.0f * scale_;
            const float xc = (xr.left + xr.right) * 0.5f;
            painter_.DrawGlyph(L"\xE711", D2D1::RectF(xc - xs * 0.5f, cy - xs * 0.5f, xc + xs * 0.5f, cy + xs * 0.5f),
                               x_hot ? theme.text : theme.text_secondary);
        }
        for (const auto& mark : bar.or_marks) {
            const float cy = (mark.top + mark.bottom) * 0.5f;
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            text(pulse::l10n::Get(pulse::l10n::StringId::TagOr), mark.left, mark.right, cy, theme.text_secondary);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        if (!pane.tag_query_refine.empty()) {
            const float cy = (bar.refine_label.top + bar.refine_label.bottom) * 0.5f;
            text(pulse::l10n::Get(pulse::l10n::StringId::TagRefineLabel), bar.refine_label.left,
                 bar.refine_label.right, cy, theme.text_secondary);
            for (size_t i = 0; i < pane.tag_query_refine.size(); ++i) {
                const D2D1_RECT_F rc = bar.refine[i];
                if (rc.right <= rc.left) continue;
                const Chip& chip = pane.tag_query_refine[i];
                const bool hot = pane_hot && IsHovered(vm, HitTestResult::TagQueryRefine, static_cast<int>(i));
                const float rcy = (rc.top + rc.bottom) * 0.5f;
                if (hot) painter_.FillRoundedRect(rc, (rc.bottom - rc.top) * 0.5f, theme.fill_hover);
                float left = rc.left + 8.0f * scale_;
                if (chip.kind == Chip::RefineLocation) {
                    glyph(kIconFolder, left, rcy, 14.0f * scale_, theme.text_secondary);
                    left += 20.0f * scale_;
                } else {
                    dot(left + 4.0f * scale_, rcy, chip.color);
                    left += 14.0f * scale_;
                }
                const std::wstring count = chip.count >= 0 ? std::to_wstring(chip.count) : std::wstring{};
                const float count_w = count.empty() ? 0.0f
                    : MeasureTextWidth(compositor_->DwriteFactory(), fmt, count) + 2.0f * scale_;
                const float right = rc.right - 8.0f * scale_;
                text(chip.label, left, right - count_w - 6.0f * scale_, rcy, theme.text);
                if (!count.empty()) text(count, right - count_w, right, rcy, theme.text_secondary);
            }
        }
        y += TagQueryBarHeight(pane, scale_);
    }

    if (pane.is_changes) {
        const std::wstring labels[] = { pane.change_time_label, pane.change_type_label, pulse::l10n::Get(pulse::l10n::StringId::ChangeMore) };
        for (int i = 0; i < 3; ++i) {
            if (i == 2 && !pane.change_has_more) continue;
            fluent::ButtonSpec button;
            const float cw = std::max(0.0f, (w - 16 * scale_) / 3);
            button.bounds = D2D1::RectF(x + 8 * scale_ + i * cw, y + 2 * scale_, x + 8 * scale_ + (i + 1) * cw - 4 * scale_, y + 32 * scale_);
            button.text = labels[i];
            painter_.DrawButton(button);
        }
        y += 36 * scale_;
    }
    if (pane.dup_funnel.on) {
        // 重复文件 tab: ① 大小相同 › ② 开头 64 KB › ③ 逐字节, real counts as they come.
        const DupFunnelView& f = pane.dup_funnel;
        const float gap = 8.0f * scale_, pad = 10.0f * scale_, radius = 6.0f * scale_;
        const float cw = std::max(1.0f, (w - 16.0f * scale_ - 2.0f * gap) / 3.0f);
        const float top = y + 2.0f * scale_, card_bottom = y + (kDupFunnelDip - 8.0f) * scale_;
        IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
        IDWriteTextFormat* body = compositor_->TextFormat();
        for (int i = 0; i < 3; ++i) {
            const float left = x + 8.0f * scale_ + static_cast<float>(i) * (cw + gap);
            const D2D1_RECT_F rc = D2D1::RectF(left, top, left + cw, card_bottom);
            const bool done = i < f.stage, running = i == f.stage;
            painter_.FillRoundedRect(rc, radius, theme.surface_card);
            painter_.StrokeRoundedRect(rc, radius, running ? WithAlpha(theme.accent, 0.6f) : theme.stroke_card);
            const float inner = std::max(0.0f, cw - 2.0f * pad - (done ? 18.0f * scale_ : 0.0f));
            MakeBrush(dc, done || running ? theme.text_secondary : WithAlpha(theme.text_secondary, 0.6f), brTextSecondary_);
            DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), f.title[i], rc.left + pad, rc.top + 6.0f * scale_,
                                inner, 18.0f * scale_);
            if (done)
                DrawIconText(rc.right - pad - 14.0f * scale_, rc.top + 6.0f * scale_, 14.0f * scale_, 18.0f * scale_,
                             L"\xE73E", L"", theme.accent, 0.8f);
            MakeBrush(dc, done || running ? theme.text : WithAlpha(theme.text_secondary, 0.7f), brTextSecondary_);
            DrawTextEndEllipsis(compositor_, body, brTextSecondary_.get(), f.value[i], rc.left + pad, rc.top + 25.0f * scale_,
                                std::max(0.0f, cw - 2.0f * pad), 22.0f * scale_);
            if (running && f.progress[i] >= 0.0f) {
                const D2D1_RECT_F track = D2D1::RectF(rc.left + pad, rc.bottom - 9.0f * scale_, rc.right - pad,
                                                      rc.bottom - 6.0f * scale_);
                painter_.FillRoundedRect(track, 1.5f * scale_, WithAlpha(theme.accent, 0.18f));
                D2D1_RECT_F fill = track;
                fill.right = track.left + (track.right - track.left) * std::clamp(f.progress[i], 0.0f, 1.0f);
                if (fill.right > fill.left + 1.0f) painter_.FillRoundedRect(fill, 1.5f * scale_, theme.accent);
            }
        }
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        y += kDupFunnelDip * scale_;
    }
    if (const float overviewH = DriveOverviewHeight(pane, scale_); overviewH > 0.0f) {
        const D2D1_RECT_F card = D2D1::RectF(x + 8.0f * scale_, y + 4.0f * scale_, x + w - 8.0f * scale_,
                                             y + overviewH - 8.0f * scale_);
        DrawDriveOverview(pane, card, CapacityReveal(pane, pane_index), theme);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        y += overviewH;
    }
    if (ShowsColumnHeader(pane.view_mode) && pane.column_strip.Active()) {
        // Column view: the folder's name as a caption like the columns beside
        // it, at the header's height so rows line up across the columns.
        const std::wstring& caption = pane.header_text.empty() ? pane.path : pane.header_text;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(),
                            brTextSecondary_.get(), caption, x + 12.0f * scale_, y,
                            w - 24.0f * scale_, column_header_height_);
        y += column_header_height_;
    } else if (ShowsColumnHeader(pane.view_mode)) {
        // Match the pane's material opacity. Copy over a cut-through pane so
        // the header does not become opaque by stacking the same tint twice;
        // otherwise blend over the existing card, never making it clearer.
        MakeBrush(dc, pane_cut ? pane_color_ : WithAlpha(theme.fill_input, theme.fill_input.a * card_alpha_),
                  brFillInput_);
        if (pane_cut) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        FillRect(dc, brFillInput_.get(), x, y, w, column_header_height_);
        if (pane_cut) dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        FillRect(dc, brStrokeDivider_.get(), x, y + column_header_height_ - 1, w, 1);
        const DetailsColumnLayout columns = AnimateColumns(DetailsColumns(bounds, pane), pane_index);
        if (pane_index >= 0 && pane_index < static_cast<int>(painted_columns_.size())) {
            uint32_t mask = columns.two_line ? (1u << 8) : 0u;
            for (int col = 0; col < columns.count; ++col) {
                // This PC's 总大小 and the media columns are no global columns
                // (and bit 8 is two_line).
                if (columns.kinds[static_cast<size_t>(col)] >= ColumnKind::Total) continue;
                mask |= 1u << static_cast<uint32_t>(columns.kinds[static_cast<size_t>(col)]);
            }
            painted_columns_[static_cast<size_t>(pane_index)] = mask;
        }
        float cx = columns.left;
        // Item check boxes: a select-all box heads the gutter, the Name label follows it.
        const bool headerCheck = CheckGutterDip(pane) > 0.0f && !pane.curated_order &&
                                 !pane.column_strip.Active() && pane.EntryCount() > 0;
        const float nameLead = headerCheck ? CheckGutterDip(pane) * scale_ : 0.0f;
        if (headerCheck) {
            const D2D1_RECT_F box = HeaderCheckRect(columns.left, y);
            const size_t total = pane.EntryCount();
            const size_t picked = static_cast<size_t>(std::max(0, pane.selected_count));
            const bool hot = vm.hover_region == static_cast<int>(HitTestResult::HeaderCheck) &&
                             vm.hover_pane_index == pane_index;
            EnsureRoundCapStroke(dc, roundCapStroke_);
            MakeBrush(dc, theme.accent, brTagDot_);
            DrawItemCheckBox(dc, brTagDot_.get(), roundCapStroke_.get(), box, scale_, theme, IsHighContrast(),
                             picked == 0 ? 0 : picked >= total ? 1 : 2, hot, 1.0f, 1.0f);
        }
        auto drawCol = [&](const std::wstring& label, SortColumn col, float cw, bool right = false,
                           bool sortable = true) {
            const bool active = sortable && !pane.curated_order && pane.sort_column == col;
            MakeBrush(dc, active ? theme.accent : theme.text, brText_);
            IDWriteTextFormat* fmt = compositor_->HeaderFormat();
            const float textInset = 8.0f * scale_;
            const float lead = col == SortColumn::Name ? nameLead : 0.0f;
            const float contentLeft = cx + textInset + lead;
            const float availableW = std::max(0.0f, cw - textInset * 2.0f - lead);
            const float iconW = active ? 12.0f * scale_ : 0.0f;
            const float iconGap = active ? 3.0f * scale_ : 0.0f;
            const float labelAvailable = std::max(0.0f, availableW - iconW - iconGap);
            // LumaText clips to the supplied surface bounds.  DWrite's
            // measured width can differ slightly from LumaText's raster
            // advance (the final stem of a lowercase 'd' is a common case),
            // so use both measurements and retain a small trailing allowance.
            const float dwriteLabelW = MeasureTextWidth(
                compositor_->DwriteFactory(), fmt, label);
            float lumaLabelW = 0.0f;
            compositor_->MeasureLumaText(label, fmt, lumaLabelW);
            const float measuredLabelW = std::max(dwriteLabelW, lumaLabelW);
            const float labelW = std::min(labelAvailable,
                                          measuredLabelW + 4.0f * scale_);
            const float groupW = labelW + iconGap + iconW;
            const float groupLeft = right
                ? contentLeft + availableW - groupW : contentLeft;
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (measuredLabelW > labelAvailable) {
                DrawTextEndEllipsis(compositor_, fmt, brText_.get(), label,
                    groupLeft, y, labelW, column_header_height_);
            } else {
                DrawTextRect(dc, fmt, brText_.get(), label, groupLeft, y,
                             labelW, column_header_height_);
            }
            if (active) {
                DrawIconText(groupLeft + labelW + iconGap, y, iconW,
                             column_header_height_,
                             pane.sort_direction == SortDirection::Asc
                                 ? kIconChevronUp : kIconChevronDown,
                             pane.sort_direction == SortDirection::Asc ? L"^" : L"v",
                             theme.accent, 0.75f);
            }
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            cx += cw;
        };
        // Header drag (column reorder): the column being dragged.
        const int drag_from = vm.column_drag_pane == pane_index && vm.column_drag_from >= columns.FirstOrderable() &&
            vm.column_drag_from < columns.count && columns.reorderable ? vm.column_drag_from : -1;
        // The path column is display-only: never an active/clickable sort.
        auto drawKind = [&](int col) {
            const float cw = columns.widths[static_cast<size_t>(col)];
            switch (columns.kinds[static_cast<size_t>(col)]) {
            case ColumnKind::Name:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnName), SortColumn::Name, cw, false);
                break;
            case ColumnKind::Path:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnPath), SortColumn::Path, cw, false);
                break;
            case ColumnKind::Date:
                drawCol(pane.this_pc_columns ? pulse::l10n::Get(pulse::l10n::StringId::ColumnUsage)
                        : pane.date_column_label.empty()
                            ? pulse::l10n::Get(pulse::l10n::StringId::ColumnModified)
                            : pane.date_column_label,
                        SortColumn::Mtime, cw, false);
                break;
            case ColumnKind::Type:
                drawCol(pane.type_column_label.empty() ? pulse::l10n::Get(pulse::l10n::StringId::ColumnType)
                                                       : pane.type_column_label,
                        SortColumn::Type, cw, false);
                break;
            case ColumnKind::Size:
                drawCol(pulse::l10n::Get(pane.this_pc_columns ? pulse::l10n::StringId::ColumnFree
                                                              : pulse::l10n::StringId::ColumnSize),
                        SortColumn::Size, cw, true);
                break;
            case ColumnKind::Total:   // This PC: display-only
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnTotalSize), SortColumn::Name, cw, true, false);
                break;
            case ColumnKind::MediaTaken: case ColumnKind::MediaDims: case ColumnKind::MediaCamera:
            case ColumnKind::MediaTitle: case ColumnKind::MediaArtist: case ColumnKind::MediaAlbum:
            case ColumnKind::MediaDuration: case ColumnKind::MediaFps: {
                const ColumnKind media_kind = columns.kinds[static_cast<size_t>(col)];
                drawCol(MediaColumnLabel(media_kind, pane.media_columns), MediaSortColumn(media_kind), cw,
                        media_kind == ColumnKind::MediaDuration || media_kind == ColumnKind::MediaFps);
                break;
            }
            case ColumnKind::Created:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnCreated), SortColumn::Created, cw, false);
                break;
            case ColumnKind::Accessed:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnAccessed), SortColumn::Accessed, cw, false);
                break;
            case ColumnKind::Status:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnStatus), SortColumn::Name, cw, false, false);
                break;
            }
        };
        for (int col = 0; col < columns.count; ++col) {
            if (col == drag_from) {
                // Its slot stays, emptied: the header floats with the pointer.
                MakeBrush(dc, WithAlpha(theme.text, 0.05f), brFillInput_);
                FillRect(dc, brFillInput_.get(), cx, y, columns.widths[static_cast<size_t>(col)],
                         column_header_height_ - 1.0f);
                cx += columns.widths[static_cast<size_t>(col)];
                continue;
            }
            drawKind(col);
        }
        const bool divider_active =
            (vm.hover_region == static_cast<int>(HitTestResult::ColumnDivider) ||
             vm.column_resize_pressed) && vm.hover_pane_index == pane_index;
        const int active_divider = divider_active
            ? std::clamp(vm.hover_control_index, 0, std::max(0, columns.count - 2))
            : -1;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        for (int divider = 0; divider < columns.count - 1; ++divider) {
            const float dividerX = columns.DividerX(divider);
            FillRect(dc, brStrokeDivider_.get(),
                     dividerX, y + 7.0f * scale_,
                     std::max(1.0f, scale_),
                     column_header_height_ - 14.0f * scale_);
        }
        if (active_divider >= 0) {
            const float dividerX = columns.DividerX(active_divider);
            FillRect(dc, brAccent_.get(), dividerX - scale_, y + 4.0f * scale_,
                     2.0f * scale_, column_header_height_ - 8.0f * scale_);
        }
        // Width badge while a divider is dragged (motion v2 "colresize").
        if (vm.column_resize_pressed && vm.hover_pane_index == pane_index && active_divider >= 0 &&
            !IsHighContrast()) {
            const float x0 = active_divider == 0 ? columns.left : columns.DividerX(active_divider - 1);
            const float wpx = columns.DividerX(active_divider) - x0;
            auto& b = col_badge_;
            if (!b.on || b.pane != pane_index || b.divider != active_divider) {
                b = {};
                b.on = true;
                b.pane = pane_index;
                b.divider = active_divider;
                b.w0 = wpx;
                b.since = motion_now_;
            }
            b.w = wpx;
            b.guide_x = columns.DividerX(active_divider);
            b.guide_top = y + column_header_height_;
            b.guide_bottom = bottom;
            switch (columns.kinds[static_cast<size_t>(active_divider)]) {
            case ColumnKind::Name: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnName); break;
            case ColumnKind::Path: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnPath); break;
            case ColumnKind::Date:
                b.name = pane.date_column_label.empty() ? pulse::l10n::Get(pulse::l10n::StringId::ColumnModified)
                                                        : pane.date_column_label;
                break;
            case ColumnKind::Type:
                b.name = pane.type_column_label.empty() ? pulse::l10n::Get(pulse::l10n::StringId::ColumnType)
                                                        : pane.type_column_label;
                break;
            case ColumnKind::Size: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnSize); break;
            case ColumnKind::Created: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnCreated); break;
            case ColumnKind::Accessed: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnAccessed); break;
            case ColumnKind::Status: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnStatus); break;
            case ColumnKind::Total: b.name = pulse::l10n::Get(pulse::l10n::StringId::ColumnTotalSize); break;
            default: b.name = MediaColumnLabel(columns.kinds[static_cast<size_t>(active_divider)], pane.media_columns); break;
            }
        } else if (col_badge_.on && col_badge_.pane == pane_index) {
            col_badge_.on = false;
            col_badge_.released = motion_now_;
        }
        if (drag_from >= 0) {
            // The floating header, kept inside the header strip, then the
            // line where the column would land.
            const float cw = columns.widths[static_cast<size_t>(drag_from)];
            const float min_left = columns.DividerX(columns.FirstOrderable() - 1);
            const float left = std::clamp(vm.column_drag_cursor_x - vm.column_drag_grab_dx, min_left,
                                          std::max(min_left, columns.right - cw));
            const D2D1_RECT_F plate = D2D1::RectF(left, y + 2.0f * scale_, left + cw,
                                                  y + column_header_height_ - 2.0f * scale_);
            painter_.FillRoundedRect(plate, 4.0f * scale_, theme.surface_flyout);
            painter_.FillRoundedRect(plate, 4.0f * scale_, WithAlpha(theme.accent, 0.10f));
            painter_.StrokeRoundedRect(plate, 4.0f * scale_, WithAlpha(theme.accent, 0.55f), std::max(1.0f, scale_));
            cx = left;
            drawKind(drag_from);
            if (vm.column_drag_slot >= 0) {
                // Over the plate: the line marks the slot even when the plate covers it.
                const float lx = std::round(vm.column_drag_line_x);
                MakeBrush(dc, theme.accent, brAccent_);
                FillRect(dc, brAccent_.get(), lx - scale_, y + 1.0f * scale_, 2.0f * scale_,
                         column_header_height_ - 2.0f * scale_);
            }
        }
        y += column_header_height_;
    }
    float listH = bottom - y;
    dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if ((!pane.loading || pane.is_changes) && pane.EntryCount() == 0)
        DrawPaneEmptyState(vm, pane, D2D1::RectF(x, y, x + w, bottom), pane_index, theme);
    else
        group_hover_pane_ = vm.hover_pane_index;
    // Pulse 助手's overlay: DrawList draws the rows' part; the spark, the line
    // under the column header and the title row's pill come after the clip.
    AiListPainter ai_list(pane.ai_list.get(), compositor_, theme, scale_, motion_now_, pane_index);
    g_ai_list_painter = &ai_list;
    g_ai_list_pane = pane_index;
    DrawList(pane, x, y, w, listH, theme, vm.hover_region, vm.hover_control_index, focused || !split,
             pane_index);
    g_ai_list_painter = nullptr;
    g_ai_list_pane = -1;
    dc->PopAxisAlignedClip();
    if (ai_list.On()) {
        ai_list.Margin(D2D1::RectF(x, y, x + w, bottom));
        ai_list.Watch(detailsRc.left - 10.0f * scale_, y0, pane_header_height_, x + 90.0f * scale_);
        if (ai_list.Animating()) RequestMotionFrame();
    }
    if (!pane.drag_parent_path.empty()) {
        const float top = ShowsColumnHeader(pane.view_mode) ? y - column_header_height_ : pane_rect.top + 5.0f * scale_;
        const auto bar = D2D1::RectF(x + 6.0f * scale_, top, x + w - 6.0f * scale_, top + 30.0f * scale_);
        painter_.FillRoundedRect(bar, 6.0f * scale_, theme.surface_flyout);
        if (pane.drop_parent) painter_.FillRoundedRect(bar, 6.0f * scale_, WithAlpha(theme.accent, 0.12f));
        if (!dashStroke_.get()) {
            ID2D1Factory* factory = nullptr; dc->GetFactory(&factory);
            if (factory) {
                D2D1_STROKE_STYLE_PROPERTIES props{}; props.dashStyle = D2D1_DASH_STYLE_DASH;
                factory->CreateStrokeStyle(props, nullptr, 0, &dashStroke_); factory->Release();
            }
        }
        MakeBrush(dc, pane.drop_parent ? theme.accent : theme.stroke_card, brFillInput_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(bar, 6.0f * scale_, 6.0f * scale_),
            brFillInput_.get(), 1.5f * scale_, dashStroke_.get());
        MakeBrush(dc, theme.accent, brFillInput_);
        const float cx = bar.left + 16.0f * scale_, cy = (bar.top + bar.bottom) * 0.5f;
        dc->DrawLine(D2D1::Point2F(cx, cy + 5 * scale_), D2D1::Point2F(cx, cy - 5 * scale_), brFillInput_.get(), 1.6f * scale_);
        dc->DrawLine(D2D1::Point2F(cx - 4 * scale_, cy - scale_), D2D1::Point2F(cx, cy - 5 * scale_), brFillInput_.get(), 1.6f * scale_);
        dc->DrawLine(D2D1::Point2F(cx + 4 * scale_, cy - scale_), D2D1::Point2F(cx, cy - 5 * scale_), brFillInput_.get(), 1.6f * scale_);
        wchar_t text[1024]{};
        const auto slash = pane.drag_parent_path.find_last_of(L"\\/");
        const std::wstring parent_name = slash != std::wstring::npos && slash + 1 < pane.drag_parent_path.size()
            ? pane.drag_parent_path.substr(slash + 1) : pane.drag_parent_path;
        swprintf_s(text, pulse::l10n::Get(pulse::l10n::StringId::DragDropParentFormat).c_str(), parent_name.c_str());
        const bool show_hint = bar.right - bar.left > 320.0f * scale_;
        const float hint_width = show_hint ? 116.0f * scale_ : 0.0f;
        painter_.DrawText(text, D2D1::RectF(bar.left + 34 * scale_, bar.top, bar.right - 12 * scale_ - hint_width, bar.bottom),
            compositor_->SmallFormat(), pane.drop_parent ? theme.accent : theme.text_secondary, fluent::HorizontalAlignment::Left);
        if (show_hint) painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::DragDropPause),
            D2D1::RectF(bar.right - hint_width, bar.top, bar.right - 12 * scale_, bar.bottom),
            compositor_->SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Right);
        if (pane.drop_parent && pane.drop_parent_progress >= 0) {
            const float progress = std::clamp(pane.drop_parent_progress, 0.0f, 1.0f);
            painter_.FillRoundedRect(D2D1::RectF(bar.left + 4 * scale_, bar.bottom - 4 * scale_,
                bar.left + 4 * scale_ + (bar.right - bar.left - 8 * scale_) * progress, bar.bottom - 2 * scale_), scale_, theme.accent);
            if (progress < 1.0f) RequestMotionFrame();
        }
    }
    if (col_badge_.pane == pane_index) DrawColumnBadge(pane_index, theme);
    if (focused && pane.wall_hint_since)
        DrawPaneWallHint(pane, pane_rect, vm.hover_region, vm.hover_control_index, theme);

    const float radius = theme.radius_control * scale_;
    D2D1_COLOR_F frameColor = CardFrameColor(theme, vm.dark, pane_alpha_ >= 0.0f ? pane_alpha_ : card_alpha_);
    if (focused) frameColor = IsHighContrast() ? theme.accent : WithAlpha(theme.accent, 0.24f);
    else if (target) frameColor = WithAlpha(theme.accent, 0.22f);
    const float strokeW = focused && IsHighContrast() ? 1.5f * scale_ : 1.0f * scale_;
    D2D1_ROUNDED_RECT frame = D2D1::RoundedRect(
        D2D1::RectF(pane_rect.left + 0.5f * scale_, pane_rect.top + 0.5f * scale_,
                    pane_rect.right - 0.5f * scale_, pane_rect.bottom - 0.5f * scale_),
        radius, radius);
    if (split && focused && !IsHighContrast()) {
        const float marker_height = (std::min)(22.0f * scale_, pane_header_height_ - 8.0f * scale_);
        MakeBrush(dc, theme.accent, brAccent_);
        FillRoundedRect(dc, brAccent_.get(), pane_rect.left + 1.5f * scale_,
            pane_rect.top + (pane_header_height_ - marker_height) * 0.5f,
            2.5f * scale_, marker_height, 1.25f * scale_);
    }
    MakeBrush(dc, frameColor, brAccent_);
    if (target && !focused) {
        if (!dashStroke_.get() && dc) {
            ID2D1Factory* factory = nullptr;
            dc->GetFactory(&factory);
            if (factory) {
                D2D1_STROKE_STYLE_PROPERTIES props{};
                props.dashStyle = D2D1_DASH_STYLE_DASH;
                props.dashCap = D2D1_CAP_STYLE_FLAT;
                factory->CreateStrokeStyle(props, nullptr, 0, &dashStroke_);
                factory->Release();
            }
        }
        dc->DrawRoundedRectangle(frame, brAccent_.get(), 1.5f * scale_, dashStroke_.get());
    } else {
        dc->DrawRoundedRectangle(frame, brAccent_.get(), strokeW);
    }

    dc->PopAxisAlignedClip();
}

IDWriteTextFormat* MainRenderer::NameSemiboldFormat() {
    IDWriteTextFormat* base = compositor_ ? compositor_->FileNameFormat() : nullptr;
    IDWriteFactory2* factory = compositor_ ? compositor_->DwriteFactory() : nullptr;
    if (!base || !factory) return nullptr;
    if (name_semibold_format_.get() && name_semibold_base_ == base && name_semibold_size_ == base->GetFontSize())
        return name_semibold_format_.get();
    name_semibold_format_.reset();
    name_semibold_base_ = base;
    name_semibold_size_ = base->GetFontSize();
    wchar_t family[128]{};
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (FAILED(base->GetFontFamilyName(family, ARRAYSIZE(family))) ||
        FAILED(base->GetLocaleName(locale, ARRAYSIZE(locale))))
        return nullptr;
    ComPtr<IDWriteFontCollection> collection;
    base->GetFontCollection(&collection);
    if (FAILED(factory->CreateTextFormat(family, collection.get(), DWRITE_FONT_WEIGHT_SEMI_BOLD, base->GetFontStyle(),
                                         base->GetFontStretch(), base->GetFontSize(), locale, &name_semibold_format_)))
        return nullptr;
    IDWriteTextFormat* bold = name_semibold_format_.get();
    bold->SetParagraphAlignment(base->GetParagraphAlignment());
    bold->SetWordWrapping(base->GetWordWrapping());
    DWRITE_LINE_SPACING_METHOD method{};
    FLOAT spacing = 0.0f, baseline = 0.0f;
    if (SUCCEEDED(base->GetLineSpacing(&method, &spacing, &baseline))) bold->SetLineSpacing(method, spacing, baseline);
    // CJK names keep the same fallback fonts as the regular face.
    ComPtr<IDWriteTextFormat1> base1, bold1;
    if (SUCCEEDED(base->QueryInterface(__uuidof(IDWriteTextFormat1), reinterpret_cast<void**>(&base1))) &&
        SUCCEEDED(bold->QueryInterface(__uuidof(IDWriteTextFormat1), reinterpret_cast<void**>(&bold1)))) {
        ComPtr<IDWriteFontFallback> fallback;
        if (SUCCEEDED(base1->GetFontFallback(&fallback)) && fallback.get()) bold1->SetFontFallback(fallback.get());
    }
    return bold;
}

bool MainRenderer::DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                                     const Theme& theme, bool selected, const std::vector<NameMatchRange>& matches,
                                     bool dim_extension) {
    ++frame_perf_.name_draws;
    (void)selected;
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() ||
        !compositor_->FileNameFormat() || name.empty() || w <= 1.0f) {
        return !name.empty();   // nothing of it shown
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    // 今天修改的名称加粗: the caller measured the row with the same format.
    const bool semibold = name_semibold_ && name_semibold_format_.get();
    IDWriteTextFormat* base_fmt = semibold ? name_semibold_format_.get() : compositor_->FileNameFormat();
    const size_t format_slot = semibold ? 1u : 0u;
    if (name_nowrap_bases_[format_slot] != base_fmt || !name_nowrap_formats_[format_slot].get()) {
        name_nowrap_formats_[format_slot].reset();
        name_nowrap_bases_[format_slot] = base_fmt;
        wchar_t family[128]{};
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        if (SUCCEEDED(base_fmt->GetFontFamilyName(family, ARRAYSIZE(family))) &&
            SUCCEEDED(base_fmt->GetLocaleName(locale, ARRAYSIZE(locale)))) {
            ComPtr<IDWriteFontCollection> collection;
            base_fmt->GetFontCollection(&collection);
            if (SUCCEEDED(factory->CreateTextFormat(family, collection.get(), base_fmt->GetFontWeight(),
                    base_fmt->GetFontStyle(), base_fmt->GetFontStretch(), base_fmt->GetFontSize(), locale,
                    &name_nowrap_formats_[format_slot]))) {
                IDWriteTextFormat* copy = name_nowrap_formats_[format_slot].get();
                copy->SetParagraphAlignment(base_fmt->GetParagraphAlignment());
                copy->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                copy->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                DWRITE_LINE_SPACING_METHOD method{};
                FLOAT spacing = 0.0f, baseline = 0.0f;
                if (SUCCEEDED(base_fmt->GetLineSpacing(&method, &spacing, &baseline)))
                    copy->SetLineSpacing(method, spacing, baseline);
                ComPtr<IDWriteTextFormat1> source1, copy1;
                if (SUCCEEDED(base_fmt->QueryInterface(__uuidof(IDWriteTextFormat1),
                        reinterpret_cast<void**>(&source1))) &&
                    SUCCEEDED(copy->QueryInterface(__uuidof(IDWriteTextFormat1),
                        reinterpret_cast<void**>(&copy1)))) {
                    ComPtr<IDWriteFontFallback> fallback;
                    if (SUCCEEDED(source1->GetFontFallback(&fallback)) && fallback.get())
                        copy1->SetFontFallback(fallback.get());
                }
            }
        }
    }
    IDWriteTextFormat* fmt = name_nowrap_formats_[format_slot].get()
        ? name_nowrap_formats_[format_slot].get() : base_fmt;
    MakeBrush(dc, theme.text, brText_);

    const auto name_fit_start = std::chrono::steady_clock::now();
    const std::wstring shown = FitHighlightedFileName(compositor_, factory, fmt, name, w, matches, scale_);
    const auto name_draw_start = std::chrono::steady_clock::now();
    frame_perf_.name_fit_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(name_draw_start - name_fit_start).count());
    const D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    const auto visible_matches = VisibleNameMatchRanges(name, shown, matches);
    // The extension is drawn a step dimmer so the distinguishing stem reads first.
    size_t ext_at = std::wstring::npos;
    if (dim_extension && !IsHighContrast()) {
        const size_t dot = shown.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0 && shown.size() - dot <= 8 &&
            shown.find(L'\u2026', dot) == std::wstring::npos)
            ext_at = dot;
    }
    const D2D1_COLOR_F ext_color = WithAlpha(theme.text, (theme.bg.r < 0.5f) ? 0.58f : 0.62f);
    // Shape colored filenames in one layout. Splitting at the extension gives
    // each substring its own clipping/ellipsis and loses glyph positioning
    // across the boundary (including the dot's ink and the stem's last glyph).
    // LumaText locates the extension with that layout's caret before drawing it.
    bool drawn = false;
    if (visible_matches.empty() && !IsHighContrast()) {
        drawn = ext_at != std::wstring::npos
            ? compositor_->DrawLumaTextSplit(shown, fmt, rc, brText_->GetColor(),
                  static_cast<std::uint32_t>(ext_at), ext_color, theme.bg)
            : compositor_->DrawLumaText(shown, fmt, rc, brText_->GetColor(), theme.bg,
                  DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    ComPtr<IDWriteTextLayout> highlighted;
    if (!drawn && (!visible_matches.empty() || ext_at != std::wstring::npos) &&
        SUCCEEDED(factory->CreateTextLayout(shown.c_str(),
        static_cast<UINT32>(shown.size()), fmt, w, h, &highlighted))) {
        highlighted->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        highlighted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        highlighted->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<ID2D1SolidColorBrush> ext_brush;
        if (ext_at != std::wstring::npos && SUCCEEDED(dc->CreateSolidColorBrush(ext_color, &ext_brush)))
            highlighted->SetDrawingEffect(ext_brush.get(), {static_cast<UINT32>(ext_at),
                                                            static_cast<UINT32>(shown.size() - ext_at)});
        // Baseline on a whole pixel row, as the LumaText names beside it.
        float y_snap = y;
        DWRITE_TEXT_METRICS text_metrics{};
        DWRITE_LINE_METRICS line_metrics{};
        UINT32 line_count = 0;
        if (SUCCEEDED(highlighted->GetMetrics(&text_metrics)) &&
            SUCCEEDED(highlighted->GetLineMetrics(&line_metrics, 1, &line_count)) && line_count > 0) {
            const float baseline = y + text_metrics.top + line_metrics.baseline;
            y_snap += std::round(baseline) - baseline;
        }
        DrawNameHighlightBackground(compositor_, highlighted.get(), {x, y_snap}, rc, visible_matches, theme, scale_);
        compositor_->DrawTextLayout({x, y_snap}, highlighted.get(), brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else if (!drawn) {
        dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    frame_perf_.name_draw_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - name_draw_start).count());
    return shown != name;
}

namespace {
struct IconNameText {
    std::wstring shown;                    // lines joined by U+2028
    std::vector<NameMatchRange> matches;   // search hits, in `shown` positions
    bool truncated = false;
};

using NameLine = FileNameLine;
// Same break rules as the wrapped rename field (file_name_breaks.h).
// Breaks are measured with plain DirectWrite advances, as the rename field
// measures them, so a name keeps its line breaks when editing starts.
std::vector<NameLine> WrapIconName(IDWriteFactory2* factory, IDWriteTextFormat* fmt,
                                   const std::wstring& s, float width, size_t limit) {
    return WrapFileName([&](const std::wstring& t) { return typography::MeasureAdvance(factory, fmt, t); },
                        s, width, limit);
}

// The part of a shown icon name that is the file's extension: the tail of
// `shown` when it ends with it (a cut name keeps its extension too).
DWRITE_TEXT_RANGE ShownExtension(const std::wstring& name, const std::wstring& shown) {
    const size_t dot = FileNameExtensionDot(name);
    if (dot == std::wstring::npos) return {0, 0};
    const size_t n = name.size() - dot;
    if (shown.size() < n || shown.compare(shown.size() - n, n, name, dot, n) != 0) return {0, 0};
    return {static_cast<UINT32>(shown.size() - n), static_cast<UINT32>(n)};
}

struct IconNameLayout {
    std::wstring shown;
    struct Piece { size_t shown, source, length; };
    std::vector<Piece> pieces;   // runs of `shown` copied from the name
    bool truncated = false;
};

IconNameLayout LayoutIconName(Compositor* compositor, IDWriteFactory2* factory, IDWriteTextFormat* fmt,
                              const std::wstring& name, float width, int max_lines) {
    IconNameLayout out;
    const auto measure = [&](const std::wstring& s) {
        return MeasureLayoutText(compositor, factory, fmt, s);
    };
    const float budget = std::max(1.0f, width - 1.0f);
    const size_t limit = static_cast<size_t>(std::max(1, max_lines));
    const auto lines = WrapIconName(factory, fmt, name, budget, limit);
    const auto add = [&](size_t source, size_t length) {
        out.pieces.push_back({out.shown.size(), source, length});
        out.shown.append(name, source, length);
    };
    const auto trimmed = [&](const NameLine& line) {
        size_t n = line.length;
        while (n > 0 && name[line.start + n - 1] == L' ') --n;
        return n;
    };
    const size_t kept = std::min(lines.size(), lines.size() <= limit ? limit : limit - 1);
    for (size_t i = 0; i < kept; ++i) {
        if (i) out.shown.push_back(L'\u2028');
        add(lines[i].start, trimmed(lines[i]));
    }
    if (lines.size() <= limit) return out;
    // Last line: "head…tail", the tail being the extension and up to six
    // characters before it - where versions, sizes and dates usually are.
    out.truncated = true;
    const size_t rest_at = lines[limit - 1].start;
    const std::wstring rest = name.substr(rest_at);
    size_t ext = 0;
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > rest_at && name.size() - dot <= 9) ext = name.size() - dot;
    const std::wstring ellipsis(1, L'\u2026');
    size_t tail_n = std::min(rest.size() - 1, ext + 6);
    while (tail_n > ext && measure(ellipsis + rest.substr(rest.size() - tail_n)) > budget * 0.6f) --tail_n;
    while (tail_n > 0 && measure(ellipsis + rest.substr(rest.size() - tail_n)) > budget) --tail_n;
    if (tail_n > 0 && IS_LOW_SURROGATE(rest[rest.size() - tail_n])) --tail_n;
    const std::wstring tail = rest.substr(rest.size() - tail_n);
    size_t lo = 0, hi = rest.size() - tail_n;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(rest.substr(0, mid) + ellipsis + tail) <= budget) lo = mid;
        else hi = mid - 1;
    }
    if (lo > 0 && IS_HIGH_SURROGATE(rest[lo - 1])) --lo;
    if (kept) out.shown.push_back(L'\u2028');
    add(rest_at, lo);
    out.shown += ellipsis;
    add(name.size() - tail_n, tail_n);
    return out;
}

// Wraps an icon-grid name to at most `max_lines` lines; a longer name keeps
// its first lines and ends the last one with a middle ellipsis:
// "ph_kloofendal_48d_" / "partly…y_16k.hdr". Layouts are cached per name and
// width (the list redraws every frame while it scrolls or animates).
IconNameText FitIconName(Compositor* compositor, IDWriteFactory2* factory, IDWriteTextFormat* fmt,
                         const std::wstring& name, float width, int max_lines,
                         const std::vector<NameMatchRange>& matches) {
    IconNameText out{name, matches, false};
    if (!factory || !fmt || name.empty()) return out;
    using NameLru = std::list<const std::wstring*>;
    struct CachedName { IconNameLayout layout; NameLru::iterator position; size_t bytes; };
    static thread_local NameLru lru;
    static thread_local std::unordered_map<std::wstring, CachedName> cache;
    static thread_local size_t cache_bytes = 0;
    static thread_local uint64_t cache_generation = 0;
    if (cache_generation != typography::Generation()) {
        lru.clear(); cache.clear(); cache_bytes = 0; cache_generation = typography::Generation();
    }
    std::wstring key = name;
    key += L'\x1F';
    key += std::to_wstring(std::lround(width * 4.0f)) + L':' + std::to_wstring(max_lines) + L':' +
           std::to_wstring(std::lround(fmt->GetFontSize() * 100.0f)) + L':';
    wchar_t family[64]{};
    if (SUCCEEDED(fmt->GetFontFamilyName(family, ARRAYSIZE(family)))) key += family;
    key += L':' + std::to_wstring(reinterpret_cast<uintptr_t>(fmt));
    auto it = cache.find(key);
    IconNameLayout uncached;
    if (it == cache.end()) {
        uncached = LayoutIconName(compositor, factory, fmt, name, width, max_lines);
        constexpr size_t budget = 2 * 1024 * 1024;
        const size_t bytes = (key.size() + uncached.shown.size()) * sizeof(wchar_t) +
            uncached.pieces.size() * sizeof(uncached.pieces[0]);
        if (bytes <= budget) {
            while (!cache.empty() && (cache.size() >= 4096 || cache_bytes + bytes > budget)) {
                auto oldest = cache.find(*lru.front());
                lru.pop_front();
                cache_bytes -= oldest->second.bytes;
                cache.erase(oldest);
            }
            it = cache.emplace(std::move(key), CachedName{std::move(uncached), {}, bytes}).first;
            lru.push_back(&it->first);
            it->second.position = std::prev(lru.end());
            cache_bytes += bytes;
        }
    } else lru.splice(lru.end(), lru, it->second.position);
    const IconNameLayout& layout = it == cache.end() ? uncached : it->second.layout;
    out.shown = layout.shown;
    out.truncated = layout.truncated;
    out.matches.clear();
    for (const auto& match : matches) {
        for (const auto& piece : layout.pieces) {
            const size_t a = std::max<size_t>(match.start, piece.source);
            const size_t b = std::min<size_t>(match.start + match.length, piece.source + piece.length);
            if (a < b)
                out.matches.push_back({static_cast<UINT32>(piece.shown + a - piece.source),
                                       static_cast<UINT32>(b - a)});
        }
    }
    return out;
}

// Name lines for a folder's icon grids (AutoIconNameLines), measured on up to
// 128 names spread over the folder at the tier's widest cell, so resizing or
// filtering does not change them. Cached per snapshot; search results and
// other entry lists keep the default.
int FolderIconNameLines(Compositor* compositor, const PaneViewModel& vm, float scale) {
    IDWriteTextFormat* fmt = compositor ? compositor->FileNameFormat() : nullptr;
    if (!fmt || !compositor->DwriteFactory() || !vm.snapshot || vm.snapshot->empty() ||
        vm.content_results || !IsZoomIconMode(vm.view_mode))
        return kIconGridNameLines;
    const float width = IconGridNameWidth(vm.view_mode, scale, vm.icon_dip);
    const float font = fmt->GetFontSize();
    struct Cached {
        std::weak_ptr<const std::vector<fs::DirEntry>> snapshot;
        float width, font;
        int lines;
    };
    static std::vector<Cached> cache;
    const auto same = [&](const Cached& c) {
        return !c.snapshot.expired() && !c.snapshot.owner_before(vm.snapshot) &&
               !vm.snapshot.owner_before(c.snapshot) && c.width == width && c.font == font;
    };
    for (const Cached& c : cache) if (same(c)) return c.lines;
    const float budget = std::max(1.0f, width - 1.0f);  // as LayoutIconName
    const size_t total = vm.snapshot->size();
    constexpr size_t kSamples = 128;
    const size_t step = std::max<size_t>(1, total / kSamples);
    std::vector<int> needed;
    needed.reserve(kSamples);
    for (size_t i = 0; i < total && needed.size() < kSamples; i += step) {
        const ListEntryView& e = MakeVisibleEntry(vm, i);
        const std::wstring name = e.alias.empty() ? ShownName(e) : e.alias;
        if (name.empty()) continue;
        needed.push_back(static_cast<int>(WrapIconName(compositor->DwriteFactory(), fmt, name, budget,
            static_cast<size_t>(kIconGridMaxNameLines)).size()));
    }
    const bool has_folders = std::any_of(vm.snapshot->begin(), vm.snapshot->end(),
                                         [](const fs::DirEntry& d) { return d.is_dir; });
    const int lines = AutoIconNameLines(needed, has_folders);
    std::erase_if(cache, [](const Cached& c) { return c.snapshot.expired(); });
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back({vm.snapshot, width, font, lines});
    return lines;
}

// Pictures and videos: tiles in their own aspect on the photo wall.
bool WallPicture(const std::wstring& name, bool is_dir) { return IsWallPicture(name, is_dir); }

D2D1_SIZE_F IconGridNameSize(Compositor* compositor, const std::wstring& name, float width, int max_lines) {
    IDWriteFactory2* factory = compositor ? compositor->DwriteFactory() : nullptr;
    if (!factory || !compositor->FileNameFormat() || name.empty()) return D2D1::SizeF(0.0f, 0.0f);
    const IconNameText fit = FitIconName(compositor, factory, compositor->FileNameFormat(), name, width,
                                         std::max(1, max_lines), {});
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(fit.shown.c_str(), static_cast<UINT32>(fit.shown.size()),
                                         compositor->FileNameFormat(), width, 4096.0f, &layout)) || !layout.get())
        return D2D1::SizeF(width, 0.0f);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    layout->SetTrimming(&untrimmed, nullptr);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return D2D1::SizeF(width, 0.0f);
    return D2D1::SizeF(metrics.width, metrics.height);
}

// Per-window and per-tab immutable rows. Source preparation and reflow run on
// one cancellable worker; UI submission and unchanged cache lookup are bounded.
struct WallRowsCache {
    std::weak_ptr<const std::vector<fs::DirEntry>> snapshot;
    std::weak_ptr<const PaneViewModel::FilterMap> order;
    bool ordered;
    size_t count;
    float width, row_px, scale;
    uint64_t aspect_version = 0;
    std::shared_ptr<WallLayoutSource> source;
    std::shared_ptr<const WallRows> rows;  // stable while layouts point at it
    std::shared_ptr<const WallRows> prev;  // before the last aspect change
    ULONGLONG changed_at = 0;
    std::shared_ptr<WallLayoutJob> job = std::make_shared<WallLayoutJob>();
    HWND owner = nullptr;
    uintptr_t view_identity = 0;
    uint64_t demand_owner = 0;
    int demand_first = -1, demand_last = -1;
};
std::vector<WallRowsCache>& WallRowsCaches() {
    static std::vector<WallRowsCache> cache;
    return cache;
}
constexpr ULONGLONG kWallGlideMs = 280;

// Rows a cache entry let go of. Layouts hold plain WallRows pointers for the
// length of one paint or hit test, and the aspects can move on between two
// PaneWallRows calls of the same frame (the probe runs in the background), so
// replaced rows are freed a while later instead of under a live layout.
constexpr ULONGLONG kWallRetireMs = 2000;
void RetireWallRows(std::shared_ptr<const WallRows> rows) {
    static std::vector<std::pair<ULONGLONG, std::shared_ptr<const WallRows>>> retired;
    const ULONGLONG now = GetTickCount64();
    std::erase_if(retired, [now](const auto& r) { return now - r.first >= kWallRetireMs; });
    if (rows) retired.emplace_back(now, std::move(rows));
}
void RetireWallEntry(WallRowsCache& c) {
    c.job->Cancel();
    if (c.demand_owner) WallAspects::Instance().RemoveDemand(c.demand_owner);
    RetireWallRows(std::move(c.rows));
    RetireWallRows(std::move(c.prev));
}

void UpdateWallDemand(WallRowsCache& c, const PaneViewModel& vm, float height) {
    int first = 0, last = std::min(128, static_cast<int>(c.count));
    if (c.rows && c.rows->RowCount()) {
        const int first_row = std::max(0, c.rows->RowAtY(vm.scroll_y));
        const int last_row = std::max(first_row, c.rows->RowAtY(vm.scroll_y + height));
        first = c.rows->row_first[static_cast<size_t>(first_row)];
        last = c.rows->row_first[std::min(static_cast<size_t>(last_row + 1), c.rows->RowCount())];
    }
    if (first == c.demand_first && last == c.demand_last) return;
    c.demand_first = first;
    c.demand_last = last;
    std::vector<std::pair<uint64_t, std::wstring>> files;
    const auto add = [&](int row) {
        if (row < 0 || row >= static_cast<int>(c.count) || files.size() >= 512) return;
        const int src = vm.SourceIndex(row);
        if (src < 0 || static_cast<size_t>(src) >= vm.snapshot->size()) return;
        const auto& entry = (*vm.snapshot)[static_cast<size_t>(src)];
        if (!WallPicture(entry.name, entry.is_dir)) return;
        auto path = WallLayoutSource::Path(entry, vm.path, fs::IsVirtualPath(vm.path));
        if (path.empty()) return;
        const uint64_t modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) | entry.mtime.dwLowDateTime;
        files.emplace_back(WallAspects::ContentHash(path, modified, entry.size), std::move(path));
    };
    for (int row = first; row < last && row < first + 384; ++row) add(row);
    for (int offset = 0; offset < 64; ++offset) { add(last + offset); add(first - offset - 1); }
    WallAspects::Instance().ReplaceDemand(c.demand_owner, c.owner, std::move(files));
}
const WallRows* PaneWallRows(const PaneViewModel& vm, float width, float scale, HWND owner) {
    if (!vm.wall || !IsZoomIconMode(vm.view_mode) || !vm.snapshot || vm.content_results) return nullptr;
    const size_t count = vm.EntryCount();
    if (count == 0) return nullptr;
    const float row_px = 0.78f * IconDip(vm.view_mode, vm.icon_dip, scale) * scale;
    auto& cache = WallRowsCaches();
    const auto same = [](const auto& weak, const auto& strong) {
        return !weak.expired() && !weak.owner_before(strong) && !strong.owner_before(weak);
    };
    const bool ordered = vm.filter_map != nullptr;
    std::erase_if(cache, [&](WallRowsCache& c) {
        const bool obsolete = c.owner == owner && c.view_identity == vm.wall_view_identity &&
            (!same(c.snapshot, vm.snapshot) || c.ordered != ordered || (ordered && !same(c.order, vm.filter_map)));
        if (obsolete) RetireWallEntry(c);
        return obsolete;
    });
    for (WallRowsCache& c : cache) {
        if (c.owner == owner && c.view_identity == vm.wall_view_identity && same(c.snapshot, vm.snapshot) && c.ordered == ordered && (!ordered || same(c.order, vm.filter_map)) &&
            c.count == count) {
            const bool geometry_changed = c.width != width || c.row_px != row_px || c.scale != scale;
            if (geometry_changed) {
                c.job->Cancel();
                c.width = width;
                c.row_px = row_px;
                c.scale = scale;
            }
            const uint64_t version = WallAspects::Instance().Version();
            const bool aspects_changed = c.aspect_version != version && !c.job->Busy();
            if (aspects_changed || geometry_changed) c.aspect_version = version;
            if (!c.rows) if (auto ready = c.job->TakeReady()) {
                RetireWallRows(std::move(c.prev));
                RetireWallRows(std::move(c.rows));
                c.rows = std::move(ready);
                c.changed_at = GetTickCount64();
            }
            if (aspects_changed || geometry_changed)
                c.job->RequestPrepared([source = c.source, geometry_changed](const auto& generation, uint64_t expected) {
                    return source->Prepare(generation, expected, geometry_changed);
                }, width, row_px, scale, [source = c.source] { source->Committed(); }, true);
            // Poll through the existing paint path; no worker touches a window handle.
            if (owner && (c.job->Busy() || c.aspect_version != WallAspects::Instance().Version())) SetTimer(owner, 0x574C, 16,
                [](HWND hwnd, UINT, UINT_PTR id, DWORD) { KillTimer(hwnd, id); InvalidateRect(hwnd, nullptr, FALSE); });
            return c.rows.get();
        }
    }
    for (WallRowsCache& c : cache)
        if (c.snapshot.expired()) RetireWallEntry(c);
    std::erase_if(cache, [](const WallRowsCache& c) { return c.snapshot.expired(); });
    if (cache.size() >= 16) {
        RetireWallEntry(cache.front());
        cache.erase(cache.begin());
    }
    RetireWallRows(nullptr);  // frees what has waited long enough
    WallRowsCache c{};
    c.snapshot = vm.snapshot; c.order = vm.filter_map; c.ordered = ordered; c.count = count;
    c.width = width; c.row_px = row_px; c.scale = scale;
    static uint64_t next_demand_owner = 0;
    c.demand_owner = ++next_demand_owner;
    c.owner = owner;
    c.view_identity = vm.wall_view_identity;
    c.aspect_version = WallAspects::Instance().Version();
    c.source = WallLayoutSource::Create(vm.snapshot, vm.filter_map, vm.path, fs::IsVirtualPath(vm.path));
    c.job->RequestPrepared([source = c.source](const auto& generation, uint64_t expected) {
        return source->Prepare(generation, expected, true);
    }, width, row_px, scale, [source = c.source] { source->Committed(); }, true);
    if (owner && (c.job->Busy() || c.aspect_version != WallAspects::Instance().Version())) SetTimer(owner, 0x574C, 16,
                [](HWND hwnd, UINT, UINT_PTR id, DWORD) { KillTimer(hwnd, id); InvalidateRect(hwnd, nullptr, FALSE); });
    cache.push_back(std::move(c));
    return cache.back().rows.get();
}

// The rows these replaced on an aspect change and how far the glide is
// (eased, 1 = done, prev null).
float WallGlide(const WallRows* rows, const WallRows** prev) {
    *prev = nullptr;
    if (!rows) return 1.0f;
    for (const WallRowsCache& c : WallRowsCaches()) {
        if (c.rows.get() != rows || !c.prev) continue;
        const ULONGLONG age = GetTickCount64() - c.changed_at;
        if (age >= kWallGlideMs || c.prev->x.size() != rows->x.size()) return 1.0f;
        *prev = c.prev.get();
        const float t = static_cast<float>(age) / static_cast<float>(kWallGlideMs);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    }
    return 1.0f;
}
} // namespace

void MainRenderer::PruneWallViews(const std::vector<PaneSlotView>& panes) const {
    std::erase_if(WallRowsCaches(), [&](WallRowsCache& c) {
        if (c.owner != notify_hwnd_) return false;
        const bool visible = std::any_of(panes.begin(), panes.end(), [&](const auto& pane) {
            return pane.pane.wall && pane.pane.wall_view_identity == c.view_identity;
        });
        if (visible) return false;
        RetireWallEntry(c);
        return true;
    });
}
MainRenderer::~MainRenderer() {
    std::erase_if(WallRowsCaches(), [&](WallRowsCache& c) {
        if (c.owner != notify_hwnd_) return false;
        RetireWallEntry(c);
        return true;
    });
}
void MainRenderer::ReconcileWallAnchor(PaneViewModel& vm, const D2D1_RECT_F& bounds) const {
    if (!vm.wall || !vm.snapshot) {
        std::erase_if(WallRowsCaches(), [&](WallRowsCache& c) {
            if (c.owner != notify_hwnd_ || c.view_identity != vm.wall_view_identity) return false;
            RetireWallEntry(c);
            return true;
        });
        return;
    }
    const auto list = PaneListRect(vm, bounds);
    PaneWallRows(vm, std::max(1.0f, list.right - list.left), scale_, notify_hwnd_);
    for (auto& c : WallRowsCaches()) {
        if (c.owner != notify_hwnd_ || c.view_identity != vm.wall_view_identity ||
            c.snapshot.lock() != vm.snapshot || c.ordered != bool(vm.filter_map) ||
            (c.ordered && c.order.lock() != vm.filter_map) || c.count != vm.EntryCount()) continue;
        UpdateWallDemand(c, vm, list.bottom - list.top);
        auto ready = c.job->TakeReady();
        if (!ready) return;
        // Snapshot and ordering are identical: the view index identifies the
        // same immutable source entry, including while aspect rows reflow.
        if (c.rows) vm.scroll_y = WallAnchorScroll(*c.rows, *ready, vm.scroll_y);
        RetireWallRows(std::move(c.prev));
        RetireWallRows(std::move(c.rows));
        c.rows = std::move(ready);
        return;
    }
}
MainRenderer::WallHintRects MainRenderer::PaneWallHintRects(const PaneViewModel& vm,
                                                            const D2D1_RECT_F& pane_bounds) const {
    WallHintRects r;
    if (!vm.wall_hint_since || !compositor_) return r;
    const uint64_t now = GetTickCount64();
    const auto progress = [now](uint64_t from) {
        return now <= from ? 0.0f : std::min(1.0f, static_cast<float>(now - from) / 200.0f);
    };
    const float shown = progress(vm.wall_hint_since);
    const float gone = vm.wall_hint_until ? progress(vm.wall_hint_until) : 0.0f;
    r.animating = vm.wall_hint_until ? gone < 1.0f : shown < 1.0f;
    r.alpha = std::min(shown, 1.0f - gone);
    IDWriteTextFormat* fmt = compositor_->SmallFormat();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    if (!fmt || !factory) { r.alpha = 0.0f; return r; }
    const float s = scale_;
    // Prototype .sugg: padding 7 8 7 12, gap 10, a 16 px icon, the message,
    // an accent button (padding 4 12) and the close glyph.
    const float textW = MeasureTextWidth(factory, fmt, l10n::Get(l10n::StringId::PhotoWallSuggest)) * 1.03f + 2.0f * s;
    const float btnW = MeasureTextWidth(factory, fmt, l10n::Get(l10n::StringId::PhotoWallSuggestSwitch)) + 24.0f * s;
    const float btnH = 24.0f * s, closeW = 22.0f * s, gap = 10.0f * s;
    const float chrome = 12.0f * s + 16.0f * s + gap + gap + btnW + 6.0f * s + closeW + 6.0f * s;
    const float maxW = pane_bounds.right - pane_bounds.left - 24.0f * s;
    if (maxW < chrome + 48.0f * s) { r.alpha = 0.0f; r.animating = false; return r; }
    const float cardW = std::round(std::min(chrome + textW, maxW));
    const float cardH = std::round(38.0f * s);
    const float eased = 1.0f - (1.0f - r.alpha) * (1.0f - r.alpha) * (1.0f - r.alpha);
    const float top = std::round(pane_bounds.top + pane_header_height_ + 6.0f * s - (1.0f - eased) * 8.0f * s);
    const float left = std::round((pane_bounds.left + pane_bounds.right - cardW) * 0.5f);
    r.card = D2D1::RectF(left, top, left + cardW, top + cardH);
    const float cy = top + cardH * 0.5f;
    r.close = D2D1::RectF(r.card.right - 6.0f * s - closeW, std::round(cy - closeW * 0.5f),
                          r.card.right - 6.0f * s, std::round(cy + closeW * 0.5f));
    r.button = D2D1::RectF(r.close.left - 6.0f * s - btnW, std::round(cy - btnH * 0.5f),
                           r.close.left - 6.0f * s, std::round(cy + btnH * 0.5f));
    r.text = D2D1::RectF(left + 12.0f * s + 16.0f * s + gap, top, r.button.left - gap, r.card.bottom);
    return r;
}

void MainRenderer::DrawPaneWallHint(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                    int hover_region, int hover_index, const Theme& theme) {
    const WallHintRects r = PaneWallHintRects(vm, pane_bounds);
    if (r.animating) RequestMotionFrame();
    if (r.alpha <= 0.01f) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->SmallFormat();
    if (!dc || !factory || !fmt) return;
    const float s = scale_, a = r.alpha;
    const bool hc = IsHighContrast();
    const auto faded = [a](D2D1_COLOR_F c) { c.a *= a; return c; };
    const float radius = 8.0f * s;
    // box-shadow 0 8px 24px rgba(0,0,0,.35): a few soft layers below the card.
    if (!hc) {
        for (int i = 4; i >= 1; --i) {
            const float grow = static_cast<float>(i) * 3.0f * s;
            MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f * a), brFillInput_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.card.left - grow, r.card.top - grow + 6.0f * s,
                r.card.right + grow, r.card.bottom + grow + 6.0f * s), radius + grow, radius + grow), brFillInput_.get());
        }
    }
    D2D1_COLOR_F surface = hc ? theme.bg : theme.surface_flyout;
    surface.a = a;  // opaque: the list must not show through the words
    MakeBrush(dc, surface, brFillInput_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.card, radius, radius), brFillInput_.get());
    MakeBrush(dc, faded(hc ? theme.text : theme.stroke_card), brFillInput_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.card.left + 0.5f, r.card.top + 0.5f,
        r.card.right - 0.5f, r.card.bottom - 0.5f), radius, radius), brFillInput_.get(), 1.0f);
    // Icon, then the message with 照片墙 bold in the accent colour.
    const float iconLeft = r.card.left + 12.0f * s;
    const float cy = (r.card.top + r.card.bottom) * 0.5f;
    DrawPaneHeaderIcon(D2D1::RectF(iconLeft - 1.0f * s, cy - 9.0f * s, iconLeft + 17.0f * s, cy + 9.0f * s),
                       PaneHeaderIcon::Wall, faded(theme.accent));
    const std::wstring& message = l10n::Get(l10n::StringId::PhotoWallSuggest);
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(factory->CreateTextLayout(message.c_str(), static_cast<UINT32>(message.size()), fmt,
            std::max(1.0f, r.text.right - r.text.left), r.text.bottom - r.text.top, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(fmt, &ellipsis))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis.get());
        }
        ComPtr<ID2D1SolidColorBrush> accent;
        const std::wstring& bold = l10n::Get(l10n::StringId::PhotoWall);
        const size_t at = bold.empty() ? std::wstring::npos : message.find(bold);
        if (at != std::wstring::npos) {
            const DWRITE_TEXT_RANGE range{static_cast<UINT32>(at), static_cast<UINT32>(bold.size())};
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
            if (!hc && SUCCEEDED(dc->CreateSolidColorBrush(faded(theme.accent), &accent)))
                layout->SetDrawingEffect(accent.get(), range);
        }
        MakeBrush(dc, faded(theme.text), brText_);
        compositor_->DrawTextLayout(D2D1::Point2F(r.text.left, r.text.top), layout.get(), brText_.get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    // 切换: the accent button.
    const bool overCard = hover_region == static_cast<int>(HitTestResult::PaneWallHint);
    const bool overButton = overCard && hover_index == 1;
    const bool overClose = overCard && hover_index == 2;
    MakeBrush(dc, faded(hc ? theme.text : overButton ? theme.accent_hover : theme.accent), brFillInput_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r.button, 5.0f * s, 5.0f * s), brFillInput_.get());
    const std::wstring& action = l10n::Get(l10n::StringId::PhotoWallSuggestSwitch);
    ComPtr<IDWriteTextLayout> label;
    if (SUCCEEDED(factory->CreateTextLayout(action.c_str(), static_cast<UINT32>(action.size()), fmt,
            r.button.right - r.button.left, r.button.bottom - r.button.top, &label))) {
        label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        label->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        MakeBrush(dc, faded(hc ? theme.bg : D2D1::ColorF(1.0f, 1.0f, 1.0f)), brText_);
        compositor_->DrawTextLayout(D2D1::Point2F(r.button.left, r.button.top), label.get(), brText_.get());
    }
    // ✕: don't show again.
    if (overClose) {
        MakeBrush(dc, faded(theme.fill_hover), brFillInput_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r.close, 4.0f * s, 4.0f * s), brFillInput_.get());
    }
    const float k = 4.0f * s;
    const float ccx = (r.close.left + r.close.right) * 0.5f, ccy = (r.close.top + r.close.bottom) * 0.5f;
    MakeBrush(dc, faded(overClose ? theme.text : theme.text_secondary), brFillInput_);
    dc->DrawLine(D2D1::Point2F(ccx - k, ccy - k), D2D1::Point2F(ccx + k, ccy + k), brFillInput_.get(), 1.2f * s);
    dc->DrawLine(D2D1::Point2F(ccx + k, ccy - k), D2D1::Point2F(ccx - k, ccy + k), brFillInput_.get(), 1.2f * s);
}

void MainRenderer::DrawWallTileChrome(const D2D1_RECT_F& tile, bool hover, bool selected, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || tile.right - tile.left < 2.0f || tile.bottom - tile.top < 2.0f) return;
    const bool hc = IsHighContrast();
    const float strip = std::min(tile.bottom - tile.top, 36.0f * scale_);
    if (hc) {
        // Names stay readable on a plain strip of the window colour.
        MakeBrush(dc, theme.bg, brFillInput_);
        dc->FillRectangle(D2D1::RectF(tile.left, tile.bottom - std::min(strip, 26.0f * scale_), tile.right, tile.bottom),
                          brFillInput_.get());
    } else {
        const bool dark = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
        MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, dark ? 0.25f : 0.12f), brFillInput_);
        dc->DrawRectangle(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f, tile.right - 0.5f, tile.bottom - 0.5f),
                          brFillInput_.get(), 1.0f);
        if (hover || selected) {
            const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
                                                {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)}};
            ComPtr<ID2D1Device> device;
            dc->GetDevice(&device);
            if (device.get() != wall_gradient_device_.get()) {
                wall_gradient_.reset();
                wall_gradient_device_ = std::move(device);
            }
            if (!wall_gradient_.get()) {
                ComPtr<ID2D1GradientStopCollection> collection;
                if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)))
                    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(tile.left, tile.bottom - strip), D2D1::Point2F(tile.left, tile.bottom)),
                        collection.get(), &wall_gradient_);
            }
            if (wall_gradient_.get()) {
                wall_gradient_->SetStartPoint(D2D1::Point2F(tile.left, tile.bottom - strip));
                wall_gradient_->SetEndPoint(D2D1::Point2F(tile.left, tile.bottom));
                dc->FillRectangle(D2D1::RectF(tile.left, tile.bottom - strip, tile.right, tile.bottom), wall_gradient_.get());
            }
        }
    }
    if (selected) {
        const float t = 2.0f * scale_;
        MakeBrush(dc, hc ? theme.text : theme.accent, brFillInput_);
        dc->DrawRectangle(D2D1::RectF(tile.left + t * 0.5f, tile.top + t * 0.5f, tile.right - t * 0.5f, tile.bottom - t * 0.5f),
                          brFillInput_.get(), t);
    }
}

// Extension in the secondary ink, as the details view shows it.
void MainRenderer::DimNameExtension(IDWriteTextLayout* layout, const std::wstring& name,
                                    const std::wstring& shown, const D2D1_COLOR_F& color) {
    const DWRITE_TEXT_RANGE ext = ShownExtension(name, shown);
    if (!layout || ext.length == 0 || IsHighContrast() || !compositor_ || !compositor_->Dc()) return;
    MakeBrush(compositor_->Dc(), D2D1::ColorF(color.r, color.g, color.b, color.a * 0.6f), brNameExt_);
    if (brNameExt_.get()) layout->SetDrawingEffect(brNameExt_.get(), ext);
}

bool MainRenderer::DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color, const Theme& theme, const std::vector<NameMatchRange>& matches,
                                        bool* truncated, int max_lines, float* text_height,
                                        bool dim_extension) {
    if (truncated) *truncated = false;
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() || name.empty()) return false;
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    const IconNameText fit = FitIconName(compositor_, compositor_->DwriteFactory(),
        compositor_->FileNameFormat(), name, width, std::max(1, max_lines), matches);
    if (truncated) *truncated = fit.truncated;
    ComPtr<IDWriteTextLayout> layout;
    float measured_height = height;
    const auto generation = typography::Generation();
    if (icon_final_generation_ != generation) {
        icon_final_layouts_.clear();
        icon_final_text_bytes_ = 0;
        icon_final_generation_ = generation;
    }
    for (auto it = icon_final_layouts_.begin(); it != icon_final_layouts_.end(); ++it) {
        if (it->text != fit.shown || it->width != width || it->height != height ||
            it->format.get() != compositor_->FileNameFormat()) continue;
        layout.p = it->layout.get();
        measured_height = it->measured_height;
        ++icon_final_hits_;
        layout->AddRef();
        auto entry = std::move(*it);
        icon_final_layouts_.erase(it);
        icon_final_layouts_.push_back(std::move(entry));
        break;
    }
    const bool created = !layout.get();
    if (created && (FAILED(compositor_->DwriteFactory()->CreateTextLayout(
            fit.shown.c_str(), static_cast<UINT32>(fit.shown.size()), compositor_->FileNameFormat(),
            width, height, &layout)) || !layout.get())) {
        return false;
    }
    if (created) {
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        // Already shortened to fit: the format's own trimming would cut again.
        const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&untrimmed, nullptr);
        ++icon_final_creations_;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) measured_height = metrics.height;
    }
    constexpr size_t budget = 512 * 1024;
    const size_t bytes = fit.shown.size() * sizeof(wchar_t);
    if (created && bytes <= budget) {
        while (!icon_final_layouts_.empty() &&
               (icon_final_layouts_.size() >= 512 || icon_final_text_bytes_ + bytes > budget)) {
            icon_final_text_bytes_ -= icon_final_layouts_.front().text.size() * sizeof(wchar_t);
            icon_final_layouts_.erase(icon_final_layouts_.begin());
        }
        IconFinalLayout entry;
        entry.text = fit.shown; entry.width = width; entry.height = height;
        entry.measured_height = measured_height;
        entry.format.p = compositor_->FileNameFormat(); entry.format->AddRef();
        entry.layout.p = layout.get(); layout->AddRef();
        icon_final_layouts_.push_back(std::move(entry));
        icon_final_text_bytes_ += bytes;
    }
    if (text_height) {
        *text_height = measured_height;
    }
    DrawNameHighlightBackground(compositor_, layout.get(), {bounds.left, bounds.top}, bounds, fit.matches, theme, scale_);
    if (dim_extension) DimNameExtension(layout.get(), name, fit.shown, color);
    MakeBrush(compositor_->Dc(), color, brText_);
    compositor_->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(),
                                      brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->SetDrawingEffect(nullptr, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(fit.shown.size())});
    return fit.truncated;
}

float MainRenderer::DrawExpandedIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color, const Theme& theme,
                                        const std::vector<NameMatchRange>& matches, bool pane_focused,
                                        const D2D1_RECT_F& view, const std::wstring& note,
                                        const D2D1_COLOR_F& note_color, bool dim_extension) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || !compositor_->DwriteFactory() || name.empty()) return 0.0f;
    constexpr int kExpandedLines = 6;
    const float line = kIconGridNameLineDip * scale_;
    const float edge = 4.0f * scale_;
    // A folder's size stays under its name, inside the plate.
    const float note_h = note.empty() ? 0.0f : line;
    // Every line down to the bottom of the list, at least the six the cell
    // width gets: the selected item shows its whole name.
    const int room = std::max(1, static_cast<int>(std::floor(
        (view.bottom - edge - note_h - bounds.top) / line)));
    const float cell_w = std::max(1.0f, bounds.right - bounds.left);
    float width = cell_w;
    IconNameText fit = FitIconName(compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(),
                                   name, width, std::min(kExpandedLines, room), matches);
    if (fit.truncated) {
        // Longer than six lines at the cell width: widen like the rename
        // field, to two cells (280 dip at most), inside the list.
        const float wide = std::min({2.0f * cell_w + 8.0f * scale_, std::max(cell_w, 280.0f * scale_),
                                     std::max(1.0f, view.right - view.left - 2.0f * edge)});
        width = std::max(cell_w, wide);
        fit = FitIconName(compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(),
                          name, width, room, matches);
    }
    // Lines the list had no room for: the caller may glide the list up.
    float overflow = 0.0f;
    if (fit.truncated) {
        const IconNameText all = FitIconName(compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(),
                                             name, width, 64, matches);
        const int need = 1 + static_cast<int>(std::count(all.shown.begin(), all.shown.end(), L'\u2028'));
        if (need > room)
            overflow = std::min(static_cast<float>(need - room) * line,
                                std::max(0.0f, bounds.top - view.top - edge - line));
    }
    const float mid0 = (bounds.left + bounds.right) * 0.5f;
    const float left = std::clamp(mid0 - width * 0.5f, view.left + edge,
                                  std::max(view.left + edge, view.right - edge - width));
    const D2D1_RECT_F text = D2D1::RectF(left, bounds.top, left + width,
                                         bounds.top + static_cast<float>(room) * line + 2.0f * scale_);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(fit.shown.c_str(),
            static_cast<UINT32>(fit.shown.size()), compositor_->FileNameFormat(),
            std::max(1.0f, text.right - text.left), text.bottom - text.top, &layout)) || !layout.get())
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    layout->SetTrimming(&untrimmed, nullptr);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    float note_w = 0.0f;
    if (!note.empty() && compositor_->SmallFormat())
        note_w = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->SmallFormat(), note);
    const float pad_x = 4.0f * scale_;
    const float plate_w = std::min(text.right - text.left,
                                   std::max(metrics.widthIncludingTrailingWhitespace, note_w)) + pad_x * 2.0f;
    const float mid = (text.left + text.right) * 0.5f;
    const float name_h = std::max(line, metrics.height);
    const D2D1_RECT_F plate = D2D1::RectF(mid - plate_w * 0.5f, text.top - 2.0f * scale_,
        mid + plate_w * 0.5f, text.top + name_h + note_h + 3.0f * scale_);
    // Opaque: it lies over the next row. Same tint as the selected cell.
    const D2D1_COLOR_F base = theme.bg;
    const D2D1_COLOR_F tint = pane_focused ? theme.fill_selected : theme.fill_selected_inactive;
    const D2D1_COLOR_F fill = IsHighContrast() ? theme.bg : D2D1::ColorF(
        base.r + (tint.r - base.r) * tint.a, base.g + (tint.g - base.g) * tint.a,
        base.b + (tint.b - base.b) * tint.a, 1.0f);
    MakeBrush(dc, fill, brFillInput_);
    const float radius = theme.radius_control * scale_;
    FillRoundedRect(dc, brFillInput_.get(), plate.left, plate.top, plate.right - plate.left,
                    plate.bottom - plate.top, radius);
    MakeBrush(dc, IsHighContrast() ? theme.text : WithAlpha(theme.accent, 0.55f), brFillInput_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(plate, radius, radius), brFillInput_.get(), scale_);
    DrawNameHighlightBackground(compositor_, layout.get(), {text.left, text.top}, plate, fit.matches, theme, scale_);
    if (dim_extension) DimNameExtension(layout.get(), name, fit.shown, color);
    MakeBrush(dc, color, brText_);
    compositor_->DrawTextLayout(D2D1::Point2F(text.left, text.top), layout.get(), brText_.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (note_w > 0.0f) {
        auto* format = compositor_->SmallFormat();
        const auto alignment = format->GetTextAlignment();
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        MakeBrush(dc, note_color, brTextSecondary_);
        DrawTextEndEllipsis(compositor_, format, brTextSecondary_.get(), note,
                            plate.left + pad_x, text.top + name_h, plate.right - plate.left - 2.0f * pad_x, line);
        format->SetTextAlignment(alignment);
    }
    return overflow;
}
D2D1_RECT_F MainRenderer::RenameFieldRect(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                          int source_index) {
    // Mirror of the frame geometry in DrawList for vm.rename_index — keep in sync.
    if (!compositor_ || !compositor_->DwriteFactory() || source_index < 0) return {};
    const int view = vm.ViewIndex(source_index);
    if (view < 0) return {};
    const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                      scale_, ListRowHeightDip(vm, list), vm.Groups());
    D2D1_RECT_F nameRc = layout.NameRect(view);
    if (const float lead = DetailsLeadDip(vm, view); lead > 0.0f)
        nameRc.left = std::min(nameRc.right, nameRc.left + std::round(lead * scale_));
    const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                          vm.view_mode == ViewMode::LargeIcons ||
                          vm.view_mode == ViewMode::MediumIcons;
    float nameX = nameRc.left;
    float textY = nameRc.top;
    float textH = std::max(1.0f, nameRc.bottom - nameRc.top);
    const float dot = 8.0f * scale_;
    const std::vector<D2D1_COLOR_F>* tagDots = nullptr;
    const std::vector<int>* tagIndices = vm.tag_catalog
        ? vm.tag_catalog->TagIndicesForPath(e.path) : nullptr;
    if (!tagIndices && vm.tag_dots) {
        const auto found = vm.tag_dots->find(source_index);
        if (found != vm.tag_dots->end()) tagDots = &found->second;
    }
    const size_t totalTags = tagIndices ? tagIndices->size() : (tagDots ? tagDots->size() : 0);
    const int tagDotCount = static_cast<int>(std::min<size_t>(3, totalTags));
    const float nameColRight = vm.view_mode == ViewMode::Details
        ? (vm.column_strip.Active() ? DetailsColumns(list, vm).right - 18.0f * scale_
                                    : DetailsColumns(list, vm).DividerX(0) - margin_)
        : nameRc.right;
    if (iconGrid && tagDotCount > 0) {
        const float fullNameW = MeasureLayoutText(
            compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(), ShownName(e));
        const float nameGap = 4.0f * scale_;
        const float cellW = nameRc.right - nameRc.left;
        const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
        const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
            ? SpreadTagsWidth(tagDotCount, dot, nameGap)
            : OverlapTagsWidth(tagDotCount, dot);
        const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
        nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
        textY = nameRc.top;
        textH = std::min(textH, 24.0f * scale_);
    }
    // The field is sized for the name being typed, not the old one: the whole
    // name column in details, at least 360 dip in lists and tiles, so added
    // dates and versions stay visible while editing. Icon grids instead fit
    // the field to the text, centered on the cell like the name it replaces:
    // from the cell's width up to two cells (280 dip at most), never narrower
    // while typing; longer names wrap, the field growing down to six lines.
    const float minW = 40.0f * scale_;
    const float wideW = 360.0f * scale_;
    const float listLeft = list.left + margin_;
    const float listRight = std::max(listLeft + minW, list.right - margin_);
    float fieldLeft = nameX;
    float field_w = std::max(minW, nameColRight - nameX);
    const std::wstring& typed = vm.rename_index == source_index ? vm.rename_text : e.name;
    // Text box of the EDIT inside the field: frame inset 3 dip a side and
    // LumaText's 2 px pad a side (one spare px against rounding).
    const auto editTextWidth = [&](float field) { return std::max(1.0f, field - 6.0f * scale_ - 5.0f); };
    if (iconGrid) {
        const D2D1_RECT_F cell = layout.ItemRect(view);
        const float cellW = cell.right - cell.left;
        float maxW = std::max(minW, std::min({2.0f * cellW + 8.0f * scale_,
                                              std::max(cellW, 280.0f * scale_), listRight - listLeft}));
        // Photo wall (prototype Rename): narrow portrait tiles still get a
        // usable field, 150 to 300 dip, on the tile's bottom.
        const bool wall = layout.Wall() != nullptr;
        if (wall) maxW = std::max(maxW, std::min(300.0f * scale_, listRight - listLeft));
        const float floorW = std::min(maxW, wall ? std::max(cellW, 150.0f * scale_) : cellW);
        float natural = 0.0f;
        ComPtr<IDWriteTextLayout> line;
        if (compositor_->FileNameFormat() && SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                typed.c_str(), static_cast<UINT32>(typed.size()), compositor_->FileNameFormat(), 1.0e6f, 1.0e6f,
                &line)) && line.get()) {
            line->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(line->GetMetrics(&metrics))) natural = metrics.widthIncludingTrailingWhitespace;
        }
        // Room for the caret after the last glyph, so a fitted line never wraps.
        const float hug = natural + 6.0f * scale_ + 5.0f + 4.0f * scale_;
        field_w = std::clamp(std::ceil(hug), floorW, maxW);
        // A name that wraps under its icon gets a text box exactly as wide
        // as the selected name expands to (DrawExpandedIconName), so its
        // lines stay where they were when editing starts.
        const float nameW = std::max(1.0f, nameRc.right - nameRc.left);
        if (!wall && natural + 1.0f > nameW) {
            float textW = nameW;
            if (WrapIconName(compositor_->DwriteFactory(), compositor_->FileNameFormat(), typed,
                    std::max(1.0f, nameW - 1.0f), 6).size() > 6)
                textW = std::max(nameW, std::min({2.0f * nameW + 8.0f * scale_, std::max(nameW, 280.0f * scale_),
                                                  listRight - listLeft - 8.0f * scale_}));
            field_w = std::max(minW, std::min(listRight - listLeft, textW + 6.0f * scale_ + 4.0f));
            maxW = std::max(maxW, field_w);
        }
        if (vm.rename_index == source_index) field_w = std::min(maxW, std::max(field_w, vm.rename_field_w));
        const float center = (cell.left + cell.right) * 0.5f;
        fieldLeft = std::clamp(center - field_w * 0.5f, listLeft, std::max(listLeft, listRight - field_w));
    } else if (vm.view_mode != ViewMode::Details) {
        field_w = std::max(minW, std::min(std::max(field_w, wideW), listRight - nameX));
    }
    float field_h = std::max(22.0f * scale_, std::min(textH, 30.0f * scale_));
    if (iconGrid && compositor_->FileNameFormat()) {
        // Wrap exactly as the EDIT will: the shared file-name breaks
        // (LumaText's wrapped layout), in the file-name format.
        const float textW = editTextWidth(field_w);
        float wrappedH = 18.0f * scale_;
        IDWriteFactory2* factory = compositor_->DwriteFactory();
        IDWriteTextFormat* nameFormat = compositor_->FileNameFormat();
        const BrokenFileName broken = BreakFileName([&](const std::wstring& part) {
            return typography::MeasureAdvance(factory, nameFormat, part);
        }, typed, textW);
        ComPtr<IDWriteTextLayout> wrapped;
        if (SUCCEEDED(factory->CreateTextLayout(broken.text.c_str(),
                static_cast<UINT32>(broken.text.size()), nameFormat, textW + 1.0f, 1.0e6f, &wrapped)) &&
            wrapped.get()) {
            wrapped->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            // Untrimmed like the EDIT's layout: the format's ellipsis would cut
            // a long name without spaces on one line instead of wrapping it.
            const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            wrapped->SetTrimming(&untrimmed, nullptr);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(wrapped->GetMetrics(&metrics))) {
                wrappedH = metrics.height;
                // Six lines at most; the rest scrolls inside the field.
                if (metrics.lineCount > 6) wrappedH = metrics.height * 6.0f / static_cast<float>(metrics.lineCount);
            }
        }
        field_h = std::max(24.0f * scale_, std::ceil(wrappedH) + 6.0f * scale_);
    }
    // A refused character shakes the field (the EDIT rides on this rect):
    // three damped swings over 360 ms.
    if (vm.rename_index == source_index && vm.rename_shake_ms >= 0) {
        const float t = std::clamp(static_cast<float>(vm.rename_shake_ms) / 360.0f, 0.0f, 1.0f);
        fieldLeft += std::round(5.0f * scale_ * std::sin(t * 6.2831853f * 3.0f) * (1.0f - t));
    }
    return D2D1::RectF(fieldLeft, textY, fieldLeft + field_w, textY + field_h);
}

void MainRenderer::DrawRenameHint(const PaneViewModel& vm, const D2D1_RECT_F& field, const D2D1_RECT_F& view,
                                  const Theme& theme) {
    IDWriteTextFormat* format = compositor_ ? compositor_->SmallFormat() : nullptr;
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!format || !dc || !compositor_->DwriteFactory() || vm.rename_hint.empty() || vm.rename_hint_ms == 0) return;
    const float a = vm.rename_hint_ms < 0 ? 1.0f : std::clamp(static_cast<float>(vm.rename_hint_ms) / 250.0f, 0.0f, 1.0f);
    if (vm.rename_hint_ms > 0) RequestMotionFrame();
    const float pad = 10.0f * scale_;
    const float w = std::min(std::max(field.right - field.left, 220.0f * scale_),
                             std::max(60.0f * scale_, view.right - view.left - 8.0f * scale_));
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(vm.rename_hint.c_str(),
            static_cast<UINT32>(vm.rename_hint.size()), format, std::max(1.0f, w - 2.0f * pad), 1.0e6f, &layout)) ||
        !layout.get())
        return;
    // The small format centers its line in the box; this box is open-ended.
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    layout->SetTrimming(&untrimmed, nullptr);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;
    const float padY = 7.0f * scale_;
    const float h = std::ceil(metrics.height) + 2.0f * padY;
    const float left = std::clamp((field.left + field.right - w) * 0.5f, view.left + 4.0f * scale_,
                                  std::max(view.left + 4.0f * scale_, view.right - 4.0f * scale_ - w));
    float top = field.bottom + 6.0f * scale_;
    if (top + h > view.bottom - 2.0f * scale_ && field.top - 6.0f * scale_ - h >= view.top)
        top = field.top - 6.0f * scale_ - h;
    const D2D1_RECT_F card = D2D1::RectF(left, top, left + w, top + h);
    const float radius = theme.radius_control * scale_;
    painter_.FillRoundedRect(card, radius, WithAlpha(theme.surface_card, a));
    painter_.StrokeRoundedRect(card, radius, WithAlpha(theme.stroke_card, a));
    painter_.FillRoundedRect(D2D1::RectF(card.left, card.top + 4.0f * scale_, card.left + 3.0f * scale_,
                                         card.bottom - 4.0f * scale_), 1.5f * scale_, WithAlpha(theme.accent, a));
    MakeBrush(dc, WithAlpha(theme.text, a), brText_);
    compositor_->DrawTextLayout(D2D1::Point2F(card.left + pad, card.top + padY), layout.get(), brText_.get());
}

namespace {
// Offsets one list row while it slides (restores the transform on scope exit,
// including `continue`).
struct RowShiftTransform {
    ID2D1DeviceContext* dc = nullptr;
    D2D1_MATRIX_3X2_F saved{};
    bool active = false;
    RowShiftTransform(ID2D1DeviceContext* context, float dx, float dy) : dc(context) {
        if (!dc || (dx == 0.0f && dy == 0.0f)) return;
        dc->GetTransform(&saved);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * saved);
        active = true;
    }
    ~RowShiftTransform() {
        if (active) dc->SetTransform(saved);
    }
    RowShiftTransform(const RowShiftTransform&) = delete;
    RowShiftTransform& operator=(const RowShiftTransform&) = delete;
};
constexpr uint64_t kFolderSizePendingLimitMs = 10000;
} // namespace

void MainRenderer::DrawListSkeleton(const D2D1_RECT_F& viewport, const Theme& theme,
                                    int pane_index) {
    list_loading_active_ = true;
    if (pane_index < 0 || pane_index >= static_cast<int>(list_loading_since_.size())) return;
    const ULONGLONG now = GetTickCount64();
    uint64_t& since = list_loading_since_[pane_index];
    if (since == 0) {
        since = now;
        list_loading_animate_ = motion::SystemAnimationsEnabled();
    }
    const uint64_t age = now - since;
    // Fast folders finish before this and never show placeholders.
    if (age < 150 || IsHighContrast()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float appear = list_loading_animate_
        ? std::min(1.0f, static_cast<float>(age - 150) / 200.0f) : 1.0f;
    const float w = viewport.right - viewport.left;
    const float row_h = row_height_dip_ * scale_;
    const int rows = std::min(10, static_cast<int>((viewport.bottom - viewport.top) / row_h));
    static constexpr float kNameWidth[] = {0.46f, 0.34f, 0.52f, 0.40f, 0.30f,
                                           0.48f, 0.38f, 0.56f, 0.33f, 0.44f};
    ComPtr<ID2D1LinearGradientBrush> shimmer;
    if (list_loading_animate_) {
        const float band = 160.0f * scale_;
        const float travel = w + band * 2.0f;
        const float cx = viewport.left - band +
            travel * static_cast<float>(age % 1400) / 1400.0f;
        D2D1_COLOR_F glow = theme.text;
        glow.a = theme.bg.r < 0.5f ? 0.07f : 0.06f;
        D2D1_COLOR_F clear = glow;
        clear.a = 0.0f;
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, clear}, {0.5f, glow}, {1.0f, clear}};
        ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &collection)) && collection.get()) {
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - band, 0.0f),
                                                    D2D1::Point2F(cx + band, 0.0f)),
                collection.get(), &shimmer);
        }
    }
    for (int r = 0; r < rows; ++r) {
        const float fade = appear * (1.0f - 0.07f * static_cast<float>(r));
        D2D1_COLOR_F base = theme.text;
        base.a = (theme.bg.r < 0.5f ? 0.08f : 0.06f) * fade;
        MakeBrush(dc, base, brFillInput_);
        const float top = viewport.top + 6.0f * scale_ + static_cast<float>(r) * row_h;
        const float cy = top + row_h * 0.5f;
        const float bar = 10.0f * scale_;
        const D2D1_RECT_F bars[] = {
            D2D1::RectF(viewport.left + 16.0f * scale_, cy - 8.0f * scale_,
                        viewport.left + 32.0f * scale_, cy + 8.0f * scale_),
            D2D1::RectF(viewport.left + 44.0f * scale_, cy - bar * 0.5f,
                        viewport.left + 44.0f * scale_ + w * kNameWidth[r] * 0.8f, cy + bar * 0.5f),
            D2D1::RectF(viewport.left + w * 0.60f, cy - bar * 0.5f,
                        viewport.left + w * 0.72f, cy + bar * 0.5f),
        };
        for (const auto& rc : bars) {
            if (rc.right <= rc.left) continue;
            const float radius = rc.bottom - rc.top <= bar ? bar * 0.5f : 4.0f * scale_;
            FillRoundedRect(dc, brFillInput_.get(), rc.left, rc.top, rc.right - rc.left,
                            rc.bottom - rc.top, radius);
            if (shimmer.get()) {
                shimmer->SetOpacity(fade);
                dc->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), shimmer.get());
            }
        }
    }
}

float MainRenderer::CapacityReveal(const PaneViewModel& vm, int pane_index) {
    // This PC tiles and its details view (and the overview card above it):
    // the capacity bars grow from empty once per visit, after the listing
    // lands. Safe to ask more than once a frame: the answer is the same.
    NavEnter* nav = pane_index >= 0 && pane_index < static_cast<int>(nav_enter_.size())
        ? &nav_enter_[static_cast<size_t>(pane_index)] : nullptr;
    if (!nav) return 1.0f;
    const bool grows = vm.view_mode == ViewMode::Tiles || vm.this_pc_columns;
    if (nav->capacity_path != vm.path) {
        nav->capacity_path = vm.path;
        nav->capacity_pending = grows && motion::SystemAnimationsEnabled() && !IsHighContrast();
        nav->capacity_start = 0;
    }
    if (nav->capacity_pending && !vm.loading) {
        nav->capacity_pending = false;
        // Drives list first; a folder of files has none to grow.
        const size_t probe = std::min<size_t>(vm.EntryCount(), 64);
        for (size_t k = 0; k < probe; ++k) {
            const ListEntryView& probe_entry = MakeVisibleEntry(vm, k);
            if (probe_entry.drive_used >= 0.0f && !probe_entry.device) {
                nav->capacity_start = motion_now_;
                break;
            }
        }
    }
    if (nav->capacity_pending) {
        RequestMotionFrame();
        return 0.0f;
    }
    if (nav->capacity_start) {
        const float t = static_cast<float>(motion_now_ - nav->capacity_start) / kCapacityRevealMs;
        if (t >= 1.0f || !grows) {
            nav->capacity_start = 0;
        } else {
            RequestMotionFrame();
            return motion::EaseOutCubic(t);
        }
    }
    return 1.0f;
}

void MainRenderer::DrawList(const PaneViewModel& vm, float x, float y, float w, float h,
                            const Theme& theme, int hover_region, int hover_control_index,
                            bool pane_focused, int pane_index) {
    struct ListPerfScope {
        FramePerf& perf;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ListPerfScope() {
            perf.list_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        }
    } perf_scope{frame_perf_};
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (pane_index >= 0 && pane_index < static_cast<int>(hover_names_.size()))
        hover_names_[static_cast<size_t>(pane_index)] = {};
    const auto highlight_terms = NameHighlightTerms(vm.filter_text, vm.is_search ? vm.search_query : L" ");
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, pane_focused ? theme.fill_selected : theme.fill_selected_inactive, brFillSelected_);
    MakeBrush(dc, theme.accent, brAccent_);
    if (vm.loading && !vm.search_retaining_results) {
        // Directory enumeration is asynchronous. Leave the list quiet for its
        // brief initial frame instead of presenting it like a search request;
        // slow folders get placeholder rows after 150 ms.
        if (UsesThumbnails(vm.view_mode)) thumbnail_cache_.Warm();
        DrawListSkeleton(D2D1::RectF(x, y, x + w, y + h), theme, pane_index);
        return;
    }
    if (pane_index >= 0 && pane_index < static_cast<int>(list_loading_since_.size()))
        list_loading_since_[pane_index] = 0;
    // Folder size answers usually land within ~50 ms; placeholders only show
    // for slower ones so fast navigation never flashes them.
    uint64_t folder_size_pending_age = 0;
    if (pane_index >= 0 && pane_index < static_cast<int>(folder_size_pending_since_.size())) {
        uint64_t& since = folder_size_pending_since_[static_cast<size_t>(pane_index)];
        if (vm.folder_size_pending.empty()) {
            since = 0;
        } else {
            if (since == 0) since = motion_now_;
            folder_size_pending_age = motion_now_ - since;
            // Past kFolderSizePendingLimitMs the placeholders give way to the
            // calculate offer instead of shimmering indefinitely.
            if (folder_size_pending_age <= kFolderSizePendingLimitMs) folder_size_anim_active_ = true;
        }
    }
    const bool folder_size_animate = motion::SystemAnimationsEnabled() && !IsHighContrast();
    const std::wstring folder_size_offer = pulse::l10n::Get(pulse::l10n::StringId::FolderSizeCalculate);
    const std::wstring folder_size_scanning = pulse::l10n::Get(pulse::l10n::StringId::Calculating);
    const size_t entryCount = vm.EntryCount();
    // Motion v2 "nav": a pane that changed folders lets the new rows enter
    // from the direction of travel once its listing has arrived.
    NavEnter* nav = pane_index >= 0 && pane_index < static_cast<int>(nav_enter_.size())
        ? &nav_enter_[static_cast<size_t>(pane_index)] : nullptr;
    if (nav) {
        if (nav->path != vm.path) {
            const bool animate = !nav->path.empty() && !vm.is_search && motion::SystemAnimationsEnabled() &&
                                 !IsHighContrast();
            const auto under = [](const std::wstring& child, const std::wstring& parent) {
                if (parent.empty() || child.size() <= parent.size()) return false;
                if (_wcsnicmp(child.c_str(), parent.c_str(), parent.size()) != 0) return false;
                return parent.back() == L'\\' || child[parent.size()] == L'\\';
            };
            nav->dir = under(vm.path, nav->path) ? 1 : under(nav->path, vm.path) ? -1 : 0;
            nav->pending = animate;
            nav->start = 0;
            nav->path = vm.path;
        }
        if (nav->pending && !vm.loading) {
            nav->pending = false;
            if (entryCount > 0) nav->start = motion_now_;
        }
        if (nav->start && motion_now_ - nav->start > 130u + motion::SpringSettleMs(motion::spring::Smooth))
            nav->start = 0;
        if (nav->start || nav->pending) RequestMotionFrame();
    }
    // This PC: the capacity bars grow from empty, once per visit.
    const float capacity_reveal = CapacityReveal(vm, pane_index);

    ItemCheckMotion* checks = item_checks_ && pane_index >= 0 &&
        pane_index < static_cast<int>(item_check_motion_.size()) ? &item_check_motion_[pane_index] : nullptr;
    const bool animate_checks = checks && motion::SystemAnimationsEnabled() && !IsHighContrast();
    if (checks) checks->Begin(vm.path, motion_frame_);
    if (entryCount == 0) return;
    const D2D1_RECT_F viewport = D2D1::RectF(x, y, x + w, y + h);

    // One set of params per frame: a second ListRowHeightDip could pick up
    // newer wall aspects than the layout below was built from.
    const ViewLayoutParams list_params = ListRowHeightDip(vm, viewport);
    ViewLayout layout(vm.view_mode, viewport, entryCount, vm.scroll_x, vm.scroll_y, scale_, list_params, vm.Groups());
    // Photo wall: rows rebuilt for newly known aspects glide from the old ones.
    const WallRows* wall_rows = layout.Wall();
    const WallRows* wall_prev = nullptr;
    const float wall_glide = WallGlide(wall_rows, &wall_prev);
    if (wall_prev) RequestMotionFrame();
    const auto [startIdx, endIdx] = layout.VisibleRange();
    const DetailsColumnLayout detailsColumns = AnimateColumns(DetailsColumns(viewport, vm), pane_index);
    const bool detailsView = vm.view_mode == ViewMode::Details;
    D2D1_COLOR_F zebraColor = theme.text;
    zebraColor.a = (theme.bg.r < 0.5f) ? 0.035f : 0.025f;
    FileItemSelectionPainter selection_painter(dc, theme, scale_, pane_focused,
                                              IsHighContrast(), list_selection_outline_);

    // Rows slide into place after small inserts/removals and new rows flash
    // (ui_motion.h ListShiftMotion). Same folder, view, filter and scroll only.
    const uint64_t folder_context = std::hash<std::wstring>{}(vm.path) ^
        (std::hash<std::wstring>{}(vm.filter_text) * 31u);
    // The filter and the sort order are not part of the shift context: a
    // change of either is a re-arrangement the visible rows travel through
    // (motion v2 "sort" / "filter").
    // Tree expansion changes visibility, not filesystem membership. Start a
    // fresh motion context so revealed rows are not flashed as new files.
    const uint64_t list_context = std::hash<std::wstring>{}(vm.path) ^
        (static_cast<uint64_t>(vm.view_mode) << 56) ^
        (vm.tree ? vm.tree->revision * 0x9e3779b97f4a7c15ULL : 0);
    const uint64_t arrangement = (std::hash<std::wstring>{}(vm.filter_text) * 31u) ^
        (static_cast<uint64_t>(vm.sort_column) << 40) ^ (static_cast<uint64_t>(vm.sort_direction) << 48);
    motion::ListShiftMotion* shift =
        pane_index >= 0 && pane_index < static_cast<int>(list_shift_.size())
            ? &list_shift_[pane_index] : nullptr;
    if (shift) {
        shift->BeginFrame(list_context, vm.scroll_x, vm.scroll_y, entryCount, motion_frame_,
                          motion_now_, !vm.is_search && !vm.loading && !(nav && nav->start), arrangement,
                          detailsView);
    }
    // View-mode switch: items travel from their previous cell/icon to the new
    // layout (ui_view_morph.h). Same folder and filter, continuous frames only.
    motion::ViewMorphMotion* morph =
        pane_index >= 0 && pane_index < static_cast<int>(view_morph_.size())
            ? &view_morph_[pane_index] : nullptr;
    if (morph) {
        morph->BeginFrame(folder_context, static_cast<int>(vm.view_mode), motion_frame_,
                          motion_now_, !vm.loading);
        morph->NoteLayout(viewport, vm.scroll_x, vm.scroll_y, list_params.row_height_dip,
                          entryCount, list_params.icon_dip);
    }
    const bool morphing = morph && morph->Running();

    // 滚动时固定上级: the rows under the pinned folders are not drawn at all,
    // so the pins need no background of their own - the pane shows through
    // them exactly as it does through the rows (any frame transparency).
    const StickyTree sticky = morphing ? StickyTree{} : StickyFromLayout(vm, layout, viewport, tree_sticky_);
    if (!sticky.pins.empty())
        dc->PushAxisAlignedClip(D2D1::RectF(viewport.left, sticky.Bottom(), viewport.right, viewport.bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
    // Hover plate: one plate per pane that glides between rows instead of
    // blinking row to row. Drawn under every row so text stays on top.
    bool hover_drawn = false;
    if (!morphing && vm.hover_index >= 0 && pane_index >= 0 &&
        pane_index < static_cast<int>(list_hover_motion_.size())) {
        for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
            if (vm.SourceIndex(i) != vm.hover_index) continue;
            const D2D1_RECT_F cell = layout.ItemRect(i);
            const float inset = 4.0f * scale_;
            const D2D1_RECT_F target = D2D1::RectF(cell.left + inset, cell.top + scale_,
                std::max(cell.left + inset, cell.right - inset),
                std::max(cell.top + scale_, cell.bottom - scale_));
            list_hover_motion_[pane_index].SetAnimationsEnabled(folder_size_animate);
            const D2D1_RECT_F plate = list_hover_motion_[pane_index].Update(
                list_context, vm.hover_index, target, motion_frame_, motion_now_);
            hover_drawn = true;
            dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
            FillRoundedRect(dc, brFillHover_.get(), plate.left, plate.top,
                plate.right - plate.left, plate.bottom - plate.top,
                theme.radius_control * scale_);
            dc->PopAxisAlignedClip();
            break;
        }
    }

    if (!hover_drawn && pane_index >= 0 && pane_index < static_cast<int>(list_hover_motion_.size()))
        list_hover_motion_[pane_index].Hide();

    // Mid-morph the list draws twice: shapes (backgrounds, icons) first, then
    // every label inside one faded layer - one layer per pane, not per row.
    // Spread invisible text preparation across the frames before the fade-in.
    // Preparing every cold label in the first frame blocks the icon motion.
    // Once text becomes visible, always draw the complete label pass.
    const float text_alpha = morphing ? morph->TextOpacity() : 1.0f;
    const int passes = morphing ? 2 : 1;
    AiListPainter* const ai_painter = g_ai_list_pane == pane_index && g_ai_list_painter && g_ai_list_painter->On() &&
                                      !morphing ? g_ai_list_painter : nullptr;
    if (!ai_painter && !AiGhostEntries().empty() && g_ai_list_pane == pane_index) AiGhostEntries().clear();
    float ai_icon_dx = 8.0f * scale_, ai_icon_size = 16.0f * scale_, ai_name_dx = 34.0f * scale_;
    struct {
        bool valid = false;
        std::wstring name;
        D2D1_RECT_F bounds{};
        D2D1_COLOR_F color{};
        std::vector<NameMatchRange> matches;
        std::wstring note;          // a folder's size, kept under the name
        D2D1_COLOR_F note_color{};
        bool dim_extension = false;
        int source = -1;
    } expanded_name;
    // Details tree (inline folder expansion): chevrons, guide lines and the
    // rows that stand in for a folder's children (loading, empty, failed,
    // more). The guides go on after every row fill (see below), so zebra
    // stripes and selection fills no longer break them up. The hovered or
    // focused row's folder gets an accent line and chevron.
    TreeGuides tree_guides;
    int tree_hi_row = -1;
    if (detailsView && vm.tree && !vm.Groups() && startIdx >= 0 && endIdx >= startIdx) {
        const int focus_src = vm.hover_index >= 0 && !vm.marquee_active ? vm.hover_index
            : vm.focus_ring_source >= 0 ? vm.focus_ring_source : vm.selected_index;
        tree_guides = BuildTreeGuides(*vm.tree, startIdx, endIdx, vm.ViewIndex(focus_src));
        if (tree_guides.hi_level >= 0) tree_hi_row = tree_guides.hi_first - 1;
    }
    if (pane_index >= 0 && pane_index < static_cast<int>(tree_guides_drawn_.size()))
        tree_guides_drawn_[static_cast<size_t>(pane_index)] = !tree_guides.rows.empty();
    const auto drawTreeChevron = [&](const DetailsTreeRow& tr, const D2D1_RECT_F& cell, float baseX,
                                     bool strong, bool accent) {
        const float cx = baseX + (static_cast<float>(tr.depth) * kTreeIndentDip +
                                  kTreeChevronDip * 0.5f) * scale_;
        const float cy = (cell.top + cell.bottom) * 0.5f;
        MakeBrush(dc, accent && !IsHighContrast() ? theme.accent : strong ? theme.text : theme.text_secondary,
                  brFillInput_);
        DrawTreeChevron(dc, brFillInput_.get(), cx, cy, 3.5f * scale_, 1.4f * scale_, tr.expanded);
    };
    const auto drawTreePlaceholder = [&](const DetailsTreeRow& tr, const D2D1_RECT_F& cell, float baseX) {
        const float x0 = baseX + TreeOffsetDip(tr) * scale_;
        const float rowH = cell.bottom - cell.top;
        const float textW = std::max(0.0f, cell.right - x0 - 8.0f * scale_);
        IDWriteFactory2* factory = compositor_->DwriteFactory();
        IDWriteTextFormat* fmt = compositor_->TextFormat();
        switch (tr.kind) {
        case TreeRowKind::Loading: {
            list_loading_active_ = true;  // keeps frames coming for the delay and the pulse
            const uint64_t age = motion_now_ > tr.since_ms ? motion_now_ - tr.since_ms : 0;
            if (age < kTreeLoadingDelayMs) return;
            const float fade = std::min(1.0f, static_cast<float>(age - kTreeLoadingDelayMs) / 150.0f);
            const float wave = 0.5f + 0.5f * std::sin(static_cast<float>(age % 1200) / 1200.0f * 6.2831853f);
            MakeBrush(dc, WithAlpha(theme.accent, (0.35f + 0.5f * wave) * fade), brFillInput_);
            const float dot = 3.0f * scale_;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + dot + 2.0f * scale_, (cell.top + cell.bottom) * 0.5f),
                                          dot, dot), brFillInput_.get());
            MakeBrush(dc, WithAlpha(theme.text_secondary, theme.text_secondary.a * fade), brTextSecondary_);
            DrawTextEndEllipsis(compositor_, fmt, brTextSecondary_.get(),
                                l10n::Get(l10n::StringId::LoadingEllipsis), x0 + 14.0f * scale_, cell.top,
                                std::max(0.0f, textW - 14.0f * scale_), rowH);
            return;
        }
        case TreeRowKind::Empty:
        case TreeRowKind::Failed:
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextEndEllipsis(compositor_, fmt, brTextSecondary_.get(),
                                l10n::Get(tr.kind == TreeRowKind::Failed ? l10n::StringId::TreeReadFailed
                                                                         : l10n::StringId::EmptyFolder),
                                x0, cell.top, textW, rowH);
            return;
        case TreeRowKind::More: {
            const TreeMoreLayout more = LayoutTreeMore(tr, cell, x0, scale_, factory, fmt);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextEndEllipsis(compositor_, fmt, brTextSecondary_.get(), more.label, more.label_x, cell.top,
                                std::min(textW, more.label_w + 2.0f * scale_), rowH);
            const float pad = 10.0f * scale_;
            for (int k = 0; k < more.count; ++k) {
                const D2D1_RECT_F& b = more.buttons[k];
                const bool primary = more.actions[k] == 0;
                MakeBrush(dc, primary ? WithAlpha(theme.accent, 0.14f) : WithAlpha(theme.text, 0.07f), brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), b.left, b.top, b.right - b.left, b.bottom - b.top,
                                6.0f * scale_);
                MakeBrush(dc, primary ? theme.accent : theme.text, brTextSecondary_);
                DrawTextEndEllipsis(compositor_, fmt, brTextSecondary_.get(), more.texts[k], b.left + pad,
                                    cell.top, std::max(0.0f, b.right - b.left - pad), rowH);
            }
            return;
        }
        default:
            return;
        }
    };
    for (int pass = 0; pass < passes; ++pass) {
        const bool draw_shapes = pass == 0;
        const bool draw_text = !morphing || pass == 1;
        const bool text_layer = morphing && pass == 1 && text_alpha < 0.999f;
        if (text_layer) {
            // Never exactly 0: Direct2D skips the content of a fully
            // transparent layer, which would defeat the warm-up above.
            // Keep the warm-up layer at the minimum nonzero opacity.
            dc->PushLayer(D2D1::LayerParameters1(viewport, nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
                              D2D1::IdentityMatrix(), std::max(text_alpha, 1.0f / 255.0f), nullptr,
                              D2D1_LAYER_OPTIONS1_NONE), nullptr);
        }
        const bool warm_text = morphing && pass == 1 && text_alpha <= 0.0f;
        const auto warm_start = std::chrono::steady_clock::now();
        int morph_order = 0;
        // Icon grids: names the rename card lies over fade to 38%, so the
        // card reads as floating above them.
        D2D1_RECT_F rename_card{};
        const bool rename_covers = (vm.view_mode == ViewMode::ExtraLargeIcons ||
                                    vm.view_mode == ViewMode::LargeIcons ||
                                    vm.view_mode == ViewMode::MediumIcons) &&
            vm.rename_index >= 0 && vm.ViewIndex(vm.rename_index) >= 0;
        if (rename_covers) rename_card = RenameFieldRect(vm, viewport, vm.rename_index);
        // 修改时间提示: one clock per pass; the change log's view has its own times.
        const MtimeHint& mtime = mtime_hint_;
        const bool mtime_on = mtime.style != 0 && !vm.is_changes && !vm.is_recycle;
        const MtimeClock mtime_clock = mtime_on ? MtimeClock::Now() : MtimeClock{};
        const int mtime_last = MtimeRangeLastBucket(mtime.range_days);
        if (mtime_flash_started_.size() > 256) {
            for (auto it = mtime_flash_started_.begin(); it != mtime_flash_started_.end();)
                it = motion_now_ - it->second.second > 10000 ? mtime_flash_started_.erase(it) : std::next(it);
        }
        // Partial redraw: a full frame notes where each details row went; a
        // partial one draws only the rows that meet the damage.
        auto* drawn_rows = pane_index >= 0 && pane_index < static_cast<int>(drawn_rows_.size())
            ? &drawn_rows_[static_cast<size_t>(pane_index)] : nullptr;
        if (drawn_rows && !frame_partial_) drawn_rows->clear();
        uint32_t prefetch_pixels = 0;  // grid request size, for the next screen
        for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
            if (warm_text) {
                if (i < morph->TextWarmupNext()) { ++morph_order; continue; }
                if (std::chrono::steady_clock::now() - warm_start >= std::chrono::milliseconds(8)) break;
                morph->AdvanceTextWarmup(i + 1);
            }
            D2D1_RECT_F cell = layout.ItemRect(i);
            if (cell.right < x || cell.left > x + w || cell.bottom < y || cell.top > y + h) continue;
            if (cell.bottom - cell.top < 0.5f) continue;   // row inside a collapsed group
            const int src = vm.SourceIndex(i);
            if (src < 0) continue;
            ++frame_perf_.list_rows;
            if (frame_partial_) {
                if (cell.right <= frame_damage_.left || cell.left >= frame_damage_.right ||
                    cell.bottom <= frame_damage_.top || cell.top >= frame_damage_.bottom) continue;
            } else if (drawn_rows && detailsView) {
                drawn_rows->emplace_back(src, cell);
            }
            if(vm.content_results && !vm.content_results->Ready(static_cast<size_t>(src))) {
                vm.content_results->Prefetch(static_cast<size_t>(src));
                if (!draw_text) continue;
                MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                DrawTextEndEllipsis(compositor_, compositor_->TextFormat(),brTextSecondary_.get(),
                    l10n::Get(l10n::StringId::LoadingEllipsis),cell.left+40*scale_,cell.top,
                    cell.right-cell.left-40*scale_,cell.bottom-cell.top);
                continue;
            }
            if (detailsView && vm.tree) {
                const DetailsTreeRow* standIn = vm.tree->At(i);
                if (standIn && standIn->kind != TreeRowKind::Entry) {
                    // The "more" row takes keyboard focus (Enter shows more).
                    if (draw_shapes && standIn->kind == TreeRowKind::More && vm.IsRowSelected(src))
                        selection_painter.Selected(cell);
                    if (draw_text) drawTreePlaceholder(*standIn, cell, layout.IconRect(i).left + CheckGutterDip(vm) * scale_);
                    continue;
                }
            }
            RowPerfScope row_perf(frame_perf_.row_prepare_us, frame_perf_.row_prepare_fallbacks, compositor_);
            const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));
            const uint64_t row_key = std::hash<std::wstring_view>{}(std::wstring_view(e.path.empty() ? e.name : e.path));
            D2D1_POINT_2F row_shift{0.0f, 0.0f};
            if (shift) row_shift = shift->Offset(row_key, D2D1::Point2F(cell.left, cell.top));
            // Motion 5b: a row on its way to the Recycle Bin slides 14 px left
            // and fades out (120 ms); the gap closes once the listing drops it.
            float row_leave = 0.0f;
            if (!vm.leaving_names.empty() && vm.leaving_names.contains(e.name)) {
                row_leave = (std::clamp)(static_cast<float>(motion_now_ - vm.leaving_since) / 120.0f,
                                         0.0f, 1.0f);
                if (row_leave >= 1.0f) continue;
                row_leave_until_ = motion_now_ + 60;
            }
            // Pulse 助手: read walk, destination tags, items sliding into folders.
            AiListPainter::RowMotion ai_row;
            if (ai_painter) {
                ai_row = ai_painter->Row(e.name, cell);
                if (ai_row.gone) continue;
                if (ai_row.fly) AiGhostEntries()[AiListLower(e.name)] = e;
            }

            const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                                  vm.view_mode == ViewMode::LargeIcons ||
                                  vm.view_mode == ViewMode::MediumIcons;
            D2D1_RECT_F nameRc = layout.NameRect(i);
            const auto changeIt = vm.change_badges.find(src);
            const ChangeBadge* change = changeIt != vm.change_badges.end() && HasChangeBadge(changeIt->second) ? &changeIt->second : nullptr;
            D2D1_RECT_F iconRect = layout.IconRect(i);
            // Thumbnails fill the whole slot (4:3 for extra large icons); shell
            // icons keep their square on its bottom edge.
            D2D1_RECT_F thumbRect = layout.ThumbnailRect(i);
            // Photo wall: a picture is the whole tile (the morph flies it there);
            // folders and documents keep their icon above a one-line name.
            const bool wallPic = wall_rows && WallPicture(e.name, e.is_dir);
            if (wall_rows && !wallPic) thumbRect = iconRect;
            if (wallPic) iconRect = thumbRect;
            if (wall_prev) {
                const size_t at = static_cast<size_t>(i);
                const int now_row = wall_rows->RowOf(i), was_row = wall_prev->RowOf(i);
                if (now_row >= 0 && was_row >= 0) {
                    const float ox = cell.left - wall_rows->x[at];
                    const float oy = cell.top - wall_rows->row_top[static_cast<size_t>(now_row)];
                    const auto lerp = [&](float a, float b) { return a + (b - a) * wall_glide; };
                    const float gl = lerp(ox + wall_prev->x[at], cell.left);
                    const float gt = lerp(oy + wall_prev->row_top[static_cast<size_t>(was_row)], cell.top);
                    const float gw = lerp(wall_prev->w[at], cell.right - cell.left);
                    const float gh = lerp(wall_prev->row_h[static_cast<size_t>(was_row)], cell.bottom - cell.top);
                    const float dx = gl - cell.left, dy = gt - cell.top;
                    const auto shift_by = [&](D2D1_RECT_F& r) { r.left += dx; r.right += dx; r.top += dy; r.bottom += dy; };
                    if (wallPic) {
                        cell = D2D1::RectF(gl, gt, gl + gw, gt + gh);
                        thumbRect = iconRect = cell;
                        nameRc = D2D1::RectF(cell.left + 2.0f * scale_, std::max(cell.top, cell.bottom - 19.0f * scale_),
                                             cell.right - 2.0f * scale_, cell.bottom - 2.0f * scale_);
                    } else {
                        shift_by(cell); shift_by(iconRect); shift_by(thumbRect); shift_by(nameRc);
                    }
                }
            }
            // Details tree: rows indent by depth after the chevron slot.
            const DetailsTreeRow* treeRow = detailsView && vm.tree ? vm.tree->At(i) : nullptr;
            // The check box gutter (item_checks_) comes first, then the chevron slot.
            const float checkBaseX = iconRect.left;
            const float treeBaseX = iconRect.left + (detailsView ? CheckGutterDip(vm) * scale_ : 0.0f);
            if (const float lead = detailsView ? DetailsLeadDip(vm, i) : 0.0f; lead > 0.0f) {
                const float dx = std::round(lead * scale_);
                iconRect.left += dx;
                iconRect.right += dx;
                thumbRect.left += dx;
                thumbRect.right += dx;
                nameRc.left = std::min(nameRc.right, nameRc.left + dx);
            }
            // Landing: a row a drag just dropped here stays hidden while the
            // drag's cards fly onto it, then fades in (its flash runs on).
            float row_land_hide = 0.0f;
            if (!vm.landing_names.empty() && vm.landing_names.contains(e.name)) {
                if (draw_shapes) {
                    D2D1_MATRIX_3X2_F m{};
                    dc->GetTransform(&m);
                    const auto at = [&m](float x, float y) {
                        return D2D1::Point2F(x * m._11 + y * m._21 + m._31, x * m._12 + y * m._22 + m._32);
                    };
                    const D2D1_POINT_2F a = at(iconRect.left, cell.top);
                    const D2D1_POINT_2F b = at(cell.right, cell.bottom);
                    landing_rects_.push_back(D2D1::RectF(a.x, a.y, b.x, b.y));
                }
                row_land_hide = vm.landing_reveal_at == 0 || motion_now_ < vm.landing_reveal_at ? 1.0f
                    : 1.0f - (std::min)(1.0f, static_cast<float>(motion_now_ - vm.landing_reveal_at) / 140.0f);
                if (row_land_hide > 0.0f) RequestMotionFrame();
                if (row_land_hide >= 1.0f) continue;
            }
            if (change && iconGrid) {
                const float icon_inset = std::min(18 * scale_, (iconRect.bottom - iconRect.top) * 0.3f);
                iconRect.top += icon_inset;
                iconRect.left += icon_inset * 0.5f; iconRect.right -= icon_inset * 0.5f;
                thumbRect.top += icon_inset;
                thumbRect.left += icon_inset * 0.5f; thumbRect.right -= icon_inset * 0.5f;
            }

            // Where this item is drawn now. Outside a view switch it is exactly
            // the target layout; Record keeps it for the next switch.
            motion::ViewMorphMotion::Sample morph_sample{};
            morph_sample.cell = cell;
            morph_sample.icon = iconRect;
            if (morph) {
                morph_sample = morph->Get(row_key, cell, iconRect, morph_order++, 12.0f * scale_);
                if (draw_shapes) morph->Record(row_key, morph_sample.cell, morph_sample.icon);
            }
            const bool morph_item = morphing && morph_sample.animating;
            // Whole-pixel offset keeps text crisp while the row slides or morphs.
            const float row_dx = std::round(row_shift.x + morph_sample.cell.left - cell.left -
                                            14.0f * scale_ * row_leave * row_leave + ai_row.dx);
            const float row_dy = std::round(row_shift.y + morph_sample.cell.top - cell.top);
            // Nav entry: smooth spring, 40 ms + 9 ms per visible row (max 90).
            float nav_dx = 0.0f, nav_dy = 0.0f, nav_alpha = 1.0f;
            if (nav && nav->start && !morph_item) {
                const int order = (std::max)(0, i - startIdx);
                const float delay = 40.0f + (std::min)(static_cast<float>(order) * 9.0f, 90.0f);
                const float p = motion::SpringProgress(motion::spring::Smooth,
                                                       static_cast<float>(motion_now_ - nav->start) - delay);
                nav_alpha = (std::clamp)(p, 0.0f, 1.0f);
                if (nav->dir != 0) nav_dx = static_cast<float>(nav->dir) * 16.0f * scale_ * (1.0f - p);
                else nav_dy = 6.0f * scale_ * (1.0f - p);
            }
            const float row_appear = shift ? shift->Appear(row_key) : 1.0f;
            const RowShiftTransform row_transform(dc, row_dx + std::round(nav_dx), row_dy + std::round(nav_dy));
            const float row_opacity = (1.0f - row_leave) * (1.0f - row_land_hide) * nav_alpha * row_appear *
                                      ai_row.opacity;
            if (nav_alpha <= 0.0f || row_appear <= 0.0f) continue;
            struct RowFade {
                ID2D1DeviceContext* dc;
                bool on;
                ~RowFade() { if (on) dc->PopLayer(); }
            } row_fade{dc, row_opacity < 0.999f};
            if (row_fade.on) {
                // Navigation rows only paint inside their cell; bound the opacity
                // layer instead of allocating a viewport-sized intermediate per row.
                const auto fade_bounds = morph_item || !detailsView ? viewport : D2D1::RectF(
                    cell.left-4*scale_,cell.top-4*scale_,cell.right+4*scale_,cell.bottom+4*scale_);
                dc->PushLayer(D2D1::LayerParameters1(fade_bounds, nullptr,
                                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(),
                                                     row_opacity, nullptr,
                                                     D2D1_LAYER_OPTIONS1_NONE), nullptr);
            }
            // Backgrounds take the morphing cell's shape; the transform already
            // moved everything by (row_dx, row_dy).
            const D2D1_RECT_F bg = morph_item
                ? D2D1::RectF(morph_sample.cell.left - row_dx, morph_sample.cell.top - row_dy,
                              morph_sample.cell.right - row_dx, morph_sample.cell.bottom - row_dy)
                : cell;
            const float row_flash = shift ? shift->Flash(row_key) : 0.0f;
            // Motion v2 "newfolder": a row that just appeared grows open from
            // its top edge (Pop) while the rows below slide down.
            struct RowGrowClip {
                ID2D1DeviceContext* dc;
                bool on;
                ~RowGrowClip() { if (on) dc->PopAxisAlignedClip(); }
            } row_grow{dc, false};
            if (row_flash > 0.0f && !morph_item) {
                const float age = (1.0f - row_flash) * static_cast<float>(motion::ListShiftMotion::kFlashMs);
                if (age < 260.0f) {
                    const float g = (std::clamp)(motion::SpringProgress(motion::spring::Pop, age), 0.02f, 1.0f);
                    dc->PushAxisAlignedClip(D2D1::RectF(cell.left - 4.0f * scale_, cell.top,
                                                        cell.right + 4.0f * scale_,
                                                        cell.top + (cell.bottom - cell.top) * g),
                                            D2D1_ANTIALIAS_MODE_ALIASED);
                    row_grow.on = true;
                }
            }

            bool selected = vm.IsRowSelected(src);
            float check_tick = 1.0f;
            if (draw_shapes && checks && !e.path.empty()) {
                const auto sample = checks->Update(e.path, selected, motion_frame_, motion_now_, animate_checks);
                check_tick = sample.progress;
                if (sample.active) RequestMotionFrame();
            }

            // A marquee sweeps across many rows: hover feedback (row highlight,
            // link pill) would flicker under the pointer and animate every link.
            bool hover = (src == vm.hover_index) && !vm.marquee_active;
            bool cut = e.record_only || vm.cut_names.contains(e.name);
            const float hidden_alpha = HiddenEntryAlpha(e.attrs);
            // Text follows the row's own time; the mark may take a folder's
            // newest change inside (change tracking), so a folder whose files
            // changed today reads as fresh without bolding its own date.
            int mtimeOwn = kMtimeBucketNone, mtimeMark = kMtimeBucketNone;
            uint64_t mtimeMarkTime = 0;
            if (mtime_on && !e.record_only && e.modified_value) {
                mtimeOwn = MtimeBucket(e.modified_value, mtime_clock);
                mtimeMarkTime = e.modified_value;
                if (mtime.tree && e.is_dir && change && change->last_change > mtimeMarkTime)
                    mtimeMarkTime = change->last_change;
                mtimeMark = MtimeBucket(mtimeMarkTime, mtime_clock, &mtime_next_boundary_);
            }
            const bool mtimeMarked = mtimeMark <= mtime_last;
            const bool mtimeFresh = mtime.bold && mtimeOwn <= 1 && mtimeOwn <= mtime_last;
            const bool mtimeOld = mtime_on && mtime.fade && mtimeOwn > mtime_last && !mtimeMarked;
            const D2D1_COLOR_F mtimeColor = mtimeMarked
                ? MtimeColor(mtime.palette, mtimeMark, theme.bg.r < 0.5f, theme.accent, IsHighContrast())
                : D2D1_COLOR_F{};
            const bool expand_link = e.is_link && !vm.marquee_active && (hover || selected) &&
                src != vm.rename_index;
            const float link_expansion = e.is_link && pane_index >= 0 && pane_index < 8
                ? link_pill_motion_[pane_index].Update(list_context, row_key, expand_link,
                    motion_frame_, motion_now_, !IsHighContrast() && !vm.marquee_active)
                : (expand_link ? 1.0f : 0.0f);
            const std::wstring link_label = !e.is_link ? std::wstring()
                : e.link_destination.empty()
                ? std::wstring(l10n::Get(l10n::StringId::TargetUnavailable).c_str()) : e.link_destination;
            D2D1_RECT_F artwork = iconRect;
            bool artwork_hit_valid = false;

            const float inset = 4.0f * scale_;
            if (draw_shapes && detailsView && list_zebra_ && (i & 1) && !selected && !hover && !IsHighContrast()) {
                MakeBrush(dc, zebraColor, brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), bg.left + inset, bg.top,
                    std::max(0.0f, bg.right - bg.left - inset * 2),
                    std::max(0.0f, bg.bottom - bg.top), theme.radius_control * scale_);
            }
            // Folder compare: status tint + a colored bar in the left gutter.
            const uint8_t compare_mark = vm.compare_marks &&
                static_cast<size_t>(src) < vm.compare_marks->size()
                ? (*vm.compare_marks)[static_cast<size_t>(src)] : 0;
            if (draw_shapes && compare_mark != 0) {
                const bool dark_bg = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
                const D2D1_COLOR_F mark_color = CompareMarkColor(compare_mark, dark_bg);
                if (!selected && !IsHighContrast()) {
                    MakeBrush(dc, WithAlpha(mark_color, dark_bg ? 0.10f : 0.08f), brFillInput_);
                    FillRoundedRect(dc, brFillInput_.get(), bg.left + inset, bg.top + scale_,
                        std::max(0.0f, bg.right - bg.left - inset * 2),
                        std::max(0.0f, bg.bottom - bg.top - scale_ * 2), theme.radius_control * scale_);
                }
                MakeBrush(dc, mark_color, brFillInput_);
                const float bar_h = std::max(0.0f, bg.bottom - bg.top - 10.0f * scale_);
                FillRoundedRect(dc, brFillInput_.get(), bg.left + 0.5f * scale_,
                    (bg.top + bg.bottom - bar_h) * 0.5f, 2.5f * scale_, bar_h, 1.25f * scale_);
            }
            // A check box already marks the row: no selection bar beside it.
            if (draw_shapes && selected)
                selection_painter.Selected(bg, !(item_checks_ && (detailsView || iconGrid) && !morph_item && !morphing));
            if (draw_shapes && mtime_on && mtime.flash && !morph_item && !IsHighContrast() && !e.path.empty() &&
                e.modified_value && e.modified_value + 3 * kMtimeSecond > mtime_clock.now &&
                e.modified_value < mtime_clock.now + 2 * kMtimeSecond) {
                // 文件刚被修改时闪一下: a file saved seconds ago glows once. A file
                // written continuously (a download) does not glow again for 4 s.
                auto [mtSeen, mtAdded] = mtime_flash_started_.try_emplace(e.path, e.modified_value, motion_now_);
                if (!mtAdded && mtSeen->second.first != e.modified_value && motion_now_ - mtSeen->second.second > 4000)
                    mtSeen->second = {e.modified_value, motion_now_};
                const float mtGlow = static_cast<float>(motion_now_ - mtSeen->second.second) / 1200.0f;
                if (mtGlow < 1.0f) {
                    const float mtFade = (1.0f - mtGlow) * (1.0f - mtGlow);
                    MakeBrush(dc, WithAlpha(theme.accent, 0.22f * mtFade), brFillInput_);
                    FillRoundedRect(dc, brFillInput_.get(), bg.left + inset, bg.top + scale_,
                        std::max(0.0f, bg.right - bg.left - inset * 2),
                        std::max(0.0f, bg.bottom - bg.top - scale_ * 2), theme.radius_control * scale_);
                    mtime_flash_until_ = std::max(mtime_flash_until_, mtSeen->second.second + 1200);
                }
            }
            if (draw_shapes && row_flash > 0.0f && !IsHighContrast()) {
                // Newly appeared (pasted / created / extracted) row: fading accent wash.
                MakeBrush(dc, WithAlpha(theme.accent, 0.20f * row_flash), brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), bg.left + inset, bg.top + scale_,
                    std::max(0.0f, bg.right - bg.left - inset * 2),
                    std::max(0.0f, bg.bottom - bg.top - scale_ * 2),
                    theme.radius_control * scale_);
            }
            if (draw_shapes && ai_painter && !IsHighContrast())
                ai_painter->Background(bg, inset, theme.radius_control * scale_);
            if (draw_shapes && src == vm.drop_target_index) {
                D2D1_RECT_F rc = D2D1::RectF(bg.left + inset, bg.top + scale_,
                                             bg.right - inset, bg.bottom - scale_);
                dc->DrawRoundedRectangle(
                    D2D1::RoundedRect(rc, theme.radius_control * scale_, theme.radius_control * scale_),
                    brAccent_.get(), 2.0f * scale_);
                // Spring-loading: a ring fills around the folder icon until
                // the folder opens under the drag.
                if (vm.drop_spring_progress >= 0.0f && !IsHighContrast()) {
                    DrawSpringRing(dc, iconRect, vm.drop_spring_progress, theme);
                    if (vm.drop_spring_progress < 1.0f) spring_ring_until_ = motion_now_ + 60;
                }
            }
            if (draw_shapes && ((i == 0 && vm.focus_first_hint) || src == vm.focus_ring_source) &&
                !selected && pane_focused) {
                // Folder just opened: keyboard focus without a selection, so a
                // stray Ctrl+C / Ctrl+X cannot collect the first item.
                if (!dashStroke_.get()) {
                    ID2D1Factory* factory = nullptr;
                    dc->GetFactory(&factory);
                    if (factory) {
                        D2D1_STROKE_STYLE_PROPERTIES props{};
                        props.dashStyle = D2D1_DASH_STYLE_DASH;
                        props.dashCap = D2D1_CAP_STYLE_FLAT;
                        factory->CreateStrokeStyle(props, nullptr, 0, &dashStroke_);
                        factory->Release();
                    }
                }
                MakeBrush(dc, IsHighContrast() ? theme.text : WithAlpha(theme.accent, 0.75f), brFillInput_);
                const D2D1_RECT_F rc = D2D1::RectF(bg.left + inset, bg.top + scale_,
                                                   bg.right - inset, bg.bottom - scale_);
                dc->DrawRoundedRectangle(
                    D2D1::RoundedRect(rc, theme.radius_control * scale_, theme.radius_control * scale_),
                    brFillInput_.get(), 1.0f * scale_, dashStroke_.get());
            }

            if (draw_shapes && treeRow) {
                if (treeRow->expandable)
                    drawTreeChevron(*treeRow, cell, treeBaseX, hover || selected, i == tree_hi_row);
            }
            if (draw_text && change && iconGrid) {
                const float bw = std::min(ChangeBadgeWidth(*change, scale_, compositor_), cell.right - cell.left - 12 * scale_);
                const float bx = (cell.left + cell.right - bw) * 0.5f;
                DrawChangeBadge(compositor_, painter_, *change, D2D1::RectF(bx, cell.top + 2 * scale_, bx + bw, cell.top + 20 * scale_), theme, scale_);
            }
            row_perf.Next(frame_perf_.row_icons_us, frame_perf_.row_icons_fallbacks);
            bool drewCloudThumbnail = false;  // the cloud badge goes inside a picture
            if (draw_shapes && morph_item) {
                DrawMorphIcon(e, vm, morph_sample, row_dx, row_dy, iconRect,
                              static_cast<ViewMode>(morph->FromMode()), theme);
            } else if (draw_shapes) {
                const float snappedIconW = std::max(1.0f, std::round(iconRect.right - iconRect.left));
                const float snappedIconH = std::max(1.0f, std::round(iconRect.bottom - iconRect.top));
                iconRect.left = std::round(iconRect.left);
                iconRect.top = std::round(iconRect.top);
                iconRect.right = iconRect.left + snappedIconW;
                iconRect.bottom = iconRect.top + snappedIconH;
                const float snappedThumbW = std::max(1.0f, std::round(thumbRect.right - thumbRect.left));
                const float snappedThumbH = std::max(1.0f, std::round(thumbRect.bottom - thumbRect.top));
                thumbRect.left = std::round(thumbRect.left);
                thumbRect.top = std::round(thumbRect.top);
                thumbRect.right = thumbRect.left + snappedThumbW;
                thumbRect.bottom = thumbRect.top + snappedThumbH;
                // Pulse 助手: the icon pops as the row is read, bumps as an item lands.
                struct AiIconScale {
                    ID2D1DeviceContext* dc;
                    D2D1::Matrix3x2F prev;
                    bool on;
                    AiIconScale(ID2D1DeviceContext* d, float k, const D2D1_RECT_F& r) : dc(d), prev(), on(k > 1.001f) {
                        if (!on) return;
                        dc->GetTransform(&prev);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F((r.left + r.right) * 0.5f,
                                                                                      (r.top + r.bottom) * 0.5f)) * prev);
                    }
                    ~AiIconScale() { if (on) dc->SetTransform(prev); }
                    AiIconScale(const AiIconScale&) = delete;
                    AiIconScale& operator=(const AiIconScale&) = delete;
                } ai_icon_scale(dc, ai_painter && !iconGrid ? ai_painter->IconScale() : 1.0f, iconRect);
                // Photo wall picture: the picture fills the tile; until it is
                // decoded a soft plate with the file's icon in the middle.
                bool wallDrawn = false;
                uint32_t wall_duration_ms = 0;
                if (wallPic && !e.record_only) {
                    float aspect = 0.0f;
                    wallDrawn = thumbnail_cache_.DrawWallThumbnail(dc, thumbRect, e.path, e.attrs,
                        ThumbnailRequestPixels(vm.view_mode, std::max(snappedThumbW, snappedThumbH)),
                        vm.view_generation, e.modified_value, e.size_value, hidden_alpha, &aspect,
                        &wall_duration_ms) == PreviewDrawResult::Bitmap;
                    if (aspect > 0.0f && !e.path.empty() &&
                        WallAspects::Instance().Note(WallAspects::ContentHash(e.path, e.modified_value, e.size_value), aspect))
                        RequestMotionFrame();  // the rows reflow next frame
                    if (wallDrawn) {
                        artwork = thumbRect;
                    } else {
                        MakeBrush(dc, WithAlpha(theme.text, 0.06f), brFillInput_);
                        dc->FillRectangle(thumbRect, brFillInput_.get());
                        const float side = std::round(std::min(48.0f * scale_, std::min(snappedThumbW, snappedThumbH) * 0.5f));
                        iconRect.left = thumbRect.left + std::round((snappedThumbW - side) * 0.5f);
                        iconRect.top = thumbRect.top + std::round((snappedThumbH - side) * 0.5f);
                        iconRect.right = iconRect.left + side;
                        iconRect.bottom = iconRect.top + side;
                    }
                }
                const float iconX = iconRect.left;
                const float iconY = iconRect.top;
                // (the wall's pending plate shrank iconRect to its centre icon)
                const float renderedIconSize = wallPic ? std::min(iconRect.right - iconRect.left, iconRect.bottom - iconRect.top)
                                                       : std::min(snappedIconW, snappedIconH);
                uint32_t media_duration_ms = 0;
                // A folder probed empty: neither a contents thumbnail nor the shell icon.
                const bool empty_folder = e.is_dir && !e.record_only && vm.empty_folders.contains(src);
                if (empty_folder) {
                    // Both forms put their ink at x 1/12..11/12, y 1/6..5/6: rest it
                    // on the name baseline like the shell artwork below.
                    const float align_bottom = iconGrid ? renderedIconSize / 6.0f : 0.0f;
                    if (iconGrid)
                        artwork = {iconX + renderedIconSize / 12.0f, iconY + renderedIconSize / 6.0f + align_bottom,
                                   iconX + renderedIconSize * 11.0f / 12.0f, iconRect.bottom};
                    DrawEmptyFolderIcon(iconX, iconY + align_bottom, renderedIconSize, theme, hidden_alpha);
                    artwork_hit_valid = true;
                }
                if (wallDrawn) media_duration_ms = wall_duration_ms;
                const uint32_t thumb_pixels = ThumbnailRequestPixels(vm.view_mode, std::max(snappedThumbW, snappedThumbH));
                if (UsesThumbnails(vm.view_mode) && !wall_rows && !e.is_dir && !e.record_only) prefetch_pixels = thumb_pixels;
                const bool drewThumbnail = wallDrawn || (!wallPic && !empty_folder && !e.record_only && UsesThumbnails(vm.view_mode) &&
                    DrawEntryThumbnail(dc, thumbRect, e, vm.view_mode, vm.view_generation, thumb_pixels,
                        hidden_alpha, iconGrid, &artwork,
                        &media_duration_ms)
                        == PreviewDrawResult::Bitmap);
                drewCloudThumbnail = drewThumbnail;
                artwork_hit_valid = artwork_hit_valid || drewThumbnail;
                // Photos and videos look alike as thumbnails: say which program
                // opens the file, and how long a video plays (like Explorer).
                if (drewThumbnail && thumbnail_badges_ && iconGrid && !e.is_dir && !e.is_link)
                    DrawThumbnailBadges(e, artwork, renderedIconSize, media_duration_ms, theme);
                if (!drewThumbnail && !empty_folder) {
                    // Change records and 网盘程序入口 draw their own icon (DrawEntryIcon).
                    const bool own_icon = e.record_only || e.launcher_rgb != 0 || e.device != 0 || e.net_kind != 0;
                    ID2D1Bitmap* grid_bitmap = iconGrid && !morphing && !own_icon
                        ? icon_cache_.BitmapFor(e.path, e.name, e.is_dir, e.attrs, renderedIconSize) : nullptr;
                    artwork_hit_valid = grid_bitmap != nullptr;
                    float align_bottom = 0.0f;
                    // An icon with only a small image is drawn at most twice its
                    // size, centered, instead of filling the cell.
                    float icon_side = renderedIconSize;
                    float icon_inset = 0.0f;
                    if (iconGrid && !own_icon) {
                        const auto ink = icon_cache_.CachedArtworkBounds(e.path, e.name, e.is_dir, e.attrs, renderedIconSize);
                        icon_side = IconDrawSide(renderedIconSize,
                            icon_cache_.CachedNativeSide(e.path, e.name, e.is_dir, e.attrs, renderedIconSize));
                        icon_inset = std::round((renderedIconSize - icon_side) * 0.5f);
                        align_bottom = renderedIconSize - icon_side * ink.bottom;
                        artwork = {iconRect.left + icon_inset + ink.left * icon_side,
                            iconRect.top + ink.top * icon_side + align_bottom,
                            iconRect.left + icon_inset + ink.right * icon_side, iconRect.bottom};
                    }
                    // Align the actual artwork, not the transparent bitmap square,
                    // to the name baseline. The name/edit/hit rectangles stay fixed.
                    const RowShiftTransform artwork_transform(dc, 0.0f, align_bottom);
                    // An item already settled while others still glide: no
                    // synchronous conversion burst in the tail frames; the
                    // first static frame after the morph converts the rest.
                    ID2D1Bitmap* settled = grid_bitmap ? grid_bitmap : morphing && !own_icon
                        ? icon_cache_.CachedBitmapFor(e.path, e.name, e.is_dir, e.attrs, renderedIconSize)
                        : nullptr;
                    if (!settled && morphing && !own_icon)
                        icon_cache_.Prefetch(e.path, e.name, e.is_dir, e.attrs, renderedIconSize);
                    if (settled) {
                        const D2D1_RECT_F dest = D2D1::RectF(iconX + icon_inset, iconY,
                                                             iconX + icon_inset + icon_side, iconY + icon_side);
                        dc->DrawBitmap(settled, &dest, hidden_alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                       nullptr, nullptr);
                    } else if (morphing && !own_icon) {
                        if (e.is_dir) DrawFolderIcon(iconX, iconY, renderedIconSize, theme, hidden_alpha);
                        else DrawFileIcon(iconX, iconY, renderedIconSize, theme, hidden_alpha);
                    } else {
                        DrawEntryIcon(e, iconX, iconY, renderedIconSize, theme, hidden_alpha);
                    }
                }
            }
            if (draw_shapes && iconGrid) {
                item_artwork_hits_.Remember({vm.view_generation,src,static_cast<int>(vm.view_mode),
                    e.path,e.name,e.modified_value,e.size_value,cell},artwork_hit_valid ? artwork : D2D1_RECT_F{});
            }
            if (draw_shapes && ai_painter && !morph_item && !iconGrid) {
                ai_painter->IconDecor(iconRect);
                ai_icon_dx = iconRect.left - cell.left;
                ai_icon_size = iconRect.right - iconRect.left;
            }
            if (draw_shapes && !morph_item && !e.record_only && e.is_link) {
                const bool expand_on_icon = iconGrid;
                // A collapsed pill shows no text: skip building and measuring it every frame.
                const bool show_caption = expand_on_icon && link_expansion > 0.0f;
                const std::wstring caption = !show_caption ? std::wstring()
                    : e.link_destination.find(L"://") != std::wstring::npos
                    ? link_label : FileNameOf(link_label);
                DrawLinkOverlay(iconRect.left, iconRect.top,
                    std::min(iconRect.right - iconRect.left, iconRect.bottom - iconRect.top), theme,
                    cut ? 0.55f : hidden_alpha, show_caption ? caption : std::wstring(),
                    show_caption ? link_expansion : 0.0f, cell.right - 8.0f * scale_, &artwork);
            }
            if (draw_shapes && !morph_item && !e.record_only && IsProtectedSystemEntry(e.attrs))
                DrawProtectedBadge(dc, iconGrid ? artwork : iconRect, scale_, theme);
            // Cloud folders: where each item's content lives, as in the 状态 column.
            // On pictures the badge takes the top-left corner (DrawCloudBadge).
            bool cloudTopLeft = false;
            if (draw_shapes && !morph_item && !e.record_only && !e.is_link && vm.cloud_status_column &&
                (iconGrid || wallPic)) {
                const bool picture = wallPic || (drewCloudThumbnail && !e.is_dir);
                const D2D1_RECT_F cloudArt = wallPic ? thumbRect : artwork;
                DrawCloudBadge(e, cloudArt, picture, theme, cut ? 0.55f : hidden_alpha);
                cloudTopLeft = picture && std::min(cloudArt.right - cloudArt.left, cloudArt.bottom - cloudArt.top) >= 24.0f * scale_;
            }
            if (draw_shapes && !morph_item && mtimeMarked && mtime.other_views && mtime.style != 3 &&
                vm.view_mode != ViewMode::Details) {
                // Views without a date column: the mark on the icon's top-left
                // corner (links and protected badges use the bottom corners),
                // top-right when a picture's cloud badge holds the top-left.
                const D2D1_RECT_F mtArt = iconGrid ? artwork : iconRect;
                const float mtSide = std::clamp((mtArt.right - mtArt.left) * 0.2f, 7.0f * scale_, 12.0f * scale_);
                const float mtX = cloudTopLeft ? mtArt.right - mtSide * 0.5f - scale_ : mtArt.left + mtSide * 0.5f + scale_;
                const float mtY = mtArt.top + mtSide * 0.5f + scale_;
                D2D1_COLOR_F mtHalo = theme.bg;
                mtHalo.a = 0.92f;
                MakeBrush(dc, mtHalo, brTagDot_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mtX, mtY), mtSide * 0.5f + 1.5f * scale_,
                                              mtSide * 0.5f + 1.5f * scale_), brTagDot_.get());
                MakeBrush(dc, mtimeColor, brTagDot_);
                DrawMtimeMark(dc, brTagDot_.get(), mtX, mtY, mtSide, mtimeMark, mtimeColor);
            }
            if (draw_shapes && wallPic && !morph_item) DrawWallTileChrome(thumbRect, hover && src != vm.rename_index, selected, theme);
            // Item check boxes: on the hovered and selected rows, on every row
            // while several are selected.
            if (draw_shapes && item_checks_ && !morph_item && !morphing && src != vm.rename_index &&
                (hover || selected || vm.selected_count >= 2)) {
                const D2D1_RECT_F box = ItemCheckRect(vm, cell, checkBaseX);
                if (box.right > box.left) {
                    if (!detailsView) {
                        // Icon grids: a ring in the card colour keeps it off the thumbnail.
                        MakeBrush(dc, theme.bg, brTagDot_);
                        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left - 2.0f * scale_,
                            box.top - 2.0f * scale_, box.right + 2.0f * scale_, box.bottom + 2.0f * scale_),
                            6.0f * scale_, 6.0f * scale_), brTagDot_.get());
                    }
                    EnsureRoundCapStroke(dc, roundCapStroke_);
                    MakeBrush(dc, theme.accent, brTagDot_);
                    // Shown only because several are selected: faint until hovered.
                    DrawItemCheckBox(dc, brTagDot_.get(), roundCapStroke_.get(), box, scale_, theme, IsHighContrast(),
                                     selected ? 1 : 0, hover, check_tick, !hover && !selected ? 0.5f : 1.0f);
                }
            }
            // Mid-morph the labels are drawn in a second, faded pass.
            row_perf.Next(frame_perf_.row_text_us, frame_perf_.row_text_fallbacks);
            if (!draw_text) continue;

            float nameX = nameRc.left;
            float textY = nameRc.top;
            float textH = std::max(1.0f, nameRc.bottom - nameRc.top);
            const float dot = 8.0f * scale_;
            const std::vector<D2D1_COLOR_F>* tagDots = nullptr;
            const std::vector<int>* tagIndices = vm.tag_catalog
                ? vm.tag_catalog->TagIndicesForPath(e.path) : nullptr;
            if (!tagIndices && vm.tag_dots) {
                const auto tagIt = vm.tag_dots->find(src);
                if (tagIt != vm.tag_dots->end()) tagDots = &tagIt->second;
            }
            const size_t totalTags = tagIndices ? tagIndices->size() : (tagDots ? tagDots->size() : 0);
            const int tagDotCount = static_cast<int>(std::min<size_t>(3, totalTags));
            // Column view rows: name and a chevron on folders, no row actions.
            const bool stripRow = vm.view_mode == ViewMode::Details && vm.column_strip.Active();
            const bool stripChevron = stripRow && e.is_dir;
            const bool showRowActions = vm.view_mode == ViewMode::Details && !stripRow &&
                src != vm.rename_index &&
                (hover || (selected && vm.selected_count == 1) || e.starred);
            // OneDrive: 1 online only, 2 on this device, 3 always kept, 4 waiting
            // to sync, 5 syncing, 6 sync error, 7 warning, 8 not synced. Details
            // rows end their name column with it; the hover buttons take its place.
            int cloudState = 0;
            if (vm.onedrive_status && !e.record_only && vm.view_mode == ViewMode::Details && !stripRow) {
                // STORAGEPROVIDERSTATE_* (propkey.h) to the states above.
                static constexpr int kShellCloudState[11] = {0, 1, 2, 3, 4, 4, 5, 6, 7, 8, 4};
                const uint8_t shellState = vm.OneDriveStateAt(src);
                if (shellState > 0 && shellState <= 10) cloudState = kShellCloudState[shellState];
                else if (e.attrs & files::kAttributePinned) cloudState = 3;
                else if (e.cloud_recall || files::PreviewWouldRecall(e.attrs))   // recall bits or offline
                    cloudState = 1;
                else cloudState = 2;
            }
            const float cloudReserve = cloudState != 0 && !showRowActions ? 20.0f * scale_ : 0.0f;
            // This PC tiles: the used percent ends the name row, over the bar's end.
            const bool driveTile = vm.view_mode == ViewMode::Tiles && !e.device && e.drive_used >= 0.0f;
            const std::wstring drivePercent = driveTile ? CapacityPercentText(e.drive_used) : std::wstring();
            const float driveBarW = driveTile ? DriveTileBarWidth(nameRc, scale_) : 0.0f;
            const float drivePercentW = driveTile
                ? MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->SmallFormat(), drivePercent)
                : 0.0f;
            // A lone name column has no divider: it ends at the layout's right.
            const float nameColRight = driveTile
                ? std::max(nameRc.left, nameRc.left + driveBarW - drivePercentW - 8.0f * scale_)
                : (vm.view_mode == ViewMode::Details
                ? (stripRow ? detailsColumns.right : detailsColumns.DividerX(0) - margin_)
                : nameRc.right) - (stripChevron ? 18.0f * scale_ : 0.0f) - cloudReserve;
            if (stripChevron) {
                DrawIconText(nameColRight + 2.0f * scale_, cell.top, 18.0f * scale_, cell.bottom - cell.top,
                             kIconChevronRight, L">", theme.text_secondary, 0.55f);
            }
            const bool showStar = showRowActions;
            const bool showNewTab = showRowActions && hover && e.is_dir;
            const bool showMore = showRowActions && (hover || (selected && vm.selected_count == 1));
            const bool rowSnippet = !e.snippet.empty() && vm.view_mode == ViewMode::Details;
            // Narrow search panes show the folder under the name instead of a column.
            const bool rowPathLine = !rowSnippet && detailsView && detailsColumns.two_line;
            DetailsNameLine nameLine = MakeDetailsNameLine(nameRc, cell, scale_, rowSnippet || rowPathLine);
            if (rowSnippet || rowPathLine) {
                textY = nameLine.y;
                textH = nameLine.h;
            }
            // 文件扩展名 off: a known type is drawn without its extension.
            const std::wstring shown_name = ShownName(e);
            // 别名: the alias leads and the real name follows, quieter.
            const bool has_alias = !e.alias.empty();
            // 只显示别名 drops the real name (the tooltip still has it).
            const bool alias_both = has_alias && app::AliasStore::Instance().Display() == app::AliasDisplay::Both;
            const std::wstring alias_line = !has_alias ? std::wstring()
                : alias_both ? e.alias + L"\u2003\u2003" + shown_name : e.alias;
            static const std::vector<NameMatchRange> kNoAliasMatches;
            const auto name_matches = has_alias ? kNoAliasMatches : NameMatchRanges(shown_name, highlight_terms);
            if (iconGrid && tagDotCount > 0) {
                const float fullNameW = MeasureLayoutText(
                    compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(), shown_name) +
                    HighlightPaddingWidth(shown_name, shown_name, name_matches, scale_);
                const float nameGap = 4.0f * scale_;
                const float cellW = nameRc.right - nameRc.left;
                const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
                const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
                    ? SpreadTagsWidth(tagDotCount, dot, nameGap)
                    : OverlapTagsWidth(tagDotCount, dot);
                const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
                nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
                textY = nameRc.top;
                textH = std::min(textH, 24.0f * scale_);
            }
            const bool inline_link = !iconGrid && e.is_link && link_expansion > 0.0f &&
                src != vm.rename_index && !change && e.badge.empty();
            // "已暂存" pill (tray redesign): takes the badge slot when nothing else uses it.
            const bool staged_pill = vm.view_mode == ViewMode::Details && !change && e.badge.empty() &&
                vm.staged && !vm.staged->empty() && vm.staged->contains(StagedPathHash(e.path));
            const std::wstring& staged_text = l10n::Get(l10n::StringId::TrayStagedBadge);
            // In-app Git: a changed file gets its state pill when the slot is free.
            const GitMark gitMark = vm.GitMarkAt(src);
            const bool git_pill = vm.view_mode == ViewMode::Details && !e.is_dir && gitMark != GitMark::None &&
                !change && e.badge.empty() && !staged_pill;
            // Phone tiles: 手机 / iPhone + 只读 / 未解锁 chips after the name.
            std::wstring device_chip;
            std::wstring device_chip2;
            if (e.device && vm.view_mode == ViewMode::Tiles && !(e.device & fs::kPcDevicePlaceholder)) {
                if (e.device & fs::kPcDeviceLocked) device_chip = l10n::Get(l10n::StringId::DeviceLockedChip);
                else if (e.device & fs::kPcDeviceIPhone) {
                    device_chip = L"iPhone";
                    device_chip2 = l10n::Get(l10n::StringId::DeviceReadOnlyChip);
                } else device_chip = l10n::Get(l10n::StringId::DeviceKindPhone);
            }
            const float device_chip_w = device_chip.empty() ? 0.0f : painter_.MeasureTagWidth(device_chip);
            const float device_chip2_w = device_chip2.empty() ? 0.0f : painter_.MeasureTagWidth(device_chip2);
            const float badgeW = device_chip_w > 0.0f ? device_chip_w + (device_chip2_w > 0.0f ? 4.0f * scale_ + device_chip2_w : 0.0f)
                : change && !iconGrid ? ChangeBadgeWidth(*change, scale_, compositor_) : vm.view_mode == ViewMode::Details && !e.badge.empty()
                ? std::min(108.0f * scale_, painter_.MeasureTagWidth(e.badge))
                : staged_pill ? std::min(108.0f * scale_, painter_.MeasureTagWidth(staged_text))
                : git_pill ? std::min(108.0f * scale_, painter_.MeasureTagWidth(GitMarkText(gitMark))) : 0.0f;
            // 今天修改的名称加粗: measured and drawn at semibold, so tags and pills still follow it.
            IDWriteTextFormat* nameFormat = compositor_->FileNameFormat();
            if (mtimeFresh && !iconGrid && !wallPic && !has_alias && src != vm.rename_index)
                if (IDWriteTextFormat* semibold = NameSemiboldFormat()) nameFormat = semibold;
            // List rows: [mark] alias  real name.
            const bool show_mark = has_alias && app::AliasStore::Instance().ShowMark();
            const bool list_mark = show_mark && !iconGrid;
            // A batch rename's new-name pill takes its room from the name (≤ 60%).
            const float ai_reserve = ai_painter && !iconGrid
                ? (std::min)(ai_painter->AfterNameReserve(), (nameColRight - nameX) * 0.6f) : 0.0f;
            const NameTrail trail = LayoutNameTrail(
                list_mark ? nameX + kListAliasMarkDip * scale_ : nameX, textY, textH, nameColRight - ai_reserve, cell.top, cell.bottom, scale_,
                has_alias ? alias_line : shown_name, tagDotCount, badgeW, showStar, showNewTab, showMore,
                compositor_, compositor_->DwriteFactory(), nameFormat, change != nullptr,
                vm.view_mode == ViewMode::Details ? (e.is_dir ? 3 : 2) : 0, name_matches, row_actions_);
            if (src == vm.rename_index) {
                // The rename frame is drawn after every row (below): it can be
                // wider than its own cell.
            } else {
                D2D1_COLOR_F nameColor = theme.text;
                if (list_tag_names_ && tagDotCount > 0 && !IsHighContrast()) {
                    // First tag tints the name, pulled toward the text color so
                    // yellow on light and deep blue on dark stay readable.
                    D2D1_COLOR_F tint{};
                    bool have = false;
                    if (tagIndices && !tagIndices->empty()) {
                        const int tagIndex = (*tagIndices)[0];
                        if (tagIndex >= 0 && tagIndex < static_cast<int>(vm.tag_catalog->tags.size())) {
                            tint = HexColor(vm.tag_catalog->tags[static_cast<size_t>(tagIndex)].rgb);
                            have = true;
                        }
                    } else if (tagDots && !tagDots->empty()) {
                        tint = (*tagDots)[0];
                        have = true;
                    }
                    if (have) {
                        const float k = theme.bg.r < 0.5f ? 0.22f : 0.30f;
                        const D2D1_COLOR_F base = theme.text;
                        nameColor = D2D1::ColorF(tint.r + (base.r - tint.r) * k,
                                                 tint.g + (base.g - tint.g) * k,
                                                 tint.b + (base.b - tint.b) * k, 1.0f);
                    }
                }
                if (gitMark != GitMark::None && !IsHighContrast())
                    nameColor = GitMarkColor(gitMark, theme.bg.r < 0.5f);
                if (cut) nameColor = WithAlpha(nameColor, 0.55f);
                else if (!IsHighContrast()) nameColor = HiddenEntryNameColor(e.attrs, theme, nameColor);
                if (rename_covers && nameRc.left < rename_card.right && nameRc.right > rename_card.left &&
                    nameRc.top < rename_card.bottom && nameRc.bottom > rename_card.top)
                    nameColor.a *= 0.38f;
                MakeBrush(dc, nameColor, brText_);
                // Single click opens: a highlighter band under the name's lower
                // half on hover (or a resting 2 px line with "always").
                if (click_single_ && !iconGrid && !wallPic && trail.name_w > 0.0f && (hover || click_band_ == 1)) {
                    const float mid = std::round(textY + textH * 0.5f);
                    D2D1_RECT_F band = D2D1::RectF(trail.name_x - scale_, mid - scale_,
                                                   trail.name_x + trail.name_w + scale_, mid + 6.0f * scale_);
                    if (IsHighContrast()) {
                        band.top = band.bottom - scale_;
                        MakeBrush(dc, theme.accent, brTagDot_);
                    } else {
                        if (!hover) band.top = band.bottom - 2.0f * scale_;
                        // Dark backgrounds swallow a thin tint: a little more there.
                        MakeBrush(dc, WithAlpha(theme.accent, theme.bg.r < 0.5f ? 0.34f : 0.24f), brTagDot_);
                    }
                    dc->FillRectangle(band, brTagDot_.get());
                }
                bool name_truncated = true;
                if (wallPic) {
                    // Photo wall: no names under the pictures; hovered or
                    // selected ones show theirs on the picture (prototype .ov).
                    name_truncated = true;  // otherwise only the tooltip names it
                    if (hover || selected || IsHighContrast()) {  // high contrast: names always
                        Theme on_photo = theme;
                        if (!IsHighContrast()) on_photo.text = D2D1::ColorF(1.0f, 1.0f, 1.0f, cut ? 0.6f : 1.0f);
                        const float line = std::min(18.0f * scale_, thumbRect.bottom - thumbRect.top);
                        MakeBrush(dc, on_photo.text, brText_);
                        name_truncated = DrawTruncatedName(has_alias ? alias_line : shown_name, thumbRect.left + 6.0f * scale_,
                            thumbRect.bottom - 4.0f * scale_ - line, std::max(1.0f, thumbRect.right - thumbRect.left - 12.0f * scale_),
                            line, on_photo, false, name_matches, false);
                    }
                } else if (iconGrid && tagDotCount == 0) {
                    // A folder showing its size keeps the last line for it.
                    const bool size_line = e.is_dir && vm.folder_size_labels.contains(src);
                    const int name_lines = wall_rows ? 1 : layout.Metrics().name_lines;
                    DrawCenteredIconName(has_alias ? e.alias : shown_name, nameRc, nameColor, theme, name_matches,
                                         &name_truncated, size_line ? std::max(1, name_lines - 1) : name_lines,
                                         nullptr, !e.is_dir && !has_alias && !e.hidden_ext);
                    // The single selected item shows its whole name on top of
                    // the rows below (drawn after every row), a folder's size
                    // under it.
                    if (name_truncated && selected && vm.selected_count == 1 && !vm.marquee_active &&
                        !morphing) {
                        expanded_name.valid = true;
                        expanded_name.source = src;
                        expanded_name.name = has_alias ? e.alias : shown_name;
                        expanded_name.dim_extension = !e.is_dir && !has_alias && !e.hidden_ext;
                        expanded_name.bounds = D2D1::RectF(nameRc.left + row_dx, nameRc.top + row_dy,
                                                           nameRc.right + row_dx, nameRc.bottom + row_dy);
                        expanded_name.color = nameColor;
                        expanded_name.matches = name_matches;
                        expanded_name.note.clear();
                        if (size_line) {
                            const auto size = vm.folder_size_labels.find(src);
                            expanded_name.note = size->second;
                            const bool quiet = vm.folder_size_idle.contains(src) ||
                                vm.folder_size_muted.contains(src) || size->second == folder_size_scanning;
                            expanded_name.note_color = cut ? WithAlpha(quiet ? theme.text_secondary : theme.text, 0.55f)
                                                           : (quiet ? theme.text_secondary : theme.text);
                        }
                    }
                } else if (has_alias) {
                    if (show_mark) {
                        const float m = kAliasMarkDip * scale_;
                        const float mx = std::round(trail.name_x - kListAliasMarkDip * scale_);
                        const float my = std::round(textY + (textH - m) * 0.5f);
                        painter_.DrawAliasMark(D2D1::RectF(mx, my, mx + m, my + m));
                    }
                    Theme name_theme = theme;
                    name_theme.text = nameColor;
                    const float alias_w = MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
                                                            compositor_->FileNameFormat(), e.alias) + 1.0f;
                    const float shown_w = std::min(trail.name_w, alias_w);
                    DrawTruncatedName(e.alias, trail.name_x, textY, shown_w, textH, name_theme,
                                      selected, kNoAliasMatches, false);
                    const float real_x = trail.name_x + shown_w + 8.0f * scale_;
                    const float real_w = trail.name_x + trail.name_w - real_x;
                    if (alias_both && real_w >= 28.0f * scale_) {
                        // The real name steps back (the faint color, a size
                        // smaller) so the alias reads first.
                        IDWriteTextFormat* small_format = compositor_->SmallFormat();
                        IDWriteFactory2* dwrite = compositor_->DwriteFactory();
                        const D2D1_COLOR_F faint = WithAlpha(theme.text_secondary, cut ? 0.35f : 0.62f);
                        ComPtr<IDWriteTextLayout> real_layout;
                        if (small_format && dwrite) {
                            const std::wstring fitted = FitHighlightedFileName(compositor_, dwrite, small_format, shown_name,
                                                                               real_w, kNoAliasMatches, scale_);
                            if (SUCCEEDED(dwrite->CreateTextLayout(fitted.c_str(), static_cast<UINT32>(fitted.size()),
                                                                    small_format, real_w, textH, &real_layout))) {
                                real_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                                real_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                real_layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                MakeBrush(dc, faint, brText_);
                                compositor_->DrawTextLayout({real_x, textY}, real_layout.get(), brText_.get(),
                                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            }
                        }
                        if (!real_layout.get()) {
                            Theme real_theme = theme;
                            real_theme.text = faint;
                            DrawTruncatedName(shown_name, real_x, textY, real_w, textH, real_theme,
                                              selected, kNoAliasMatches, false);
                        }
                    }
                    name_truncated = true;   // the row tooltip names the real item
                } else {
                    Theme name_theme = theme;
                    name_theme.text = nameColor;
                    name_semibold_ = nameFormat != compositor_->FileNameFormat();
                    const AiListPainter::NameMorph nm =
                        ai_painter && !iconGrid ? ai_painter->Morph() : AiListPainter::NameMorph{};
                    if (nm.was) {
                        // An assistant rename on its way: the old name until its turn.
                        name_truncated = DrawTruncatedName(*nm.was, trail.name_x, textY, trail.name_w, textH, name_theme,
                                                           selected, {}, detailsView && !e.is_dir && !e.hidden_ext);
                    } else if (nm.alpha < 1.0f) {
                        // ...then the new one rises into place (inside the row).
                        D2D1_MATRIX_3X2_F before;
                        dc->GetTransform(&before);
                        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(trail.name_x - 2.0f * scale_, cell.top,
                                                                         trail.name_x + trail.name_w + 2.0f * scale_, cell.bottom),
                                                             nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(),
                                                             std::max(nm.alpha, 1.0f / 255.0f), nullptr,
                                                             D2D1_LAYER_OPTIONS1_NONE),
                                      nullptr);
                        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, nm.dy) * before);
                        name_truncated = DrawTruncatedName(shown_name, trail.name_x, textY, trail.name_w, textH, name_theme,
                                                           selected, name_matches, detailsView && !e.is_dir && !e.hidden_ext);
                        dc->SetTransform(before);
                        dc->PopLayer();
                    } else {
                        name_truncated = DrawTruncatedName(shown_name, trail.name_x, textY, trail.name_w, textH, name_theme,
                                                           selected, name_matches, detailsView && !e.is_dir && !e.hidden_ext);
                    }
                    name_semibold_ = false;
                    // Icon grids with tag dots keep the name on one line; the
                    // single selected item still shows all of it.
                    if (iconGrid && !wallPic && name_truncated && selected && vm.selected_count == 1 &&
                        !vm.marquee_active && !morphing) {
                        expanded_name.valid = true;
                        expanded_name.source = src;
                        expanded_name.name = shown_name;
                        expanded_name.dim_extension = !e.is_dir && !e.hidden_ext;
                        expanded_name.bounds = D2D1::RectF(nameRc.left + row_dx, nameRc.top + row_dy,
                                                           nameRc.right + row_dx, nameRc.bottom + row_dy);
                        expanded_name.color = nameColor;
                        expanded_name.matches = name_matches;
                        expanded_name.note.clear();
                    }
                }
                // The row tooltip repeats the name only when it is cut off (B站 #15).
                if (hover && pane_index >= 0 && pane_index < static_cast<int>(hover_names_.size()))
                    hover_names_[static_cast<size_t>(pane_index)] = {src, name_truncated};
                if (change && !iconGrid) DrawChangeBadge(compositor_, painter_, *change, trail.badge, theme, scale_);
                if (cloudReserve > 0.0f) {
                    const bool darkTheme = theme.bg.r < 0.5f;
                    const D2D1_COLOR_F blue = darkTheme ? HexColor(0x62ABF5) : HexColor(0x0F6CBD);
                    const D2D1_COLOR_F green = darkTheme ? HexColor(0x6CCB5F) : HexColor(0x107C10);
                    const D2D1_COLOR_F red = darkTheme ? HexColor(0xFF99A4) : HexColor(0xC42B1C);
                    const D2D1_COLOR_F amber = darkTheme ? HexColor(0xFCE100) : HexColor(0x9D5D00);
                    const D2D1_COLOR_F inks[] = {blue, blue, green, green, theme.text_secondary, blue, red, amber,
                                                 theme.text_secondary};
                    const D2D1_COLOR_F ink = IsHighContrast() ? theme.text : inks[std::clamp(cloudState, 0, 8)];
                    // Cloud outline / circled check / solid circled check / sync arrows /
                    // error / warning / blocked, as File Explorer shows them.
                    static constexpr const wchar_t* kCloudGlyphs[] = {L"\xE753", L"\xE753", L"\xE930", L"\xEC61",
                        L"\xE895", L"\xE895", L"\xEB90", L"\xE7BA", L"\xE733"};
                    const wchar_t* glyph = kCloudGlyphs[std::clamp(cloudState, 0, 8)];
                    DrawIconText(nameColRight + 2.0f * scale_, cell.top, 16.0f * scale_, cell.bottom - cell.top,
                                 glyph, L"", cut ? WithAlpha(ink, 0.55f) : ink, 0.78f);
                }
                const float link_left = (trail.tag_n > 0
                    ? trail.tag_x0 + trail.tag_r * 2 + (trail.tag_n - 1) * trail.tag_step
                    : trail.name_x + trail.name_w) + 6.0f * scale_;
                const float link_right = ai_reserve > 0.0f ? (std::min)(link_left + ai_reserve, nameColRight)
                    : std::min(link_left + 220.0f * scale_, trail.name_x + trail.line_w);
                if (ai_painter && !iconGrid) {
                    ai_painter->AfterName(link_left, link_right, textY, textH);
                    ai_name_dx = trail.name_x - cell.left;
                }
                if (inline_link && link_right - link_left >= 36.0f * scale_) {
                    const float diameter = std::min(18.0f * scale_, textH);
                    const auto anchor = D2D1::RectF(link_left, textY + (textH - diameter) * 0.5f,
                        link_left + diameter, textY + (textH + diameter) * 0.5f);
                    DrawLinkOverlay(anchor.left, anchor.top, diameter / 0.30f, theme,
                        link_expansion, link_label, link_expansion, link_right, &anchor);
                }
                if (!change && !e.badge.empty() && trail.badge.right > trail.badge.left) {
                    painter_.DrawTag({trail.badge, e.badge, e.badge_color});
                }
                if (device_chip_w > 0.0f && trail.badge.right > trail.badge.left) {
                    const bool locked = (e.device & fs::kPcDeviceLocked) != 0;
                    fluent::TagSpec chip;
                    chip.bounds = trail.badge;
                    chip.bounds.right = std::min(trail.badge.right, trail.badge.left + device_chip_w);
                    chip.text = device_chip;
                    chip.color = locked ? HexColor(theme.bg.r < 0.5f ? 0xE3A846u : 0xB7791Fu) : theme.accent;
                    chip.bordered = false;
                    painter_.DrawTag(chip);
                    if (device_chip2_w > 0.0f && chip.bounds.right + 4.0f * scale_ + device_chip2_w <= trail.badge.right + 0.5f) {
                        fluent::TagSpec plain = chip;
                        plain.bounds.left = chip.bounds.right + 4.0f * scale_;
                        plain.bounds.right = plain.bounds.left + device_chip2_w;
                        plain.text = device_chip2;
                        plain.color = theme.text_secondary;
                        painter_.DrawTag(plain);
                    }
                }
                if (staged_pill && trail.badge.right > trail.badge.left) {
                    fluent::TagSpec pill;
                    pill.bounds = trail.badge;
                    pill.text = staged_text;
                    pill.color = theme.accent;
                    pill.bordered = false;
                    painter_.DrawTag(pill);
                }
                if (git_pill && trail.badge.right > trail.badge.left) {
                    fluent::TagSpec pill;
                    pill.bounds = trail.badge;
                    pill.text = GitMarkText(gitMark);
                    pill.color = GitMarkColor(gitMark, theme.bg.r < 0.5f);
                    pill.bordered = false;
                    painter_.DrawTag(pill);
                }
                D2D1_COLOR_F halo = theme.bg;
                halo.a = 1.0f;
                for (int d = trail.tag_n - 1; d >= 0; --d) {
                    D2D1_COLOR_F color{};
                    if (tagIndices && d < static_cast<int>(tagIndices->size())) {
                        const int tagIndex = (*tagIndices)[static_cast<size_t>(d)];
                        if (tagIndex >= 0 && tagIndex < static_cast<int>(vm.tag_catalog->tags.size()))
                            color = HexColor(vm.tag_catalog->tags[static_cast<size_t>(tagIndex)].rgb);
                    } else if (tagDots) {
                        color = (*tagDots)[static_cast<size_t>(d)];
                    }
                    const float cx = trail.tag_x0 + trail.tag_r + static_cast<float>(d) * trail.tag_step;
                    const float cy = trail.tag_cy;
                    MakeBrush(dc, halo, brFillInput_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy),
                        trail.tag_r + 1.5f * scale_, trail.tag_r + 1.5f * scale_), brFillInput_.get());
                    // Dedicated brush: reusing brAccent_ leaked tag color into the
                    // next row's selection emphasis strip.
                    MakeBrush(dc, color, brTagDot_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), trail.tag_r, trail.tag_r),
                                    brTagDot_.get());
                    if (IsHighContrast()) {
                        MakeBrush(dc, theme.text, brText_);
                        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), trail.tag_r, trail.tag_r),
                                        brText_.get(), 1.0f * scale_);
                    }
                }
                if (rowSnippet) {
                    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), e.snippet,
                        trail.name_x, nameLine.snippet_y, trail.line_w, nameLine.snippet_h);
                } else if (rowPathLine) {
                    const float lineW = std::max(0.0f, nameColRight - trail.name_x);
                    const auto [head, last] = MiddleEllipsisPath(FolderOf(e.path), lineW,
                        [&](const std::wstring& s) { return CellTextWidth(s, true); });
                    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                        FitEndEllipsis(head + last, lineW, [&](const std::wstring& s) { return CellTextWidth(s, true); }),
                        trail.name_x, nameLine.snippet_y, lineW, nameLine.snippet_h);
                }
            }

            row_perf.Next(frame_perf_.row_columns_us, frame_perf_.row_columns_fallbacks);
            MakeBrush(dc, cut ? WithAlpha(theme.text_secondary, 0.55f) : theme.text_secondary, brTextSecondary_);
            if (vm.view_mode == ViewMode::Details) {
                const auto draw_detail_text = [&](std::wstring_view text, float left, float width,
                                                  DWRITE_TEXT_ALIGNMENT alignment) {
                    // Every metadata column centers in the row, independently of
                    // the filename's optional two-line name/snippet layout.
                    const auto text_bounds = D2D1::RectF(left, cell.top + scale_, left + width, cell.bottom - scale_);
                    if (!IsHighContrast() &&
                        typography::MeasureLine(compositor_, compositor_->FileNameFormat(), text) <= width &&
                        compositor_->DrawLumaText(
                            text, compositor_->FileNameFormat(), text_bounds,
                            brTextSecondary_->GetColor(), theme.bg, alignment)) {
                        return;
                    }
                    auto* format = compositor_->FileNameFormat();
                    const auto previous = format->GetTextAlignment();
                    const auto previous_paragraph = format->GetParagraphAlignment();
                    format->SetTextAlignment(alignment);
                    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DrawTextEndEllipsis(compositor_, format, brTextSecondary_.get(),
                        std::wstring(text), left, text_bounds.top, width, text_bounds.bottom - text_bounds.top);
                    format->SetTextAlignment(previous);
                    format->SetParagraphAlignment(previous_paragraph);
                };
                const float textInset = 8.0f * scale_;
                auto fit = [&](const std::wstring& text, float width) {
                    return FitEndEllipsis(text, width, [&](const std::wstring& s) { return CellTextWidth(s); });
                };
                const auto secondary = brTextSecondary_->GetColor();
                for (int col = 1; col < detailsColumns.count; ++col) {
                    const float colX = detailsColumns.DividerX(col - 1);
                    const float colW = detailsColumns.widths[static_cast<size_t>(col)];
                    const float left = colX + textInset;
                    const float avail = std::max(0.0f, colW - textInset * 2.0f);
                    if (avail <= 1.0f) continue;
                    if (vm.this_pc_columns) {
                        const ColumnKind pc_kind = detailsColumns.kinds[static_cast<size_t>(col)];
                        if ((e.device & fs::kPcDevicePlaceholder) && !e.drive_row) {
                            // 没有连接的手机: how to connect one, across the columns.
                            if (col == 1) {
                                const float pc_span = std::max(0.0f, detailsColumns.right - left - textInset);
                                draw_detail_text(fit(e.type_text, pc_span), left, pc_span, DWRITE_TEXT_ALIGNMENT_LEADING);
                            }
                            continue;
                        }
                        if (pc_kind == ColumnKind::Date || pc_kind == ColumnKind::Size || pc_kind == ColumnKind::Total) {
                            if (!e.drive_row) continue;
                            const DWRITE_TEXT_ALIGNMENT pc_align = pc_kind == ColumnKind::Date
                                ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_TRAILING;
                            if (e.drive_total == 0 || e.drive_used < 0.0f) {
                                // No media, a locked volume, a share that did not answer.
                                static const std::wstring kDash = L"\u2014";
                                draw_detail_text(kDash, left, avail, pc_align);
                                continue;
                            }
                            const CapacityLevel pc_level = CapacityLevelFor(e.drive_used, e.drive_free);
                            const auto pc_ink = [cut](D2D1_COLOR_F pc_c) { return cut ? WithAlpha(pc_c, pc_c.a * 0.55f) : pc_c; };
                            if (pc_kind == ColumnKind::Date) {
                                // 使用情况: the bar, then the percent in the level's colour.
                                const float pc_pct_w = std::min(avail, 40.0f * scale_);
                                const float pc_bar_w = avail - pc_pct_w - 8.0f * scale_;
                                if (pc_bar_w >= 24.0f * scale_) {
                                    const float pc_cy = std::round((cell.top + cell.bottom) * 0.5f);
                                    const D2D1_RECT_F pc_bar = D2D1::RectF(left, pc_cy - 3.0f * scale_, left + pc_bar_w,
                                                                        pc_cy + 3.0f * scale_);
                                    painter_.DrawCapacityBar(pc_bar, e.drive_used, theme.accent, capacity_reveal,
                                                             e.drive_free);
                                }
                                MakeBrush(dc, pc_ink(painter_.CapacityInk(pc_level)), brTextSecondary_);
                                draw_detail_text(CapacityPercentText(e.drive_used), left + avail - pc_pct_w, pc_pct_w,
                                                 DWRITE_TEXT_ALIGNMENT_TRAILING);
                                MakeBrush(dc, secondary, brTextSecondary_);
                            } else if (pc_kind == ColumnKind::Size) {
                                // 可用: semibold in the text colour, red once critical.
                                const std::wstring pc_free = fit(pulse::format::ByteSize(e.drive_free), avail * 0.94f);
                                IDWriteTextFormat* pc_bold = NameSemiboldFormat();
                                const D2D1_COLOR_F pc_color = pc_level == CapacityLevel::Critical
                                    ? painter_.CapacityInk(pc_level) : theme.text;
                                if (pc_bold && !IsHighContrast()) {
                                    MakeBrush(dc, pc_ink(pc_color), brText_);
                                    const auto pc_prev = pc_bold->GetTextAlignment();
                                    const auto pc_prev_para = pc_bold->GetParagraphAlignment();
                                    pc_bold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                                    pc_bold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                    DrawTextEndEllipsis(compositor_, pc_bold, brText_.get(), pc_free, left,
                                                        cell.top + scale_, avail, cell.bottom - cell.top - 2.0f * scale_);
                                    pc_bold->SetTextAlignment(pc_prev);
                                    pc_bold->SetParagraphAlignment(pc_prev_para);
                                } else {
                                    draw_detail_text(pc_free, left, avail, DWRITE_TEXT_ALIGNMENT_TRAILING);
                                }
                            } else {
                                draw_detail_text(fit(pulse::format::ByteSize(e.drive_total), avail), left, avail,
                                                 DWRITE_TEXT_ALIGNMENT_TRAILING);
                            }
                            continue;
                        }
                    }
                    if (IsMediaColumn(detailsColumns.kinds[static_cast<size_t>(col)])) {
                        // 媒体文件夹专用列: read off the UI thread; a dash until known.
                        const ColumnKind media_kind = detailsColumns.kinds[static_cast<size_t>(col)];
                        static const std::wstring kMediaDash = L"\u2014";
                        const std::wstring media_text =
                            vm.media_cell ? vm.media_cell(src, MediaSortColumn(media_kind)) : std::wstring();
                        draw_detail_text(media_text.empty() ? kMediaDash : fit(media_text, avail), left, avail,
                                         media_kind == ColumnKind::MediaDuration || media_kind == ColumnKind::MediaFps
                                             ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
                        continue;
                    }
                    switch (detailsColumns.kinds[static_cast<size_t>(col)]) {
                    case ColumnKind::Path: {
                        if (!vm.path_roots.empty()) {
                            // 重复文件 tab: 下载\工作\2026 Q3 rather than C:\…\Downloads\工作.
                            const std::wstring folder = FolderOf(e.path);
                            std::wstring rel;
                            for (const auto& [root, label] : vm.path_roots) {
                                if (folder.size() >= root.size() &&
                                    _wcsnicmp(folder.c_str(), root.c_str(), root.size()) == 0 &&
                                    (folder.size() == root.size() || folder[root.size()] == L'\\')) {
                                    rel = label + folder.substr(root.size());
                                    break;
                                }
                            }
                            if (!rel.empty()) {
                                draw_detail_text(fit(rel, avail), left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                                break;
                            }
                        }
                        // Keep the drive and the nearest folders; elide the middle.
                        const auto [head, last] = MiddleEllipsisPath(FolderOf(e.path), avail,
                            [&](const std::wstring& s) { return CellTextWidth(s); });
                        if (last.empty()) {
                            draw_detail_text(fit(head, avail), left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                        } else {
                            const float headW = std::min(avail, CellTextWidth(head));
                            draw_detail_text(head, left, headW + 2.0f * scale_, DWRITE_TEXT_ALIGNMENT_LEADING);
                            MakeBrush(dc, WithAlpha(theme.text, cut ? 0.5f : 0.82f), brTextSecondary_);
                            draw_detail_text(fit(last, avail - headW), left + headW, avail - headW,
                                             DWRITE_TEXT_ALIGNMENT_LEADING);
                            MakeBrush(dc, secondary, brTextSecondary_);
                        }
                        break;
                    }
                    case ColumnKind::Date: {
                        const std::wstring date = list_smart_date_ && e.modified_value && !e.date_text.empty()
                            ? SmartListDate(e.modified_value) : e.date_text;
                        if (!mtime_on) {
                            draw_detail_text(fit(date, avail), left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                            break;
                        }
                        // 修改时间提示: dots sit in the cell's lead (every row's text
                        // moves the same, so dates stay aligned); bars run under the text.
                        const float mtLead = mtime.style == 1 ? kMtimeDotLeadDip * scale_ : 0.0f;
                        const float mtLeft = left + mtLead;
                        const float mtAvail = std::max(0.0f, avail - mtLead);
                        if (mtimeMarked && mtime.style == 1 && mtLead > 0.0f) {
                            const float mtSide = 8.0f * scale_;
                            MakeBrush(dc, mtimeColor, brTagDot_);
                            DrawMtimeMark(dc, brTagDot_.get(), colX + 11.0f * scale_, (cell.top + cell.bottom) * 0.5f,
                                          mtSide, mtimeMark, cut ? WithAlpha(mtimeColor, 0.55f) : mtimeColor);
                        }
                        if (mtimeFresh && mtAvail > 1.0f) {
                            // Changed today: semibold in the text color.
                            IDWriteTextFormat* mtBold = NameSemiboldFormat();
                            ComPtr<IDWriteTextLayout> mtLayout;
                            const std::wstring mtShown = fit(date, mtAvail * 0.94f);
                            if (mtBold && SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(mtShown.c_str(),
                                    static_cast<UINT32>(mtShown.size()), mtBold, mtAvail, cell.bottom - cell.top - 2.0f * scale_,
                                    &mtLayout))) {
                                mtLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                                mtLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                mtLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                MakeBrush(dc, cut ? WithAlpha(theme.text, 0.55f) : theme.text, brText_);
                                compositor_->DrawTextLayout(D2D1::Point2F(mtLeft, cell.top + scale_), mtLayout.get(), brText_.get(),
                                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
                            } else {
                                draw_detail_text(fit(date, mtAvail), mtLeft, mtAvail, DWRITE_TEXT_ALIGNMENT_LEADING);
                            }
                        } else if (mtimeOld) {
                            // Outside the range: a step lighter, so recent dates lead.
                            MakeBrush(dc, WithAlpha(secondary, secondary.a * 0.62f), brTextSecondary_);
                            draw_detail_text(fit(date, mtAvail), mtLeft, mtAvail, DWRITE_TEXT_ALIGNMENT_LEADING);
                            MakeBrush(dc, secondary, brTextSecondary_);
                        } else {
                            draw_detail_text(fit(date, mtAvail), mtLeft, mtAvail, DWRITE_TEXT_ALIGNMENT_LEADING);
                        }
                        if (mtimeMarked && mtime.style == 2 && mtAvail > 8.0f * scale_) {
                            // 时间条: as long as the time left in the range.
                            const uint64_t mtSpan = mtime.range_days <= 1
                                ? std::max<uint64_t>(mtime_clock.now - mtime_clock.midnight, kMtimeSecond)
                                : static_cast<uint64_t>(ClampMtimeRange(mtime.range_days)) * kMtimeDay;
                            const uint64_t mtAge = mtime_clock.now > mtimeMarkTime ? mtime_clock.now - mtimeMarkTime : 0;
                            const float mtFrac = std::clamp(1.0f - static_cast<float>(static_cast<double>(mtAge) /
                                                                                 static_cast<double>(mtSpan)), 0.06f, 1.0f);
                            const float mtBarW = std::round(std::min(96.0f * scale_, mtAvail) * mtFrac);
                            D2D1_COLOR_F mtBar = mtimeColor;
                            if (!IsHighContrast()) mtBar.a *= 0.75f;
                            if (cut) mtBar.a *= 0.55f;
                            MakeBrush(dc, mtBar, brTagDot_);
                            FillRoundedRect(dc, brTagDot_.get(), mtLeft, cell.bottom - 6.0f * scale_, mtBarW,
                                            2.0f * scale_, 1.0f * scale_);
                        }
                        break;
                    }
                    case ColumnKind::Status: {
                        // Read from the attributes the listing already has: no
                        // extra I/O and nothing that could trigger a download.
                        const float iconW = 16.0f * scale_;
                        if (avail < iconW) break;
                        const CloudLook look = CloudLookFor(e, theme);
                        const CloudSwapPose pose = CloudSwapFor(this, e.path, 1, look.glyph[0], motion_now_);
                        if (pose.running) RequestMotionFrame();
                        DrawIconText(left, cell.top, iconW, cell.bottom - cell.top, look.glyph, L"",
                                     WithAlpha(look.ink, (cut ? 0.55f : 1.0f) * pose.alpha), 0.8f * pose.scale);
                        const float textLeft = left + iconW + 6.0f * scale_;
                        const float textAvail = std::max(0.0f, left + avail - textLeft);
                        if (textAvail > 1.0f)
                            draw_detail_text(fit(look.label, textAvail), textLeft, textAvail,
                                             DWRITE_TEXT_ALIGNMENT_LEADING);
                        break;
                    }
                    case ColumnKind::Created:
                    case ColumnKind::Accessed: {
                        const bool created = detailsColumns.kinds[static_cast<size_t>(col)] == ColumnKind::Created;
                        const std::wstring& text = created ? e.created_text : e.accessed_text;
                        const uint64_t value = created ? e.created_value : e.accessed_value;
                        if (text.empty()) break;
                        const std::wstring date = list_smart_date_ && value ? SmartListDate(value) : text;
                        draw_detail_text(fit(date, avail), left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                        break;
                    }
                    case ColumnKind::Type: {
                        if (vm.dup_chips && !e.type_text.empty() && !IsHighContrast()) {
                            // 重复文件 tab (任务46 效果图): 保留 in the accent, 多出 neutral grey,
                            // 都留 amber, as small rounded tags.
                            const bool darkTheme = theme.bg.r < 0.5f;
                            const D2D1_COLOR_F base = darkTheme ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.0f, 0.0f);
                            D2D1_COLOR_F ink = secondary, fill = WithAlpha(base, darkTheme ? 0.09f : 0.05f);
                            if (e.type_text == pulse::l10n::Get(pulse::l10n::StringId::AiDupKeep)) {
                                ink = darkTheme ? HexColor(0x9FD2BDu) : theme.accent;
                                fill = WithAlpha(theme.accent, darkTheme ? 0.28f : 0.13f);
                            } else if (e.type_text == pulse::l10n::Get(pulse::l10n::StringId::AiDupBoth)) {
                                ink = HexColor(darkTheme ? 0xF0C27Eu : 0xA0681Bu);
                                fill = darkTheme ? HexColor(0x4A3A22u) : HexColor(0xFBF1E1u);
                            }
                            const float textW = typography::MeasureLine(compositor_, compositor_->FileNameFormat(), e.type_text);
                            const float pillW = std::min(avail, textW + 16.0f * scale_);
                            const float pillH = 20.0f * scale_;
                            const float pillTop = std::round((cell.top + cell.bottom - pillH) * 0.5f);
                            MakeBrush(dc, fill, brFillInput_);
                            FillRoundedRect(dc, brFillInput_.get(), left, pillTop, pillW, pillH, 5.0f * scale_);
                            MakeBrush(dc, WithAlpha(ink, cut ? 0.55f : 1.0f), brTextSecondary_);
                            const float labelW = std::max(0.0f, pillW - 16.0f * scale_);
                            draw_detail_text(fit(e.type_text, labelW), left + 8.0f * scale_, labelW, DWRITE_TEXT_ALIGNMENT_LEADING);
                            MakeBrush(dc, secondary, brTextSecondary_);
                            break;
                        }
                        const bool deleted_change = vm.is_changes &&
                            e.type_text == pulse::l10n::Get(pulse::l10n::StringId::ChangeDeleted);
                        float typeLeft = left;
                        const std::wstring chip = vm.is_changes ? std::wstring() : TypeChipLabel(e.name, e.is_dir);
                        const float chipW = TypeChipWidthDip(chip) * scale_;
                        if (!chip.empty() && chipW < avail * 0.6f && !IsHighContrast()) {
                            const float chipBoxW = chipW - kTypeChipGapDip * scale_;
                            const float chipH = 17.0f * scale_;
                            const float chipTop = std::round((cell.top + cell.bottom - chipH) * 0.5f);
                            // Tonal chip: theme-derived ink (hue mixed toward white on dark, toward
                            // black on light) keeps contrast without neon red on dark or muddy
                            // brown on light; a 1 px same-hue edge defines the shape.
                            const bool darkTheme = theme.bg.r < 0.5f;
                            const auto mixTo = [](D2D1_COLOR_F c, float target, float k) {
                                c.r += (target - c.r) * k; c.g += (target - c.g) * k; c.b += (target - c.b) * k;
                                return c;
                            };
                            D2D1_COLOR_F ink{}, fill{}, edge{};
                            if (TypeChipIsAux(chip)) {
                                const D2D1_COLOR_F base = darkTheme ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.0f, 0.0f);
                                ink = secondary;
                                fill = WithAlpha(base, darkTheme ? 0.05f : 0.04f);
                                edge = WithAlpha(base, darkTheme ? 0.08f : 0.07f);
                            } else {
                                const D2D1_COLOR_F hue = HexColor(TypeChipRgb(chip));
                                ink = darkTheme ? mixTo(hue, 1.0f, 0.38f) : mixTo(hue, 0.0f, 0.30f);
                                fill = WithAlpha(hue, darkTheme ? 0.15f : 0.09f);
                                edge = WithAlpha(hue, darkTheme ? 0.28f : 0.20f);
                            }
                            const float chipRadius = 4.0f * scale_;
                            MakeBrush(dc, fill, brFillInput_);
                            FillRoundedRect(dc, brFillInput_.get(), left, chipTop, chipBoxW, chipH, chipRadius);
                            MakeBrush(dc, edge, brTagDot_);
                            const D2D1_ROUNDED_RECT chipEdge = D2D1::RoundedRect(
                                D2D1::RectF(left + 0.5f, chipTop + 0.5f, left + chipBoxW - 0.5f, chipTop + chipH - 0.5f),
                                chipRadius - 0.5f, chipRadius - 0.5f);
                            dc->DrawRoundedRectangle(chipEdge, brTagDot_.get(), 1.0f);
                            const auto chipRc = D2D1::RectF(left, chipTop, left + chipBoxW, chipTop + chipH);
                            DrawTypeChipLabel(dc, chip, chipRc, WithAlpha(ink, ink.a * (cut ? 0.55f : 1.0f)), theme.bg);
                            typeLeft += chipW;
                        }
                        if (deleted_change) MakeBrush(dc, HexColor(0xC58A38), brTextSecondary_);
                        const float typeAvail = std::max(0.0f, left + avail - typeLeft);
                        draw_detail_text(fit(e.type_text, typeAvail), typeLeft, typeAvail,
                                         DWRITE_TEXT_ALIGNMENT_LEADING);
                        if (deleted_change) MakeBrush(dc, secondary, brTextSecondary_);
                        break;
                    }
                    case ColumnKind::Size: {
                        const auto folder_size = vm.folder_size_labels.find(src);
                        const std::wstring* size_text = &e.size_text;
                        float size_left = left, size_avail = avail;
                        if (e.is_dir && folder_size != vm.folder_size_labels.end()) {
                            size_text = &folder_size->second;
                            // Only rows that will not resolve by themselves offer the
                            // action, and only while hovered; otherwise a faint dash.
                            const bool stale = vm.folder_size_pending.contains(src) &&
                                folder_size_pending_age > kFolderSizePendingLimitMs;
                            const bool idle = vm.folder_size_idle.contains(src) || stale;
                            static const std::wstring kDash = L"\u2014";
                            if (stale) size_text = &kDash;
                            const bool offer = idle && src == vm.hover_index;
                            if (offer) size_text = &folder_size_offer;
                            const bool scanning = !idle && *size_text == folder_size_scanning;
                            D2D1_COLOR_F ink = offer ? theme.accent :
                                idle ? WithAlpha(theme.text_secondary, theme.text_secondary.a * 0.6f) :
                                scanning || vm.folder_size_muted.contains(src) ? theme.text_secondary : theme.text;
                            if (scanning && folder_size_animate) {
                                folder_size_anim_active_ = true;
                                const float phase = static_cast<float>(motion_now_ % 1200) / 1200.0f;
                                ink.a *= 1.0f - 0.55f * (0.5f - 0.5f * std::cos(phase * 6.2831853f));
                            }
                            MakeBrush(dc, cut ? WithAlpha(ink, ink.a * 0.55f) : ink, brTextSecondary_);
                            if (vm.folder_size_pending.contains(src) && !stale && folder_size_pending_age >= 150 &&
                                !IsHighContrast() && size_avail > 0.0f) {
                                const float appear = folder_size_animate
                                    ? std::min(1.0f, static_cast<float>(folder_size_pending_age - 150) / 200.0f) : 1.0f;
                                const float barW = std::min(size_avail, 46.0f * scale_);
                                const float barH = 9.0f * scale_;
                                const float cy = (cell.top + cell.bottom) * 0.5f;
                                const D2D1_RECT_F bar = D2D1::RectF(size_left + size_avail - barW, cy - barH * 0.5f,
                                                                    size_left + size_avail, cy + barH * 0.5f);
                                D2D1_COLOR_F base = theme.text;
                                base.a = (theme.bg.r < 0.5f ? 0.08f : 0.06f) * appear;
                                MakeBrush(dc, base, brFillInput_);
                                dc->FillRoundedRectangle(D2D1::RoundedRect(bar, barH * 0.5f, barH * 0.5f), brFillInput_.get());
                                if (folder_size_animate) {
                                    // A soft highlight sweeps across the bar every 1.4 s.
                                    const float cx = bar.left - barW + 3.0f * barW *
                                        static_cast<float>(motion_now_ % 1400) / 1400.0f;
                                    D2D1_COLOR_F glow = theme.text;
                                    glow.a = (theme.bg.r < 0.5f ? 0.07f : 0.06f) * appear;
                                    D2D1_COLOR_F clear = glow;
                                    clear.a = 0.0f;
                                    const D2D1_GRADIENT_STOP stops[] = {{0.0f, clear}, {0.5f, glow}, {1.0f, clear}};
                                    ComPtr<ID2D1GradientStopCollection> collection;
                                    ComPtr<ID2D1LinearGradientBrush> sweep;
                                    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &collection)) && collection.get() &&
                                        SUCCEEDED(dc->CreateLinearGradientBrush(
                                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx - barW * 0.5f, 0.0f),
                                                                                D2D1::Point2F(cx + barW * 0.5f, 0.0f)),
                                            collection.get(), &sweep)) && sweep.get())
                                        dc->FillRoundedRectangle(D2D1::RoundedRect(bar, barH * 0.5f, barH * 0.5f), sweep.get());
                                }
                            }
                        }
                        // Number right-aligned, unit in its own sub-column: digits line up.
                        const size_t space = size_text->find_last_of(L' ');
                        const float unitW = kSizeUnitDip * scale_;
                        const auto number = space == std::wstring::npos ? std::wstring() : size_text->substr(0, space);
                        const bool numeric = !number.empty() && (iswdigit(number.front()) || number.front() == L'\u2265');
                        if (numeric && size_avail > unitW * 1.8f && CellTextWidth(number) <= size_avail - unitW) {
                            const std::wstring unit = size_text->substr(space + 1);
                            draw_detail_text(number, size_left, size_avail - unitW, DWRITE_TEXT_ALIGNMENT_TRAILING);
                            MakeBrush(dc, WithAlpha(secondary, secondary.a * 0.72f), brTextSecondary_);
                            draw_detail_text(unit, size_left + size_avail - unitW + 4.0f * scale_, unitW - 4.0f * scale_,
                                             DWRITE_TEXT_ALIGNMENT_LEADING);
                        } else {
                            const float value_width = IsSizePlaceholder(*size_text) && size_avail > unitW * 1.8f
                                ? size_avail - unitW : size_avail;
                            draw_detail_text(fit(*size_text, value_width), size_left, value_width, DWRITE_TEXT_ALIGNMENT_TRAILING);
                        }
                        MakeBrush(dc, secondary, brTextSecondary_);
                        if (list_size_bar_ && !e.is_dir && e.size_value > 0 && !IsHighContrast()) {
                            // Log scale: 1 KB .. 100 GB spans the cell.
                            const double lg = std::log10(static_cast<double>(e.size_value));
                            const float frac = static_cast<float>(std::clamp((lg - 3.0) / 8.0, 0.02, 1.0));
                            const float barH = std::max(1.0f, 2.0f * scale_);
                            const float barY = cell.bottom - 5.0f * scale_;
                            MakeBrush(dc, WithAlpha(theme.accent, 0.14f), brFillInput_);
                            FillRoundedRect(dc, brFillInput_.get(), left, barY, avail, barH, barH * 0.5f);
                            MakeBrush(dc, WithAlpha(theme.accent, 0.7f), brFillInput_);
                            FillRoundedRect(dc, brFillInput_.get(), left + avail * (1.0f - frac), barY,
                                            avail * frac, barH, barH * 0.5f);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
            } else if (vm.view_mode == ViewMode::Tiles && e.device) {
                IDWriteTextFormat* device_fmt = compositor_->SmallFormat();
                const float textW = std::max(0.0f, nameRc.right - nameRc.left);
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                if (e.device & fs::kPcDevicePlaceholder) {
                    // No phone: how to connect one (opening the tile shows the help).
                    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), device_fmt, brTextSecondary_.get(),
                        e.type_text, nameRc.left, nameRc.top + 25.0f * scale_, textW, 20.0f * scale_);
                } else if (e.device & fs::kPcDeviceLocked) {
                    // What to do on the phone; the tile updates by itself.
                    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), device_fmt, brTextSecondary_.get(),
                        l10n::Get((e.device & fs::kPcDeviceIPhone) ? l10n::StringId::DeviceUnlockHintIPhone
                                                                   : l10n::StringId::DeviceUnlockHintAndroid),
                        nameRc.left, nameRc.top + 25.0f * scale_, textW, 20.0f * scale_);
                } else {
                    // One row per storage: name, usage, what is left (.pbars).
                    const float rowH = 16.0f * scale_;
                    const float labelW = std::min(64.0f * scale_, textW * 0.3f);
                    const float freeW = std::min(112.0f * scale_, textW * 0.4f);
                    float rowY = nameRc.top + 24.0f * scale_;
                    const size_t rows = std::min<size_t>(2, e.device_bars.size());
                    for (size_t b = 0; b < rows; ++b) {
                        const auto& bar = e.device_bars[b];
                        DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), device_fmt, brTextSecondary_.get(),
                            bar.label, nameRc.left, rowY, labelW, rowH);
                        const float barL = nameRc.left + labelW + 6.0f * scale_;
                        const float barR = std::max(barL, nameRc.left + textW - freeW - 6.0f * scale_);
                        const float mid = rowY + rowH * 0.5f;
                        if (bar.used >= 0.0f && barR - barL > 8.0f * scale_)
                            painter_.DrawCapacityBar(D2D1::RectF(barL, mid - 2.5f * scale_, barR, mid + 2.5f * scale_),
                                                     bar.used, theme.accent);
                        DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), device_fmt, brTextSecondary_.get(),
                            bar.free_text, barR + 6.0f * scale_, rowY, freeW, rowH);
                        rowY += rowH + 2.0f * scale_;
                    }
                }
            } else if (vm.view_mode == ViewMode::Tiles && e.drive_mounted && e.drive_used < 0.0f) {
                // Mounted drives without capacity data retain the network hint.
                // Available capacity uses the same bar as local drives below.
                IDWriteTextFormat* chip_fmt = compositor_->SmallFormat();
                const std::wstring chip = l10n::Get(l10n::StringId::MountedDriveChip);
                const float chipH = 18.0f * scale_;
                const float chipW = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), chip_fmt, chip) +
                                    12.0f * scale_;
                const float top = nameRc.top + 25.0f * scale_;
                MakeBrush(dc, WithAlpha(theme.accent, 0.12f), brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), nameRc.left, top, chipW, chipH, 4.0f * scale_);
                MakeBrush(dc, theme.accent, brAccent_);
                DrawTextRect(dc, chip_fmt, brAccent_.get(), chip, nameRc.left + 6.0f * scale_, top,
                             chipW - 12.0f * scale_ + 1.0f, chipH);
                const float noteX = nameRc.left + chipW + 6.0f * scale_;
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), chip_fmt, brTextSecondary_.get(),
                    l10n::Get(l10n::StringId::MountedDriveNote), noteX, top,
                    std::max(0.0f, nameRc.right - noteX), chipH);
            } else if (driveTile) {
                // This PC tiles: name and used percent, the bar, then
                // "75.2 GB 可用 · 共 476.6 GB" with the free size semibold.
                const float textW = std::max(0.0f, nameRc.right - nameRc.left);
                const CapacityLevel level = CapacityLevelFor(e.drive_used, e.drive_free);
                const D2D1_COLOR_F levelInk = painter_.CapacityInk(level);
                MakeBrush(dc, cut ? WithAlpha(levelInk, 0.55f) : levelInk, brCapacityInk_);
                DrawTextRect(dc, compositor_->SmallFormat(), brCapacityInk_.get(), drivePercent,
                             nameRc.left + driveBarW - drivePercentW, nameRc.top + 1.0f * scale_,
                             drivePercentW + 1.0f, 20.0f * scale_);
                const float barTop = nameRc.top + 26.0f * scale_;
                const D2D1_RECT_F bar{ nameRc.left, barTop, nameRc.left + driveBarW, barTop + 8.0f * scale_ };
                painter_.DrawCapacityBar(bar, e.drive_used, theme.accent, capacity_reveal, e.drive_free);
                DrawDriveSpaceLine(dc, e, level, theme, nameRc.left, bar.bottom + 3.0f * scale_, textW,
                                   20.0f * scale_, cut);
            } else if (vm.view_mode == ViewMode::Tiles || vm.view_mode == ViewMode::Content) {
                std::wstring meta = !e.snippet.empty() ? e.snippet : e.type_text;
                if (e.snippet.empty() && !e.size_text.empty())
                    meta += (meta.empty() ? L"" : L" \u00B7 ") + e.size_text;
                if (e.snippet.empty() && vm.view_mode == ViewMode::Content && !e.date_text.empty())
                    meta += (meta.empty() ? L"" : L" \u00B7 ") + e.date_text;
                if (e.is_dir) {
                    const auto size = vm.folder_size_labels.find(src);
                    if (size != vm.folder_size_labels.end()) meta = size->second;
                }
                const bool manual_size = e.is_dir && vm.folder_size_idle.contains(src);
                const bool offer_size = manual_size && src == vm.hover_index;
                if (offer_size) meta = folder_size_offer;
                if (e.is_dir && vm.folder_size_labels.contains(src)) {
                    const auto ink = offer_size ? theme.accent :
                        manual_size ? WithAlpha(theme.text_secondary, theme.text_secondary.a * 0.6f) :
                        vm.folder_size_muted.contains(src) || meta == folder_size_scanning ? theme.text_secondary : theme.text;
                    MakeBrush(dc, cut ? WithAlpha(ink, 0.55f) : ink, brTextSecondary_);
                }
                DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(),
                    brTextSecondary_.get(), meta,
                    nameRc.left, nameRc.top + 25.0f * scale_,
                    std::max(0.0f, nameRc.right - nameRc.left), 22.0f * scale_);
            }

            if (iconGrid && e.is_dir && src != vm.rename_index) {
                const auto size = vm.folder_size_labels.find(src);
                if (size != vm.folder_size_labels.end()) {
                    const auto bounds = layout.FolderSizeRect(i);
                    auto* format = compositor_->SmallFormat();
                    const auto alignment = format->GetTextAlignment();
                    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    const bool manual = vm.folder_size_idle.contains(src);
                    const bool offer = manual && src == vm.hover_index;
                    const auto ink = offer ? theme.accent :
                        manual ? WithAlpha(theme.text_secondary, theme.text_secondary.a * 0.6f) :
                        vm.folder_size_muted.contains(src) || size->second == folder_size_scanning
                            ? theme.text_secondary : theme.text;
                    MakeBrush(dc, cut ? WithAlpha(ink, 0.55f) : ink, brTextSecondary_);
                    DrawTextEndEllipsis(compositor_, format,
                        brTextSecondary_.get(), offer ? folder_size_offer : size->second,
                        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
                    format->SetTextAlignment(alignment);
                }
            }

            if (trail.show_star || trail.show_new_tab || trail.show_more) {
                const bool starHot = hover_region == static_cast<int>(HitTestResult::RowStar) &&
                                     hover_control_index == src;
                const bool newTabHot = hover_region == static_cast<int>(HitTestResult::RowNewTab) &&
                                       hover_control_index == src;
                const bool moreHot = hover_region == static_cast<int>(HitTestResult::RowMore) &&
                                     hover_control_index == src;
                auto draw_action = [&](const D2D1_RECT_F& rc, bool hot, const wchar_t* glyph,
                                       const wchar_t* fallback, const D2D1_COLOR_F& color, float size) {
                    if (rc.right <= rc.left) return;
                    if (hot) {
                        MakeBrush(dc, theme.fill_selected, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 5.0f * scale_);
                    }
                    DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                 glyph, fallback, color, size);
                };
                if (trail.show_star) {
                    if (e.starred) {
                        MakeBrush(dc, WithAlpha(theme.accent, starHot ? 0.28f : 0.18f), brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), trail.star.left, trail.star.top,
                            trail.star.right - trail.star.left, trail.star.bottom - trail.star.top,
                            5.0f * scale_);
                    }
                    draw_action(trail.star, starHot && !e.starred,
                                e.starred ? L"\xE735" : L"\xE734", L"*",
                                e.starred ? theme.accent : theme.text_secondary,
                                1.0f);
                }
                if (trail.show_new_tab)
                    draw_action(trail.new_tab, newTabHot, L"\xE8A7", L"\x2197",
                                theme.text_secondary, 0.9f);
                if (trail.show_more)
                    draw_action(trail.more, moreHot, L"\xE712", L"...", theme.text_secondary, 0.72f);
            }
        }
        if (draw_shapes && !morphing && !tree_guides.rows.empty()) {
            const bool guideHc = IsHighContrast();
            D2D1_COLOR_F guideColor = theme.text;
            guideColor.a = guideHc ? 1.0f : (theme.bg.r < 0.5f ? 0.17f : 0.16f);
            ComPtr<ID2D1SolidColorBrush> plainBr, hiBr;
            dc->CreateSolidColorBrush(guideColor, &plainBr);
            dc->CreateSolidColorBrush(guideHc ? theme.text : theme.accent, &hiBr);
            const float lineW = std::max(1.0f, std::floor(scale_));
            if (plainBr.get() && hiBr.get()) {
                dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
                const D2D1_RECT_F guideIcon = layout.IconRect(tree_guides.first);
                DrawTreeGuides(dc, *vm.tree, tree_guides, [&](int row) { return layout.ItemRect(row); },
                               guideIcon.left + CheckGutterDip(vm) * scale_, scale_, plainBr.get(), hiBr.get(),
                               lineW, lineW * 1.5f);
                dc->PopAxisAlignedClip();
            }
        }
        if (text_layer) dc->PopLayer();
        PrefetchNextScreen(vm, startIdx, endIdx, prefetch_pixels, pane_index);
    }
    if (!sticky.pins.empty()) {
        dc->PopAxisAlignedClip();
        // Innermost first: a pin being pushed out slides under the one above
        // it (each pin is clipped to the part below its outer neighbour).
        const float rowH = sticky.row_h;
        const bool hc = IsHighContrast();
        const bool pane_hot = group_hover_pane_ == pane_index &&
                              hover_region == static_cast<int>(HitTestResult::TreeSticky);
        static const std::vector<NameMatchRange> kNoMatches;
        for (size_t k = sticky.pins.size(); k-- > 0;) {
            const TreePin& pin = sticky.pins[k];
            const int src = vm.SourceIndex(pin.row);
            const DetailsTreeRow* tr = vm.tree->At(pin.row);
            if (src < 0 || !tr) continue;
            const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));
            const D2D1_RECT_F home = layout.ItemRect(pin.row);
            const float top = sticky.top + pin.y;
            const float dy = top - home.top;
            const D2D1_RECT_F cell = D2D1::RectF(home.left, top, home.right, top + rowH);
            const float above = k > 0 ? sticky.top + sticky.pins[k - 1].y + rowH : viewport.top;
            dc->PushAxisAlignedClip(D2D1::RectF(viewport.left, std::max(above, cell.top), viewport.right,
                                                cell.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
            const bool hot = pane_hot && hover_control_index == pin.row;
            if (hot) {
                const float inset = 4.0f * scale_;
                FillRoundedRect(dc, brFillHover_.get(), cell.left + inset, cell.top + scale_,
                                std::max(0.0f, cell.right - cell.left - 2.0f * inset),
                                std::max(0.0f, rowH - 2.0f * scale_), theme.radius_control * scale_);
            }
            const D2D1_RECT_F icon_home = layout.IconRect(pin.row);
            drawTreeChevron(*tr, cell, icon_home.left + CheckGutterDip(vm) * scale_, hot, pin.row == tree_hi_row);
            const float dx = std::round(DetailsLeadDip(vm, pin.row) * scale_);
            const float side = std::max(1.0f, std::round(std::min(icon_home.right - icon_home.left,
                                                                  icon_home.bottom - icon_home.top)));
            DrawEntryIcon(e, std::round(icon_home.left + dx), std::round(icon_home.top + dy), side, theme, 1.0f);
            const D2D1_RECT_F name_home = layout.NameRect(pin.row);
            const float name_x = std::min(name_home.right, name_home.left + dx);
            const float name_right = detailsColumns.DividerX(0) - margin_;
            DrawTruncatedName(e.name, name_x, name_home.top + dy, std::max(1.0f, name_right - name_x),
                              std::max(1.0f, name_home.bottom - name_home.top), theme, false, kNoMatches, false);
            dc->PopAxisAlignedClip();
        }
        // The stack's lower edge: a hairline, then a soft shadow over the rows.
        const float edge = sticky.Bottom();
        const float hair = std::max(1.0f, std::floor(scale_));
        MakeBrush(dc, hc ? theme.text : WithAlpha(theme.text, theme.bg.r < 0.5f ? 0.16f : 0.11f), brFillInput_);
        dc->FillRectangle(D2D1::RectF(viewport.left, edge - hair, viewport.right, edge), brFillInput_.get());
        if (!hc) {
            const D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, theme.bg.r < 0.5f ? 0.24f : 0.07f)},
                {1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)},
            };
            ComPtr<ID2D1GradientStopCollection> stop_list;
            ComPtr<ID2D1LinearGradientBrush> shade;
            const float depth = 6.0f * scale_;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &stop_list)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, edge), D2D1::Point2F(0.0f, edge + depth)),
                    stop_list.get(), &shade)))
                dc->FillRectangle(D2D1::RectF(viewport.left, edge, viewport.right, edge + depth), shade.get());
        }
    }
    if (ai_painter) {
        // Cards of the items flying into their folders (.ghost).
        ai_painter->Flights(D2D1::RectF(x, y, x + w, y + h), [&](const std::wstring& key, const D2D1_RECT_F& r) {
            auto it = AiGhostEntries().find(key);
            if (it == AiGhostEntries().end()) return;
            const ListEntryView& ge = it->second;
            const float iy = std::round((r.top + r.bottom - ai_icon_size) * 0.5f);
            DrawEntryIcon(ge, std::round(r.left + ai_icon_dx), iy, ai_icon_size, theme, 1.0f);
            static const std::vector<NameMatchRange> kNoMatches;
            const float nx = r.left + ai_name_dx;
            DrawTruncatedName(ge.name, nx, r.top, std::max(1.0f, r.right - nx - 12.0f * scale_), r.bottom - r.top,
                              theme, false, kNoMatches, !ge.is_dir);
        });
    }
    if (pane_focused) {
        expanded_overflow_px_ = 0.0f;
        expanded_overflow_index_ = -1;
    }
    if (expanded_name.valid) {
        dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float overflow = DrawExpandedIconName(expanded_name.name, expanded_name.bounds, expanded_name.color, theme,
                             expanded_name.matches, pane_focused, viewport, expanded_name.note,
                             expanded_name.note_color, expanded_name.dim_extension);
        dc->PopAxisAlignedClip();
        if (pane_focused && overflow > 0.0f) {
            expanded_overflow_px_ = overflow;
            expanded_overflow_index_ = expanded_name.source;
        }
    }
    if (vm.rename_index >= 0 && vm.ViewIndex(vm.rename_index) >= 0) {
        // On top of the neighbouring cells it overlaps in icon and tile views.
        dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        fluent::ControlState fieldState{};
        fieldState.focused = true;
        const D2D1_RECT_F field = RenameFieldRect(vm, viewport, vm.rename_index);
        if (vm.rename_shake_ms >= 0) RequestMotionFrame();
        float pop = 1.0f, age = 1e6f;
        if (pane_index >= 0 && pane_index < static_cast<int>(rename_pop_.size())) {
            RenamePop& rp = rename_pop_[static_cast<size_t>(pane_index)];
            if (rp.index != vm.rename_index) {
                rp.index = vm.rename_index;
                rp.since = motion::SystemAnimationsEnabled() && !IsHighContrast() ? motion_now_ : 0;
            }
            if (rp.since) {
                age = static_cast<float>(motion_now_ - rp.since);
                if (age < static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) {
                    pop = 0.96f + 0.04f * motion::SpringProgress(motion::spring::Pop, age);
                    RequestMotionFrame();
                } else {
                    rp.since = 0;
                }
            }
        }
        D2D1_MATRIX_3X2_F saved{};
        dc->GetTransform(&saved);
        if (pop != 1.0f) {
            const D2D1_POINT_2F c = D2D1::Point2F((field.left + field.right) * 0.5f, (field.top + field.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(pop, pop, c) * saved);
        }
        const bool card = vm.view_mode == ViewMode::ExtraLargeIcons || vm.view_mode == ViewMode::LargeIcons ||
                          vm.view_mode == ViewMode::MediumIcons;
        if (card && !IsHighContrast()) {
            // A floating card: a soft shadow below it (three widening
            // translucent layers, cheaper than a blur).
            const float r0 = theme.radius_control * scale_;
            const float ink = theme.bg.r < 0.5f ? 0.16f : 0.06f;
            for (int k = 3; k >= 1; --k) {
                const float g = static_cast<float>(k) * 2.0f * scale_;
                MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, ink), brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), field.left - g * 0.5f, field.top + 1.0f * scale_,
                                field.right - field.left + g, field.bottom - field.top + g, r0 + g * 0.5f);
            }
        }
        painter_.DrawTextFieldFrame(field, fieldState);
        dc->SetTransform(saved);
        if (age < 320.0f) {
            // The ripple: 0 -> 6 dip out, fading.
            const float t = age / 320.0f;
            const float grow = 6.0f * scale_ * motion::EaseOutCubic(t);
            MakeBrush(dc, WithAlpha(theme.accent, 0.45f * (1.0f - t)), brFillInput_);
            const float r = theme.radius_control * scale_ + grow;
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(field.left - grow, field.top - grow,
                                                                   field.right + grow, field.bottom + grow), r, r),
                                     brFillInput_.get(), 1.5f * scale_);
        }
        DrawRenameHint(vm, field, D2D1::RectF(x, y, x + w, y + h), theme);
        dc->PopAxisAlignedClip();
    } else if (pane_index >= 0 && pane_index < static_cast<int>(rename_pop_.size())) {
        rename_pop_[static_cast<size_t>(pane_index)] = {};
    }
    if (morphing) DrawMorphFrom(vm, *morph, viewport, theme);
    if (shift) shift->EndFrame();
    DrawGroupHeaders(vm, layout, viewport, theme, hover_region, hover_control_index, pane_index);

    if (vm.marquee_active) {
        D2D1_RECT_F clip = D2D1::RectF(x, y, x + w, y + h);
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_RECT_F rc = vm.marquee_rect;
        const float mw = rc.right - rc.left;
        const float mh = rc.bottom - rc.top;
        if (mw > 0.0f && mh > 0.0f) {
            MakeBrush(dc, WithAlpha(theme.accent, 0.16f), brAccent_);
            FillRect(dc, brAccent_.get(), rc.left, rc.top, mw, mh);
            MakeBrush(dc, theme.accent, brAccent_);
            dc->DrawRectangle(rc, brAccent_.get(), 1.0f * scale_);
        }
        dc->PopAxisAlignedClip();
    }

    if (!vm.history_preview_paths.empty()) {
        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        MakeBrush(dc, theme.accent, brAccent_);
        for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
            const int source = vm.SourceIndex(i);
            if (source < 0) continue;
            const auto& entry = MakeVisibleEntry(vm, static_cast<size_t>(source));
            if (std::any_of(vm.history_preview_paths.begin(), vm.history_preview_paths.end(), [&](const auto& path) {
                return _wcsicmp(path.c_str(), entry.path.c_str()) == 0;
            })) {
                const auto cell = layout.ItemRect(i);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 2 * scale_, cell.top + 4 * scale_,
                    cell.left + 5 * scale_, cell.bottom - 4 * scale_), 1.5f * scale_, 1.5f * scale_), brAccent_.get());
            }
        }
        dc->PopAxisAlignedClip();
    }
    DrawScrollbar(vm, x, y, w, h, theme);
    if (vm.view_mode == ViewMode::List && layout.MaxScrollX() > 0.0f)
        painter_.DrawScrollbar(PaneHScrollbarSpec(vm, D2D1::RectF(x, y, x + w, y + h), layout.MaxScrollX(), scale_));
}

// "Group by" headers: chevron, label, count, rule and group bytes. The group
// under the viewport top is pinned (sticky) and pushed up by the next one.
std::wstring MainRenderer::GroupLabel(const PaneViewModel& vm, const ListGroup& g) {
    using l10n::StringId;
    if (!g.label.empty()) return g.label;
    switch (vm.group_by) {
    case 1: {
        static const wchar_t* const kNames[] = {L"0 \x2013 9", L"A \x2013 H", L"I \x2013 P", L"Q \x2013 Z"};
        if (g.rank >= 0 && g.rank < 4) return kNames[g.rank];
        return l10n::Get(StringId::GroupNameOther);
    }
    case 2:
        return l10n::Get(static_cast<StringId>(IDS_GROUP_D_FUTURE + std::clamp(g.rank, 0, 8)));
    case 3:
        if (g.rank == 0) return l10n::Get(StringId::GroupFolders);
        {
            // Type text alone is ambiguous ("File" for many extensions), so
            // the extension is appended: "Text document (.log)".
            const std::wstring ext = g.key.size() > 2 ? g.key.substr(2) : std::wstring();
            std::wstring type = g.sample >= 0
                ? MakeVisibleEntry(vm, static_cast<size_t>(g.sample)).type_text : std::wstring();
            if (type.empty()) return ext.empty() ? std::wstring(L"-") : L"." + ext;
            if (!ext.empty()) type += L" (." + ext + L")";
            return type;
        }
    case 4:
        return l10n::Get(static_cast<StringId>(IDS_GROUP_FOLDERS + std::clamp(g.rank, 0, 7)));
    case 5:
        return g.label.empty() ? l10n::Get(StringId::GroupUntagged) : g.label;
    case 6:
        return g.label.empty() ? std::wstring(L"-") : g.label;
    case 7:  // This PC
        return l10n::Get(g.rank == 2 ? StringId::GroupThisPcLaunchers
                         : g.rank == 1 ? StringId::GroupThisPcDevices : StringId::GroupThisPcDrives);
    case 8:  // network page (DirEntry::net_group)
        return l10n::Get(g.rank == 4 ? StringId::GroupNetworkRecent
                         : g.rank == 3 ? StringId::GroupNetworkDevices
                         : g.rank == 2 ? StringId::GroupNetworkDrives : StringId::GroupNetworkPinned);
    case 9:  // the assistant's duplicates list: the group's file name
        return g.label;
    case 10: case 11: case 12: case 13: case 14:   // media groups (app::GroupBy::Media*)
        return g.label.empty() ? l10n::Get(StringId::GroupMediaUnknown) : g.label;
    default:
        return {};
    }
}

void MainRenderer::DrawGroupHeaders(const PaneViewModel& vm, const ViewLayout& layout,
                                    const D2D1_RECT_F& viewport, const Theme& theme,
                                    int hover_region, int hover_control_index, int pane_index) {
    const ListGroups* groups = vm.Groups();
    if (!groups || !layout.Grouped() || !compositor_) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* label_fmt = compositor_->TextFormat();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const bool pane_hot = group_hover_pane_ == pane_index;
    const bool header_hot_region = pane_hot &&
        (hover_region == static_cast<int>(HitTestResult::GroupHeader) ||
         hover_region == static_cast<int>(HitTestResult::GroupSelect));
    const bool select_hot_region = pane_hot &&
        hover_region == static_cast<int>(HitTestResult::GroupSelect);
    D2D1_RECT_F sticky_rc{};
    const int sticky = layout.StickyHeader(&sticky_rc);

    auto draw_header = [&](int index, const D2D1_RECT_F& hr, bool pinned) {
        const ListGroup& g = (*groups)[static_cast<size_t>(index)];
        const D2D1_RECT_F rc = DetailsContentRect(hr, scale_);
        const float h = hr.bottom - hr.top;
        const bool hot = header_hot_region && hover_control_index == index;
        if (pinned) {
            MakeBrush(dc, WithAlpha(theme.bg, 0.78f), brFillHover_);
            FillRect(dc, brFillHover_.get(), hr.left, hr.top, hr.right - hr.left, h);
            MakeBrush(dc, theme.header_bg, brFillHover_);
            FillRect(dc, brFillHover_.get(), hr.left, hr.top, hr.right - hr.left, h);
            MakeBrush(dc, theme.stroke_divider, brFillHover_);
            FillRect(dc, brFillHover_.get(), hr.left, hr.bottom - 1.0f * scale_, hr.right - hr.left, 1.0f * scale_);
        }
        if (hot) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), rc.left, hr.top + 3.0f * scale_,
                            rc.right - rc.left, h - 6.0f * scale_, 5.0f * scale_);
        }
        float x = rc.left + 4.0f * scale_;
        DrawIconText(x, hr.top, 16.0f * scale_, h, g.collapsed ? L"\xE76C" : L"\xE70D",
                     g.collapsed ? L">" : L"v", theme.text_secondary, 0.62f);
        x += 22.0f * scale_;
        const D2D1_RECT_F select_rc = GroupSelectRect(rc, scale_);
        const bool show_select = hot && GroupSelectVisible(rc, scale_);
        const float text_right = (show_select ? select_rc.left : rc.right) - 8.0f * scale_;

        if (vm.group_by == 5 && !g.label.empty()) {
            MakeBrush(dc, HexColor(g.color_rgb), brAccent_);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 4.0f * scale_, hr.top + h * 0.5f),
                4.0f * scale_, 4.0f * scale_), brAccent_.get());
            x += 14.0f * scale_;
        }
        std::wstring label = GroupLabel(vm, g);
        // 重复文件 tab (dup_finder.cpp): "name\tmeta\twhy\twaste\tboth" - the
        // name in the text color, 2 份 · 每份 98.3 MB, the 保留理由, and 多出
        // 98.3 MB (or 都留) at the right in place of the group's bytes.
        std::wstring dup_part[5];
        bool dup_head = false;
        if (vm.dup_chips && label.find(L'\t') != std::wstring::npos) {
            size_t at = 0;
            for (int i = 0; i < 5; ++i) {
                const size_t tab = label.find(L'\t', at);
                dup_part[i] = label.substr(at, tab == std::wstring::npos ? std::wstring::npos : tab - at);
                if (tab == std::wstring::npos) break;
                at = tab + 1;
            }
            label = dup_part[0];
            dup_head = true;
        }
        const D2D1_COLOR_F amber = HexColor(theme.bg.r < 0.5f ? 0xF0C27Eu : 0xA0681Bu);
        const float label_w = std::min(std::max(0.0f, text_right - x),
                                       MeasureLayoutText(compositor_, factory, label_fmt, label) + 1.0f);
        MakeBrush(dc, dup_head ? theme.text : theme.accent, brAccent_);
        DrawTextEndEllipsis(compositor_, label_fmt, brAccent_.get(), label, x, hr.top, label_w, h);
        x += label_w + (dup_head ? 10.0f : 8.0f) * scale_;

        wchar_t count[64]{};
        swprintf_s(count, l10n::Get(g.count == 1 ? l10n::StringId::GroupCountOne
                                                 : l10n::StringId::GroupCount).c_str(), g.count);
        const std::wstring count_text = dup_head ? dup_part[1] : std::wstring(count);
        const float count_w = MeasureLayoutText(compositor_, factory, small_fmt, count_text) + 1.0f;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        if (x + count_w <= text_right)
            DrawTextRect(dc, small_fmt, brTextSecondary_.get(), count_text, x, hr.top, count_w, h);
        x += count_w + 10.0f * scale_;
        // The right end first, so the 保留理由 knows how much room is left.
        std::wstring right_text;
        if (!show_select) right_text = dup_head ? dup_part[3] : (g.bytes > 0 ? pulse::format::ByteSize(g.bytes, true) : std::wstring());
        const float right_w = right_text.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, small_fmt, right_text) + 1.0f;
        const float right_x = rc.right - 8.0f * scale_ - right_w;
        if (dup_head && !dup_part[2].empty()) {
            const float why_right = (right_w > 0.0f ? right_x - 14.0f * scale_ : text_right);
            const float why_w = std::min(std::max(0.0f, why_right - x),
                                         MeasureLayoutText(compositor_, factory, small_fmt, dup_part[2]) + 1.0f);
            if (why_w > 48.0f * scale_) {
                MakeBrush(dc, WithAlpha(theme.text_secondary, 0.82f), brFillHover_);
                DrawTextEndEllipsis(dc, factory, small_fmt, brFillHover_.get(), dup_part[2], x, hr.top, why_w, h);
                x += why_w + 10.0f * scale_;
            }
        }
        if (vm.group_by == 7 && g.rank == 2) {
            // 网盘程序入口: say what these icons are, ahead of the rule.
            const std::wstring note = l10n::Get(l10n::StringId::GroupThisPcLaunchersNote);
            const float note_w = std::min(std::max(0.0f, text_right - x),
                                          MeasureLayoutText(compositor_, factory, small_fmt, note) + 1.0f);
            if (note_w > 48.0f * scale_) {
                DrawTextEndEllipsis(dc, factory, small_fmt, brTextSecondary_.get(), note, x, hr.top, note_w, h);
                x += note_w + 10.0f * scale_;
            }
        }

        float rule_right = text_right;
        if (right_w > 0.0f && right_x > x + (dup_head ? 4.0f : 24.0f) * scale_) {
            if (dup_head) {
                MakeBrush(dc, dup_part[4].empty() ? theme.accent : amber, brAccent_);
                DrawTextRect(dc, small_fmt, brAccent_.get(), right_text, right_x, hr.top, right_w, h);
            } else {
                DrawTextRect(dc, small_fmt, brTextSecondary_.get(), right_text, right_x, hr.top, right_w, h);
            }
            rule_right = right_x - 10.0f * scale_;
        }
        if (!dup_head && rule_right > x) {
            MakeBrush(dc, theme.stroke_divider, brFillHover_);
            FillRect(dc, brFillHover_.get(), x, std::floor(hr.top + h * 0.5f), rule_right - x, 1.0f * scale_);
        }
        if (show_select) {
            const bool select_hot = select_hot_region;
            MakeBrush(dc, select_hot ? theme.fill_selected : WithAlpha(theme.accent, 0.10f), brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), select_rc.left, select_rc.top,
                            select_rc.right - select_rc.left, select_rc.bottom - select_rc.top, 5.0f * scale_);
            const std::wstring text = l10n::Get(l10n::StringId::GroupSelect);
            const float tw = MeasureLayoutText(compositor_, factory, small_fmt, text) + 1.0f;
            const float tx = select_rc.left + std::max(0.0f, (select_rc.right - select_rc.left - tw) * 0.5f);
            DrawTextRect(dc, small_fmt, brAccent_.get(), text, tx, select_rc.top, tw,
                         select_rc.bottom - select_rc.top);
        }
    };

    dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const int count = layout.GroupCount();
    for (int g = 0; g < count; ++g) {
        const D2D1_RECT_F hr = layout.HeaderRect(g);
        if (hr.bottom < viewport.top) continue;
        if (hr.top > viewport.bottom) break;
        draw_header(g, hr, false);
    }
    if (sticky >= 0) draw_header(sticky, sticky_rc, true);
    dc->PopAxisAlignedClip();
}

// The file list's bars are the shared fluent scrollbar, like settings and
// the sidebar: thin at rest, full width with its track while hovered or dragged.
fluent::ScrollbarSpec PaneScrollbarSpec(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                        float content_h, float scale) {
    fluent::ScrollbarSpec bar;
    bar.viewport = D2D1::RectF(list.right - 12.0f * scale, list.top, list.right - 2.0f * scale, list.bottom);
    bar.offset = vm.scroll_y;
    bar.viewport_extent = list.bottom - list.top;
    bar.content_extent = content_h;
    bar.expand_progress = vm.scrollbar_expand;
    bar.opacity = vm.scrollbar_opacity;
    return bar;
}

fluent::ScrollbarSpec PaneHScrollbarSpec(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                         float max_scroll_x, float scale) {
    fluent::ScrollbarSpec bar;
    bar.horizontal = true;
    bar.viewport = D2D1::RectF(list.left, list.bottom - 12.0f * scale, list.right, list.bottom - 2.0f * scale);
    bar.offset = vm.scroll_x;
    bar.viewport_extent = list.right - list.left;
    bar.content_extent = bar.viewport_extent + max_scroll_x;
    bar.expand_progress = vm.scrollbar_expand;
    bar.opacity = vm.scrollbar_opacity;
    return bar;
}

void MainRenderer::DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme) {
    (void)theme;
    const D2D1_RECT_F list = D2D1::RectF(x, y, x + w, y + h);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(),
                      vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    painter_.DrawScrollbar(PaneScrollbarSpec(vm, list, layout.ContentHeight(), scale_));
}
bool MainRenderer::PaneScrollbarGeometry(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                          D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    const auto list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    max_scroll = layout.MaxScrollY();
    // The drawn bar's thumb (DrawScrollbar), a little wider to grab.
    const D2D1_RECT_F drawn = fluent::ScrollbarThumbRect(
        PaneScrollbarSpec(vm, list, layout.ContentHeight(), scale_), scale_);
    if (drawn.bottom <= drawn.top) return false;
    track = D2D1::RectF(list.right - 14 * scale_, list.top, list.right, list.bottom);
    thumb = D2D1::RectF(track.left, drawn.top, track.right, drawn.bottom);
    return true;
}

float MainRenderer::MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.MaxScrollY();
}

float MainRenderer::MaxScrollXForPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& full_bounds) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.MaxScrollX();
}

D2D1_RECT_F MainRenderer::ItemRectInPane(const PaneViewModel& vm,
                                         const D2D1_RECT_F& full_bounds,
                                         int view_index) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.ItemRect(view_index);
}

int MainRenderer::MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                int current, int dx, int dy) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.MoveIndex(current, dx, dy);
}

int MainRenderer::PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.PageDelta();
}

std::pair<int, int> MainRenderer::VisibleRangeInPane(
    const PaneViewModel& vm, const D2D1_RECT_F& full_bounds) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    return layout.VisibleRange();
}

void MainRenderer::ItemsInRectInPane(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                     const D2D1_RECT_F& rect, std::vector<int>& sources) const {
    sources.clear();
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    const auto [first, last] = layout.Metrics().column_major ? layout.RangeForSpan(rect.left, rect.right)
                                                             : layout.RangeForSpan(rect.top, rect.bottom);
    for (int view = first; view >= 0 && view <= last; ++view) {
        const D2D1_RECT_F item = layout.ItemRect(view);
        if (item.bottom <= item.top) continue; // inside a collapsed group
        if (item.right <= rect.left || item.left >= rect.right ||
            item.bottom <= rect.top || item.top >= rect.bottom) continue;
        const int source = vm.SourceIndex(view);
        if (source >= 0) sources.push_back(source);
    }
}

int MainRenderer::RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const {
    return ItemFromPointInPane(vm, pane_bounds, PaneBodyBounds(vm, pane_bounds).left + 1.0f, y);
}

int MainRenderer::ItemFromPointInPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& full_bounds,
                                      float x, float y) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    int idx = layout.HitTest(x, y);
    if (idx < 0) return -1;
    return vm.SourceIndex(idx);
}

} // namespace pulse::ui
