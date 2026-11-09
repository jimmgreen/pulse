#pragma once
#include "ui_view_model.h"
#include "command_icons.h"
#include "../common/localization.h"
#include "typography.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {
// HTML: 26x22 buttons, 1 DIP gap, 1 DIP horizontal padding;
// the divider occupies 1 + 4 + 4 DIP, with one extra flex gap.
inline D2D1_RECT_F PaneLayoutButtonBounds(D2D1_RECT_F strip, int index, float scale) {
    const float left=strip.left+(1+27*index+(index>=5?10:0))*scale;
    const float top=(strip.top+strip.bottom)*.5f-11*scale;
    return {left,top,left+26*scale,top+22*scale};
}
// Status-bar text widths for the strip's room check. Called on every frame
// and every mouse move (bounds and hit tests), so the last few strings are
// remembered instead of building a DirectWrite layout each time.
inline float PaneLayoutMeasure(const std::wstring& value, float scale, IDWriteFactory* factory, IDWriteTextFormat* format) {
    if(value.empty())return 0.0f;
    if(!factory||!format)return static_cast<float>(value.size())*14*scale;
    // Formats are recreated at reused addresses; the generation retires them.
    struct Entry { IDWriteTextFormat* format; std::uint64_t generation; std::wstring text; float width; };
    thread_local std::vector<Entry> cache;
    const std::uint64_t generation=typography::Generation();
    for(const auto& entry:cache)if(entry.format==format&&entry.generation==generation&&entry.text==value)return entry.width;
    IDWriteTextLayout* layout=nullptr;DWRITE_TEXT_METRICS metrics{};
    if(FAILED(factory->CreateTextLayout(value.c_str(),static_cast<UINT32>(value.size()),format,10000,100,&layout))||!layout)return 0.0f;
    layout->GetMetrics(&metrics);layout->Release();
    if(cache.size()>=16)cache.erase(cache.begin());
    cache.push_back({format,generation,value,metrics.widthIncludingTrailingWhitespace});
    return metrics.widthIncludingTrailingWhitespace;
}
// HTML .zstrip: the label pill (8 + 10 icon + 5 + words + 8), 6 gap, the
// 46 DIP mini-map, 6 gap, the 26 DIP restore button.
inline float PaneZoomLabelWidth(float scale, IDWriteFactory* factory, IDWriteTextFormat* format, bool words) {
    return (words?31*scale+PaneLayoutMeasure(l10n::Get(l10n::StringId::PaneZoomBadge),scale,factory,format):26*scale);
}
inline float PaneLayoutStripWidth(const WindowViewModel& vm, float width, float scale, IDWriteFactory* factory=nullptr, IDWriteTextFormat* format=nullptr) {
    if (!vm.layout_controls.visible) return 0;
    if (vm.layout_controls.zoom_index >= 0) {
        const float cap=std::min(190.0f*scale, width*0.38f);
        const float full=84*scale+PaneZoomLabelWidth(scale,factory,format,true);
        return full<=cap?full:std::min(cap,84*scale+PaneZoomLabelWidth(scale,factory,format,false));
    }
    const auto& status=vm.status;
    const auto measure=[&](const std::wstring& value) { return PaneLayoutMeasure(value,scale,factory,format); };
    const float text_budget=measure(status.status_text)+measure(status.selection_text);
    const float ai_budget=status.ai_state==StatusAiState::Hidden?0:68*scale;
    const float history_budget=std::min(240.0f*scale,measure(status.undo_label)+(status.can_redo?98:70)*scale);
    const float task_budget=status.task_active||!status.task_text.empty()?190*scale:0;
    // Collapsed (HTML .strip-collapsed): the caret and the current layout,
    // 1 + 12 + 1 + 26 + 1 DIP; the leading divider sits outside the strip.
    return (width >= text_budget+ai_budget+history_budget+task_budget+260*scale?200.0f:42.0f)*scale;
}
inline D2D1_RECT_F PaneLayoutStripBounds(const WindowViewModel& vm, D2D1_RECT_F full, float scale, IDWriteFactory* factory=nullptr, IDWriteTextFormat* format=nullptr) {
    return D2D1::RectF(full.right-8*scale-PaneLayoutStripWidth(vm,full.right-full.left,scale,factory,format), full.bottom-25*scale,
        full.right-8*scale,full.bottom-3*scale);
}
inline D2D1_RECT_F PaneLayoutMiniBounds(D2D1_RECT_F strip,float scale) {
    const float center=(strip.top+strip.bottom)*.5f;
    // 46x20, then the HTML's 6 DIP gap before the 26 DIP restore button.
    return {strip.right-78*scale,center-10*scale,strip.right-32*scale,center+10*scale};
}
inline std::vector<D2D1_RECT_F> PaneLayoutCells(int preset, D2D1_RECT_F r, float gap) {
    const float mx=(r.left+r.right)*.5f,my=(r.top+r.bottom)*.5f;
    if(preset==1) return {{r.left,r.top,mx-gap*.5f,r.bottom},{mx+gap*.5f,r.top,r.right,r.bottom}};
    if(preset==2) return {{r.left,r.top,r.right,my-gap*.5f},{r.left,my+gap*.5f,r.right,r.bottom}};
    if(preset==3) return {{r.left,r.top,mx-gap*.5f,r.bottom},{mx+gap*.5f,r.top,r.right,my-gap*.5f},{mx+gap*.5f,my+gap*.5f,r.right,r.bottom}};
    if(preset==4) return {{r.left,r.top,mx-gap*.5f,my-gap*.5f},{mx+gap*.5f,r.top,r.right,my-gap*.5f},{r.left,my+gap*.5f,mx-gap*.5f,r.bottom},{mx+gap*.5f,my+gap*.5f,r.right,r.bottom}};
    return {r};
}
inline D2D1_RECT_F PaneLayoutTrayBounds(const WindowViewModel& vm,D2D1_RECT_F full,float scale,IDWriteFactory* factory=nullptr,IDWriteTextFormat* format=nullptr) {
    const auto strip=PaneLayoutStripBounds(vm,full,scale,factory,format);
    return {std::max(full.left+8*scale,strip.right-200*scale),strip.top-2*scale,strip.right,strip.bottom+2*scale};
}
inline D2D1_RECT_F PaneLayoutPeekBounds(const WindowViewModel& vm,D2D1_RECT_F full,float scale,IDWriteFactory* factory=nullptr,IDWriteTextFormat* format=nullptr) {
    const auto strip=PaneLayoutStripBounds(vm,full,scale,factory,format);
    return {std::max(full.left+8*scale,strip.right-304*scale),std::max(full.top+55*scale,strip.top-282*scale),strip.right,strip.top-8*scale};
}
inline HitTestResult PaneLayoutHit(const WindowViewModel& vm,D2D1_RECT_F full,float x,float y,float scale,IDWriteFactory* factory=nullptr,IDWriteTextFormat* format=nullptr) {
    HitTestResult hit;
    const auto& c=vm.layout_controls;
    if(!c.visible) return hit;
    const auto contains=[&](D2D1_RECT_F r){return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;};
    if(c.peek_index>=0&&contains(PaneLayoutPeekBounds(vm,full,scale,factory,format))) {hit.region=HitTestResult::PaneLayoutPeek;hit.index=c.peek_index;hit.sub_index=1;return hit;}
    auto strip=PaneLayoutStripBounds(vm,full,scale,factory,format);
    const bool tray=c.tray_open&&c.zoom_index<0&&contains(PaneLayoutTrayBounds(vm,full,scale,factory,format));
    if(!tray&&!contains(strip))return hit;
    if(c.zoom_index>=0) {
        if(x>=strip.right-29*scale){hit.region=HitTestResult::PaneLayoutRestore;return hit;}
        const auto cells=PaneLayoutCells(c.preset,PaneLayoutMiniBounds(strip,scale),2*scale);
        for(size_t i=0;i<cells.size();++i)if(contains(cells[i])){hit.region=HitTestResult::PaneLayoutCell;hit.index=static_cast<int>(i);return hit;}
        hit.region=HitTestResult::PaneLayoutTray;return hit;
    }
    if(!tray&&strip.right-strip.left<100*scale){hit.region=HitTestResult::PaneLayoutTray;return hit;}
    if(tray)strip=PaneLayoutTrayBounds(vm,full,scale,factory,format);
    for(int index=0;index<7;++index) {
        if(!contains(PaneLayoutButtonBounds(strip,index,scale)))continue;
        hit.region=index<5?HitTestResult::PaneLayoutPreset:index==5?HitTestResult::PaneLayoutCompare:HitTestResult::PaneLayoutZoom;
        hit.index=index<5?index:-1;return hit;
    }
    hit.region=HitTestResult::PaneLayoutTray;
    return hit;
}
}
