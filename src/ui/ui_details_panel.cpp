// ui_details_panel.cpp — Details panel draw and content height.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
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

D2D1_RECT_F MainRenderer::DetailsPanelRect(float w, float h) const {
    if (DetailsPanelWidth(w) <= 0.0f) return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    const float pw = details_width_ * scale_;
    const float top = title_bar_height_ + toolbar_height_ + margin_;
    const float bottom = h - status_height_ - margin_;
    return D2D1::RectF(w - margin_ - pw, top, w - margin_, bottom);
}
void MainRenderer::DrawDetailsPanel(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                    const Theme& theme) {
    const D2D1_RECT_F panel = DetailsPanelRect(rect.right, rect.bottom);
    if (panel.right - panel.left <= 1.0f || !compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float s = scale_;
    const DetailsPanelView& d = vm.details;
    const float previewH = DetailsPreviewHeight(panel, s);
    DetailsHitRects hit;
    LayoutDetailsPanel(panel, s, d, compositor_->DwriteFactory(),
                       compositor_->SmallFormat(), compositor_, previewH, hit);

    // Same surface as the list; a left rule separates the column.
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    FillRect(dc, brStrokeCard_.get(), panel.left, panel.top, 1.0f, panel.bottom - panel.top);

    fluent::SplitterSpec resizeSplitter;
    resizeSplitter.bounds = D2D1::RectF(panel.left - 4.0f * s, panel.top,
                                        panel.left + 4.0f * s, panel.bottom);
    resizeSplitter.vertical = true;
    resizeSplitter.state.hovered =
        vm.hover_region == static_cast<int>(HitTestResult::DetailsResize);
    resizeSplitter.state.pressed = vm.details_resize_pressed;
    painter_.DrawSplitter(resizeSplitter);

    auto text = [&](const std::wstring& str, const D2D1_RECT_F& rc,
                    IDWriteTextFormat* fmt, const D2D1_COLOR_F& color,
                    bool right = false) {
        MakeBrush(dc, color, brText_);
        if (!fmt) return;
        const auto old = fmt->GetTextAlignment();
        fmt->SetTextAlignment(right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                    : DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawTextRect(dc, fmt, brText_.get(), str, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top);
        fmt->SetTextAlignment(old);
    };
    auto section = [&](const wchar_t* label, float y, int id) {
        const bool hovered = IsHovered(vm, HitTestResult::DetailsSection, id);
        if (hovered) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), panel.left + 6.0f * s, y,
                            panel.right - panel.left - 12.0f * s, 28.0f * s, 6.0f * s);
        }
        // Vector chevron: icon fonts on some machines lack the right-chevron glyph.
        const bool collapsed = (d.collapsed_mask >> id) & 1u;
        MakeBrush(dc, theme.text_secondary, brText_);
        const float cx = panel.left + 17.0f * s;
        const float cy = y + 14.0f * s;
        const float t = 1.3f * s;
        if (collapsed) {
            dc->DrawLine(D2D1::Point2F(cx - 2.0f * s, cy - 4.0f * s),
                         D2D1::Point2F(cx + 2.0f * s, cy), brText_.get(), t);
            dc->DrawLine(D2D1::Point2F(cx + 2.0f * s, cy),
                         D2D1::Point2F(cx - 2.0f * s, cy + 4.0f * s), brText_.get(), t);
        } else {
            dc->DrawLine(D2D1::Point2F(cx - 4.0f * s, cy - 2.0f * s),
                         D2D1::Point2F(cx, cy + 2.0f * s), brText_.get(), t);
            dc->DrawLine(D2D1::Point2F(cx, cy + 2.0f * s),
                         D2D1::Point2F(cx + 4.0f * s, cy - 2.0f * s), brText_.get(), t);
        }
        text(label, D2D1::RectF(panel.left + 28.0f * s, y + 4.0f * s,
                                panel.right - 12.0f * s, y + 24.0f * s),
             compositor_->SmallFormat(), theme.accent);
    };
    auto centeredText = [&](const std::wstring& str, const D2D1_RECT_F& rc,
                            IDWriteTextFormat* fmt, const D2D1_COLOR_F& color) {
        if (!fmt) return;
        MakeBrush(dc, color, brText_);
        const auto oldAlignment = fmt->GetTextAlignment();
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawTextRect(dc, fmt, brText_.get(), str, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top);
        fmt->SetTextAlignment(oldAlignment);
    };
    auto ghostButton = [&](const D2D1_RECT_F& rc, const wchar_t* glyph,
                           const std::wstring& label, bool hovered) {
        if (hovered) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
        }
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                 brStrokeCard_.get(), 1.0f);
        if (glyph) DrawIconText(rc.left + 6.0f * s, rc.top, 20.0f * s,
                                rc.bottom - rc.top, glyph, L"?", theme.text_secondary, 0.72f);
        text(label, D2D1::RectF(rc.left + 28.0f * s, rc.top, rc.right - 4.0f * s, rc.bottom),
             compositor_->SmallFormat(), theme.text);
    };

    if (!d.has_selection) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        const D2D1_RECT_F content = D2D1::RectF(
            panel.left + 16.0f * s, panel.top + 16.0f * s,
            panel.right - 16.0f * s, panel.bottom - 16.0f * s);
        const float availableWidth = std::max(0.0f, content.right - content.left);
        const float artWidth = std::min(180.0f * s, availableWidth);
        const float artHeight = artWidth * 240.0f / 320.0f;
        const float titleHeight = 24.0f * s;
        const float gap = 2.0f * s;
        const float totalHeight = artHeight + gap + titleHeight;
        const float top = content.top + std::max(
            0.0f, ((content.bottom - content.top) - totalHeight) * 0.5f);
        const D2D1_RECT_F art = D2D1::RectF(
            (content.left + content.right - artWidth) * 0.5f, top,
            (content.left + content.right + artWidth) * 0.5f, top + artHeight);
        const float svgOpacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
        if (!DrawNoSelectionSvg(art, svgOpacity)) {
            fluent::EmptyStateSpec empty;
            empty.bounds = content;
            empty.glyph = kIconFile;
            empty.title = pulse::l10n::Get(pulse::l10n::StringId::DetailsNoSelection);
            painter_.DrawEmptyState(empty);
            return;
        }
        centeredText(pulse::l10n::Get(pulse::l10n::StringId::DetailsNoSelection),
            D2D1::RectF(content.left, art.bottom + gap,
                        content.right, art.bottom + gap + titleHeight),
            compositor_->HeaderFormat(), theme.text_secondary);
        return;
    }

    const float pad = 12.0f * s;
    dc->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float previewTop = panel.top + pad;
    float y = previewTop;

    // Direct preview stays pinned so the HWND overlay can track it.
    {
        const D2D1_RECT_F previewRc = D2D1::RectF(panel.left + pad, previewTop,
            panel.right - 36.0f * s, previewTop + previewH);
        const bool placeholderOnly = d.is_dir || d.multi_count > 1;
        const bool handlerPreview = !vm.safe_mode && !placeholderOnly &&
            PreviewHandlerHost::CanHost(d.path);
        if (!placeholderOnly) {
            MakeBrush(dc, theme.fill_hover, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), previewRc.left, previewRc.top,
                            previewRc.right - previewRc.left,
                            previewRc.bottom - previewRc.top, 6.0f * s);
        }

        const float grip = 8.0f * s;
        const D2D1_RECT_F overlayRc = D2D1::RectF(previewRc.left + 2.0f * s,
            previewRc.top + 2.0f * s, previewRc.right - 2.0f * s,
            previewRc.bottom - grip);
        const D2D1_RECT_F contentRc = overlayRc;
        std::wstring previewText;
        bool truncated = false;
        uint32_t bytesRead = 0;
        std::wstring previewError;
        PreviewDrawResult previewResult = PreviewDrawResult::Failed;
        if (!d.is_dir && d.multi_count <= 1) {
            std::vector<PreviewProperty> ignored;
            thumbnail_cache_.Properties(d.path, d.attrs, d.view_generation,
                                        d.modified_value, d.size_value, ignored);
            if (!handlerPreview) {
                previewResult = thumbnail_cache_.Draw(dc, contentRc, d.path, d.attrs, 512u,
                    d.view_generation, d.modified_value, d.size_value, 1.0f,
                    &previewText, &truncated, &bytesRead, true, &previewError);
            }
        }

        preview_handler_.Sync(notify_hwnd_, overlayRc, d.path, d.attrs, d.view_generation,
                              d.modified_value, d.size_value, vm.dark, theme.bg, theme.text,
                              handlerPreview);
        const auto handlerState = preview_handler_.state();
        if (handlerPreview) {
            if (handlerState == PreviewHandlerHost::State::Shown) {
                previewResult = PreviewDrawResult::Pending;
            } else if (handlerState == PreviewHandlerHost::State::Loading ||
                       handlerState == PreviewHandlerHost::State::Idle) {
                previewResult = PreviewDrawResult::Pending;
            } else {
                previewResult = PreviewDrawResult::Failed;
                previewError = L"handler-failed";
            }
        }

        if (!handlerPreview && (previewResult == PreviewDrawResult::Text ||
            previewResult == PreviewDrawResult::Hex)) {
            if (!preview_mono_format_.get() && compositor_->DwriteFactory()) {
                typography::CreateTextFormat(compositor_->DwriteFactory(),
                    {typography::FontRole::Monospace, 11.0f * s},
                    &preview_mono_format_);
                if (preview_mono_format_.get()) {
                    preview_mono_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    preview_mono_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }
            }
            if (previewText.empty()) {
                text(pulse::l10n::Get(pulse::l10n::StringId::EmptyFile), contentRc,
                     compositor_->SmallFormat(), theme.text_secondary);
            } else if (preview_mono_format_.get()) {
                MakeBrush(dc, theme.text, brText_);
                D2D1_RECT_F clipRc = contentRc;
                clipRc.bottom -= 18.0f * s;
                D2D1_RECT_F textRc = clipRc;
                textRc.top -= d.preview_scroll_y * s;
                textRc.bottom += 4000.0f * s;
                dc->PushAxisAlignedClip(clipRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                dc->DrawTextW(previewText.data(), static_cast<UINT32>(previewText.size()),
                              preview_mono_format_.get(), textRc, brText_.get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);
                dc->PopAxisAlignedClip();
            }
            std::wstring footer = pulse::l10n::Get(previewResult == PreviewDrawResult::Hex
                ? pulse::l10n::StringId::HexPrefix : pulse::l10n::StringId::TextPrefix);
            footer += pulse::format::ByteSize(bytesRead, true);
            if (truncated) footer += pulse::l10n::Get(pulse::l10n::StringId::TruncatedSuffix);
            text(footer, D2D1::RectF(contentRc.left, contentRc.bottom - 18.0f * s,
                                     contentRc.right, contentRc.bottom),
                 compositor_->SmallFormat(), theme.text_secondary);
        } else if (handlerPreview && handlerState == PreviewHandlerHost::State::Shown) {
            // System preview handler paints into the overlay HWND.
        } else if (previewResult != PreviewDrawResult::Bitmap) {
            const bool filePlaceholder = !d.is_dir && d.multi_count <= 1;
            std::wstring state;
            if (d.multi_count > 1)
                {
                    wchar_t count[64]{};
                    swprintf_s(count,
                        pulse::l10n::Get(pulse::l10n::StringId::ItemsCountFormat).c_str(),
                        d.multi_count);
                    state = count;
                }
            else if ((d.attrs & (0x00040000u | 0x00400000u)) &&
                     !(d.attrs & 0x00080000u))
                state = pulse::l10n::Get(pulse::l10n::StringId::OnlineToPreview);
            else if (previewResult == PreviewDrawResult::Pending)
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoading);
            else if (previewError == L"path-unavailable")
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewMoved);
            else if (previewError == L"image-decode-failed")
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewImageDecode);
            else if (previewError == L"provider-failed")
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewNoSystemProvider);
            else if (previewError == L"handler-failed")
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewLoadFailed);
            else
                state = pulse::l10n::Get(pulse::l10n::StringId::PreviewUnsupported);
            if (filePlaceholder) {
                centeredText(state, contentRc, compositor_->SmallFormat(),
                             theme.text_secondary);
            } else {
                const float icon = 58.0f * s;
                const bool showState = !d.is_dir || d.multi_count > 1;
                const float stateGap = 8.0f * s;
                const float stateHeight = 20.0f * s;
                const float groupHeight = icon + (showState ? stateGap + stateHeight : 0.0f);
                const float contentCenterX = (contentRc.left + contentRc.right) * 0.5f;
                const float iconTop = contentRc.top +
                    ((contentRc.bottom - contentRc.top) - groupHeight) * 0.5f;
                const D2D1_RECT_F iconRc = D2D1::RectF(contentCenterX - icon * 0.5f,
                    iconTop, contentCenterX + icon * 0.5f, iconTop + icon);
                if (ID2D1Bitmap* bmp = icon_cache_.BitmapFor(d.path, d.name, d.is_dir, d.attrs,
                                                             icon)) {
                    dc->DrawBitmap(bmp, &iconRc, 1.0f,
                                   D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                } else if (d.is_dir) {
                    DrawFolderIcon(iconRc.left, iconRc.top, icon, theme);
                } else {
                    DrawFileIcon(iconRc.left, iconRc.top, icon, theme);
                }
                if (showState && !state.empty())
                    centeredText(state,
                        D2D1::RectF(contentRc.left, iconRc.bottom + stateGap,
                                    contentRc.right, iconRc.bottom + stateGap + stateHeight),
                        compositor_->SmallFormat(), theme.text_secondary);
            }
        }
    }
    y = previewTop + DetailsPreviewBand(previewH, s) - d.scroll_y * s;
    const D2D1_RECT_F restClip = D2D1::RectF(
        panel.left, previewTop + previewH + 4.0f * s,
        panel.right, panel.bottom);
    dc->PushAxisAlignedClip(restClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Name + star + rename pencil + subtitle.
    {
        if (d.multi_count <= 1) {
            const bool starHot = IsHovered(vm, HitTestResult::DetailsStar);
            const bool renHot = IsHovered(vm, HitTestResult::DetailsRename);
            if (starHot || d.starred) {
                MakeBrush(dc, d.starred ? WithAlpha(theme.accent, starHot ? 0.28f : 0.18f)
                                        : theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), hit.star.left, hit.star.top,
                                22.0f * s, 22.0f * s, 5.0f * s);
            }
            if (renHot) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), hit.rename.left, hit.rename.top,
                                22.0f * s, 22.0f * s, 5.0f * s);
            }
            DrawIconText(hit.star.left, hit.star.top, 22.0f * s, 22.0f * s,
                         d.starred ? L"\xE735" : L"\xE734", L"*",
                         d.starred ? theme.accent : theme.text_secondary, 1.0f);
            DrawIconText(hit.rename.left, hit.rename.top, 22.0f * s, 22.0f * s,
                         L"\xE8AC", L"ren", theme.text_secondary, 0.72f);
        }
        std::wstring shown = d.name;
        if (d.multi_count > 1) {
            wchar_t selected[64]{};
            swprintf_s(selected,
                pulse::l10n::Get(pulse::l10n::StringId::SelectedCountFormat).c_str(),
                d.multi_count);
            shown = selected;
        }
        const float nameRight = d.multi_count <= 1 ? hit.star.left - 4.0f * s
                                                   : panel.right - pad;
        text(shown, D2D1::RectF(panel.left + pad, y, nameRight,
                                y + 22.0f * s),
             compositor_->HeaderFormat(), theme.text);
        std::wstring subtitle = d.subtitle_text;
        if (subtitle.empty())
            subtitle = d.type_text.empty() ? FormatListType(d.name, d.is_dir) : d.type_text;
        if (!subtitle.empty())
            text(subtitle, D2D1::RectF(panel.left + pad, y + 22.0f * s,
                                       panel.right - pad, y + 38.0f * s),
                 compositor_->SmallFormat(), theme.text_secondary);
    }
    y += 22.0f * s + 16.0f * s + 8.0f * s;

    // Button row: 打开 / 在新标签打开 / 复制路径 / 更多 (icon over label).
    if (d.multi_count <= 1) {
        auto rowButton = [&](const D2D1_RECT_F& rc, const wchar_t* glyph,
                             const wchar_t* fallback, const wchar_t* label, bool hovered,
                             bool primary) {
            if (primary) {
                MakeBrush(dc, hovered ? theme.accent_hover : theme.accent, brAccent_);
                FillRoundedRect(dc, brAccent_.get(), rc.left, rc.top, rc.right - rc.left,
                                rc.bottom - rc.top, 6.0f * s);
            } else {
                if (hovered) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                    rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
                }
                MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                         brStrokeCard_.get(), 1.0f);
            }
            const D2D1_COLOR_F fg = primary ? theme.accent_text : theme.text;
            DrawIconText(rc.left, rc.top + 5.0f * s, rc.right - rc.left, 18.0f * s,
                         glyph, fallback, primary ? theme.accent_text
                                                  : theme.text_secondary, 1.0f);
            IDWriteTextFormat* fmt = compositor_->SmallFormat();
            if (!fmt) return;
            // Draw through a private text layout so the shared format is never
            // mutated; narrow cells ellipsize (hover tooltip keeps full text).
            const float textW = rc.right - rc.left - 4.0f * s;
            const float textH = rc.bottom - rc.top - 28.0f * s;
            ComPtr<IDWriteTextLayout> tl;
            if (compositor_->DwriteFactory() &&
                SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                    label, static_cast<UINT32>(wcslen(label)), fmt,
                    textW, textH, &tl)) && tl.get()) {
                tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                const DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                tl->SetTrimming(&trim, nullptr);
                MakeBrush(dc, fg, brText_);
                dc->DrawTextLayout(D2D1::Point2F(rc.left + 2.0f * s, rc.top + 26.0f * s),
                                   tl.get(), brText_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        };
        rowButton(hit.open, L"\xE8B7", L"O", pulse::l10n::Get(kDetailsButtonLabels[0]).c_str(),
                  IsHovered(vm, HitTestResult::DetailsOpen), true);
        rowButton(hit.new_tab, L"\xE8A7", L"\x2197", pulse::l10n::Get(kDetailsButtonLabels[1]).c_str(),
                  IsHovered(vm, HitTestResult::DetailsNewTab), false);
        rowButton(hit.copy_path, L"\xE8C8", L"C", pulse::l10n::Get(kDetailsButtonLabels[2]).c_str(),
                  IsHovered(vm, HitTestResult::DetailsCopyPath), false);
        rowButton(hit.more, L"\xE712", L"...", pulse::l10n::Get(kDetailsButtonLabels[3]).c_str(),
                  IsHovered(vm, HitTestResult::DetailsMore), false);
        y += 48.0f * s + 4.0f * s;
    }

    auto infoRow = [&](const wchar_t* label, const std::wstring& value, float& iy) {
        text(label, D2D1::RectF(panel.left + pad, iy, panel.left + pad + 96.0f * s,
                                iy + 18.0f * s),
             compositor_->SmallFormat(), theme.text_secondary);
        text(value, D2D1::RectF(panel.left + pad + 100.0f * s, iy, panel.right - pad,
                                iy + 18.0f * s),
             compositor_->SmallFormat(), theme.text);
        iy += 18.0f * s;
    };

    if (d.multi_count > 1) {
        text(pulse::l10n::Get(pulse::l10n::StringId::Information),
             D2D1::RectF(panel.left + 12.0f * s, y, panel.right - 12.0f * s,
                                          y + 20.0f * s),
             compositor_->SmallFormat(), theme.accent);
        float iy = y + 20.0f * s;
        infoRow(pulse::l10n::Get(pulse::l10n::StringId::Location).c_str(), d.location_text, iy);
        infoRow(pulse::l10n::Get(pulse::l10n::StringId::KnownSize).c_str(), d.size_text, iy);
        y = iy + 8.0f * s;
    }

    if (d.multi_count <= 1) {
        for (const auto& def : kDetailsSections) {
            // Section separator (matches the 8 DIP gap in LayoutDetailsPanel).
            y += 8.0f * s;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRect(dc, brStrokeCard_.get(), panel.left + pad, y - 4.5f * s,
                     panel.right - panel.left - pad * 2.0f, 1.0f);
            section(pulse::l10n::Get(def.label).c_str(), y, def.id);
            y += 28.0f * s;
            if ((d.collapsed_mask >> def.id) & 1u) { y += 6.0f * s; continue; }
            switch (def.id) {
            case 0: { // 基本信息
                float iy = y;
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::Location).c_str(), d.location_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::ColumnType).c_str(), !d.type_text.empty()
                            ? d.type_text : FormatListType(d.name, d.is_dir), iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::ColumnSize).c_str(),
                        d.size_pending ? pulse::l10n::Get(pulse::l10n::StringId::Calculating)
                                                : d.size_text, iy);
                if (d.is_dir) infoRow(pulse::l10n::Get(pulse::l10n::StringId::Contains).c_str(), d.contains_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::Created).c_str(), d.created_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::ModifiedTime).c_str(), d.modified_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::LastAccessed).c_str(), d.accessed_text, iy);
                for (const auto& property : d.preview_properties)
                    infoRow(property.label.c_str(), property.value, iy);
                y = iy + 8.0f * s;
                break;
            }
            case 1: { // 属性: writable checkboxes + 高级… on one row
                fluent::ControlState roState;
                roState.checked = (d.attrs & FILE_ATTRIBUTE_READONLY) != 0;
                roState.hovered = IsHovered(vm, HitTestResult::DetailsAttrToggle, 0);
                painter_.DrawCheckBox(hit.attr_readonly,
                    pulse::l10n::Get(pulse::l10n::StringId::ReadOnly), roState);
                fluent::ControlState hidState;
                hidState.checked = (d.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
                hidState.hovered = IsHovered(vm, HitTestResult::DetailsAttrToggle, 1);
                painter_.DrawCheckBox(hit.attr_hidden,
                    pulse::l10n::Get(pulse::l10n::StringId::Hidden), hidState);
                {
                    const bool hot = IsHovered(vm, HitTestResult::DetailsAttrToggle, 2);
                    const auto& rc = hit.attr_advanced;
                    if (hot) {
                        MakeBrush(dc, theme.fill_hover, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                        rc.right - rc.left, rc.bottom - rc.top, 6.0f * s);
                    }
                    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f * s, 6.0f * s),
                                             brStrokeCard_.get(), 1.0f);
                    IDWriteTextFormat* fmt = compositor_->SmallFormat();
                    if (fmt) {
                        const auto old = fmt->GetTextAlignment();
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        MakeBrush(dc, theme.text, brText_);
                        DrawTextRect(dc, fmt, brText_.get(),
                                     pulse::l10n::Get(pulse::l10n::StringId::Advanced),
                                     rc.left, rc.top, rc.right - rc.left,
                                     rc.bottom - rc.top);
                        fmt->SetTextAlignment(old);
                    }
                }
                y += 24.0f * s + 8.0f * s;
                break;
            }
            case 2: { // 标签: 添加标签 row + preset chips (toggle on click)
                ghostButton(hit.tag_add, kIconAdd,
                            pulse::l10n::Get(pulse::l10n::StringId::AddTag),
                            IsHovered(vm, HitTestResult::DetailsTagAdd));
                for (size_t i = 0; i < hit.preset_chips.size(); ++i) {
                    const auto& rc = hit.preset_chips[i];
                    const auto& chip = d.preset_tags[i];
                    const bool hot = IsHovered(vm, HitTestResult::DetailsPresetTag,
                                               chip.tag_index);
                    D2D1_COLOR_F fill = chip.color;
                    fill.a *= chip.assigned ? (hot ? 0.34f : 0.24f)
                                            : (hot ? 0.10f : 0.05f);
                    MakeBrush(dc, fill, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                                    rc.right - rc.left, rc.bottom - rc.top, 11.0f * s);
                    D2D1_COLOR_F border = chip.color;
                    border.a *= chip.assigned ? 0.55f : 0.22f;
                    MakeBrush(dc, border, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 11.0f * s, 11.0f * s),
                                             brStrokeCard_.get(), 1.0f);
                    MakeBrush(dc, chip.color, brTagDot_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rc.left + 11.0f * s,
                        (rc.top + rc.bottom) * 0.5f), 3.5f * s, 3.5f * s), brTagDot_.get());
                    const D2D1_COLOR_F saved_bg = text_background_;
                    text_background_ = BlendOver(fill, theme.bg);
                    text(chip.name, D2D1::RectF(rc.left + 19.0f * s, rc.top,
                                                rc.right - 4.0f * s, rc.bottom),
                         compositor_->SmallFormat(),
                         chip.assigned ? theme.text : theme.text_secondary);
                    text_background_ = saved_bg;
                }
                y = hit.preset_chips.empty() ? (y + 26.0f * s + 6.0f * s + 8.0f * s)
                    : (hit.preset_chips.back().bottom + 8.0f * s);
                break;
            }
            case 3: { // 安全
                float iy = y;
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::Owner).c_str(),
                        d.owner_text.empty() ? pulse::l10n::Get(pulse::l10n::StringId::LoadingEllipsis)
                                             : d.owner_text, iy);
                {
                    const bool hot = IsHovered(vm, HitTestResult::DetailsSecurityChange);
                    text(pulse::l10n::Get(pulse::l10n::StringId::Change),
                         D2D1::RectF(hit.security_change.left, y,
                                     hit.security_change.right, y + 18.0f * s),
                         compositor_->SmallFormat(),
                         hot ? theme.accent_hover : theme.accent, true);
                }
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::Permissions).c_str(),
                        d.permissions_text.empty() ? pulse::l10n::Get(pulse::l10n::StringId::LoadingEllipsis)
                                                   : d.permissions_text, iy);
                y = iy + 8.0f * s;
                break;
            }
            case 4: { // 其他
                float iy = y;
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::Drive).c_str(),
                        d.drive_text.empty() ? pulse::l10n::Get(pulse::l10n::StringId::LoadingEllipsis)
                                             : d.drive_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::FileSystem).c_str(),
                        d.fs_text.empty() ? pulse::l10n::Get(pulse::l10n::StringId::LoadingEllipsis)
                                          : d.fs_text, iy);
                infoRow(pulse::l10n::Get(pulse::l10n::StringId::FreeSpace).c_str(),
                        d.free_space_text.empty() ? pulse::l10n::Get(pulse::l10n::StringId::LoadingEllipsis)
                                                  : d.free_space_text, iy);
                y = iy + 8.0f * s;
                break;
            }
            default: break;
            }
        }
    }
    dc->PopAxisAlignedClip();
    dc->PopAxisAlignedClip();
}

float MainRenderer::DetailsContentHeightDip(const WindowViewModel& vm, float w, float h) {
    const D2D1_RECT_F panel = DetailsPanelRect(w, h);
    if (panel.right - panel.left <= 1.0f) return 0.0f;
    DetailsHitRects hit;
    LayoutDetailsPanel(panel, scale_, vm.details,
                       compositor_ ? compositor_->DwriteFactory() : nullptr,
                       compositor_ ? compositor_->SmallFormat() : nullptr,
                       compositor_, DetailsPreviewHeight(panel, scale_), hit);
    return hit.content_height_dip;
}

} // namespace pulse::ui
