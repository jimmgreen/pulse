#include "host/render/desktop_text_renderer.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace pulse::desktop::render {
using Microsoft::WRL::ComPtr;
void DesktopTextRenderer::Reset() noexcept {
    text_.Shutdown();layouts_.clear();target_.Reset();factory_.Reset();serial_=0;stats_={};
}
void DesktopTextRenderer::Bind(IDWriteFactory* factory,ID2D1RenderTarget* target) {
    if(factory_.Get()==factory&&target_.Get()==target)return;
    Reset();factory_=factory;target_=target;text_.Init(factory,target);
}
IDWriteTextLayout* DesktopTextRenderer::Layout(IDWriteFactory* factory,std::wstring_view text,
    IDWriteTextFormat* format,float width,float height,bool ellipsized,DWRITE_TEXT_ALIGNMENT alignment) {
    if(!factory||!format||text.empty()||width<=0||height<=0)return nullptr;
    Key key;key.text=text;
    key.family.resize(static_cast<size_t>(format->GetFontFamilyNameLength())+1);
    format->GetFontFamilyName(key.family.data(),static_cast<UINT32>(key.family.size()));
    key.locale.resize(static_cast<size_t>(format->GetLocaleNameLength())+1);
    format->GetLocaleName(key.locale.data(),static_cast<UINT32>(key.locale.size()));
    DWRITE_TRIMMING trim{};ComPtr<IDWriteInlineObject> sign;format->GetTrimming(&trim,&sign);
    DWRITE_LINE_SPACING_METHOD spacing{};float line=0,baseline=0;format->GetLineSpacing(&spacing,&line,&baseline);
    ComPtr<IDWriteFontCollection> collection;format->GetFontCollection(&collection);
    const auto bits=[](float value){return static_cast<uint64_t>(std::bit_cast<uint32_t>(value));};
    const auto value=[](auto v){return static_cast<uint64_t>(v);};
    key.attributes={reinterpret_cast<uintptr_t>(format),reinterpret_cast<uintptr_t>(collection.Get()),
        bits(width),bits(height),bits(format->GetFontSize()),value(format->GetFontWeight()),value(format->GetFontStyle()),value(format->GetFontStretch()),
        value(format->GetTextAlignment()),value(format->GetParagraphAlignment()),value(format->GetWordWrapping()),value(format->GetReadingDirection()),
        value(format->GetFlowDirection()),bits(format->GetIncrementalTabStop()),value(spacing),bits(line),bits(baseline),
        value(trim.granularity),trim.delimiter,trim.delimiterCount,reinterpret_cast<uintptr_t>(sign.Get()),ellipsized,value(alignment)};
    auto found=layouts_.find(key);
    if(found!=layouts_.end()){++stats_.layout_hits;found->second.used=++serial_;return found->second.layout.Get();}
    ComPtr<IDWriteTextLayout> layout;
    if(FAILED(factory->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),format,width,height,&layout)))return nullptr;
    if(ellipsized){
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);layout->SetTextAlignment(alignment);
        ComPtr<IDWriteInlineObject> ellipsis;
        if(SUCCEEDED(factory->CreateEllipsisTrimmingSign(layout.Get(),&ellipsis))){
            const DWRITE_TRIMMING clipping{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&clipping,ellipsis.Get());
        }
    }
    if(layouts_.size()>=kMaxLayouts){
        const auto oldest=std::min_element(layouts_.begin(),layouts_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        layouts_.erase(oldest);++stats_.evictions;
    }
    ++stats_.layout_builds;
    Entry entry;entry.layout=layout;entry.format=format;entry.used=++serial_;
    return layouts_.emplace(std::move(key),std::move(entry)).first->second.layout.Get();
}
void DesktopTextRenderer::DrawText(IDWriteFactory* factory,ID2D1RenderTarget* target,const wchar_t* text,UINT32 length,
    IDWriteTextFormat* format,const D2D1_RECT_F& bounds,ID2D1SolidColorBrush* brush,D2D1_DRAW_TEXT_OPTIONS options) {
    if(!target||!factory||!text||!length||!format||!brush)return;
    Bind(factory,target);
    auto* layout=Layout(factory,{text,length},format,bounds.right-bounds.left,bounds.bottom-bounds.top);
    if(layout)DrawLayout(factory,target,{bounds.left,bounds.top},layout,brush,options);
    else {++stats_.fallback_draws;text_.RecordFallback(0);target->DrawText(text,length,format,bounds,brush,options);}
}
void DesktopTextRenderer::DrawLayout(IDWriteFactory* factory,ID2D1RenderTarget* target,D2D1_POINT_2F origin,
    IDWriteTextLayout* layout,ID2D1SolidColorBrush* brush,D2D1_DRAW_TEXT_OPTIONS options) {
    if(!target||!factory||!layout||!brush)return;
    // Bind may clear the cache, so callers obtain cached layouts only after the
    // target was bound, or retain their layout across the bind.
    ComPtr<IDWriteTextLayout> keep=layout;
    Bind(factory,target);++stats_.requests;
    D2D1_MATRIX_3X2_F previous{};target->GetTransform(&previous);auto transform=previous;
    if(std::abs(previous._12)<.0001f&&std::abs(previous._21)<.0001f&&previous._11>.0001f&&previous._22>.0001f){
        origin.x+=previous._31/previous._11;origin.y+=previous._32/previous._22;
        transform._31=transform._32=0;target->SetTransform(transform);
    }
    struct Restore {ID2D1RenderTarget* target;D2D1_MATRIX_3X2_F transform;~Restore(){target->SetTransform(transform);}} restore{target,previous};
    auto color=brush->GetColor();color.a*=brush->GetOpacity();
    if(text_.DrawLayout(origin,layout,color,(options&D2D1_DRAW_TEXT_OPTIONS_CLIP)!=0))return;
    ++stats_.fallback_draws;text_.RecordFallback(1);
    target->DrawTextLayout(origin,layout,brush,options);
}
}
