#include "quick_preview_window.h"
#include "../common/localization.h"
#include <algorithm>
#include <d2d1helper.h>

namespace pulse::ui {
float QuickPreviewWindow::PeTabsHeight() const {
    if (!pe_tabs_.active) return 0;
    const int columns = (std::max)(1, (std::min)(5, static_cast<int>(compositor_.Width() / (132.0f * scale_))));
    return ((5 + columns - 1) / columns) * 38.0f * scale_;
}

D2D1_RECT_F QuickPreviewWindow::PeTabRect(int index) const {
    const float width = static_cast<float>(compositor_.Width());
    const int columns = (std::max)(1, (std::min)(5, static_cast<int>(width / (132.0f * scale_))));
    const float cell = width / columns;
    const float top = ContentRect().top - PeTabsHeight() + (index / columns) * 38.0f * scale_;
    return D2D1::RectF((index % columns) * cell, top, (index % columns + 1) * cell, top + 38.0f * scale_);
}

int QuickPreviewWindow::HitPeTab(POINT point) const {
    if (!pe_tabs_.active) return -1;
    for (int i = 0; i < 5; ++i) {
        const auto r = PeTabRect(i);
        if (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) return i;
    }
    return -1;
}

void QuickPreviewWindow::SelectPeTab(int index) {
    if (index < 0 || index >= 5 || pe_tab_ == index) return;
    pe_tab_ = index;
    text_scroll_ = 0;
    selecting_ = false;
    sel_anchor_ = sel_focus_ = 0;
    text_layout_.reset();
    preview_text_ = pe_tabs_.pages[index];
    UpdateFindMatches();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::DrawPeTabs(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    if (!pe_tabs_.active || !dc || !brush) return;
    const wchar_t* labels[] = {
        l10n::Pick(L"属性 / 版本", L"Properties / Version"),
        l10n::Pick(L"PE 头 / 节区", L"PE headers / Sections"),
        l10n::Pick(L"DLL 依赖", L"DLL Dependencies"),
        l10n::Pick(L"导入", L"Imports"), l10n::Pick(L"导出", L"Exports")};
    const auto color = brush->GetColor();
    auto* format = compositor_.SmallFormat();
    if (!format) return;
    const auto align = format->GetTextAlignment();
    const auto paragraph = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < 5; ++i) {
        const auto r = PeTabRect(i);
        if (i == pe_tab_) {
            brush->SetColor(D2D1::ColorF(dark_ ? 0xFFFFFF : 0x0078D4, dark_ ? 0.08f : 0.07f));
            dc->FillRectangle(r, brush);
            brush->SetColor(D2D1::ColorF(dark_ ? 0x60CDFF : 0x0067C0));
            dc->FillRectangle(D2D1::RectF(r.left + 10 * scale_, r.bottom - 3 * scale_, r.right - 10 * scale_, r.bottom), brush);
        }
        brush->SetColor(i == pe_tab_ ? D2D1::ColorF(dark_ ? 0x60CDFF : 0x0067C0) : color);
        const auto label = D2D1::RectF(r.left + 4 * scale_, r.top, r.right - 4 * scale_, r.bottom);
        dc->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), format, label, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(paragraph);
    brush->SetColor(color);
}

void QuickPreviewWindow::StylePeDependencies() {
    if (!pe_tabs_.active || pe_tab_ != 2 || !text_layout_.get() || !compositor_.Dc()) return;
    const wchar_t* tokens[] = {L"[OK]", L"[MISSING]", L"[WARNING]", L"[API SET]", L"[CYCLE]", L"[LIMIT]"};
    const UINT32 colors[] = {dark_ ? 0x6CCB5Fu : 0x107C10u, dark_ ? 0xFF9999u : 0xC42B1Cu,
        dark_ ? 0xFCE100u : 0x946200u, dark_ ? 0x60CDFFu : 0x0067C0u,
        dark_ ? 0x60CDFFu : 0x0067C0u, dark_ ? 0xFCE100u : 0x946200u};
    for (size_t i = 0; i < std::size(tokens); ++i) {
        compositor_.Dc()->CreateSolidColorBrush(D2D1::ColorF(colors[i]), &syntax_brushes_[i + 1]);
        if (!syntax_brushes_[i + 1].get()) continue;
        for (size_t pos = preview_text_.find(tokens[i]); pos != std::wstring::npos;
             pos = preview_text_.find(tokens[i], pos + 1)) {
            const auto start = pos >= 2 ? pos - 2 : pos;
            const DWRITE_TEXT_RANGE range{static_cast<UINT32>(start), static_cast<UINT32>(wcslen(tokens[i]) + pos - start)};
            text_layout_->SetDrawingEffect(syntax_brushes_[i + 1].get(), range);
            text_layout_->SetFontFamilyName(L"Segoe UI Symbol", {static_cast<UINT32>(start), 1});
        }
    }
}
} // namespace pulse::ui
