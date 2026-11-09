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
    using pulse::l10n::Get;
    using pulse::l10n::StringId;
    // Cloud placeholders get their own card (download and preview), not a failure notice.
    if (error == L"offline-placeholder") return {};
    if (error == L"archive-plugin-required") return Get(StringId::ArchivePluginRequired);
    if (error == L"docx-reading")
        return Get(StringId::ReadingPreviewNotWordLayoutImagesInTablesHeadersFootersAndSo).c_str();
    if (error.starts_with(L"svg-"))
    {
        if (error == L"svg-budget-exceeded")
            return Get(StringId::ThisSVGExceedsPreviewResourceLimits).c_str();
        if (error == L"svg-no-device")
            return Get(StringId::SystemSVGRenderingIsUnavailableSourceIsShownWhenReadable).c_str();
        if (error.starts_with(L"svg-unsupported:"))
            return Get(StringId::ThisSVGExceedsTheSystemRendererSSupportedFeaturesSourceIsSho).c_str();
        return Get(StringId::ThisSVGCouldNotBeFullyRenderedSourceIsShownWhenReadableToAvo).c_str();
    }
    if (error.starts_with(L"image-preview-incomplete:")) {
        const bool animation = metadata.find(L"\nT\tanimation\n") != std::wstring::npos;
        const bool fallback = metadata.find(L"\nT\tstatic-fallback\n") != std::wstring::npos;
        if (animation || (!fallback && error.substr(25) == L"frame-limit")) {
            std::wstring message = Get(StringId::IncompleteAnimationOnlyLoadedFramesPlay).c_str();
            const auto at = metadata.find(L"\nF\t");
            unsigned loaded = 0, declared = 0;
            if (at != std::wstring::npos && swscanf_s(metadata.c_str() + at, L"\nF\t%u\t%u", &loaded, &declared) == 2 && declared)
                message += L" (" + std::to_wstring(loaded) + L" / " + std::to_wstring(declared) + L")";
            return message;
        }
        return Get(StringId::StaticImageOnlyTheAnimationExceedsPreviewLimitsOrCouldNotBeF).c_str();
    }
    if (error.starts_with(L"udf-"))
        return Get(StringId::TheUDFDirectoryIsIncompleteUnsupportedLayoutDamagedDataOrAPr).c_str();
    if (error.starts_with(L"compressed-stream-"))
        return Get(StringId::ThisCompressedStreamCouldNotBeFullyReadAPreviewLimitUnsuppor).c_str();
    using preview::IntegrityState;
    using preview::IntegrityReason;
    const auto counted = [&](std::wstring message) {
        if (integrity.total) message += L" (" + std::to_wstring(integrity.loaded) +
            L" / " + std::to_wstring(integrity.total) + L")";
        return message;
    };
    if (integrity.state == IntegrityState::Partial) {
        if (integrity.reason == IntegrityReason::OnDemand)
            return Get(StringId::WorksheetsLoadOnDemandWhenSelected).c_str();
        if (integrity.reason == IntegrityReason::IncompleteDirectory)
            return Get(StringId::TheDirectoryIsIncompleteSearchCoversLoadedEntriesOnly).c_str();
        if (integrity.reason == IntegrityReason::ReadFailure)
            return counted(Get(StringId::SomeContentCouldNotBeReadSeeTheExplanationsInThePreview).c_str());
        if (integrity.reason == IntegrityReason::UnsupportedFeature)
            return Get(StringId::SomeContentIsNotSupportedSeeTheExplanationsInThePreview).c_str();
        if (integrity.reason == IntegrityReason::SourceFallback)
            return Get(StringId::RawDataIsShownTheFileFormatCouldNotBeFullyRendered).c_str();
        return Get(StringId::PartialContentThisFileExceedsPreviewLimits).c_str();
    }
    if (integrity.state == IntegrityState::Unsupported)
        return Get(StringId::PreviewingThisContentIsNotSupported).c_str();
    if (integrity.state == IntegrityState::Failed)
        return Get(StringId::ThePreviewCouldNotBeRead).c_str();
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
