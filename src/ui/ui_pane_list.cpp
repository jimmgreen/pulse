// ui_pane_list.cpp — Pane, list, empty states, columns, and icons.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "details_column_widths.h"
#include "pane_header_icons.h"
#include "toolbar_layout.h"
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

D2D1_RECT_F MainRenderer::PaneViewButtonRect(const D2D1_RECT_F& pane_bounds,
                                              float filter_expand) const {
    (void)filter_expand;
    const float right = pane_bounds.right - 8*scale_;
    return D2D1::RectF(right-kCommandIconButtonDip*scale_,pane_bounds.top+4*scale_,
        right,pane_bounds.top+pane_header_height_-4*scale_);
}

D2D1_RECT_F MainRenderer::PaneMediumIconsRect(const D2D1_RECT_F& pane_bounds,
                                               float filter_expand) const {
    const D2D1_RECT_F view = PaneViewButtonRect(pane_bounds, filter_expand);
    return D2D1::RectF(view.left - kCommandIconStepDip * scale_, view.top,
                       view.left - (kCommandIconStepDip - kCommandIconButtonDip) * scale_,
                       view.bottom);
}
D2D1_RECT_F MainRenderer::PaneDetailsRect(const D2D1_RECT_F& pane_bounds,
                                                float filter_expand) const {
    return StepLeftHeaderButton(PaneMediumIconsRect(pane_bounds, filter_expand), scale_);
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
    auto columns = DetailsColumns(pane_bounds, vm.details_column_dividers, vm.is_search,
                                  vm.search_column_dividers);
    // Explicit divider positions remain a manual override. A drag starts by
    // capturing this measured layout, so the first movement does not jump.
    const bool manual = vm.is_search
        ? std::any_of(vm.search_column_dividers.begin(), vm.search_column_dividers.end(), [](float v) { return v > 1.0f; })
        : std::any_of(vm.details_column_dividers.begin(), vm.details_column_dividers.end(), [](float v) { return v > 1.0f; });
    // Content fitting covers the classic modified / type / size trio; extra
    // date columns keep their measured automatic widths.
    if (manual || !columns.Has(ColumnKind::Date) || !columns.Has(ColumnKind::Type) ||
        !columns.Has(ColumnKind::Size) || columns.Has(ColumnKind::Created) ||
        columns.Has(ColumnKind::Accessed) || vm.view_mode != ViewMode::Details ||
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
                measure(*values[i], compositor_->FileNameFormat()) + 20.0f * scale_);
        measured[1] = std::max(measured[1], measure(entry.type_text, compositor_->FileNameFormat()) +
            (20.0f + TypeChipWidthDip(TypeChipLabel(entry.name, entry.is_dir))) * scale_);
    }
    FitDetailsMetadata(columns.widths, columns.count, measured, scale_);
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
        out.date = std::clamp(date + pad, 72.0f, 176.0f);
        out.created = std::clamp(created + pad, 72.0f, 176.0f);
        out.accessed = std::clamp(accessed + pad, 72.0f, 176.0f);
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
        // Uniform minimum (the width of "XLSX") so the type text after the chip
        // lines up across DWG / BAK / PDF / DWL2 rows; longer extensions still grow.
        w = std::max(CellTextWidth(chip, true), CellTextWidth(L"XLSX", true));
        w /= std::max(0.01f, scale_);
    } else {
        w = 8.0f * static_cast<float>(std::max<size_t>(chip.size(), 4));
    }
    return w + 2.0f * kTypeChipPadDip + kTypeChipGapDip;
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
    struct Meta { ColumnKind kind; float width; float automatic; };
    std::vector<Meta> meta;
    meta.reserve(5);
    // Display order. Search results carry no creation / access times.
    const std::pair<ColumnKind, float> shown[] = {
        {ColumnKind::Date, fitted.date}, {ColumnKind::Created, fitted.created},
        {ColumnKind::Accessed, fitted.accessed}, {ColumnKind::Type, fitted.type},
        {ColumnKind::Size, fitted.size}};
    for (const auto& [kind, automatic] : shown) {
        if (!(details_columns_ & (1u << static_cast<uint32_t>(kind)))) continue;
        const int slot = ManualColumnSlot(kind, search_view);
        if (slot < 0) continue;
        const float manual = ManualWidthDip(search_view ? search_dividers[static_cast<size_t>(slot)]
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
            !drop(ColumnKind::Date)) break;
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

float MainRenderer::ListRowHeightDip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds) const {
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

void MainRenderer::AutoFitColumnDivider(const D2D1_RECT_F& pane_bounds,
                                        DetailsColumnWidths& dividers, bool search_view,
                                        std::array<float, 4>& search_dividers, int divider_index) const {
    const DetailsColumnLayout layout = DetailsColumns(pane_bounds, dividers, search_view, search_dividers);
    if (divider_index < 0 || divider_index >= layout.count - 1) return;
    for (int side : {divider_index, divider_index + 1}) {
        const int slot = ManualColumnSlot(layout.kinds[static_cast<size_t>(side)], search_view);
        if (slot < 0) continue;
        if (search_view) search_dividers[static_cast<size_t>(slot)] = 0.0f;
        else dividers[static_cast<size_t>(slot)] = 0.0f;
    }
}

namespace {
// Move the shared edge while keeping the adjacent pair's outer edges fixed.
// Flexible name/path widths are implicit; metadata widths are stored in DIP.
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
    const float min_meta = std::min(48.0f * scale, (x2 - x0) * 0.5f);
    const float min_flex = std::min((left == K::Path ? kDetailsMinPathDip : kDetailsMinNameDip) * scale,
                                   std::max(0.0f, x2 - x0 - min_meta));
    auto put = [&](K kind, float px) {
        const int slot = MainRenderer::ManualColumnSlot(kind, search_view);
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

DetailsColumnWidths MainRenderer::ResizeDetailsColumnDivider(
    const D2D1_RECT_F& pane_bounds,
    const DetailsColumnWidths& dividers,
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
                                       const DetailsColumnWidths& column_dividers,
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
        ListRowHeightDip(vm, PaneListRect(pane_bounds, extra_top, vm.view_mode)) * scale_);
    if (const ListGroups* groups = vm.Groups()) {
        // Grouped rows are offset by headers; take the vertical band from the layout.
        const D2D1_RECT_F list = PaneListRect(pane_bounds, extra_top, vm.view_mode);
        const ViewLayout grouped(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y,
                                 scale_, ListRowHeightDip(vm, list), groups);
        const D2D1_RECT_F cell = grouped.ItemRect(view_index);
        if (cell.bottom - cell.top < 0.5f) return false;
        const float h = name.bottom - name.top;
        name.top = cell.top + std::max(0.0f, (cell.bottom - cell.top - h) * 0.5f);
        name.bottom = name.top + h;
    }
    if (vm.view_mode == ViewMode::Details) {
        const auto actual = DetailsColumns(pane_bounds, vm);
        const auto defaults = DetailsColumns(pane_bounds, vm.details_column_dividers,
                                            vm.is_search, vm.search_column_dividers);
        name.right += actual.Width(ColumnKind::Name) - defaults.Width(ColumnKind::Name);
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
        entry.name, available);
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

namespace {
// Decode size for a view's thumbnails: big grids share one 256 px decode,
// medium icons 128 px, other views their drawn size.
uint32_t ThumbnailRequestPixels(ViewMode mode, float drawn_size) noexcept {
    long pixels = std::lround(drawn_size);
    if (mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons)
        pixels = std::max(256l, pixels);
    else if (mode == ViewMode::MediumIcons)
        pixels = std::max(128l, pixels);
    return static_cast<uint32_t>(std::clamp(pixels, 32l, 512l));
}
} // namespace

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
        icon_cache_.Draw(compositor_->Dc(), dest, entry.path, entry.name, entry.is_dir, entry.attrs,
                         fs::IsLinkReparse(entry.reparse_kind))) {
        return;
    }
    if (entry.is_dir) DrawFolderIcon(x, y, size, theme);
    else DrawFileIcon(x, y, size, theme);
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
    float thumb_alpha = 0.0f;
    if (thumb_from && thumb_to) thumb_alpha = 1.0f;
    else if (thumb_from) thumb_alpha = 1.0f - motion::SmoothStep(0.55f, 0.95f, sample.progress);
    else if (thumb_to) thumb_alpha = motion::SmoothStep(0.05f, 0.45f, sample.progress);
    thumb_alpha *= sample.opacity;
    bool thumb = false;
    if (thumb_alpha > 0.01f) {
        // Shrinking: the old view's decode is already cached. Growing: request
        // the new size; the cache shows any other decoded size meanwhile.
        const bool use_from = thumb_from && !thumb_to;
        thumb = thumbnail_cache_.Draw(dc, icon, entry.path, entry.attrs,
            ThumbnailRequestPixels(use_from ? from_mode : vm.view_mode,
                                   use_from ? from_size : target_size),
            vm.view_generation, entry.modified_value, entry.size_value, thumb_alpha)
            == PreviewDrawResult::Bitmap;
    }
    // The settled view's icon is converted on the icon worker meanwhile, so
    // the first static frame after the glide only finds it cached.
    if (!entry.record_only)
        icon_cache_.Prefetch(entry.path, entry.name, entry.is_dir, entry.attrs, target_size);
    const float icon_alpha = (thumb ? 1.0f - thumb_alpha : 1.0f) * sample.opacity;
    if (icon_alpha <= 0.01f) return;
    if (entry.record_only) {
        DrawEntryIcon(entry, icon.left, icon.top, size, theme);
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
    if (bitmap) {
        const D2D1_RECT_F dest = D2D1::RectF(icon.left, icon.top, icon.left + size, icon.top + size);
        dc->DrawBitmap(bitmap, &dest, icon_alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                       nullptr, nullptr);
    } else if (sample.entering) {
        // Vector placeholders cannot fade; a fading-in item waits for its
        // icon (or thumbnail) instead of popping in at full strength.
    } else if (entry.is_dir) {
        DrawFolderIcon(icon.left, icon.top, size, theme);
    } else {
        DrawFileIcon(icon.left, icon.top, size, theme);
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
                            scale_, from.row_height_dip);
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
            DrawCenteredIconName(e.name, moved, theme.text, theme, kNoMatches);
        } else {
            DrawTruncatedName(e.name, moved.left, moved.top, std::max(1.0f, moved.right - moved.left),
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
                    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), meta_format,
                                        brTextSecondary_.get(), text, x, top, width, height);
                };
                switch (columns.kinds[static_cast<size_t>(col)]) {
                case ColumnKind::Date:
                    draw(list_smart_date_ && e.modified_value && !e.date_text.empty()
                             ? SmartListDate(e.modified_value) : e.date_text,
                         left, avail, DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
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
                        draw(e.size_text, left, avail, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
        if (staying) continue;
        // No place in the new view: the icon fades out where it was.
        const float size = std::max(1.0f, std::min(was.icon.right - was.icon.left,
                                                   was.icon.bottom - was.icon.top));
        const D2D1_RECT_F dest = D2D1::RectF(was.icon.left, was.icon.top,
                                             was.icon.left + size, was.icon.top + size);
        const bool thumb = !e.record_only && UsesThumbnails(mode) &&
            thumbnail_cache_.Draw(dc, was.icon, e.path, e.attrs, ThumbnailRequestPixels(mode, size),
                vm.view_generation, e.modified_value, e.size_value) == PreviewDrawResult::Bitmap;
        if (thumb) continue;
        if (ID2D1Bitmap* bitmap = e.record_only ? nullptr
                : icon_cache_.CachedBitmapFor(e.path, e.name, e.is_dir, e.attrs, size)) {
            dc->DrawBitmap(bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                           nullptr, nullptr);
        } else if (e.is_dir) {
            // Vector placeholder: DrawEntryIcon would convert synchronously.
            DrawFolderIcon(dest.left, dest.top, size, theme);
        } else {
            DrawFileIcon(dest.left, dest.top, size, theme);
        }
    }
    dc->PopLayer();
    dc->PopAxisAlignedClip();
    meta_format->SetTextAlignment(meta_align);
    meta_format->SetParagraphAlignment(meta_paragraph);
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
                              rc, compositor_->SmallFormat(), theme.accent_text,
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
            painter_.DrawText(pulse::l10n::Get(label), rc, compositor_->SmallFormat(), theme.accent_text,
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
    MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillHover_);
    FillRoundedRect(dc, brFillHover_.get(), pane_rect.left, pane_rect.top,
                    pane_rect.right - pane_rect.left, pane_rect.bottom - pane_rect.top,
                    theme.radius_control * scale_);

    float y = y0;
    const std::wstring& title = pane.header_text;
    const D2D1_RECT_F mediumRc = PaneMediumIconsRect(bounds, pane.filter_expand);
    const D2D1_RECT_F viewRc = PaneViewButtonRect(bounds, pane.filter_expand);
    const D2D1_RECT_F detailsRc = PaneDetailsRect(bounds);
    if (pane.header_drop) {
        MakeBrush(dc, WithAlpha(theme.accent, 0.18f), brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bounds.left + scale_, bounds.top + scale_,
            std::max(0.0f, w - 2.0f * scale_), pane_header_height_ - 2.0f * scale_,
            theme.radius_control * scale_);
    }
    // Keep the title stationary while the focused pane's controls crossfade.
    const bool split = vm.pane_slots.size() > 1;
    const float textLeft = x + 8.0f * scale_;
    const float textRight = std::max(textLeft, detailsRc.left - 8.0f * scale_);
    const auto titleBadge = ChangeTitleRect(bounds, textRight, pane_header_height_, pane.title_change_badge, scale_, compositor_, pane.header_text);
    const float textWidth = (titleBadge.right > titleBadge.left ? titleBadge.left - 4 * scale_ : textRight) - textLeft;
    MakeBrush(dc, split && !focused ? theme.text_secondary : theme.text, brText_);
    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->HeaderFormat(), brText_.get(), title,
        textLeft, y, textWidth, pane_header_height_);

    DrawChangeBadge(compositor_, painter_, pane.title_change_badge, titleBadge, theme, scale_);

    const float controlsOpacity = std::clamp(pane.header_controls_opacity, 0.0f, 1.0f);
    if (controlsOpacity > 0.0f) {
        const bool fading = controlsOpacity < 1.0f;
        if (fading) dc->PushLayer(D2D1::LayerParameters(D2D1::RectF(bounds.left, bounds.top,
            bounds.right, bounds.top + pane_header_height_), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), controlsOpacity), nullptr);
        auto headerButton = [&](const D2D1_RECT_F& rc, HitTestResult::Region region,
                                PaneHeaderIcon icon, bool active, bool enabled = true) {
            const bool hovered = focused && enabled && vm.hover_region == static_cast<int>(region) &&
                                 vm.hover_control_index == pane_index;
            const D2D1_COLOR_F fill = active
                ? WithAlpha(theme.accent, hovered ? 0.24f : 0.14f)
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
        headerButton(detailsRc, HitTestResult::PaneDetails, PaneHeaderIcon::View,
                     pane.view_mode == ViewMode::Details);
        headerButton(mediumRc, HitTestResult::PaneMediumIcons, PaneHeaderIcon::MediumIcons,
                     pane.view_mode == ViewMode::MediumIcons);
        headerButton(viewRc, HitTestResult::PaneViewButton, PaneHeaderIcon::More, false);
        if (fading) dc->PopLayer();
    }
    y += pane_header_height_;

    if (const ColumnStripLayout strip = ColumnStripGeometry(pane, pane_rect); strip.active) {
        DrawColumnStrip(vm, pane, pane_rect, pane_index, theme);
        bounds = strip.body;
        x = bounds.left;
        w = bounds.right - bounds.left;
    }

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
            if (pane.is_content_search || pane.network_live_action) {
                const auto action_bounds = bar.bounds;
                const float action_width = std::min(144.0f * scale_, (action_bounds.right - action_bounds.left) * 0.4f);
                bar.trailing_width = action_width / scale_;
                painter_.DrawInfoBar(bar);
                fluent::ButtonSpec action;
                action.bounds = D2D1::RectF(action_bounds.right - action_width, action_bounds.top, action_bounds.right, action_bounds.bottom);
                action.text = l10n::Get(pane.is_content_search ? l10n::StringId::ContentManageShort
                                                               : l10n::StringId::NetworkLiveAdd);
                action.kind = fluent::ButtonKind::Transparent;
                action.state.hovered = IsHovered(vm, pane.is_content_search
                    ? HitTestResult::ContentIndexManage : HitTestResult::NetworkIndexAdd);
                painter_.DrawButton(action);
            } else if (pane.compare_active) {
                D2D1_RECT_F diffRc{}, exitRc{};
                CompareBannerButtons(bar.bounds, scale_, diffRc, exitRc);
                bar.trailing_width = (exitRc.right - diffRc.left) / scale_;
                // Counts are drawn as colored chips (matching the row bars)
                // instead of the InfoBar message, so narrow panes still fit.
                bar.title = {};
                bar.message = {};
                painter_.DrawInfoBar(bar);
                {
                    const bool dark_bg = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
                    IDWriteTextFormat* chip_fmt = compositor_->SmallFormat();
                    const float left = bar.bounds.left + 52.0f * scale_;
                    const float right = diffRc.left - 6.0f * scale_;
                    const float cy = (bar.bounds.top + bar.bounds.bottom) * 0.5f;
                    const float line_h = 18.0f * scale_;
                    static constexpr l10n::StringId kChipLabels[] = {
                        l10n::StringId::CompareChipOnly, l10n::StringId::CompareChipNewer,
                        l10n::StringId::CompareChipOlder, l10n::StringId::CompareChipDiffers,
                    };
                    const auto& counts = pane.compare_counts;
                    const bool identical = counts[1] + counts[2] + counts[3] + counts[4] == 0;
                    if (identical) {
                        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                        DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), chip_fmt, brTextSecondary_.get(),
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
                            DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), chip_fmt, brTextSecondary_.get(),
                                t, cx, cy - line_h * 0.5f, tw + 2.0f * scale_, line_h);
                            cx += tw + gap;
                        }
                    }
                }
                fluent::ButtonSpec diff;
                diff.bounds = diffRc;
                diff.text = l10n::Get(pane.compare_diff_only ? l10n::StringId::CompareShowAll
                                                             : l10n::StringId::CompareDiffOnly);
                diff.kind = fluent::ButtonKind::Transparent;
                diff.state.hovered = IsHovered(vm, HitTestResult::CompareDiffToggle, pane_index);
                painter_.DrawButton(diff);
                fluent::ButtonSpec exit;
                exit.bounds = exitRc;
                exit.text = l10n::Get(l10n::StringId::CompareExit);
                exit.kind = fluent::ButtonKind::Transparent;
                exit.state.hovered = IsHovered(vm, HitTestResult::CompareExit, pane_index);
                painter_.DrawButton(exit);
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
            if (measuredLabelW > labelAvailable) {
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), fmt, brText_.get(), label,
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
            case ColumnKind::Created:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnCreated), SortColumn::Created, cw, false);
                break;
            case ColumnKind::Accessed:
                drawCol(pulse::l10n::Get(pulse::l10n::StringId::ColumnAccessed), SortColumn::Accessed, cw, false);
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
        group_hover_pane_ = vm.hover_pane_index;
        DrawList(pane, x, y, w, listH, theme, vm.hover_region, vm.hover_control_index, focused || !split,
                 pane_index);
    dc->PopAxisAlignedClip();

    const float radius = theme.radius_control * scale_;
    D2D1_COLOR_F frameColor = theme.stroke_card;
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

bool MainRenderer::DrawTruncatedName(const std::wstring& name, float x, float y, float w, float h,
                                     const Theme& theme, bool selected, const std::vector<NameMatchRange>& matches,
                                     bool dim_extension) {
    (void)selected;
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() ||
        !compositor_->FileNameFormat() || name.empty() || w <= 1.0f) {
        return !name.empty();   // nothing of it shown
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->FileNameFormat();
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
    // Shape colored filenames in one layout. Splitting at the extension gives
    // each substring its own clipping/ellipsis and loses glyph positioning
    // across the boundary (including the dot's ink and the stem's last glyph).
    ComPtr<IDWriteTextLayout> highlighted;
    if ((!visible_matches.empty() || ext_at != std::wstring::npos) &&
        SUCCEEDED(factory->CreateTextLayout(shown.c_str(),
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
    } else if (IsHighContrast() || !compositor_->DrawLumaText(
            shown, fmt, rc, brText_->GetColor(), theme.bg,
            DWRITE_TEXT_ALIGNMENT_LEADING)) {
        dc->DrawText(shown.c_str(), (UINT32)shown.size(), fmt, &rc, brText_.get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    fmt->SetWordWrapping(old_wrap);
    fmt->SetTextAlignment(old_align);
    return shown != name;
}

void MainRenderer::DrawCenteredIconName(const std::wstring& name, const D2D1_RECT_F& bounds,
                                        const D2D1_COLOR_F& color, const Theme& theme, const std::vector<NameMatchRange>& matches,
                                        bool* truncated) {
    if (!compositor_ || !compositor_->Dc() || !compositor_->DwriteFactory() || name.empty()) return;
    const float width = std::max(1.0f, bounds.right - bounds.left);
    const float height = std::max(1.0f, bounds.bottom - bounds.top);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor_->DwriteFactory()->CreateTextLayout(
            name.c_str(), static_cast<UINT32>(name.size()), compositor_->FileNameFormat(),
            width, height, &layout)) || !layout.get()) {
        return;
    }
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (truncated) {
        // Measured untrimmed (the format trims by default): wrapped lines
        // taller than the cell get cut.
        const DWRITE_TRIMMING untrimmed{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&untrimmed, nullptr);
        DWRITE_TEXT_METRICS metrics{};
        *truncated = SUCCEEDED(layout->GetMetrics(&metrics)) &&
            (metrics.height > height + 0.5f || metrics.widthIncludingTrailingWhitespace > width + 0.5f);
    }
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
                      scale_, ListRowHeightDip(vm, list), vm.Groups());
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

void MainRenderer::DrawList(const PaneViewModel& vm, float x, float y, float w, float h,
                            const Theme& theme, int hover_region, int hover_control_index,
                            bool pane_focused, int pane_index) {
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
        DrawListSkeleton(D2D1::RectF(x, y, x + w, y + h), theme, pane_index);
        return;
    }
    if (pane_index >= 0 && pane_index < static_cast<int>(list_loading_since_.size()))
        list_loading_since_[pane_index] = 0;
    const size_t entryCount = vm.EntryCount();
    if (entryCount == 0) return;
    const D2D1_RECT_F viewport = D2D1::RectF(x, y, x + w, y + h);
    ViewLayout layout(vm.view_mode, viewport, entryCount, vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, viewport), vm.Groups());
    const auto [startIdx, endIdx] = layout.VisibleRange();
    const DetailsColumnLayout detailsColumns = DetailsColumns(viewport, vm);
    const bool detailsView = vm.view_mode == ViewMode::Details;
    D2D1_COLOR_F zebraColor = theme.text;
    zebraColor.a = (theme.bg.r < 0.5f) ? 0.035f : 0.025f;
    // #78: the focused pane's selection is an accent gradient, strongest beside
    // the indicator bar. Unfocused panes keep their neutral fill and high
    // contrast keeps the system highlight.
    ComPtr<ID2D1LinearGradientBrush> selection_gradient;
    if (pane_focused && !IsHighContrast()) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, theme.list_selected_start},
                                            {1.0f, theme.list_selected_end}};
        ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)) && collection.get()) {
            dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                    D2D1::Point2F(1.0f, 0.0f)),
                collection.get(), &selection_gradient);
        }
    }

    // Rows slide into place after small inserts/removals and new rows flash
    // (ui_motion.h ListShiftMotion). Same folder, view, filter and scroll only.
    const uint64_t folder_context = std::hash<std::wstring>{}(vm.path) ^
        (std::hash<std::wstring>{}(vm.filter_text) * 31u);
    const uint64_t list_context = folder_context ^ (static_cast<uint64_t>(vm.view_mode) << 56);
    motion::ListShiftMotion* shift =
        pane_index >= 0 && pane_index < static_cast<int>(list_shift_.size())
            ? &list_shift_[pane_index] : nullptr;
    if (shift) {
        shift->BeginFrame(list_context, vm.scroll_x, vm.scroll_y, entryCount, motion_frame_,
                          motion_now_, !vm.is_search && !vm.loading);
    }
    // View-mode switch: items travel from their previous cell/icon to the new
    // layout (ui_view_morph.h). Same folder and filter, continuous frames only.
    motion::ViewMorphMotion* morph =
        pane_index >= 0 && pane_index < static_cast<int>(view_morph_.size())
            ? &view_morph_[pane_index] : nullptr;
    if (morph) {
        morph->BeginFrame(folder_context, static_cast<int>(vm.view_mode), motion_frame_,
                          motion_now_, !vm.loading);
        morph->NoteLayout(viewport, vm.scroll_x, vm.scroll_y, ListRowHeightDip(vm, viewport),
                          entryCount);
    }
    const bool morphing = morph && morph->Running();

    // Hover plate: one plate per pane that glides between rows instead of
    // blinking row to row. Drawn under every row so text stays on top.
    if (!morphing && vm.hover_index >= 0 && pane_index >= 0 &&
        pane_index < static_cast<int>(list_hover_motion_.size())) {
        for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
            if (vm.SourceIndex(i) != vm.hover_index) continue;
            const D2D1_RECT_F cell = layout.ItemRect(i);
            const float inset = 4.0f * scale_;
            const D2D1_RECT_F target = D2D1::RectF(cell.left + inset, cell.top + scale_,
                std::max(cell.left + inset, cell.right - inset),
                std::max(cell.top + scale_, cell.bottom - scale_));
            const D2D1_RECT_F plate = list_hover_motion_[pane_index].Update(
                list_context, vm.hover_index, target, motion_frame_, motion_now_, 100);
            dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
            FillRoundedRect(dc, brFillHover_.get(), plate.left, plate.top,
                plate.right - plate.left, plate.bottom - plate.top,
                theme.radius_control * scale_);
            dc->PopAxisAlignedClip();
            break;
        }
    }

    // Mid-morph the list draws twice: shapes (backgrounds, icons) first, then
    // every label inside one faded layer - one layer per pane, not per row.
    // The label pass runs from the very first frame even while fully
    // transparent: the new view's first text layout (cold DirectWrite
    // caches, up to tens of ms) then lands before anything moves instead of
    // stalling the glide mid-flight.
    const float text_alpha = morphing ? morph->TextOpacity() : 1.0f;
    const int passes = morphing ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        const bool draw_shapes = pass == 0;
        const bool draw_text = !morphing || pass == 1;
        const bool text_layer = morphing && pass == 1 && text_alpha < 0.999f;
        if (text_layer) {
            // Never exactly 0: Direct2D skips the content of a fully
            // transparent layer, which would defeat the warm-up above.
            // 1/255 is below what a display can show.
            dc->PushLayer(D2D1::LayerParameters1(viewport, nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
                              D2D1::IdentityMatrix(), std::max(text_alpha, 1.0f / 255.0f), nullptr,
                              D2D1_LAYER_OPTIONS1_NONE), nullptr);
        }
        int morph_order = 0;
        for (int i = startIdx; i >= 0 && i <= endIdx; ++i) {
            const D2D1_RECT_F cell = layout.ItemRect(i);
            if (cell.right < x || cell.left > x + w || cell.bottom < y || cell.top > y + h) continue;
            if (cell.bottom - cell.top < 0.5f) continue;   // row inside a collapsed group
            const int src = vm.SourceIndex(i);
            if (src < 0) continue;
            if(vm.content_results && !vm.content_results->Ready(static_cast<size_t>(src))) {
                vm.content_results->Prefetch(static_cast<size_t>(src));
                if (!draw_text) continue;
                MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                DrawTextEndEllipsis(dc,compositor_->DwriteFactory(),compositor_->TextFormat(),brTextSecondary_.get(),
                    l10n::Get(l10n::StringId::LoadingEllipsis),cell.left+40*scale_,cell.top,
                    cell.right-cell.left-40*scale_,cell.bottom-cell.top);
                continue;
            }
            const ListEntryView& e = MakeVisibleEntry(vm, static_cast<size_t>(src));
            const uint64_t row_key = std::hash<std::wstring_view>{}(std::wstring_view(e.name));
            D2D1_POINT_2F row_shift{0.0f, 0.0f};
            if (shift) row_shift = shift->Offset(row_key, D2D1::Point2F(cell.left, cell.top));

            const bool iconGrid = vm.view_mode == ViewMode::ExtraLargeIcons ||
                                  vm.view_mode == ViewMode::LargeIcons ||
                                  vm.view_mode == ViewMode::MediumIcons;
            D2D1_RECT_F nameRc = layout.NameRect(i);
            const auto changeIt = vm.change_badges.find(src);
            const ChangeBadge* change = changeIt != vm.change_badges.end() && HasChangeBadge(changeIt->second) ? &changeIt->second : nullptr;
            D2D1_RECT_F iconRect = layout.IconRect(i);
            if (change && iconGrid) {
                const float icon_inset = std::min(18 * scale_, (iconRect.bottom - iconRect.top) * 0.3f);
                iconRect.top += icon_inset;
                iconRect.left += icon_inset * 0.5f; iconRect.right -= icon_inset * 0.5f;
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
            const float row_dx = std::round(row_shift.x + morph_sample.cell.left - cell.left);
            const float row_dy = std::round(row_shift.y + morph_sample.cell.top - cell.top);
            const RowShiftTransform row_transform(dc, row_dx, row_dy);
            // Backgrounds take the morphing cell's shape; the transform already
            // moved everything by (row_dx, row_dy).
            const D2D1_RECT_F bg = morph_item
                ? D2D1::RectF(morph_sample.cell.left - row_dx, morph_sample.cell.top - row_dy,
                              morph_sample.cell.right - row_dx, morph_sample.cell.bottom - row_dy)
                : cell;
            const float row_flash = shift ? shift->Flash(row_key) : 0.0f;

            bool selected = vm.IsRowSelected(src);
            bool hover = (src == vm.hover_index);
            bool cut = e.record_only || vm.cut_names.contains(e.name);

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
            if (draw_shapes && selected) {
                const D2D1_RECT_F sel = D2D1::RectF(
                    bg.left + inset, bg.top + scale_,
                    bg.right - inset, bg.bottom - scale_);
                const float sel_radius = theme.radius_control * scale_;
                const D2D1_ROUNDED_RECT sel_rr = D2D1::RoundedRect(
                    D2D1::RectF(sel.left, sel.top, std::max(sel.left, sel.right),
                                std::max(sel.top, sel.bottom)),
                    sel_radius, sel_radius);
                if (selection_gradient.get()) {
                    selection_gradient->SetStartPoint(D2D1::Point2F(sel.left, 0.0f));
                    selection_gradient->SetEndPoint(D2D1::Point2F(sel.right, 0.0f));
                    dc->FillRoundedRectangle(&sel_rr, selection_gradient.get());
                } else {
                    dc->FillRoundedRectangle(&sel_rr, brFillSelected_.get());
                }
                if (list_selection_outline_ && sel.right - sel.left > 2.0f * scale_) {
                    // Settings > File list > Outline selected items (#78).
                    MakeBrush(dc, pane_focused ? theme.list_selected_outline
                                               : WithAlpha(theme.text, 0.30f), brFillInput_);
                    const float half = 0.5f * scale_;
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(
                        D2D1::RectF(sel.left + half, sel.top + half, sel.right - half, sel.bottom - half),
                        sel_radius, sel_radius), brFillInput_.get(), 1.0f * scale_);
                }
                if (pane_focused) {
                    MakeBrush(dc, theme.accent, brAccent_);
                    const float indicator_h = std::min(18*scale_, std::max(0.0f, sel.bottom-sel.top-8*scale_));
                    FillRoundedRect(dc, brAccent_.get(), sel.left + scale_,
                        (sel.top+sel.bottom-indicator_h)*0.5f, 2*scale_, indicator_h, scale_);
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
            if (draw_shapes && src == vm.drop_target_index) {
                D2D1_RECT_F rc = D2D1::RectF(bg.left + inset, bg.top + scale_,
                                             bg.right - inset, bg.bottom - scale_);
                dc->DrawRoundedRectangle(
                    D2D1::RoundedRect(rc, theme.radius_control * scale_, theme.radius_control * scale_),
                    brAccent_.get(), 2.0f * scale_);
            }

            if (draw_text && change && iconGrid) {
                const float bw = std::min(ChangeBadgeWidth(*change, scale_, compositor_), cell.right - cell.left - 12 * scale_);
                const float bx = (cell.left + cell.right - bw) * 0.5f;
                DrawChangeBadge(compositor_, painter_, *change, D2D1::RectF(bx, cell.top + 2 * scale_, bx + bw, cell.top + 20 * scale_), theme, scale_);
            }
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
                const float iconX = iconRect.left;
                const float iconY = iconRect.top;
                const float renderedIconSize = std::min(snappedIconW, snappedIconH);
                const bool drewThumbnail = !e.record_only && UsesThumbnails(vm.view_mode) &&
                    thumbnail_cache_.Draw(dc, iconRect, e.path, e.attrs,
                        ThumbnailRequestPixels(vm.view_mode, renderedIconSize),
                        vm.view_generation, e.modified_value, e.size_value)
                        == PreviewDrawResult::Bitmap;
                if (!drewThumbnail) {
                    // An item already settled while others still glide: no
                    // synchronous conversion burst in the tail frames; the
                    // first static frame after the morph converts the rest.
                    ID2D1Bitmap* settled = morphing && !e.record_only
                        ? icon_cache_.CachedBitmapFor(e.path, e.name, e.is_dir, e.attrs, renderedIconSize)
                        : nullptr;
                    if (!settled && morphing && !e.record_only)
                        icon_cache_.Prefetch(e.path, e.name, e.is_dir, e.attrs, renderedIconSize);
                    if (settled) {
                        const D2D1_RECT_F dest = D2D1::RectF(iconX, iconY, iconX + renderedIconSize,
                                                             iconY + renderedIconSize);
                        dc->DrawBitmap(settled, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                       nullptr, nullptr);
                    } else if (morphing && !e.record_only) {
                        if (e.is_dir) DrawFolderIcon(iconX, iconY, renderedIconSize, theme);
                        else DrawFileIcon(iconX, iconY, renderedIconSize, theme);
                    } else {
                        DrawEntryIcon(e, iconX, iconY, renderedIconSize, theme);
                    }
                }
            }
            // Mid-morph the labels are drawn in a second, faded pass.
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
                    compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(), e.name) +
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
                compositor_, compositor_->DwriteFactory(), compositor_->FileNameFormat(), change != nullptr,
                vm.view_mode == ViewMode::Details ? (e.is_dir ? 3 : 2) : 0, name_matches, row_actions_);
            if (src == vm.rename_index) {
                const D2D1_RECT_F fieldRc = RenameFieldRect(vm, viewport, src);
                fluent::ControlState fieldState{};
                fieldState.focused = true;
                painter_.DrawTextFieldFrame(fieldRc, fieldState);
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
                if (cut) nameColor = WithAlpha(nameColor, 0.55f);
                MakeBrush(dc, nameColor, brText_);
                bool name_truncated = true;
                if (iconGrid && tagDotCount == 0) {
                    DrawCenteredIconName(e.name, nameRc, nameColor, theme, name_matches,
                                         hover ? &name_truncated : nullptr);
                } else {
                    Theme name_theme = theme;
                    name_theme.text = nameColor;
                    name_truncated = DrawTruncatedName(e.name, trail.name_x, textY, trail.name_w, textH, name_theme,
                                                       selected, name_matches, detailsView && !e.is_dir);
                }
                // The row tooltip repeats the name only when it is cut off (B站 #15).
                if (hover && pane_index >= 0 && pane_index < static_cast<int>(hover_names_.size()))
                    hover_names_[static_cast<size_t>(pane_index)] = {src, name_truncated};
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
                            if (!compositor_->DrawLumaText(chip, compositor_->SmallFormat(), chipRc,
                                                           WithAlpha(ink, ink.a * (cut ? 0.55f : 1.0f)), theme.bg,
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
                        const auto folder_size = vm.folder_size_labels.find(src);
                        if (e.is_dir && folder_size != vm.folder_size_labels.end()) {
                            draw_detail_text(fit(folder_size->second, avail), left, avail,
                                             DWRITE_TEXT_ALIGNMENT_TRAILING);
                            break;
                        }
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
            } else if (vm.view_mode == ViewMode::Tiles && e.drive_used >= 0.0f) {
                // This PC tiles, as in File Explorer: usage bar, then free of total.
                const float textW = std::max(0.0f, nameRc.right - nameRc.left);
                const float barTop = nameRc.top + 27.0f * scale_;
                const D2D1_RECT_F bar{ nameRc.left, barTop,
                    nameRc.left + std::min(textW, 240.0f * scale_), barTop + 6.0f * scale_ };
                painter_.DrawCapacityBar(bar, e.drive_used, theme.accent);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                    brTextSecondary_.get(), e.drive_space_text,
                    nameRc.left, bar.bottom + 2.0f * scale_, textW, 20.0f * scale_);
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
                const bool manual_size = e.is_dir &&
                    meta == pulse::l10n::Get(pulse::l10n::StringId::FolderSizeCalculate);
                if (manual_size) MakeBrush(dc, theme.accent, brAccent_);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                    manual_size ? brAccent_.get() : brTextSecondary_.get(), meta,
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
                    const bool manual = size->second == pulse::l10n::Get(pulse::l10n::StringId::FolderSizeCalculate);
                    if (manual) MakeBrush(dc, theme.accent, brAccent_);
                    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), format,
                        manual ? brAccent_.get() : brTextSecondary_.get(), size->second,
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
        if (text_layer) dc->PopLayer();
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

// "Group by" headers: chevron, label, count, rule and group bytes. The group
// under the viewport top is pinned (sticky) and pushed up by the next one.
std::wstring MainRenderer::GroupLabel(const PaneViewModel& vm, const ListGroup& g) {
    using l10n::StringId;
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
        const std::wstring label = GroupLabel(vm, g);
        const float label_w = std::min(std::max(0.0f, text_right - x),
                                       MeasureLayoutText(compositor_, factory, label_fmt, label) + 1.0f);
        MakeBrush(dc, theme.accent, brAccent_);
        DrawTextEndEllipsis(dc, factory, label_fmt, brAccent_.get(), label, x, hr.top, label_w, h);
        x += label_w + 8.0f * scale_;

        wchar_t count[64]{};
        swprintf_s(count, l10n::Get(g.count == 1 ? l10n::StringId::GroupCountOne
                                                 : l10n::StringId::GroupCount).c_str(), g.count);
        const std::wstring count_text = count;
        const float count_w = MeasureLayoutText(compositor_, factory, small_fmt, count_text) + 1.0f;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        if (x + count_w <= text_right)
            DrawTextRect(dc, small_fmt, brTextSecondary_.get(), count_text, x, hr.top, count_w, h);
        x += count_w + 10.0f * scale_;

        float rule_right = text_right;
        if (!show_select && g.bytes > 0) {
            const std::wstring bytes = pulse::format::ByteSize(g.bytes, true);
            const float bytes_w = MeasureLayoutText(compositor_, factory, small_fmt, bytes) + 1.0f;
            const float bx = rc.right - 8.0f * scale_ - bytes_w;
            if (bx > x + 24.0f * scale_) {
                DrawTextRect(dc, small_fmt, brTextSecondary_.get(), bytes, bx, hr.top, bytes_w, h);
                rule_right = bx - 10.0f * scale_;
            }
        }
        if (rule_right > x) {
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

void MainRenderer::DrawScrollbar(const PaneViewModel& vm, float x, float y, float w, float h, const Theme& theme) {
    (void)theme;
    ID2D1DeviceContext* dc = compositor_->Dc();
    ViewLayout layout(vm.view_mode, D2D1::RectF(x, y, x + w, y + h), vm.EntryCount(),
                      vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, D2D1::RectF(x, y, x + w, y + h)), vm.Groups());
    const float totalH = layout.ContentHeight();
    auto sb = ComputeScrollbar(h, totalH, vm.scroll_y, layout.Metrics().cell_height);
    if (!sb.valid) return;
    float thumbW = 6 * scale_;
    FillRoundedRect(dc, brScrollbar_.get(), x + w - 10.0f * scale_, y + sb.thumbY,
        thumbW, sb.thumbH, thumbW * 0.5f);
}
bool MainRenderer::PaneScrollbarGeometry(const PaneViewModel& vm, const D2D1_RECT_F& full_bounds,
                                          D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) const {
    const D2D1_RECT_F pane_bounds = PaneBodyBounds(vm, full_bounds);
    const float extra = PaneExtraTop(vm, scale_, pane_bounds.right - pane_bounds.left, compositor_);
    const auto list = PaneListRect(pane_bounds, extra, vm.view_mode);
    ViewLayout layout(vm.view_mode, list, vm.EntryCount(), vm.scroll_x, vm.scroll_y, scale_, ListRowHeightDip(vm, list), vm.Groups());
    max_scroll = layout.MaxScrollY();
    const auto metrics = ComputeScrollbar(list.bottom - list.top, layout.ContentHeight(),
                                           vm.scroll_y, layout.Metrics().cell_height);
    if (!metrics.valid) return false;
    track = D2D1::RectF(list.right - 14 * scale_, list.top, list.right, list.bottom);
    thumb = D2D1::RectF(track.left, list.top + metrics.thumbY, track.right,
                         list.top + metrics.thumbY + metrics.thumbH);
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