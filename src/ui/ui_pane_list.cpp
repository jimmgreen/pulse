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

D2D1_RECT_F MainRenderer::FilterBoxRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    const float w = pane_bounds.right - pane_bounds.left;
    const float expandedW = std::max(32.0f * scale_,
        std::min(220.0f * scale_, w * 0.34f));
    const float t = std::clamp(expand, 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t);
    const float filterW = 32.0f * scale_ +
        (expandedW - 32.0f * scale_) * eased;
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

D2D1_RECT_F MainRenderer::FilterEditRect(const D2D1_RECT_F& pane_bounds, float expand) const {
    D2D1_RECT_F rc = FilterBoxRect(pane_bounds, expand);
    rc.left += 34.0f * scale_;
    rc.right -= 10.0f * scale_;
    if (rc.right < rc.left + 24.0f * scale_) rc.right = rc.left + 24.0f * scale_;
    return rc;
}

MainRenderer::DetailsColumnLayout MainRenderer::DetailsColumns(
    const D2D1_RECT_F& pane_bounds, const PaneViewModel& vm) const {
    return DetailsColumns(pane_bounds, vm.details_column_dividers, vm.is_search,
                          vm.search_column_dividers);
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
    if (total <= 0.0f) return out;

    if (search_view) {
        // Search results: 名称 / 路径 / 修改日期 / 类型 / 大小. Folder-view
        // dividers do not apply; search keeps its own 4-edge ratios.
        out.count = 5;
        std::array<float, 5> minimums{
            kDetailsMinNameDip * scale_, kDetailsMinPathDip * scale_,
            kDetailsMinDateDip * scale_, kDetailsMinTypeDip * scale_,
            kDetailsMinSizeDip * scale_ };
        const float minimumTotal = minimums[0] + minimums[1] + minimums[2] +
                                   minimums[3] + minimums[4];
        if (minimumTotal > total) {
            const float shrink = total / minimumTotal;
            for (float& width : minimums) width *= shrink;
        }
        const bool customized = search_dividers[0] > 0.0f &&
            search_dividers[1] > search_dividers[0] &&
            search_dividers[2] > search_dividers[1] &&
            search_dividers[3] > search_dividers[2] &&
            search_dividers[3] < 1.0f;
        std::array<float, 4> desired{};
        if (customized) {
            for (size_t i = 0; i < desired.size(); ++i)
                desired[i] = search_dividers[i] * total;
        } else {
            const float fixed = (kDetailsDateDip + kDetailsTypeDip + kDetailsSizeDip) * scale_;
            const float flexible = (std::max)(0.0f, total - fixed);
            desired[0] = flexible * 0.55f;
            desired[1] = flexible;
            desired[2] = desired[1] + kDetailsDateDip * scale_;
            desired[3] = desired[2] + kDetailsTypeDip * scale_;
        }
        const float edge0 = std::clamp(
            desired[0], minimums[0],
            total - minimums[1] - minimums[2] - minimums[3] - minimums[4]);
        const float edge1 = std::clamp(
            desired[1], edge0 + minimums[1],
            total - minimums[2] - minimums[3] - minimums[4]);
        const float edge2 = std::clamp(
            desired[2], edge1 + minimums[2],
            total - minimums[3] - minimums[4]);
        const float edge3 = std::clamp(
            desired[3], edge2 + minimums[3], total - minimums[4]);
        out.widths = { edge0, edge1 - edge0, edge2 - edge1, edge3 - edge2,
                       total - edge3 };
        return out;
    }

    std::array<float, 4> minimums{
        kDetailsMinNameDip * scale_, kDetailsMinDateDip * scale_,
        kDetailsMinTypeDip * scale_, kDetailsMinSizeDip * scale_ };
    const float minimumTotal = minimums[0] + minimums[1] +
                               minimums[2] + minimums[3];
    if (minimumTotal > total) {
        const float shrink = total / minimumTotal;
        for (float& width : minimums) width *= shrink;
    }

    const bool customized = dividers[0] > 0.0f &&
        dividers[1] > dividers[0] && dividers[2] > dividers[1] &&
        dividers[2] < 1.0f;
    std::array<float, 3> desired{};
    if (customized) {
        for (size_t i = 0; i < desired.size(); ++i)
            desired[i] = dividers[i] * total;
    } else {
        desired[0] = total - (kDetailsDateDip + kDetailsTypeDip + kDetailsSizeDip) * scale_;
        desired[1] = total - (kDetailsTypeDip + kDetailsSizeDip) * scale_;
        desired[2] = total - kDetailsSizeDip * scale_;
    }

    const float edge0 = std::clamp(
        desired[0], minimums[0],
        total - minimums[1] - minimums[2] - minimums[3]);
    const float edge1 = std::clamp(
        desired[1], edge0 + minimums[1],
        total - minimums[2] - minimums[3]);
    const float edge2 = std::clamp(
        desired[2], edge1 + minimums[2], total - minimums[3]);
    out.widths = {edge0, edge1 - edge0, edge2 - edge1, total - edge2};
    return out;
}

std::array<float, 3> MainRenderer::ResizeDetailsColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 3>& dividers,
    int divider_index, float cursor_x) const {
    DetailsColumnLayout layout = DetailsColumns(pane_bounds, dividers);
    const float total = layout.right - layout.left;
    if (divider_index < 0 || divider_index >= 3 || total <= 0.0f)
        return dividers;

    const float adjacentTotal = layout.widths[static_cast<size_t>(divider_index)] +
        layout.widths[static_cast<size_t>(divider_index + 1)];
    const float outerLeft = divider_index == 0
        ? layout.left : layout.DividerX(divider_index - 1);
    const float minScale = std::min(1.0f, total /
        ((kDetailsMinNameDip + kDetailsMinDateDip + kDetailsMinTypeDip +
          kDetailsMinSizeDip) * scale_));
    const float minimumDip[4] = {
        kDetailsMinNameDip, kDetailsMinDateDip, kDetailsMinTypeDip, kDetailsMinSizeDip};
    const float leftMinimum = minimumDip[divider_index] * scale_ * minScale;
    const float rightMinimum = minimumDip[divider_index + 1] * scale_ * minScale;
    const float divider = std::clamp(
        cursor_x, outerLeft + leftMinimum,
        outerLeft + adjacentTotal - rightMinimum);

    std::array<float, 3> result{};
    for (int i = 0; i < 3; ++i)
        result[static_cast<size_t>(i)] = layout.DividerX(i);
    result[static_cast<size_t>(divider_index)] = divider;
    for (float& edge : result) edge = (edge - layout.left) / total;
    return result;
}

std::array<float, 4> MainRenderer::ResizeSearchColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const std::array<float, 4>& dividers,
    int divider_index, float cursor_x) const {
    DetailsColumnLayout layout = DetailsColumns(pane_bounds, {}, true, dividers);
    const float total = layout.right - layout.left;
    if (divider_index < 0 || divider_index >= 4 || total <= 0.0f)
        return dividers;

    const float adjacentTotal = layout.widths[static_cast<size_t>(divider_index)] +
        layout.widths[static_cast<size_t>(divider_index + 1)];
    const float outerLeft = divider_index == 0
        ? layout.left : layout.DividerX(divider_index - 1);
    const float minScale = std::min(1.0f, total /
        ((kDetailsMinNameDip + kDetailsMinPathDip + kDetailsMinDateDip +
          kDetailsMinTypeDip + kDetailsMinSizeDip) * scale_));
    const float minimumDip[5] = {
        kDetailsMinNameDip, kDetailsMinPathDip, kDetailsMinDateDip,
        kDetailsMinTypeDip, kDetailsMinSizeDip};
    const float leftMinimum = minimumDip[divider_index] * scale_ * minScale;
    const float rightMinimum = minimumDip[divider_index + 1] * scale_ * minScale;
    const float divider = std::clamp(
        cursor_x, outerLeft + leftMinimum,
        outerLeft + adjacentTotal - rightMinimum);

    std::array<float, 4> result{};
    for (int i = 0; i < 4; ++i)
        result[static_cast<size_t>(i)] = layout.DividerX(i);
    result[static_cast<size_t>(divider_index)] = divider;
    for (float& edge : result) edge = (edge - layout.left) / total;
    return result;
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
    D2D1_RECT_F name = NameCellRect(
        pane_bounds, view_index, vm.scroll_y,
        vm.banner_message.empty() ? 0.0f : 36.0f * scale_,
        vm.view_mode, vm.scroll_x, vm.EntryCount(), vm.details_column_dividers,
        vm.is_search, vm.search_column_dividers, ListRowHeightDip(vm) * scale_);
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
    const float textWidth = textRight - textLeft;
    MakeBrush(dc, theme.text, brText_);
    DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), title,
        textLeft, y, textWidth, pane_header_height_);

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
        painter_.DrawTextField(filter);
    }
    y += pane_header_height_;

    const float bannerH = pane.banner_message.empty() ? 0.0f : 36.0f * scale_;
    if (bannerH > 0.0f) {
        fluent::InfoBarSpec bar;
        bar.bounds = D2D1::RectF(x + 8.0f * scale_, y, x + w - 8.0f * scale_, y + bannerH - 4.0f * scale_);
        bar.title = pane.banner_title;
        bar.message = pane.banner_message;
        bar.kind = static_cast<fluent::InfoBarKind>(std::clamp(pane.banner_kind, 0, 3));
        bar.show_close = false;
        painter_.DrawInfoBar(bar);
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

    if (ShowsColumnHeader(pane.view_mode)) {
        // Re-set the brush: earlier drawing (tray deck pills, toolbar) may
        // have left a different color on this shared member.
        MakeBrush(dc, theme.fill_input, brFillInput_);
        FillRect(dc, brFillInput_.get(), x, y, w, column_header_height_);
        FillRect(dc, brStrokeDivider_.get(), x, y + column_header_height_ - 1, w, 1);
        const DetailsColumnLayout columns = DetailsColumns(bounds, pane);
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
        drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnName),
                SortColumn::Name, columns.widths[0], false);
        int col_index = 1;
        // The path column is display-only: never an active/clickable sort.
        if (pane.is_search)
            drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnPath),
                    SortColumn::Path, columns.widths[col_index++], false);
        drawCol(pane.date_column_label.empty()
                    ? pulse::l10n::Get(pulse::l10n::StringId::ColumnModified)
                    : pane.date_column_label,
                SortColumn::Mtime, columns.widths[col_index++], false);
        drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnType),
                SortColumn::Type, columns.widths[col_index++], false);
        drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnSize),
                SortColumn::Size, columns.widths[col_index], true);
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
    if (!pane.loading && pane.EntryCount() == 0)
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
                                     const Theme& theme, bool selected) {
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

    const std::wstring shown = FitFileName(compositor_, factory, fmt, name, w);
    const D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    if (IsHighContrast() || !compositor_->DrawLumaText(
            shown, fmt, rc, brText_->GetColor(), theme.bg,
            DWRITE_TEXT_ALIGNMENT_LEADING)) {
        dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    fmt->SetWordWrapping(old_wrap);
    fmt->SetTextAlignment(old_align);
}

void MainRenderer::DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color) {
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
                      scale_, ListRowHeightDip(vm));
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
    const float field_w = std::max(40.0f * scale_, trail.name_w);
    const float field_h = std::max(22.0f * scale_, std::min(textH, 30.0f * scale_));
    return D2D1::RectF(nameX, textY, nameX + field_w, textY + field_h);
}

void MainRenderer::DrawList(const PaneViewModel& vm, float x, float y, float w, float h,
                            const Theme& theme, int hover_region, int hover_control_index) {
    ID2D1DeviceContext* dc = compositor_->Dc();
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
    ViewLayout layout(vm.view_mode, viewport, entryCount, vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    const auto [startIdx, endIdx] = layout.VisibleRange();
    const DetailsColumnLayout detailsColumns = DetailsColumns(viewport, vm);
    const float dateW = detailsColumns.widths[detailsColumns.count - 3];
    const float typeW = detailsColumns.widths[detailsColumns.count - 2];
    const float sizeW = detailsColumns.widths[detailsColumns.count - 1];

    for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
        const D2D1_RECT_F cell = layout.ItemRect(i);
        if (cell.right < x || cell.left > x + w || cell.bottom < y || cell.top > y + h) continue;
        const int src = vm.SourceIndex(i);
        if (src < 0) continue;
        const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));

        bool selected = vm.IsRowSelected(src);
        bool focused = (src == vm.selected_index);
        bool hover = (src == vm.hover_index);
        bool cut = vm.cut_names.contains(e.name);

        const float inset = 4.0f * scale_;
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
        D2D1_RECT_F iconRect = layout.IconRect(i);
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
        const bool drewThumbnail = UsesThumbnails(vm.view_mode) &&
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
        DetailsNameLine nameLine = MakeDetailsNameLine(nameRc, cell, scale_, rowSnippet);
        if (rowSnippet) {
            textY = nameLine.y;
            textH = nameLine.h;
        }
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
        const float badgeW = vm.view_mode == ViewMode::Details && !e.badge.empty()
            ? std::min(108.0f * scale_, painter_.MeasureTagWidth(e.badge)) : 0.0f;
        const NameTrail trail = LayoutNameTrail(
            nameX, textY, textH, nameColRight, cell.top, cell.bottom, scale_,
            e.name, tagDotCount, badgeW, showStar, showNewTab, showMore,
            compositor_, compositor_->DwriteFactory(), compositor_->TextFormat());
        if (src == vm.rename_index) {
            const D2D1_RECT_F fieldRc = RenameFieldRect(vm, viewport, src);
            fluent::ControlState fieldState{};
            fieldState.focused = true;
            painter_.DrawTextFieldFrame(fieldRc, fieldState);
        } else {
            D2D1_COLOR_F nameColor = cut ? WithAlpha(theme.text, 0.55f) : theme.text;
            MakeBrush(dc, nameColor, brText_);
            if (iconGrid && tagDotCount == 0) {
                DrawCenteredIconName(e.name, nameRc, nameColor);
            } else {
                DrawTruncatedName(e.name, trail.name_x, textY, trail.name_w, textH, theme, selected);
            }
            if (!e.badge.empty() && trail.badge.right > trail.badge.left) {
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
            }
        }

        MakeBrush(dc, cut ? WithAlpha(theme.text_secondary, 0.55f) : theme.text_secondary, brTextSecondary_);
        if (vm.view_mode == ViewMode::Details) {
            const auto draw_detail_text = [&](std::wstring_view text, float left, float top,
                                              float width, float height,
                                              DWRITE_TEXT_ALIGNMENT alignment) {
                const auto text_bounds = D2D1::RectF(left, top, left + width, top + height);
                if (!IsHighContrast() && compositor_->DrawLumaText(
                        text, compositor_->TextFormat(), text_bounds,
                        brTextSecondary_->GetColor(), theme.bg, alignment)) {
                    return;
                }
                const DWRITE_TEXT_ALIGNMENT previous = compositor_->TextFormat()->GetTextAlignment();
                compositor_->TextFormat()->SetTextAlignment(alignment);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(),
                    compositor_->TextFormat(), brTextSecondary_.get(),
                    std::wstring(text), left, top, width, height);
                compositor_->TextFormat()->SetTextAlignment(previous);
            };
            float colX = detailsColumns.DividerX(0);
            const float textInset = 8.0f * scale_;
            if (vm.is_search) {
                const float pathW = detailsColumns.widths[1];
                draw_detail_text(FolderOf(e.path), colX + textInset, textY,
                    std::max(0.0f, pathW - textInset * 2.0f), textH,
                    DWRITE_TEXT_ALIGNMENT_LEADING);
                colX += pathW;
            }
            draw_detail_text(e.date_text, colX + textInset, textY,
                std::max(0.0f, dateW - textInset * 2.0f), textH,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            colX += dateW;
            draw_detail_text(e.type_text, colX + textInset, textY,
                std::max(0.0f, typeW - textInset * 2.0f), textH,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            colX += typeW;
            draw_detail_text(e.size_text, colX + textInset, textY,
                std::max(0.0f, sizeW - textInset * 2.0f), textH,
                DWRITE_TEXT_ALIGNMENT_TRAILING);
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
                      vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    const float totalH = layout.ContentHeight();
    auto sb = ComputeScrollbar(h, totalH, vm.scroll_y, layout.Metrics().cell_height);
    if (!sb.valid) return;
    float thumbW = 6 * scale_;
    FillRoundedRect(dc, brScrollbar_.get(), x + w - 10.0f * scale_, y + sb.thumbY,
        thumbW, sb.thumbH, thumbW * 0.5f);
}
float MainRenderer::MaxScrollForPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.MaxScrollY();
}

float MainRenderer::MaxScrollXForPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.MaxScrollX();
}

D2D1_RECT_F MainRenderer::ItemRectInPane(const PaneViewModel& vm,
                                         const D2D1_RECT_F& pane_bounds,
                                         int view_index) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.ItemRect(view_index);
}

int MainRenderer::MoveViewIndex(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                int current, int dx, int dy) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.MoveIndex(current, dx, dy);
}

int MainRenderer::PageDelta(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.PageDelta();
}

std::pair<int, int> MainRenderer::VisibleRangeInPane(
    const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    return layout.VisibleRange();
}

int MainRenderer::RowFromYInPane(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds, float y) const {
    return ItemFromPointInPane(vm, pane_bounds, pane_bounds.left + 1.0f, y);
}

int MainRenderer::ItemFromPointInPane(const PaneViewModel& vm,
                                      const D2D1_RECT_F& pane_bounds,
                                      float x, float y) const {
    const float extra = PaneExtraTop(vm, scale_);
    D2D1_RECT_F list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm));
    int idx = layout.HitTest(x, y);
    if (idx < 0) return -1;
    return vm.SourceIndex(idx);
}

} // namespace pulse::ui
