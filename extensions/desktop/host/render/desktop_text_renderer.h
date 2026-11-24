#pragma once
#include "../../../../src/ui/lumatext_renderer.h"
#include <wrl/client.h>
#include <map>
#include <string>
#include <vector>
#include <tuple>

namespace pulse::desktop::render {
struct DesktopTextCacheStats {
    uint64_t requests=0, layout_builds=0, layout_hits=0, evictions=0, fallback_draws=0;
};
// Shared widget typography: cached DirectWrite shaping, LumaText rasterization,
// and the same physical origin for full and translated dirty-surface draws.
class DesktopTextRenderer {
public:
    void Reset() noexcept;
    void Bind(IDWriteFactory* factory, ID2D1RenderTarget* target);
    IDWriteTextLayout* Layout(IDWriteFactory* factory, std::wstring_view text, IDWriteTextFormat* format,
                             float width, float height, bool ellipsized=false,
                             DWRITE_TEXT_ALIGNMENT alignment=DWRITE_TEXT_ALIGNMENT_LEADING);
    void DrawText(IDWriteFactory* factory, ID2D1RenderTarget* target,
                  const wchar_t* text, UINT32 length, IDWriteTextFormat* format, const D2D1_RECT_F& bounds,
                  ID2D1SolidColorBrush* brush, D2D1_DRAW_TEXT_OPTIONS options=D2D1_DRAW_TEXT_OPTIONS_NONE);
    void DrawLayout(IDWriteFactory* factory, ID2D1RenderTarget* target, D2D1_POINT_2F origin,
                    IDWriteTextLayout* layout, ID2D1SolidColorBrush* brush,
                    D2D1_DRAW_TEXT_OPTIONS options=D2D1_DRAW_TEXT_OPTIONS_NONE);
    const ui::LumaTextStats& Stats() const noexcept { return text_.Stats(); }
    const DesktopTextCacheStats& CacheStats() const noexcept { return stats_; }
    size_t CacheSize() const noexcept { return layouts_.size(); }
    static constexpr size_t kMaxLayouts=384;
private:
    struct Key {
        std::wstring text, family, locale;
        std::vector<uint64_t> attributes;
        bool operator<(const Key& other) const { return std::tie(text,family,locale,attributes)<std::tie(other.text,other.family,other.locale,other.attributes); }
    };
    struct Entry {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        uint64_t used=0;
    };
    Microsoft::WRL::ComPtr<IDWriteFactory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    ui::LumaTextRenderer text_;
    std::map<Key,Entry> layouts_;
    uint64_t serial_=0;
    DesktopTextCacheStats stats_{};
};
}
