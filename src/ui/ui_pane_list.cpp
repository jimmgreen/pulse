// ui_pane_list.cpp — Pane, list, empty states, columns, and icons.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../app/search_query.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {

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

D2D1_RECT_F MainRenderer::PaneListRect(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    return PaneListRect(pane_bounds,
        PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_), vm.view_mode);
}

D2D1_RECT_F MainRenderer::FilterBoxRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    const float w = pane_bounds.right - pane_bounds.left;
    const float expandedW = std::max(32.0f * scale_, std::min(w - 16.0f * scale_,
        std::clamp(w * 0.34f, 112.0f * scale_, 220.0f * scale_)));
    const float t = std::clamp(expand, 0.0f, 1.0f);
    const float filterW = 32.0f * scale_ +
        (expandedW - 32.0f * scale_) * t;
    const float right = pane_bounds.right - 8.0f * scale_;
    return D2D1::RectF(right - filterW,
                       pane_bounds.top + 4 * scale_,
                       right,
                       pane_bounds.top + pane_header_height_ - 4 * scale_);
}

D2D1_RECT_F MainRenderer::PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                              float filter_expand) const {
    const D2D1_RECT_F filter = FilterBoxRect(pane_bounds, filter_expand);
    const float right = filter.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_;
    return D2D1::RectF(right - kCommandIconButtonDip * scale_, filter.top, right, filter.bottom);
}

D2D1_RECT_F MainRenderer::PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                               float filter_expand) const {
    const D2D1_RECT_F view = PaneViewButtonRect(pane_bounds, filter_expand);
    return D2D1::RectF(view.left - kCommandIconStepDip * scale_, view.top,
                       view.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_,
                       view.bottom);
}
D2D1_RECT_F MainRenderer::PaneNavUpRect(const D2D1_RECT_F& pane_bounds,
                                        float filter_expand) const {
    return StepLeftHeaderButton(PaneMediumIconsRect(pane_bounds, filter_expand), scale_);
}

D2D1_RECT_F MainRenderer::PaneNavForwardRect(const D2D1_RECT_F& pane_bounds,
                                             float filter_expand) const {
    return StepLeftHeaderButton(PaneNavUpRect(pane_bounds, filter_expand), scale_);
}

D2D1_RECT_F MainRenderer::PaneNavBackRect(const D2D1_RECT_F& pane_bounds,
                                          float filter_expand) const {
    return StepLeftHeaderButton(PaneNavForwardRect(pane_bounds, filter_expand), scale_);
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
    D2D1_RECT_F rc = FilterBoxRect(pane_bounds, expand);
    rc.left += 34.0f * scale_;
    rc.right -= (has_text ? 34.0f : 10.0f) * scale_;
    if (rc.right < rc.left + 24.0f * scale_) rc.right = rc.left + 24.0f * scale_;
    return rc;
}

MainRenderer::DetailsColumnLayout MainRenderer::DetailsColumns(
    const D2D1_RECT_F& pane_bounds, const PaneViewModel& vm) const {
    return DetailsColumns(pane_bounds, vm.details_column_dividers, vm.is_search,
                          vm.search_column_dividers);
}

MainRenderer::ColumnAutoWidths MainRenderer::AutoColumnWidths() const {
    using pulse::l10n::StringId;
    const std::wstring language = pulse::l10n::Get(StringId::TypeFolder) +
        pulse::l10n::Get(StringId::DateToday);
    if (auto_widths_scale_ == scale_ && auto_widths_language_ == language)
        return auto_widths_;
    ColumnAutoWidths out;
    IDWriteFactory2* factory = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->TextFormat() : nullptr;
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
        for (const auto& d : dates) date = std::max(date, measure(fmt, d));
        float type = std::max(header_w(StringId::ColumnType), measure(fmt, L"\u2014"));
        const StringId kinds[] = {StringId::TypeFolder, StringId::TypeFile, StringId::TypeTextDocument,
            StringId::TypeImage, StringId::TypeVideo, StringId::TypeAudio, StringId::TypeArchive,
            StringId::TypeApplication, StringId::Unavailable};
        const float chip = TypeChipWidthDip(L"XLSX");
        for (StringId id : kinds) type = std::max(type, measure(fmt, pulse::l10n::Get(id)) + chip);
        type = std::max(type, measure(fmt, L"AutoCAD " + pulse::l10n::Get(StringId::TypeFile)) + chip);
        float size = std::max(header_w(StringId::ColumnSize), measure(fmt, L"1023.9") + kSizeUnitDip);
        out.date = std::clamp(date + pad, 72.0f, 176.0f);
        out.type = std::clamp(type + pad, 64.0f, 196.0f);
        out.size = std::clamp(size + pad, 60.0f, 112.0f);
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
        compositor_->MeasureLumaText(chip, compositor_->SmallFormat(), w);
        w = std::max(w, MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), chip));
        w /= std::max(0.01f, scale_);
    } else {
        w = 8.0f * static_cast<float>(chip.size());
    }
    return w + 2.0f * kTypeChipPadDip + kTypeChipGapDip;
}

float MainRenderer::CellTextWidth(const std::wstring& text, bool small_text) const {
    if (text.empty() || !compositor_) return 0.0f;
    IDWriteTextFormat* fmt = small_text ? compositor_->SmallFormat() : compositor_->TextFormat();
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
    const std::array<float, 3>& dividers,
    bool search_view,
    const std::array<float, 4>& search_dividers) const {
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
    const float manual_date = ManualWidthDip(search_view ? search_dividers[1] : dividers[0]);
    const float manual_type = ManualWidthDip(search_view ? search_dividers[2] : dividers[1]);
    const float manual_size = ManualWidthDip(search_view ? search_dividers[3] : dividers[2]);
    struct Meta { ColumnKind kind; float width; };
    std::vector<Meta> meta{
        {ColumnKind::Date, (manual_date > 0.0f ? manual_date : fitted.date) * scale_},
        {ColumnKind::Type, (manual_type > 0.0f ? manual_type : fitted.type) * scale_},
        {ColumnKind::Size, (manual_size > 0.0f ? manual_size : fitted.size) * scale_}};
    auto meta_sum = [&] { float sum = 0.0f; for (const auto& m : meta) sum += m.width; return sum; };
    auto drop = [&](ColumnKind kind) {
        for (auto it = meta.begin(); it != meta.end(); ++it)
            if (it->kind == kind) { meta.erase(it); return true; }
        return false;
    };
    const float min_name = kDetailsFitNameDip * scale_;
    const float min_path = kDetailsFitPathDip * scale_;
    bool path_column = search_view;
    if (search_view && total - meta_sum() < min_name + min_path) {
        path_column = false;
        out.two_line = true;
    }
    // Low-value columns go first when the name would get too narrow.
    while (total - meta_sum() < min_name + (path_column ? min_path : 0.0f) && meta.size() > 1) {
        if (!drop(ColumnKind::Type)) drop(ColumnKind::Date);
    }
    const float floor_name = kDetailsMinNameDip * scale_;
    if (total - meta_sum() < floor_name) {
        const float scale_down = std::max(0.0f, total - floor_name) / std::max(1.0f, meta_sum());
        for (auto& m : meta) m.width *= scale_down;
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

float MainRenderer::ListRowHeightDip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float base = ListRowHeightDip(vm);
    if (vm.view_mode != ViewMode::Details || !vm.is_search || vm.content_results) return base;
    return DetailsColumns(pane_bounds, vm).two_line ? std::max(base, kDetailsTwoLineMinRowDip) : base;
}

namespace {
// Which stored slot holds the manual width of a column (-1: flexible column).
int ManualSlot(MainRenderer::ColumnKind kind, bool search_view) {
    using K = MainRenderer::ColumnKind;
    switch (kind) {
    case K::Name: return search_view ? 0 : -1;
    case K::Date: return search_view ? 1 : 0;
    case K::Type: return search_view ? 2 : 1;
    case K::Size: return search_view ? 3 : 2;
    default: return -1;
    }
}
}

void MainRenderer::AutoFitColumnDivider(const D2D1_RECT_F& pane_bounds,
                                        std::array<float, 3>& dividers, bool search_view,
                                        std::array<float, 4>& search_dividers, int divider_index) const {
    const DetailsColumnLayout layout = DetailsColumns(pane_bounds, dividers, search_view, search_dividers);
    if (divider_index < 0 || divider_index >= layout.count - 1) return;
    for (int side : {divider_index, divider_index + 1}) {
        const int slot = ManualSlot(layout.kinds[static_cast<size_t>(side)], search_view);
        if (slot < 0) continue;
        if (search_view) search_dividers[static_cast<size_t>(slot)] = 0.0f;
        else dividers[static_cast<size_t>(slot)] = 0.0f;
    }
}

namespace {
// Divider drag: a metadata column keeps an absolute width. The divider left
// of a metadata column resizes that column (its right edge stays put); the
// search name|path divider sets the name width.
template <size_t N>
std::array<float, N> ResizeColumns(const MainRenderer::DetailsColumnLayout& layout,
                                   std::array<float, N> stored, bool search_view,
                                   int divider_index, float cursor_x, float scale) {
    using K = MainRenderer::ColumnKind;
    if (divider_index < 0 || divider_index >= layout.count - 1 || scale <= 0.0f) return stored;
    for (float& value : stored) if (value <= 1.0f) value = 0.0f;   // drop legacy ratios
    const K left = layout.kinds[static_cast<size_t>(divider_index)];
    const K right = layout.kinds[static_cast<size_t>(divider_index + 1)];
    const float x0 = divider_index == 0 ? layout.left : layout.DividerX(divider_index - 1);
    const float x1 = layout.DividerX(divider_index);
    const float x2 = x1 + layout.widths[static_cast<size_t>(divider_index + 1)];
    const float min_meta = 48.0f * scale;
    const float min_flex = kDetailsMinNameDip * scale;
    auto put = [&](K kind, float px) {
        const int slot = ManualSlot(kind, search_view);
        if (slot >= 0 && static_cast<size_t>(slot) < N) stored[static_cast<size_t>(slot)] = std::max(px / scale, 1.01f);
    };
    if (left == K::Name && right == K::Path) {
        put(K::Name, std::clamp(cursor_x, x0 + min_flex, std::max(x0 + min_flex, x2 - kDetailsMinPathDip * scale)) - x0);
    } else if (left == K::Name || left == K::Path) {
        // Grow/shrink the metadata column on the right; its right edge is fixed.
        put(right, x2 - std::clamp(cursor_x, x0 + min_flex, x2 - min_meta));
    } else {
        // Between two metadata columns: resize the left one.
        put(left, std::clamp(cursor_x, x0 + min_meta, x2 - min_meta) - x0);
        put(right, layout.widths[static_cast<size_t>(divider_index + 1)]);
    }
    return stored;
}
}

std::array<float, 3> MainRenderer::ResizeDetailsColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 3>& dividers,
    int divider_index, float cursor_x) const {
    return ResizeColumns(DetailsColumns(pane_bounds, dividers), dividers, false,
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
                                       const std::array<float, 3>& column_dividers,
                                       bool search_view,
                                       const std::array<float, 4>& search_dividers,
                                       float row_height_px) const {
    const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, mode);
    if (mode != ViewMode::Details) {
        ViewLayout layout(mode, list, item_count, scroll_x, scroll_y, scale_, row_height_dip_);
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

bool MainRenderer::PointInItemName(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                   int source_index, float x, float y) const {
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
        ListRowHeightDip(vm, PaneListRect(pane_bounds, extra_top, vm.view_mode)) * scale_);
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
        compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(),
        entry.name, available);
    const float text_width = MeasureLayoutText(
        compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(), fitted);
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
void MainRenderer::DrawFolderIcon(float x, float y, float size, const Theme& theme) {
    (void)theme;
    if (compositor_ && compositor_->Dc())
        icon_cache_.Draw(compositor_->Dc(), D2D1::RectF(x, y, x + size, y + size),
                         L"", L"", true, FILE_ATTRIBUTE_DIRECTORY);
}

void MainRenderer::DrawFileIcon(float x, float y, float size, const Theme& theme) {
    (void)theme;
    if (compositor_ && compositor_->Dc())
        icon_cache_.Draw(compositor_->Dc(), D2D1::RectF(x, y, x + size, y + size),
                         L"", L"", false, FILE_ATTRIBUTE_NORMAL);
}

void MainRenderer::DrawEntryIcon(const ListEntryView& entry, float x, float y, float size,
                                 const Theme& theme) {
    const auto dest = D2D1::RectF(x, y, x + size, y + size);
    if (entry.record_only && compositor_ && compositor_->Dc()) {
        auto* dc = compositor_->Dc();
        MakeBrush(dc, WithAlpha(theme.text_secondary, 0.65f), brTextSecondary_);
        const float pad = size * 0.2f;
        const auto shape = D2D1::RoundedRect(D2D1::RectF(x + pad, y + pad,
            x + size - pad, y + size - pad), size * 0.04f, size * 0.04f);
        dc->DrawRoundedRectangle(shape, brTextSecondary_.get(), std::max(1.0f, size * 0.045f));
        dc->DrawLine({x + size * 0.36f, y + size * 0.5f},
            {x + size * 0.64f, y + size * 0.5f}, brTextSecondary_.get(), std::max(1.0f, size * 0.045f));
        return;
    }
    if (compositor_ && compositor_->Dc() &&
        icon_cache_.Draw(compositor_->Dc(), dest, entry.path, entry.name, entry.is_dir, entry.attrs)) {
        return;
    }
    if (entry.is_dir) DrawFolderIcon(x, y, size, theme);
    else DrawFileIcon(x, y, size, theme);
}
void MainRenderer::DrawPane(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    if (!vm.pane_slots.empty()) {
        for (int i = 0; i < static_cast<int>(vm.pane_slots.size()); ++i) {
            const auto& slot = vm.pane_slots[static_cast<size_t>(i)];
            DrawSinglePane(vm, slot.pane, slot.rect, i, slot.focused, slot.target, theme);
        }
        for (int i = 0; i < static_cast<int>(vm.splitters.size()); ++i) {
            const auto& sp = vm.splitters[static_cast<size_t>(i)];
            fluent::SplitterSpec spec;
            spec.bounds = sp.hit_rect;
            spec.vertical = sp.vertical;
            spec.state.hovered = vm.hover_region == static_cast<int>(HitTestResult::Splitter) &&
                                 vm.hover_control_index == i;
            spec.state.pressed = vm.splitter_pressed && spec.state.hovered;
            painter_.DrawSplitter(spec);
        }
        return;
    }
    DrawSinglePane(vm, vm.pane, ContentRect(rect.right, rect.bottom), 0, true, false, theme);
}
void MainRenderer::DrawPaneEmptyState(const WindowViewModel& vm, const PaneViewModel& pane,
                                      const D2D1_RECT_F& bounds, int pane_index,
                                      const Theme& theme) {
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
            compositor_->Dc()->DrawTextLayout({bounds.left + 16 * scale_,
                bounds.top + std::max(0.0f, (bounds.bottom - bounds.top - metrics.height) * 0.5f)},
                explanation.get(), brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }
    if (!pane.filter_text.empty()) {
        fluent::EmptyStateSpec empty;
        empty.bounds = bounds;
        empty.glyph = kIconSearch;
        empty.title = pulse::l10n::Get(pulse::l10n::StringId::NoMatches);
        painter_.DrawEmptyState(empty);
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
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::NoContent), layout.title,
                          compositor_->HeaderFormat(), theme.text,
                          fluent::HorizontalAlignment::Center);
        return;
    }

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
                                  const D2D1_RECT_F& bounds, int pane_index, bool focused, bool target,
                                  const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float x = bounds.left;
    const float y0 = bounds.top;
    const float w = bounds.right - bounds.left;
    const float bottom = bounds.bottom;
    if (w <= 1.0f || bottom - y0 <= 1.0f) return;

    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = y0;
    const std::wstring& title = pane.header_text;
    const D2D1_RECT_F filterRc = FilterBoxRect(bounds, pane.filter_expand);
    const D2D1_RECT_F mediumRc = PaneMediumIconsRect(bounds, pane.filter_expand);
    const D2D1_RECT_F viewRc = PaneViewButtonRect(bounds, pane.filter_expand);
    const D2D1_RECT_F navBackRc = PaneNavBackRect(bounds, pane.filter_expand);
    const D2D1_RECT_F navForwardRc = PaneNavForwardRect(bounds, pane.filter_expand);
    const D2D1_RECT_F navUpRc = PaneNavUpRect(bounds, pane.filter_expand);
    if (pane.header_drop) {
        MakeBrush(dc, WithAlpha(theme.accent, 0.18f), brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bounds.left + scale_, bounds.top + scale_,
            std::max(0.0f, w - 2.0f * scale_), pane_header_height_ - 2.0f * scale_,
            theme.radius_control * scale_);
    }
    const float textLeft = x + 8.0f * scale_;
    const float textRight = std::max(textLeft, navBackRc.left - 8.0f * scale_);
    const auto titleBadge = ChangeTitleRect(bounds, textRight, pane_header_height_, pane.title_change_badge, scale_, compositor_, pane.header_text);
    const float textWidth = (titleBadge.right > titleBadge.left ? titleBadge.left - 4 * scale_ : textRight) - textLeft;
    MakeBrush(dc, theme.text, brText_);
    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->HeaderFormat(), brText_.get(), title,
        textLeft, y, textWidth, pane_header_height_);

    DrawChangeBadge(compositor_, painter_, pane.title_change_badge, titleBadge, theme, scale_);

    auto headerIconButton = [&](const D2D1_RECT_F& rc, HitTestResult::Region region,
                                const wchar_t* glyph, const wchar_t* fallback,
                                bool active, float glyphScale) {
        const bool hovered = vm.hover_region == static_cast<int>(region) &&
                             vm.hover_control_index == pane_index;
        const D2D1_COLOR_F fill = active
            ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f)
            : (hovered ? theme.fill_hover : kTransparent);
        DrawButton(rc, theme, fill, glyph, fallback,
                   active ? theme.accent : theme.text,
                   true, true, glyphScale);
    };
    auto headerNavButton = [&](const D2D1_RECT_F& rc, HitTestResult::Region region,
                               const wchar_t* glyph, const wchar_t* fallback, bool enabled) {
        const bool hovered = enabled &&
                             vm.hover_region == static_cast<int>(region) &&
                             vm.hover_control_index == pane_index;
        DrawButton(rc, theme, hovered ? theme.fill_hover : kTransparent,
                   glyph, fallback, enabled ? theme.text : theme.text_disabled,
                   true, true, 0.76f);
    };
    headerNavButton(navBackRc, HitTestResult::NavBack, kIconBack, L"<", pane.can_go_back);
    headerNavButton(navForwardRc, HitTestResult::NavForward, kIconForward, L">",
                    pane.can_go_forward);
    headerNavButton(navUpRc, HitTestResult::NavUp, kIconUp, L"^", pane.can_go_up);
    headerIconButton(mediumRc, HitTestResult::PaneMediumIcons,
                     L"\xE7F4", L"M", pane.view_mode == ViewMode::MediumIcons, 0.66f);
    headerIconButton(viewRc, HitTestResult::PaneViewButton,
                     kIconView, L"=", false, 0.76f);

    if (pane.filter_expand <= 0.015f && !(focused && vm.filter_editing)) {
        const bool activeFilter = !pane.filter_text.empty();
        const bool hovered = vm.hover_region == static_cast<int>(HitTestResult::FilterBox) &&
                             vm.hover_control_index == pane_index;
        const D2D1_COLOR_F fill = activeFilter
            ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f)
            : (hovered ? theme.fill_hover : kTransparent);
        DrawButton(filterRc, theme, fill, kIconFilter, L"F",
                   activeFilter ? theme.accent : theme.text,
                   true, true, 0.76f);
    } else {
        fluent::TextFieldSpec filter{};
        filter.bounds = filterRc;
        filter.placeholder = pulse::l10n::Get(pulse::l10n::StringId::FilterPlaceholder);
        filter.text = pane.filter_text;
        filter.leading_glyph = kIconFilter;
        filter.compact_leading_glyph = true;
        filter.suppress_text = focused && vm.filter_editing;
        filter.state.focused = focused && vm.filter_editing;
        filter.state.hovered = vm.hover_region == static_cast<int>(HitTestResult::FilterBox) &&
                               vm.hover_control_index == pane_index;
        if (!pane.filter_text.empty()) filter.trailing_width = 30.0f;
        painter_.DrawTextField(filter);
        if (!pane.filter_text.empty() && pane.filter_expand >= 0.985f) {
            const bool hovered = vm.hover_region == static_cast<int>(HitTestResult::FilterClear) &&
                                 vm.hover_control_index == pane_index;
            DrawButton(FilterClearRect(bounds, pane.filter_expand), theme,
                       hovered ? theme.fill_hover : kTransparent, L"\xE711", L"×",
                       theme.text_secondary, true, true, 0.65f);
        }
    }
    y += pane_header_height_;

    const float bannerH = PaneBannerHeight(pane, w, scale_, compositor_);
    if (bannerH > 0.0f) {
        if (pane.is_changes) {
            ComPtr<IDWriteTextLayout> explanation;
            ChangeBannerLayout(pane, w, scale_, compositor_, explanation);
            if (explanation.get()) {
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                dc->DrawTextLayout({x + 16 * scale_, y + 8 * scale_}, explanation.get(),
                    brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            fluent::InfoBarSpec bar;
            bar.bounds = D2D1::RectF(x + 8 * scale_, y, x + w - 8 * scale_, y + bannerH - 4 * scale_);
            bar.title = pane.banner_title;
            bar.message = pane.banner_message;
            bar.kind = static_cast<fluent::InfoBarKind>(std::clamp(pane.banner_kind, 0, 3));
            bar.show_close = false;
            if (pane.is_content_search) {
                const auto action_bounds = bar.bounds;
                const float action_width = std::min(144.0f * scale_, (action_bounds.right - action_bounds.left) * 0.4f);
                bar.trailing_width = action_width / scale_;
                painter_.DrawInfoBar(bar);
                fluent::ButtonSpec action;
                action.bounds = D2D1::RectF(action_bounds.right - action_width, action_bounds.top, action_bounds.right, action_bounds.bottom);
                action.text = l10n::Get(l10n::StringId::ContentManageShort);
                action.kind = fluent::ButtonKind::Transparent;
                action.state.hovered = IsHovered(vm, HitTestResult::ContentIndexManage);
                painter_.DrawButton(action);
            } else painter_.DrawInfoBar(bar);
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
            segment.bounds.left += 3.0f * scale_;
            segment.bounds.right -= 3.0f * scale_;
            segment.bounds.top += 3.0f * scale_;
            segment.bounds.bottom -= 3.0f * scale_;
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
    if (ShowsColumnHeader(pane.view_mode)) {
        // Re-set the brush: earlier drawing (tray deck pills, toolbar) may
        // have left a different color on this shared member.
        MakeBrush(dc, theme.fill_input, brFillInput_);
        FillRect(dc, brFillInput_.get(), x, y, w, column_header_height_);
        FillRect(dc, brStrokeDivider_.get(), x, y + column_header_height_ - 1, w, 1);
        const DetailsColumnLayout columns = DetailsColumns(bounds, pane);
        if (pane_index >= 0 && pane_index < static_cast<int>(painted_columns_.size())) {
            uint32_t mask = columns.two_line ? (1u << 8) : 0u;
            for (int col = 0; col < columns.count; ++col)
                mask |= 1u << static_cast<uint32_t>(columns.kinds[static_cast<size_t>(col)]);
            painted_columns_[static_cast<size_t>(pane_index)] = mask;
        }
        float cx = columns.left;
        auto drawCol = [&](const std::wstring& label, SortColumn col, float cw, bool right = false) {
            const bool active = !pane.curated_order && pane.sort_column == col;
            MakeBrush(dc, active ? theme.accent : theme.text, brText_);
            IDWriteTextFormat* fmt = compositor_->HeaderFormat();
            const float textInset = 8.0f * scale_;
            const float contentLeft = cx + textInset;
            const float availableW = std::max(0.0f, cw - textInset * 2.0f);
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
            DrawTextRect(dc, fmt, brText_.get(), label, groupLeft, y,
                         labelW, column_header_height_);
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
        // The path column is display-only: never an active/clickable sort.
        for (int col = 0; col < columns.count; ++col) {
            const float cw = columns.widths[static_cast<size_t>(col)];
            switch (columns.kinds[static_cast<size_t>(col)]) {
            case ColumnKind::Name:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnName), SortColumn::Name, cw, false);
                break;
            case ColumnKind::Path:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnPath), SortColumn::Path, cw, false);
                break;
            case ColumnKind::Date:
                drawCol(pane.date_column_label.empty()
                            ? pulse::l10n::Get(pulse::l10n::StringId::ColumnModified)
                            : pane.date_column_label,
                        SortColumn::Mtime, cw, false);
                break;
            case ColumnKind::Type:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnType), SortColumn::Type, cw, false);
                break;
            case ColumnKind::Size:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnSize), SortColumn::Size, cw, true);
                break;
            }
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
        y += column_header_height_;
    }
    float listH = bottom - y;
    dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if ((!pane.loading || pane.is_changes) && pane.EntryCount() == 0)
        DrawPaneEmptyState(vm, pane, D2D1::RectF(x, y, x + w, bottom), pane_index, theme);
    else
        DrawList(pane, x, y, w, listH, theme, vm.hover_region, vm.hover_control_index);
    dc->PopAxisAlignedClip();

    const float radius = theme.radius_control * scale_;
    D2D1_COLOR_F frameColor = theme.stroke_card;
    if (focused) frameColor = WithAlpha(theme.accent, 0.28f);
    else if (target) frameColor = WithAlpha(theme.accent, 0.22f);
    MakeBrush(dc, frameColor, brAccent_);
    const float strokeW = focused ? 1.5f * scale_ : 1.0f * scale_;
    D2D1_ROUNDED_RECT frame = D2D1::RoundedRect(
        D2D1::RectF(bounds.left + 0.5f * scale_, bounds.top + 0.5f * scale_,
                    bounds.right - 0.5f * scale_, bounds.bottom - 0.5f * scale_),
        radius, radius);
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

void MainRenderer::DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                                     const Theme& theme, bool selected, const std::vector<NameMatchRange>& matches,
                                     bool dim_extension) {
    (void)selected;
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() ||
        !compositor_->TextFormat() || name.empty() || w <= 1.0f) {
        return;
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->TextFormat();
    MakeBrush(dc, theme.text, brText_);

    const auto old_wrap = fmt->GetWordWrapping();
    const auto old_align = fmt->GetTextAlignment();
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    const std::wstring shown = FitHighlightedFileName(compositor_, factory, fmt, name, w, matches, scale_);
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
    ComPtr<IDWriteTextLayout> highlighted;
    if (!visible_matches.empty() && SUCCEEDED(factory->CreateTextLayout(shown.c_str(),
        static_cast<UINT32>(shown.size()), fmt, w, h, &highlighted))) {
        highlighted->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        highlighted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        highlighted->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<ID2D1SolidColorBrush> ext_brush;
        if (ext_at != std::wstring::npos && SUCCEEDED(dc->CreateSolidColorBrush(ext_color, &ext_brush)))
            highlighted->SetDrawingEffect(ext_brush.get(), {static_cast<UINT32>(ext_at),
                                                            static_cast<UINT32>(shown.size() - ext_at)});
        DrawNameHighlightBackground(compositor_, highlighted.get(), {x, y}, rc, visible_matches, theme, scale_);
        dc->DrawTextLayout({x, y}, highlighted.get(), brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else if (ext_at != std::wstring::npos && !IsHighContrast()) {
        const std::wstring stem = shown.substr(0, ext_at);
        const std::wstring ext = shown.substr(ext_at);
        const float stem_w = std::min(w, CellTextWidth(stem));
        if (!compositor_->DrawLumaText(stem, fmt, D2D1::RectF(x, y, x + stem_w + 2.0f * scale_, y + h),
                                       brText_->GetColor(), theme.bg, DWRITE_TEXT_ALIGNMENT_LEADING) ||
            !compositor_->DrawLumaText(ext, fmt, D2D1::RectF(x + stem_w, y, x + w, y + h),
                                       ext_color, theme.bg, DWRITE_TEXT_ALIGNMENT_LEADING)) {
            dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
    } else if (IsHighContrast() || !compositor_->DrawLumaText(
            shown, fmt, rc, brText_->GetColor(), theme.bg,
            DWRITE_TEXT_ALIGNMENT_LEADING)) {
        dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    fmt->SetWordWrapping(old_wrap);
    fmt->SetTextAlignment(old_align);
}

void MainRenderer::DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color, const Theme& theme, const std::vector<NameMatchRange>& matches) {
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() || name.empty()) return;
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(
            name.c_str(), static_cast<UINT32>(name.size()), compositor_->TextFormat(),
            width, height, &layout)) || !layout.get()) {
        return;
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    compositor_->DwriteFactory()->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.get());
    DrawNameHighlightBackground(compositor_, layout.get(), {bounds.left, bounds.top}, bounds, matches, theme, scale_);
    MakeBrush(compositor_->Dc(), color, brText_);
    compositor_->Dc()->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(),
                                      brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
D2D1_RECT_F MainRenderer::RenameFieldRect(const PaneViewModel& vm, const D2D1_RECT_F& list,
                                          int source_index) {
    // Mirror of the frame geometry in DrawList for vm.rename_index — keep in sync.
    if (!compositor_ || !compositor_->DwriteFactory() || source_index < 0) return {};
    const int view = vm.ViewIndex(source_index);
    if (view < 0) return {};
    const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(source_index));
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                      scale_, ListRowHeightDip(vm, list));
    const D2D1_RECT_F cell = layout.ItemRect(view);
    const D2D1_RECT_F nameRc = layout.NameRect(view);
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
        ? DetailsColumns(list, vm).DividerX(0) - margin_
        : nameRc.right;
    if (iconGrid && tagDotCount > 0) {
        const float fullNameW = MeasureLayoutText(
            compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(), e.name);
        const float nameGap = 4.0f * scale_;
        const float cellW = nameRc.right - nameRc.left;
        const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
        const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
            ? SpreadTagsWidth(tagDotCount, dot, nameGap)
            : OverlapTagsWidth(tagDotCount, dot);
        const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
        nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
        const float lineH = std::min(textH, 24.0f * scale_);
        textY = nameRc.top + (textH - lineH) * 0.5f;
        textH = lineH;
    }
    const NameTrail trail = LayoutNameTrail(
        nameX, textY, textH, nameColRight, cell.top, cell.bottom, scale_,
        e.name, tagDotCount, 0.0f, false, false, false,
        compositor_, compositor_->DwriteFactory(), compositor_->TextFormat());
    // Reserve the frame inset and EDIT margins as well as the name's ink width.
    const float field_w = std::max(40.0f * scale_,
        std::min(nameColRight - nameX, trail.name_w + 14.0f * scale_));
    const float field_h = std::max(22.0f * scale_, std::min(textH, 30.0f * scale_));
    return D2D1::RectF(nameX, textY, nameX + field_w, textY + field_h);
}

void MainRenderer::DrawList(const PaneViewModel& vm, float x, float y, float w, float h,
                            const Theme& theme, int hover_region, int hover_control_index) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const auto highlight_terms = NameHighlightTerms(vm.filter_text, vm.is_search ? vm.search_query : L" ");
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    MakeBrush(dc, theme.accent, brAccent_);
    if (vm.loading && !vm.search_retaining_results) {
        // Directory enumeration is asynchronous. Leave the list quiet for its
        // brief initial frame instead of presenting it like a search request.
        return;
    }
    const size_t entryCount = vm.EntryCount();
    if (entryCount == 0) return;
    const D2D1_RECT_F viewport = D2D1::RectF(x, y, x + w, y + h);
    ViewLayout layout(vm.view_mode, viewport, entryCount, vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, viewport));
    const auto [startIdx, endIdx] = layout.VisibleRange();
    const DetailsColumnLayout detailsColumns = DetailsColumns(viewport, vm);
    const bool detailsView = vm.view_mode == ViewMode::Details;
    D2D1_COLOR_F zebraColor = theme.text;
    zebraColor.a = (theme.bg.r < 0.5f) ? 0.035f : 0.025f;

    for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
        const D2D1_RECT_F cell = layout.ItemRect(i);
        if (cell.right < x || cell.left > x + w || cell.bottom < y || cell.top > y + h) continue;
        const int src = vm.SourceIndex(i);
        if (src < 0) continue;
        if(vm.content_results && !vm.content_results->Ready(static_cast<size_t>(src))) {
            vm.content_results->Prefetch(static_cast<size_t>(src));
            MakeBrush(dc,theme.text_secondary,brTextSecondary_);
            DrawTextEndEllipsis(dc,compositor_->DwriteFactory(),compositor_->TextFormat(),brTextSecondary_.get(),
                l10n::Get(l10n::StringId::LoadingEllipsis),cell.left+40*scale_,cell.top,
                cell.right-cell.left-40*scale_,cell.bottom-cell.top);
            continue;
        }
        const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));

        bool selected = vm.IsRowSelected(src);
        bool focused = (src == vm.selected_index);
        bool hover = (src == vm.hover_index);
        bool cut = e.record_only || vm.cut_names.contains(e.name);

        const float inset = 4.0f * scale_;
        if (detailsView && list_zebra_ && (i & 1) && !selected && !hover && !IsHighContrast()) {
            MakeBrush(dc, zebraColor, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), cell.left + inset, cell.top,
                std::max(0.0f, cell.right - cell.left - inset * 2),
                std::max(0.0f, cell.bottom - cell.top), theme.radius_control * scale_);
        }
        if (hover) {
            FillRoundedRect(dc, brFillHover_.get(), cell.left + inset, cell.top + scale_,
                std::max(0.0f, cell.right - cell.left - inset * 2),
                std::max(0.0f, cell.bottom - cell.top - scale_ * 2),
                theme.radius_control * scale_);
        }
        if (selected) {
            const D2D1_RECT_F sel = D2D1::RectF(
                cell.left + inset, cell.top + scale_,
                cell.right - inset, cell.bottom - scale_);
            FillRoundedRect(dc, brFillSelected_.get(), sel.left, sel.top,
                std::max(0.0f, sel.right - sel.left),
                std::max(0.0f, sel.bottom - sel.top),
                theme.radius_control * scale_);
            if (focused) {
                FillRoundedAccent(dc, brAccent_.get(), sel,
                    theme.radius_control * scale_, 3.0f * scale_, AccentEdge::Left);
            }
        }
        if (src == vm.drop_target_index) {
            D2D1_RECT_F rc = D2D1::RectF(cell.left + inset, cell.top + scale_,
                                         cell.right - inset, cell.bottom - scale_);
            dc->DrawRoundedRectangle(
                D2D1::RoundedRect(rc, theme.radius_control * scale_, theme.radius_control * scale_),
                brAccent_.get(), 2.0f * scale_);
        }

        const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                              vm.view_mode == ViewMode::LargeIcons ||
                              vm.view_mode == ViewMode::MediumIcons;
        D2D1_RECT_F nameRc = layout.NameRect(i);
        const auto changeIt = vm.change_badges.find(src);
        const ChangeBadge* change = changeIt != vm.change_badges.end() && HasChangeBadge(changeIt->second) ? &changeIt->second : nullptr;
        D2D1_RECT_F iconRect = layout.IconRect(i);
        if (change && iconGrid) {
            const float bw = std::min(ChangeBadgeWidth(*change, scale_, compositor_), cell.right - cell.left - 12 * scale_);
            const float bx = (cell.left + cell.right - bw) * 0.5f;
            DrawChangeBadge(compositor_, painter_, *change, D2D1::RectF(bx, cell.top + 2 * scale_, bx + bw, cell.top + 20 * scale_), theme, scale_);
            const float icon_inset = std::min(18 * scale_, (iconRect.bottom - iconRect.top) * 0.3f);
            iconRect.top += icon_inset;
            iconRect.left += icon_inset * 0.5f; iconRect.right -= icon_inset * 0.5f;
        }
        const float snappedIconW = std::max(1.0f, std::round(iconRect.right - iconRect.left));
        const float snappedIconH = std::max(1.0f, std::round(iconRect.bottom - iconRect.top));
        iconRect.left = std::round(iconRect.left);
        iconRect.top = std::round(iconRect.top);
        iconRect.right = iconRect.left + snappedIconW;
        iconRect.bottom = iconRect.top + snappedIconH;
        const float iconX = iconRect.left;
        const float iconY = iconRect.top;
        const float renderedIconSize = std::min(snappedIconW, snappedIconH);
        long requestedPixels = std::lround(renderedIconSize);
        if (vm.view_mode == ViewMode::ExtraLargeIcons || vm.view_mode == ViewMode::LargeIcons)
            requestedPixels = std::max(256l, requestedPixels);
        else if (vm.view_mode == ViewMode::MediumIcons)
            requestedPixels = std::max(128l, requestedPixels);
        const bool drewThumbnail = !e.record_only && UsesThumbnails(vm.view_mode) &&
            thumbnail_cache_.Draw(dc, iconRect, e.path, e.attrs,
                static_cast<uint32_t>(std::clamp(requestedPixels, 32l, 512l)),
                vm.view_generation, e.modified_value, e.size_value)
                == PreviewDrawResult::Bitmap;
        if (!drewThumbnail) DrawEntryIcon(e, iconX, iconY, renderedIconSize, theme);

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
        const bool showRowActions = vm.view_mode == ViewMode::Details &&
            src != vm.rename_index &&
            (hover || (selected && vm.selected_count == 1) || e.starred);
        const float nameColRight = vm.view_mode == ViewMode::Details
            ? detailsColumns.DividerX(0) - margin_
            : nameRc.right;
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
        const auto name_matches = NameMatchRanges(e.name, highlight_terms);
        if (iconGrid && tagDotCount > 0) {
            const float fullNameW = MeasureLayoutText(
                compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(), e.name) +
                HighlightPaddingWidth(e.name, e.name, name_matches, scale_);
            const float nameGap = 4.0f * scale_;
            const float cellW = nameRc.right - nameRc.left;
            const float leftover = std::max(0.0f, cellW - fullNameW - nameGap);
            const float dotsW = SpreadTagsWidth(tagDotCount, dot, nameGap) <= leftover + 0.5f
                ? SpreadTagsWidth(tagDotCount, dot, nameGap)
                : OverlapTagsWidth(tagDotCount, dot);
            const float groupW = std::min(cellW, fullNameW + nameGap + dotsW);
            nameX = nameRc.left + std::max(0.0f, (cellW - groupW) * 0.5f);
            const float lineH = std::min(textH, 24.0f * scale_);
            textY = nameRc.top + (textH - lineH) * 0.5f;
            textH = lineH;
        }
        const float badgeW = change && !iconGrid ? ChangeBadgeWidth(*change, scale_, compositor_) : vm.view_mode == ViewMode::Details && !e.badge.empty()
            ? std::min(108.0f * scale_, painter_.MeasureTagWidth(e.badge)) : 0.0f;
        const NameTrail trail = LayoutNameTrail(
            nameX, textY, textH, nameColRight, cell.top, cell.bottom, scale_,
            e.name, tagDotCount, badgeW, showStar, showNewTab, showMore,
            compositor_, compositor_->DwriteFactory(), compositor_->TextFormat(), change != nullptr,
            vm.view_mode == ViewMode::Details ? (e.is_dir ? 3 : 2) : 0, name_matches);
        if (src == vm.rename_index) {
            const D2D1_RECT_F fieldRc = RenameFieldRect(vm, viewport, src);
            fluent::ControlState fieldState{};
            fieldState.focused = true;
            painter_.DrawTextFieldFrame(fieldRc, fieldState);
        } else {
            D2D1_COLOR_F nameColor = cut ? WithAlpha(theme.text, 0.55f) : theme.text;
            MakeBrush(dc, nameColor, brText_);
            if (iconGrid && tagDotCount == 0) {
                DrawCenteredIconName(e.name, nameRc, nameColor, theme, name_matches);
            } else {
                Theme name_theme = theme;
                name_theme.text = nameColor;
                DrawTruncatedName(e.name, trail.name_x, textY, trail.name_w, textH, name_theme, selected, name_matches,
                                  detailsView && !e.is_dir);
            }
            if (change && !iconGrid) DrawChangeBadge(compositor_, painter_, *change, trail.badge, theme, scale_);
            if (!change && !e.badge.empty() && trail.badge.right > trail.badge.left) {
                painter_.DrawTag({trail.badge, e.badge, e.badge_color});
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

        MakeBrush(dc, cut ? WithAlpha(theme.text_secondary, 0.55f) : theme.text_secondary, brTextSecondary_);
        if (vm.view_mode == ViewMode::Details) {
            const auto draw_detail_text = [&](std::wstring_view text, float left, float width,
                                              DWRITE_TEXT_ALIGNMENT alignment) {
                // Every metadata column centers in the row, independently of
                // the filename's optional two-line name/snippet layout.
                const auto text_bounds = D2D1::RectF(left, cell.top + scale_, left + width, cell.bottom - scale_);
                if (!IsHighContrast() && compositor_->DrawLumaText(
                        text, compositor_->TextFormat(), text_bounds,
                        brTextSecondary_->GetColor(), theme.bg, alignment)) {
                    return;
                }
                auto* format = compositor_->TextFormat();
                const auto previous = format->GetTextAlignment();
                const auto previous_paragraph = format->GetParagraphAlignment();
                format->SetTextAlignment(alignment);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), format, brTextSecondary_.get(),
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
                switch (detailsColumns.kinds[static_cast<size_t>(col)]) {
                case ColumnKind::Path: {
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
                    draw_detail_text(fit(date, avail), left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                }
                case ColumnKind::Type: {
                    const bool deleted_change = vm.is_changes &&
                        e.type_text == pulse::l10n::Get(pulse::l10n::StringId::ChangeDeleted);
                    float typeLeft = left;
                    const std::wstring chip = vm.is_changes ? std::wstring() : TypeChipLabel(e.name, e.is_dir);
                    const float chipW = TypeChipWidthDip(chip) * scale_;
                    if (!chip.empty() && chipW < avail * 0.6f && !IsHighContrast()) {
                        const float chipBoxW = chipW - kTypeChipGapDip * scale_;
                        const float chipH = 17.0f * scale_;
                        const float chipTop = std::round((cell.top + cell.bottom - chipH) * 0.5f);
                        D2D1_COLOR_F hue = HexColor(TypeChipRgb(chip));
                        D2D1_COLOR_F ink = hue;
                        if (!(theme.bg.r < 0.5f)) { ink.r *= 0.62f; ink.g *= 0.62f; ink.b *= 0.62f; }
                        MakeBrush(dc, WithAlpha(hue, (theme.bg.r < 0.5f) ? 0.18f : 0.14f), brFillInput_);
                        FillRoundedRect(dc, brFillInput_.get(), left, chipTop, chipBoxW, chipH, 4.0f * scale_);
                        const auto chipRc = D2D1::RectF(left, chipTop, left + chipBoxW, chipTop + chipH);
                        if (!compositor_->DrawLumaText(chip, compositor_->SmallFormat(), chipRc,
                                                       WithAlpha(ink, cut ? 0.55f : 1.0f), theme.bg,
                                                       DWRITE_TEXT_ALIGNMENT_CENTER)) {
                            MakeBrush(dc, ink, brTagDot_);
                            DrawTextRect(dc, compositor_->SmallFormat(), brTagDot_.get(), chip,
                                         chipRc.left, chipRc.top, chipBoxW, chipH);
                        }
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
                    // Number right-aligned, unit in its own sub-column: digits line up.
                    const size_t space = e.size_text.find_last_of(L' ');
                    const float unitW = kSizeUnitDip * scale_;
                    if (space != std::wstring::npos && space > 0 && avail > unitW * 1.8f) {
                        const std::wstring value = e.size_text.substr(0, space);
                        const std::wstring unit = e.size_text.substr(space + 1);
                        draw_detail_text(value, left, avail - unitW, DWRITE_TEXT_ALIGNMENT_TRAILING);
                        MakeBrush(dc, WithAlpha(secondary, secondary.a * 0.72f), brTextSecondary_);
                        draw_detail_text(unit, left + avail - unitW + 4.0f * scale_, unitW - 4.0f * scale_,
                                         DWRITE_TEXT_ALIGNMENT_LEADING);
                        MakeBrush(dc, secondary, brTextSecondary_);
                    } else {
                        draw_detail_text(fit(e.size_text, avail), left, avail, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
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
        } else if (vm.view_mode == ViewMode::Tiles || vm.view_mode == ViewMode::Content) {
            std::wstring meta = !e.snippet.empty() ? e.snippet : e.type_text;
            if (e.snippet.empty() && !e.size_text.empty())
                meta += (meta.empty() ? L"" : L" \u00B7 ") + e.size_text;
            if (e.snippet.empty() && vm.view_mode == ViewMode::Content && !e.date_text.empty())
                meta += (meta.empty() ? L"" : L" \u00B7 ") + e.date_text;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), meta,
                nameRc.left, nameRc.top + 25.0f * scale_,
                std::max(0.0f, nameRc.right - nameRc.left), 22.0f * scale_);
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

    DrawScrollbar(vm, x, y, w, h, theme);
    if (vm.view_mode == ViewMode::List && layout.MaxScrollX() > 0.0f) {
        const float viewW = std::max(1.0f, w);
        const float totalW = layout.ContentWidth();
        const float thumbW = std::max(28.0f * scale_, viewW * (viewW / totalW));
        const float travel = std::max(1.0f, viewW - thumbW);
        const float thumbX = x + (vm.scroll_x / layout.MaxScrollX()) * travel;
        FillRoundedRect(dc, brScrollbar_.get(), thumbX, y + h - 8.0f * scale_,
                        thumbW, 6.0f * scale_, 3.0f * scale_);
    }
}

void MainRenderer::DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme) {
    (void)theme;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ViewLayout layout(vm.view_mode, D2D1::RectF(x, y, x + w, y + h), vm.EntryCount(),
                      vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, D2D1::RectF(x, y, x + w, y + h)));
    const float totalH = layout.ContentHeight();
    auto sb = ComputeScrollbar(h, totalH, vm.scroll_y, layout.Metrics().cell_height);
    if (!sb.valid) return;
    float thumbW = 6 * scale_;
    FillRoundedRect(dc, brScrollbar_.get(), x + w - 10.0f * scale_, y + sb.thumbY,
        thumbW, sb.thumbH, thumbW * 0.5f);
}
bool MainRenderer::PaneScrollbarGeometry(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                          D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    const auto list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    max_scroll = layout.MaxScrollY();
    const auto metrics = ComputeScrollbar(list.bottom - list.top, layout.ContentHeight(),
                                           vm.scroll_y, layout.Metrics().cell_height);
    if (!metrics.valid) return false;
    track = D2D1::RectF(list.right - 14 * scale_, list.top, list.right, list.bottom);
    thumb = D2D1::RectF(track.left, list.top + metrics.thumbY, track.right,
                         list.top + metrics.thumbY + metrics.thumbH);
    return true;
}

float MainRenderer::MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.MaxScrollY();
}

float MainRenderer::MaxScrollXForPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.MaxScrollX();
}

D2D1_RECT_F MainRenderer::ItemRectInPane(const PaneViewModel& vm,
                                         const D2D1_RECT_F& pane_bounds,
                                         int view_index) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.ItemRect(view_index);
}

int MainRenderer::MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                int current, int dx, int dy) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.MoveIndex(current, dx, dy);
}

int MainRenderer::PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.PageDelta();
}

std::pair<int, int> MainRenderer::VisibleRangeInPane(
    const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    return layout.VisibleRange();
}

int MainRenderer::RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const {
    return ItemFromPointInPane(vm, pane_bounds, pane_bounds.left + 1.0f, y);
}

int MainRenderer::ItemFromPointInPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds,
                                      float x, float y) const {
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list));
    int idx = layout.HitTest(x, y);
    if (idx < 0) return -1;
    return vm.SourceIndex(idx);
}

} // namespace pulse::ui
