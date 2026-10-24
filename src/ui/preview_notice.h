#pragma once
#include "../common/localization.h"
#include "../common/preview_integrity.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>
#include <string>

namespace pulse::ui {
inline std::wstring PreviewNotice(const std::wstring& error, const std::wstring& metadata = {},
    const preview::Integrity& integrity = {}) {
    using pulse::l10n::Pick;
    if (error == L"docx-reading")
        return Pick(L"正文阅读预览，非 Word 原版排版；表格内图片、页眉页脚和部分嵌入内容可能遗漏。",
                    L"Reading preview, not Word layout. Images in tables, headers, footers and some embedded content may be omitted.");
    if (error.starts_with(L"svg-"))
    {
        if (error == L"svg-budget-exceeded")
            return Pick(L"此 SVG 超出预览资源限制。", L"This SVG exceeds preview resource limits.");
        if (error == L"svg-no-device")
            return Pick(L"当前系统的 SVG 渲染能力不可用，可读取时显示源码。", L"System SVG rendering is unavailable; source is shown when readable.");
        if (error.starts_with(L"svg-unsupported:"))
            return Pick(L"此 SVG 含系统渲染器不支持的内容，已显示源码，避免遗漏文字或图形。", L"This SVG exceeds the system renderer's supported features. Source is shown to avoid omitting text or graphics.");
        return Pick(L"此 SVG 未能完整渲染；可读取时显示源码，避免无声遗漏内容。",
                    L"This SVG could not be fully rendered. Source is shown when readable to avoid silently omitting content.");
    }
    if (error.starts_with(L"image-preview-incomplete:")) {
        const bool animation = metadata.find(L"\nT\tanimation\n") != std::wstring::npos;
        const bool fallback = metadata.find(L"\nT\tstatic-fallback\n") != std::wstring::npos;
        if (animation || (!fallback && error.substr(25) == L"frame-limit")) {
            std::wstring message = Pick(L"动画预览不完整：仅播放已加载的帧。", L"Incomplete animation: only loaded frames play.");
            const auto at = metadata.find(L"\nF\t");
            unsigned loaded = 0, declared = 0;
            if (at != std::wstring::npos && swscanf_s(metadata.c_str() + at, L"\nF\t%u\t%u", &loaded, &declared) == 2 && declared)
                message += L" (" + std::to_wstring(loaded) + L" / " + std::to_wstring(declared) + L")";
            return message;
        }
        return Pick(L"仅显示静态图：动画超出预览限制或未能完整解码。",
                    L"Static image only: the animation exceeds preview limits or could not be fully decoded.");
    }
    if (error.starts_with(L"udf-"))
        return Pick(L"UDF 目录未能完整读取：结构不受支持、已损坏或超出预览限制。请挂载镜像核对全部内容。",
                    L"The UDF directory is incomplete: unsupported layout, damaged data or a preview limit. Mount the image to check all contents.");
    if (error.starts_with(L"compressed-stream-"))
        return Pick(L"未能完整读取此压缩流：可能超出预览限制、格式不受支持或文件已损坏。",
                    L"This compressed stream could not be fully read: a preview limit, unsupported format or damaged data prevented it.");
    using preview::IntegrityState;
    using preview::IntegrityReason;
    const auto counted = [&](std::wstring message) {
        if (integrity.total) message += L" (" + std::to_wstring(integrity.loaded) +
            L" / " + std::to_wstring(integrity.total) + L")";
        return message;
    };
    if (integrity.state == IntegrityState::Partial) {
        if (metadata.starts_with(L"Portable Executable (PE32")) {
            std::wstring message = Pick(L"PE 预览不完整：", L"Incomplete PE preview: ");
            bool explained = false;
            const auto append = [&](bool condition, const wchar_t* chinese, const wchar_t* english) {
                if (!condition) return;
                if (explained) message += Pick(L"；", L"; ");
                message += Pick(chinese, english);
                explained = true;
            };
            append(metadata.find(L"[LIMIT]") != std::wstring::npos,
                L"依赖分析达到资源限制", L"dependency analysis reached its resource limits");
            append(metadata.find(L"[Preview truncated]") != std::wstring::npos,
                L"元数据报告已截断", L"metadata report truncated");
            append(metadata.find(L"Some metadata is invalid") != std::wstring::npos ||
                   metadata.find(L"Import metadata is incomplete") != std::wstring::npos,
                L"部分元数据无法完整读取", L"some metadata could not be fully read");
            if (!explained) message += Pick(L"部分信息未能完整分析", L"some information could not be fully analyzed");
            return message + Pick(L"。详情见各页签中的说明。", L". See the notes in each tab.");
        }
        if (integrity.reason == IntegrityReason::OnDemand)
            return Pick(L"工作表按需加载，切换标签即可读取。", L"Worksheets load on demand when selected.");
        if (integrity.reason == IntegrityReason::IncompleteDirectory)
            return Pick(L"目录尚未完整读取，搜索仅覆盖已加载的条目。", L"The directory is incomplete; search covers loaded entries only.");
        if (integrity.reason == IntegrityReason::ReadFailure)
            return counted(Pick(L"部分内容未能读取，请查看内容中的具体说明。", L"Some content could not be read; see the explanations in the preview."));
        if (integrity.reason == IntegrityReason::UnsupportedFeature)
            return Pick(L"部分内容暂不支持预览，请查看内容中的具体说明。", L"Some content is not supported; see the explanations in the preview.");
        if (integrity.reason == IntegrityReason::SourceFallback)
            return Pick(L"当前显示原始数据，无法按此文件格式完整呈现。", L"Raw data is shown; the file format could not be fully rendered.");
        return Pick(L"仅显示部分内容：文件超出预览限制。", L"Partial content: this file exceeds preview limits.");
    }
    if (integrity.state == IntegrityState::Unsupported)
        return Pick(L"当前不支持预览此内容。", L"Previewing this content is not supported.");
    if (integrity.state == IntegrityState::Failed)
        return Pick(L"预览读取失败。", L"The preview could not be read.");
    return {};
}

inline float DrawPreviewNotice(ID2D1DeviceContext* dc, IDWriteFactory* factory,
    IDWriteTextFormat* format, const std::wstring& text, const D2D1_RECT_F& rect,
    float scale, const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background) {
    if (text.empty() || !factory || !format || rect.bottom <= rect.top || rect.right <= rect.left) return 0;
    const float pad = 10.0f * scale;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
        (std::max)(1.0f, rect.right - rect.left - pad * 2), 1000.0f, &layout))) return 0;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const float height = (std::min)(metrics.height + pad * 2, rect.bottom - rect.top);
    layout->SetMaxHeight((std::max)(1.0f, height - pad * 2));
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(format, &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trimming, ellipsis.Get());
    if (dc) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill, ink;
        dc->CreateSolidColorBrush(background, &fill);
        dc->CreateSolidColorBrush(foreground, &ink);
        if (fill && ink) {
            dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->FillRectangle(D2D1::RectF(rect.left, rect.top, rect.right, rect.top + height), fill.Get());
            dc->DrawTextLayout(D2D1::Point2F(rect.left + pad, rect.top + pad), layout.Get(), ink.Get());
            dc->PopAxisAlignedClip();
        }
    }
    return height;
}
} // namespace pulse::ui
