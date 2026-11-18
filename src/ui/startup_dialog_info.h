#pragma once
#include "fluent_components.h"
#include "../common/localization.h"
#include <algorithm>
#include <wrl/client.h>

namespace pulse::ui {
struct StartupDialogInfoLayout {
    D2D1_RECT_F panel{}, preview{}, scope[2]{}, heading{}, notes[4]{};
    float bottom = 0;
};
namespace startup_info {
using I = l10n::StringId;
inline constexpr I scope_titles[] = {I::StartupScopeYes, I::StartupScopeNo};
inline constexpr I scope_bodies[] = {I::StartupScopeYesDesc, I::StartupScopeNoDesc};
inline constexpr I note_titles[] = {I::StartupKnowRestart, I::StartupKnowUser, I::StartupKnowProvider, I::StartupKnowRestore};
inline constexpr I note_bodies[] = {I::StartupKnowRestartDesc, I::StartupKnowUserDesc, I::StartupKnowProviderDesc, I::StartupKnowRestoreDesc};
inline float TitleHeight(const fluent::Painter* painter, I title, float width, float scale) {
    return painter ? painter->MeasureWrappedSectionHeight(l10n::Get(title), width) : 36*scale;
}
inline float BlockHeight(const fluent::Painter* painter, I title, I body, float width, float scale) {
    return TitleHeight(painter, title, width, scale) + scale +
        (painter ? painter->MeasureWrappedCaptionHeight(l10n::Get(body), width) : 54*scale);
}
inline StartupDialogInfoLayout Layout(float left, float right, float top, float scale, const fluent::Painter* painter) {
    StartupDialogInfoLayout result;
    const bool compact = right-left < 600*scale;
    // Match the HTML row text gutter; the intro owns its inset background.
    left += (compact ? 20.0f : 56.0f)*scale;
    right -= 20*scale;
    top += 14*scale;
    const float inner_left=left+18*scale, inner_right=right-18*scale;
    const float mini_width=(std::min)(200*scale,inner_right-inner_left);
    const float mini_height=mini_width*11/16;
    const float text_left=compact ? inner_left : inner_left+mini_width+22*scale;
    float y=top+18*scale+(compact ? mini_height+22*scale : 0.0f);
    const float scope_top=y;
    for(int i=0;i<2;++i) {
        const float height=BlockHeight(painter,scope_titles[i],scope_bodies[i],inner_right-text_left-28*scale,scale);
        result.scope[i]=D2D1::RectF(text_left,y,inner_right,y+height);
        y+=height+(i==0 ? 12*scale : 0.0f);
    }
    const float content_height=compact ? y-(top+18*scale) : (std::max)(mini_height,y-scope_top);
    result.panel=D2D1::RectF(left,top,right,top+36*scale+content_height);
    const float preview_top=top+18*scale+(compact ? 0.0f : (content_height-mini_height)/2);
    result.preview=D2D1::RectF(inner_left,preview_top,inner_left+mini_width,preview_top+mini_height);
    if(!compact) {
        const float offset=(content_height-(y-scope_top))/2;
        for(auto& r:result.scope) {r.top+=offset;r.bottom+=offset;}
    }
    y=result.panel.bottom+18*scale;
    result.heading=D2D1::RectF(left,y,right,y+22*scale);
    y=result.heading.bottom+4*scale;
    const int columns=compact ? 1 : 2;
    const float cell_width=(right-left-(columns-1)*28*scale)/columns;
    for(int i=0;i<4;i+=columns) {
        float height=0;
        for(int j=0;j<columns;++j)
            height=(std::max)(height,BlockHeight(painter,note_titles[i+j],note_bodies[i+j],cell_width-26*scale,scale));
        for(int j=0;j<columns;++j) {
            const float x=left+j*(cell_width+28*scale);
            result.notes[i+j]=D2D1::RectF(x,y,x+cell_width,y+height);
        }
        y+=height+14*scale;
    }
    result.bottom=y+6*scale;
    return result;
}
inline void Draw(ID2D1DeviceContext* dc, fluent::Painter& painter, const Theme& theme,
                 const StartupDialogInfoLayout& layout, float scale, bool enabled) {
    if(layout.bottom<=0) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if(FAILED(dc->CreateSolidColorBrush(theme.stroke_card,&brush))) return;
    auto fill=[&](D2D1_RECT_F r,D2D1_COLOR_F color,float radius=3.0f) {
        brush->SetColor(color);dc->FillRoundedRectangle(D2D1::RoundedRect(r,radius*scale,radius*scale),brush.Get());
    };
    fill(layout.panel,theme.fill_hover,10);
    const auto m=layout.preview;
    fill(m,theme.bg,8);
    brush->SetColor(theme.stroke_card);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(m,8*scale,8*scale),brush.Get(),scale);
    const float k=(m.right-m.left)/200.0f;
    auto mini=[&](float l,float t,float r,float b,D2D1_COLOR_F color,float radius=2.0f) {
        fill(D2D1::RectF(m.left+l*k,m.top+t*k,m.left+r*k,m.top+b*k),color,radius);
    };
    mini(7,7,15,15,theme.accent);
    mini(19,9,101,13,theme.stroke_card);
    mini(64,20,193,113,theme.surface_card,5);
    for(int i=0;i<5;++i) {
        const float y=25+i*10.0f;
        mini(12,y,53,y+4,i==0 ? theme.accent : theme.stroke_card);
        mini(69,y,188,y+6,i==1 ? theme.accent : theme.fill_hover);
    }
    mini(7,121,143,128,theme.surface_card);
    mini(148,120,193,129,theme.accent,3);
    auto block=[&](D2D1_RECT_F r,I title,I body,const wchar_t* glyph,D2D1_COLOR_F color,float inset) {
        const float icon_size=inset==28 ? 18.0f : 16.0f;
        painter.DrawGlyph(glyph,D2D1::RectF(r.left,r.top+2*scale,r.left+icon_size*scale,r.top+(2+icon_size)*scale),color);
        const float x=r.left+inset*scale,width=r.right-x;
        painter.DrawWrappedSection(l10n::Get(title),D2D1::Point2F(x,r.top),width,theme.text);
        painter.DrawWrappedCaption(l10n::Get(body),D2D1::Point2F(x,r.top+TitleHeight(&painter,title,width,scale)+scale),width,theme.text_secondary);
    };
    block(layout.scope[0],scope_titles[0],scope_bodies[0],L"\xE930",theme.accent,28);
    block(layout.scope[1],scope_titles[1],scope_bodies[1],L"\xE733",theme.text_secondary,28);
    painter.DrawWrappedCaption(l10n::Get(enabled?I::StartupKnowTitle:I::StartupKnowBefore),D2D1::Point2F(layout.heading.left,layout.heading.top),layout.heading.right-layout.heading.left,theme.text_secondary);
    const wchar_t* glyphs[]={L"\xE777",L"\xF102",L"\xE733",L"\xE7A7"};
    for(int i=0;i<4;++i)
        block(layout.notes[i],note_titles[i],note_bodies[i],glyphs[i],theme.text_secondary,26);
}
} // namespace startup_info
} // namespace pulse::ui
