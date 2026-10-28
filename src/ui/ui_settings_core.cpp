#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "ai_panel_icons.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"
#include <algorithm>
#include "../common/text_format.h"
#include <cmath>

namespace pulse::ui {
void MainRenderer::DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc=compositor_->Dc();
    const auto lay=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    auto text=[&](const std::wstring& value,D2D1_RECT_F r,bool is_small=false) {
        painter_.DrawText(value,r,is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),is_small ? theme.text_secondary : theme.text);
    };
    auto draw_card=[&](D2D1_RECT_F r) {
        if(r.bottom<=r.top) return;
        MakeBrush(dc,WithAlpha(theme.surface_card,card_alpha_),brFillInput_); MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r,8*scale_,8*scale_),brStrokeCard_.get(),1);
    };
    auto divider=[&](D2D1_RECT_F r) {
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        FillRect(dc,brStrokeDivider_.get(),r.left+16*scale_,r.bottom,r.right-r.left-32*scale_,1);
    };
    auto label=[&](D2D1_RECT_F r,const std::wstring& title,const std::wstring& desc,const wchar_t* icon,float text_right=0.0f,float description_height=21.0f) {
        if(r.bottom<=r.top) return;
        const float right=text_right>0 ? text_right : r.right-72*scale_;
        // 「助手刚改的」 (plans/ai batch 3, .sv-row.ai): accent tint, a 3px
        // bar on the left, a spark pill after the title.
        const bool ai_mark=!vm.settings_ai_marks.empty() &&
            std::find(vm.settings_ai_marks.begin(),vm.settings_ai_marks.end(),title)!=vm.settings_ai_marks.end();
        if(ai_mark) {
            MakeBrush(dc,WithAlpha(theme.accent,0.08f),brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+1*scale_,r.top+1*scale_,r.right-r.left-2*scale_,r.bottom-r.top-2*scale_,7*scale_);
            MakeBrush(dc,theme.accent,brAccent_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left+1*scale_,r.top+9*scale_,r.left+4*scale_,(std::min)(r.bottom,r.top+56*scale_)-9*scale_),
                1.5f*scale_,1.5f*scale_),brAccent_.get());
        }
        DrawIconText(r.left+16*scale_,r.top+17*scale_,24*scale_,24*scale_,icon,L"",theme.text_secondary,0.85f);
        text(title,D2D1::RectF(r.left+54*scale_,r.top+10*scale_,right,r.top+34*scale_));
        if(ai_mark) {
            // .aib: 10.5px semibold accent on accent-soft, radius 8, line 17,
            // padding 0 6 0 4, spark 11 + gap 3; 6 after the title.
            ComPtr<IDWriteTextLayout> tl,pl;
            const std::wstring mark=l10n::Get(I::AiSetMark);
            auto* dw=compositor_->DwriteFactory();
            if(SUCCEEDED(dw->CreateTextLayout(title.c_str(),static_cast<UINT32>(title.size()),compositor_->TextFormat(),4000.0f,40*scale_,&tl)) &&
               SUCCEEDED(dw->CreateTextLayout(mark.c_str(),static_cast<UINT32>(mark.size()),compositor_->SmallFormat(),4000.0f,20*scale_,&pl))) {
                pl->SetFontSize(10.5f*scale_,{0,static_cast<UINT32>(mark.size())});
                pl->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,{0,static_cast<UINT32>(mark.size())});
                pl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS tm{},pm{};
                tl->GetMetrics(&tm);pl->GetMetrics(&pm);
                const float x=r.left+54*scale_+tm.widthIncludingTrailingWhitespace+6*scale_;
                const float w=4*scale_+11*scale_+3*scale_+pm.widthIncludingTrailingWhitespace+6*scale_;
                const float h=17*scale_,y=r.top+22*scale_-h/2;
                if(x+w<=right) {
                    MakeBrush(dc,WithAlpha(theme.accent,0.16f),brFillHover_);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),8*scale_,8*scale_),brFillHover_.get());
                    DrawAiIcon(dc,AiIcon::Spark,D2D1::RectF(x+4*scale_,y+3*scale_,x+15*scale_,y+14*scale_),theme.accent);
                    pl->SetMaxHeight(h);
                    MakeBrush(dc,theme.accent,brAccent_);
                    compositor_->DrawTextLayout(D2D1::Point2F(x+18*scale_,y),pl.get(),brAccent_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
        if(!desc.empty()) {
            const auto bounds=D2D1::RectF(r.left+54*scale_,r.top+35*scale_,right,r.top+(35+description_height)*scale_);
            if(description_height<=21.0f) text(desc,bounds,true);
            else {
                ComPtr<IDWriteTextLayout> description;
                if(SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(desc.c_str(),static_cast<UINT32>(desc.size()),
                    compositor_->SmallFormat(),bounds.right-bounds.left,bounds.bottom-bounds.top,&description))) {
                    description->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    description->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    MakeBrush(dc,theme.text_secondary,brTextSecondary_);
                    compositor_->DrawTextLayout(D2D1::Point2F(bounds.left,bounds.top),description.get(),brTextSecondary_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int hit,float text_right=0.0f) {
        if(r.bottom<=r.top) return;
        if(IsHovered(vm,H::SettingsToggle,hit)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon,text_right,hit==15 ? 43.0f : 21.0f);
        fluent::ControlState state{}; state.checked=on; state.hovered=IsHovered(vm,H::SettingsToggle,hit);
        painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
    };
    auto button=[&](D2D1_RECT_F r,const std::wstring& value,H::Region region,int index,bool primary=false,bool enabled=true,const wchar_t* glyph=L"") {
        if(r.bottom<=r.top) return;
        fluent::ControlState st{}; st.enabled=enabled; st.hovered=enabled && IsHovered(vm,region,index);
        painter_.DrawButton({r,value,glyph,primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,st});
    };
    auto disclosure=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,int id,bool own_card) {
        if(own_card) draw_card(r);
        if(IsHovered(vm,H::SettingsDisclosure,id)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon);
        DrawIconText(r.right-38*scale_,r.top+22*scale_,18*scale_,18*scale_,
            (vm.settings_expanded&(1u<<id)) ? L"\xE70D" : L"\xE76C",L"",theme.text_secondary,0.75f);
    };
    auto section=[&](int i,I title) { text(l10n::Get(title),lay.section[i]); };
    auto segmented=[&](D2D1_RECT_F card,D2D1_RECT_F const* choices,const I* labels,const int* values,int current,H::Region hit,I title,I desc,int count=3,const wchar_t* icon=L"\xE8A4") {
        if(card.bottom<=card.top) return;
        const bool stacked=choices[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),icon,stacked ? card.right-16*scale_ : choices[0].left-12*scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[count-1].right,choices[count-1].bottom));
        for(int i=0;i<count;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(labels[i]);
            item.state.checked=current==values[i];item.state.hovered=IsHovered(vm,hit,i);
            item.shared_track=true; item.position=i==0 ? fluent::SegmentPosition::First : i==count-1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    // Continuous slider in the segment cells: accent fill, Fluent thumb, preset
    // ticks labelled with the former level names, value at the right.
    auto slider=[&](D2D1_RECT_F card,D2D1_RECT_F const* cells,int value,int max_value,const int* ticks,const I* tick_labels,
                    H::Region hit,I title,I desc,const wchar_t* unit,const wchar_t* icon) {
        if(card.bottom<=card.top) return;
        const bool stacked=cells[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),icon,stacked ? card.right-16*scale_ : cells[0].left-12*scale_);
        const auto g=SettingsSlider(cells,scale_);
        const bool hot=vm.hover_region==static_cast<int>(hit);
        const float span=(std::max)(1.0f,g.right-g.left);
        const auto x_of=[&](int v){return g.left+span*std::clamp(static_cast<float>(v)/static_cast<float>(max_value),0.0f,1.0f);};
        const float x=x_of(value);
        const float th=4*scale_;
        MakeBrush(dc,WithAlpha(theme.text,0.16f),brFillHover_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left,g.cy-th/2,g.right,g.cy+th/2),th/2,th/2),brFillHover_.get());
        MakeBrush(dc,theme.accent,brAccent_);
        if(x>g.left) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left,g.cy-th/2,x,g.cy+th/2),th/2,th/2),brAccent_.get());
        for(int i=0;i<3;++i) {
            if(ticks[i]<0) continue;
            const float tx=x_of(ticks[i]);
            MakeBrush(dc,WithAlpha(theme.text_secondary,value==ticks[i] ? 0.9f : 0.55f),brTextSecondary_);
            if(ticks[i]>0) dc->FillRectangle(D2D1::RectF(tx-0.5f*scale_,g.cy+5*scale_,tx+0.5f*scale_,g.cy+8*scale_),brTextSecondary_.get());
            // The ends align to the track's ends; the others centre on their tick.
            const bool at_end=ticks[i]>=max_value;
            const auto lr=ticks[i]==0 ? D2D1::RectF(g.left-9*scale_,g.cy+8*scale_,g.left+60*scale_,g.cy+22*scale_)
                         : at_end ? D2D1::RectF(g.right-60*scale_,g.cy+8*scale_,g.right+9*scale_,g.cy+22*scale_)
                                  : D2D1::RectF(tx-34*scale_,g.cy+8*scale_,tx+34*scale_,g.cy+22*scale_);
            painter_.DrawText(l10n::Get(tick_labels[i]),lr,compositor_->SmallFormat(),
                value==ticks[i] ? theme.text : theme.text_secondary,
                ticks[i]==0 ? fluent::HorizontalAlignment::Left : at_end ? fluent::HorizontalAlignment::Right
                                                                    : fluent::HorizontalAlignment::Center);
        }
        MakeBrush(dc,WithAlpha(theme.surface_card,1.0f),brFillInput_);
        MakeBrush(dc,theme.stroke_card,brStrokeCard_);
        const float outer=9*scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),outer,outer),brFillInput_.get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),outer,outer),brStrokeCard_.get(),1);
        const float inner=(hot ? 6.0f : 5.0f)*scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,g.cy),inner,inner),brAccent_.get());
        painter_.DrawText(std::to_wstring(value)+unit,D2D1::RectF(g.right+10*scale_,g.cy-10*scale_,cells[2].right,g.cy+10*scale_),
            compositor_->TextFormat(),theme.text,fluent::HorizontalAlignment::Right);
    };
    if(IsGeneralSettingsPage(vm.settings_page)) {
        const int page=vm.settings_page;
        for(int i=0;i<lay.section_count;++i) {
            const auto& r=lay.section[i];
            painter_.DrawText(l10n::Get(lay.section_title[i]),D2D1::RectF(r.left+2*scale_,r.top+6*scale_,r.right,r.bottom),
                compositor_->HeaderFormat(),theme.text);
        }
        for(const auto& group:lay.group) draw_card(group);
        // Dependent rows: a faint band clipped to the card's corners, with a guide line.
        for(const auto& band:lay.child_bands) {
            D2D1_RECT_F card=band;
            for(const auto& group:lay.group) if(group.top<=band.top && group.bottom>=band.bottom && group.bottom>group.top) card=group;
            ComPtr<ID2D1Factory> factory; dc->GetFactory(&factory);
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            if(factory.get()) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card,8*scale_,8*scale_),&clip);
            if(clip.get()) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),clip.get()),nullptr);
            MakeBrush(dc,WithAlpha(theme.text,vm.dark ? 0.035f : 0.024f),brFillHover_);
            dc->FillRectangle(D2D1::RectF(band.left+1,band.top,band.right-1,band.bottom),brFillHover_.get());
            if(clip.get()) dc->PopLayer();
            MakeBrush(dc,WithAlpha(theme.text_secondary,0.32f),brTextSecondary_);
            FillRoundedRect(dc,brTextSecondary_.get(),band.left+29*scale_,band.top+12*scale_,1.5f*scale_,
                (std::max)(0.0f,band.bottom-band.top-24*scale_),0.75f*scale_);
        }
        MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
        for(const auto& d:lay.dividers) FillRect(dc,brStrokeDivider_.get(),d.left,d.top,d.right-d.left,1);
        // Rows that depend on an option that is off: shown, faded and inert.
        auto dim_begin=[&](bool dim) {
            if(dim) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),0.42f,nullptr,D2D1_LAYER_OPTIONS1_NONE),nullptr);
            return dim;
        };
        auto dim_end=[&](bool dim) { if(dim) dc->PopLayer(); };
        if(page==0) {
        const bool small_theme=lay.theme_tile[0].left<lay.theme_row.left+100*scale_;
        label(lay.theme_row,l10n::Get(I::SettingsTheme),small_theme ? L"" : l10n::Get(I::SettingsThemeDesc),L"\xE706",
            small_theme ? lay.theme_row.right-16*scale_ : lay.theme_tile[0].left-12*scale_);
        const I theme_names[]={I::SettingsThemeLight,I::SettingsThemeDark,I::SettingsThemeSystem};
        const int theme_values[]={1,2,0};
        for(int i=0;i<3;++i) {
            const auto r=lay.theme_tile[i];auto preview=r;preview.bottom-=22*scale_;
            const bool selected=vm.settings_theme==theme_values[i];
            MakeBrush(dc,i==0 ? HexColor(0xF1F3EE) : HexColor(0x1A1A1A),brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(preview,5*scale_,5*scale_),brFillHover_.get());
            MakeBrush(dc,i==0 ? HexColor(0xFFFFFF) : HexColor(0x333333),brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+8*scale_,preview.right-preview.left-14*scale_,8*scale_,2*scale_);
            FillRoundedRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+21*scale_,14*scale_,preview.bottom-preview.top-28*scale_,2*scale_);
            if(i==2) {MakeBrush(dc,HexColor(0xF1F3EE),brFillHover_); FillRect(dc,brFillHover_.get(),preview.left+7*scale_,preview.top+8*scale_,(preview.right-preview.left)/2-7*scale_,preview.bottom-preview.top-15*scale_);}
            MakeBrush(dc,selected ? theme.accent : IsHovered(vm,H::SettingsTheme,theme_values[i]) ? theme.text_secondary : theme.stroke_card,brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(preview,5*scale_,5*scale_),brStrokeCard_.get(),selected ? 2*scale_ : 1);
            if(selected) DrawIconText(preview.right-23*scale_,preview.top+3*scale_,20*scale_,20*scale_,L"\xE73E",L"",theme.accent,0.7f);
            painter_.DrawText(l10n::Get(theme_names[i]),D2D1::RectF(r.left,preview.bottom+2*scale_,r.right,r.bottom),compositor_->SmallFormat(),theme.text,fluent::HorizontalAlignment::Center);
        }
        
        label(lay.accent_card,l10n::Get(I::SettingsThemeColor),l10n::Get(I::SettingsThemeColorDesc),L"\xE790",lay.accent_picker.left-16*scale_);
        if(vm.settings_bloom) {vm.settings_bloom->SetDisk(lay.accent_picker);vm.settings_bloom->Draw(dc,theme);}
        {
            // 主题色 › 自定义: hue, saturation and lightness sliders on gradient tracks.
            const bool open=(vm.settings_expanded&32u)!=0;
            button(lay.accent_custom_button,l10n::Get(open ? I::AccentCustomDone : I::AccentCustom),H::SettingsAccentCustom,0,false,true,
                open ? L"\xE70E" : L"\xE9E9");
            const int* hsl=vm.settings_accent_hsl;
            const auto color_of=[](float h,float s,float l) { return BloomHsl(h,s/100.0f,l/100.0f); };
            const auto current=color_of(static_cast<float>(hsl[0]),static_cast<float>(hsl[1]),static_cast<float>(hsl[2]));
            if(lay.accent_custom_head.bottom>lay.accent_custom_head.top) {
                const auto& head_row=lay.accent_custom_head;
                const auto& swatch=lay.accent_custom_swatch;
                label(head_row,l10n::Get(I::AccentCustomTitle),l10n::Get(I::AccentCustomDesc),L"\xEB3C",swatch.left-12*scale_);
                const auto chip=D2D1::RectF(swatch.left,swatch.top,swatch.left+(swatch.bottom-swatch.top),swatch.bottom);
                MakeBrush(dc,current,brAccent_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(chip,6*scale_,6*scale_),brAccent_.get());
                MakeBrush(dc,WithAlpha(theme.text,0.14f),brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(chip,6*scale_,6*scale_),brStrokeCard_.get(),1);
                wchar_t hex[16]{};
                swprintf_s(hex,L"#%02X%02X%02X",static_cast<unsigned>(std::lround(current.r*255)),
                    static_cast<unsigned>(std::lround(current.g*255)),static_cast<unsigned>(std::lround(current.b*255)));
                painter_.DrawText(hex,D2D1::RectF(chip.right+10*scale_,swatch.top+6*scale_,swatch.right,swatch.bottom),compositor_->TextFormat(),theme.text);
            }
            const I names[]={I::AccentHue,I::AccentSaturation,I::AccentLightness};
            for(int i=0;i<3;++i) {
                const auto& slider_row=lay.accent_slider_row[i];
                if(slider_row.bottom<=slider_row.top) continue;
                const auto* cells=lay.accent_slider_cells[i];
                const bool stacked=cells[0].left<slider_row.left+100*scale_;
                painter_.DrawText(l10n::Get(names[i]),D2D1::RectF(slider_row.left+54*scale_,slider_row.top+(stacked ? 12.0f : 21.0f)*scale_,
                    stacked ? slider_row.right-16*scale_ : cells[0].left-12*scale_,slider_row.top+(stacked ? 36.0f : 45.0f)*scale_),compositor_->TextFormat(),theme.text);
                const auto geom=SettingsSlider(cells,scale_);
                const float cy=(cells[0].top+cells[0].bottom)*0.5f;
                const int max_value=i==0 ? 360 : 100;
                // Saturation and lightness of the hue track stay readable even for greys.
                const float hs=static_cast<float>((std::max)(hsl[1],55)), hl=static_cast<float>(std::clamp(hsl[2],35,65));
                D2D1_GRADIENT_STOP stops[7]{};
                for(int k=0;k<7;++k) {
                    const float pos=static_cast<float>(k)/6.0f;
                    stops[k].position=pos;
                    stops[k].color=i==0 ? color_of(360*pos,hs,hl)
                        : i==1 ? color_of(static_cast<float>(hsl[0]),100*pos,static_cast<float>(hsl[2]))
                               : color_of(static_cast<float>(hsl[0]),static_cast<float>(hsl[1]),100*pos);
                }
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> gradient;
                dc->CreateGradientStopCollection(stops,7,&collection);
                if(collection.get()) dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(geom.left,cy),D2D1::Point2F(geom.right,cy)),collection.get(),&gradient);
                const auto track=D2D1::RoundedRect(D2D1::RectF(geom.left-5*scale_,cy-4*scale_,geom.right+5*scale_,cy+4*scale_),4*scale_,4*scale_);
                if(gradient.get()) dc->FillRoundedRectangle(track,gradient.get());
                MakeBrush(dc,WithAlpha(theme.text,0.12f),brStrokeCard_);
                dc->DrawRoundedRectangle(track,brStrokeCard_.get(),1);
                const float thumb_x=geom.left+(geom.right-geom.left)*std::clamp(static_cast<float>(hsl[i])/static_cast<float>(max_value),0.0f,1.0f);
                MakeBrush(dc,WithAlpha(theme.surface_card,1.0f),brFillInput_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x,cy),10*scale_,10*scale_),brFillInput_.get());
                MakeBrush(dc,theme.stroke_card,brStrokeCard_);
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x,cy),10*scale_,10*scale_),brStrokeCard_.get(),1);
                MakeBrush(dc,current,brAccent_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x,cy),6.5f*scale_,6.5f*scale_),brAccent_.get());
                const std::wstring value=std::to_wstring(hsl[i])+(i==0 ? L"\x00B0" : L"%");
                painter_.DrawText(value,D2D1::RectF(geom.right+10*scale_,cy-10*scale_,cells[2].right,cy+10*scale_),
                    compositor_->TextFormat(),theme.text,fluent::HorizontalAlignment::Right);
            }
        }
        
        const I effects[]={I::EffectNone,I::EffectAcrylic,I::EffectMica,I::EffectMicaAlt};
        const I languages[]={I::LanguageSystem,I::LanguageZhCN,I::LanguageZhTW,I::LanguageEnUS};
        const auto dropdown=[&](D2D1_RECT_F r,D2D1_RECT_F c,I title,I desc,const std::wstring& value,int id) {
            label(r,l10n::Get(title),l10n::Get(desc),id==0 ? L"\xE81E" : L"\xE775",c.left<r.left+100*scale_ ? r.right-16*scale_ : c.left-12*scale_);
            fluent::ButtonSpec control{};
            control.bounds=c; control.text=value; control.drop_down=true;
            control.state.hovered=IsHovered(vm,H::SettingsDropdown,id);
            painter_.DrawButton(control);
        };
        label(lay.effect_card,l10n::Get(I::SettingsWindowEffect),l10n::Get(compat::ModernWindows() ? I::SettingsWindowEffectDesc : I::EffectUnavailable),L"\xE81E",lay.effect_card.right-16*scale_);
        for(int i=0;i<kWindowEffectCount;++i) {
            const auto r=lay.effect_row[i];
            auto preview=r;preview.bottom-=22*scale_;
            const bool enabled=i==0 || compat::ModernWindows();
            const bool selected=i==static_cast<int>(compat::ModernWindows() ? vm.window_effect : WindowEffect::None);
            const bool hovered=enabled && IsHovered(vm,H::SettingsEffect,i);
            const float tint[]={0.0f,0.22f,0.065f,0.16f};
            auto background=BlendOver(WithAlpha(theme.accent,tint[i]),theme.surface_title);
            if(!enabled) background=BlendOver(WithAlpha(theme.surface_sheet,0.6f),background);
            MakeBrush(dc,background,brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(preview,6*scale_,6*scale_),brFillHover_.get());
            // A small window silhouette explains the material, not a fake DWM surface.
            dc->PushAxisAlignedClip(preview,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if(i==1 || i==3) {
                MakeBrush(dc,WithAlpha(theme.accent,vm.dark ? 0.18f : 0.13f),brFillHover_);
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(preview.right-18*scale_,preview.top+11*scale_),
                    36*scale_,26*scale_),brFillHover_.get());
            }
            const float inset=10*scale_;
            auto window=D2D1::RectF(preview.left+inset,preview.top+9*scale_,preview.right-inset,preview.bottom-9*scale_);
            const float plate_alpha=i==1 ? 0.48f : i==3 ? 0.72f : 0.96f;
            MakeBrush(dc,WithAlpha(theme.surface_card,plate_alpha),brFillInput_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(window,3*scale_,3*scale_),brFillInput_.get());
            MakeBrush(dc,WithAlpha(theme.text_secondary,0.30f),brTextSecondary_);
            FillRoundedRect(dc,brTextSecondary_.get(),window.left+6*scale_,window.top+6*scale_,
                (std::min)(18*scale_,(window.right-window.left)*0.18f),2*scale_,scale_);
            MakeBrush(dc,WithAlpha(theme.accent,0.10f),brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),window.right-22*scale_,window.top+5*scale_,
                16*scale_,window.bottom-window.top-10*scale_,2*scale_);
            dc->PopAxisAlignedClip();
            MakeBrush(dc,selected ? theme.accent : hovered ? theme.text_secondary : theme.stroke_card,brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(preview,6*scale_,6*scale_),brStrokeCard_.get(),
                selected ? 1.5f*scale_ : scale_);
            if(selected) {
                const auto center=D2D1::Point2F(preview.right-9*scale_,preview.bottom-9*scale_);
                MakeBrush(dc,theme.accent,brAccent_);
                dc->FillEllipse(D2D1::Ellipse(center,7*scale_,7*scale_),brAccent_.get());
                DrawIconText(center.x-6*scale_,center.y-6*scale_,12*scale_,12*scale_,
                    L"\xE73E",L"",theme.accent_text,0.6f);
            }
            painter_.DrawText(l10n::Get(effects[i]),D2D1::RectF(r.left,preview.bottom+3*scale_,r.right,r.bottom),
                compositor_->SmallFormat(),!enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,
                fluent::HorizontalAlignment::Center);
        }
        
        {
            // 窗口效果 › 侧边栏与标题栏: only Acrylic / Mica have a frame to see through.
            const I frames[]={I::FrameLookSolid,I::FrameLookHalf,I::FrameLookClear};const int frame_ticks[]={0,50,100};
            const bool no_material=vm.window_effect==WindowEffect::None || !vm.backdrop_enabled;
            const bool frame_dim=dim_begin(no_material);
            slider(lay.frame_look_card,lay.frame_look_row,vm.frame_transparency,kFrameTransparencyMax,frame_ticks,frames,H::SettingsFrameLook,
                   I::SettingsFrameLook,no_material ? I::SettingsFrameLookOff : I::SettingsFrameLookDesc,L"%",L"\xE90C");
            dim_end(frame_dim);
        }
        toggle(lay.menu_acrylic_row,I::MenuAcrylic,I::MenuAcrylicDesc,L"\xE700",vm.settings_menu_acrylic,36);
        dropdown(lay.language_card,lay.language_choice,I::SettingsLanguage,I::SettingsLanguageDesc,l10n::Get(languages[vm.settings_language]),1);
        const I text_render[]={I::TextRenderAuto,I::TextRenderSharp,I::TextRenderSmooth};const int text_render_values[]={0,1,2};
        segmented(lay.text_render_card,lay.text_render_row,text_render,text_render_values,vm.settings_text_render,H::SettingsTextRender,I::SettingsTextRender,I::SettingsTextRenderDesc,3,L"\xE8D2");
        const I ui_font_size[]={I::UiFontSmall,I::UiFontDefault,I::UiFontLarge,I::UiFontLarger};const int ui_font_size_values[]={90,100,112,125};
        segmented(lay.ui_font_size_card,lay.ui_font_size_row,ui_font_size,ui_font_size_values,vm.settings_ui_font_scale,H::SettingsUiFontSize,I::SettingsUiFontSize,I::SettingsUiFontSizeDesc,4,L"\xE8E9");
        if(lay.wallpaper_card.bottom>lay.wallpaper_card.top) {
        const auto& preview = lay.wallpaper_preview;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), preview.left, preview.top,
                        preview.right - preview.left, preview.bottom - preview.top, 6.0f * scale_);
        if (!vm.background_image.empty())
            material_.DrawSourceCover(dc, preview, vm.background_image);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6.0f * scale_, 6.0f * scale_),
                                 brStrokeCard_.get(), 1.0f);
        const float text_left = preview.right + 12.0f * scale_;
        const bool compact_wallpaper = lay.wallpaper_choose.top >
            lay.wallpaper_card.top + 64.0f * scale_;
        const float text_right = compact_wallpaper
            ? lay.wallpaper_card.right - 16.0f * scale_
            : lay.wallpaper_choose.left - 12.0f * scale_;
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaper),
                     text_left, lay.wallpaper_card.top + 18.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 22.0f * scale_);
        const std::wstring wallpaper_desc = vm.background_image.empty()
            ? pulse::l10n::Get(pulse::l10n::StringId::SettingsWallpaperDesc)
            : FileNameOf(vm.background_image);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), wallpaper_desc,
                     text_left, lay.wallpaper_card.top + 42.0f * scale_,
                     (std::max)(40.0f * scale_, text_right - text_left), 18.0f * scale_);
        fluent::ControlState choose{};
        choose.hovered = IsHovered(vm, HitTestResult::SettingsWallpaper, 0);
        painter_.DrawButton({ lay.wallpaper_choose,
                              pulse::l10n::Get(pulse::l10n::StringId::ChooseImage), {},
                              fluent::ButtonKind::Standard, choose });
        fluent::ControlState clear{};
        clear.enabled = !vm.background_image.empty();
        clear.hovered = clear.enabled && IsHovered(vm, HitTestResult::SettingsWallpaper, 1);
        painter_.DrawButton({ lay.wallpaper_clear,
                              pulse::l10n::Get(pulse::l10n::StringId::Clear), {},
                              fluent::ButtonKind::Standard, clear });


        }
            const I looks[]={I::WallpaperLookSubtle,I::WallpaperLookBalanced,I::WallpaperLookVivid};const int look_ticks[]={25,50,75};
            slider(lay.wallpaper_look_card,lay.wallpaper_look_row,vm.wallpaper_look,kPanelTransparencyMax,look_ticks,looks,H::SettingsWallpaperLook,I::SettingsWallpaperLook,I::SettingsWallpaperLookDesc,L"%",L"\xE7A1");
            const I blurs[]={I::WallpaperBlurOff,I::WallpaperBlurLight,I::WallpaperBlurStrong};const int blur_ticks[]={0,14,28};
            const bool blur_dim=dim_begin(vm.background_image.empty());
            slider(lay.wallpaper_blur_card,lay.wallpaper_blur_row,vm.wallpaper_blur,kWallpaperBlurMax,blur_ticks,blurs,H::SettingsWallpaperBlur,I::SettingsWallpaperBlur,I::SettingsWallpaperBlurDesc,L" px",L"\xEB42");
            dim_end(blur_dim);
        }
        if(page==11) {
        {
            draw_card(lay.integration_card);
            const bool master_on=vm.settings_integration_enabled;
            auto hover_fill=[&](D2D1_RECT_F r) {
                MakeBrush(dc,theme.fill_hover,brFillHover_);
                FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
            };
            {
                const auto r=lay.default_manager_row;
                fluent::ControlState state{};
                state.checked=master_on;state.hovered=IsHovered(vm,H::SettingsIntegration,0);
                if(state.hovered) hover_fill(r);
                label(r,l10n::Get(I::IntegrationMaster),L"",L"\xEC50",lay.integration_text_right);
                painter_.DrawWrappedCaption(l10n::Get(I::IntegrationMasterDesc),D2D1::Point2F(r.left+54*scale_,r.top+35*scale_),
                    (std::max)(40*scale_,lay.integration_text_right-r.left-54*scale_),theme.text_secondary);
                const auto badge=MakeIntegrationBadge(vm);
                fluent::BadgeSpec pill{};
                pill.bounds=lay.integration_badge;pill.text=badge.text;pill.kind=badge.kind;
                painter_.DrawBadge(pill);
                painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
                if(lay.integration_bar.bottom<=lay.integration_bar.top) divider(r);
            }
            if(lay.integration_bar.bottom>lay.integration_bar.top) {
                const bool failed=vm.settings_integration_state==3;
                // The spec holds views: keep the strings alive until it is drawn.
                const auto title=IntegrationProblemTitle(vm);
                const auto message=IntegrationProblemMessage(vm);
                fluent::InfoBarSpec bar{};
                bar.bounds=lay.integration_bar;
                bar.title=title;
                bar.message=message;
                bar.kind=failed ? fluent::InfoBarKind::Error : fluent::InfoBarKind::Warning;
                bar.show_close=false;
                // Tinted against the card it sits in, not the page.
                const float ca=card_alpha_;
                bar.backdrop=D2D1::ColorF(theme.bg.r+(theme.surface_card.r-theme.bg.r)*ca,
                    theme.bg.g+(theme.surface_card.g-theme.bg.g)*ca,theme.bg.b+(theme.surface_card.b-theme.bg.b)*ca,1.0f);
                painter_.DrawInfoBar(bar);
                if(lay.integration_retry.bottom>lay.integration_retry.top)
                    button(lay.integration_retry,l10n::Get(IntegrationRetryLabel(vm)),H::SettingsIntegration,5,true);
                if(lay.integration_restore.bottom>lay.integration_restore.top)
                    button(lay.integration_restore,l10n::Get(I::IntegrationRestore),H::SettingsIntegration,6);
            }
            {
                const auto r=lay.integration_list_head;
                painter_.DrawText(l10n::Get(master_on ? I::IntegrationScope : I::IntegrationScopeDesc),
                    D2D1::RectF(r.left+54*scale_,r.top+8*scale_,r.right-16*scale_,r.bottom),
                    compositor_->SmallFormat(),theme.text_secondary);
            }
            // Read-only tiles: what opens in Pulse right now (registry read-back), faded while off.
            // DrawIconText's last argument is a size factor: fade with the colour, keep the size.
            const wchar_t* cover_icons[]={L"\xE8B7",L"\xE7F4",L"\xE765"};
            for(int i=0;i<3;++i) {
                const auto r=lay.integration_cover[i];
                if(r.bottom<=r.top) continue;
                const bool active=IntegrationCoverActive(vm,i);
                const float alpha=master_on ? 1.0f : 0.55f;
                MakeBrush(dc,WithAlpha(theme.fill_input,theme.fill_input.a*alpha),brFillInput_);
                MakeBrush(dc,WithAlpha(theme.stroke_card,theme.stroke_card.a*alpha),brStrokeCard_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(r,6*scale_,6*scale_),brFillInput_.get());
                dc->DrawRoundedRectangle(D2D1::RoundedRect(r,6*scale_,6*scale_),brStrokeCard_.get(),1);
                const float mid=(r.top+r.bottom)/2;
                DrawIconText(r.left+10*scale_,mid-8*scale_,16*scale_,16*scale_,cover_icons[i],L"",
                    master_on ? theme.accent : WithAlpha(theme.text_secondary,theme.text_secondary.a*0.5f),0.95f);
                const float check=active ? 22*scale_ : 0.0f;
                painter_.DrawText(l10n::Get(kIntegrationCover[i]),
                    D2D1::RectF(r.left+34*scale_,r.top,r.right-8*scale_-check,r.bottom),
                    compositor_->TextFormat(),master_on ? theme.text : theme.text_secondary);
                if(active) DrawIconText(r.right-24*scale_,mid-7*scale_,14*scale_,14*scale_,L"\xE73E",L"",theme.accent,0.95f);
            }
            {
                const auto r=lay.integration_options_head;
                painter_.DrawText(l10n::Get(I::IntegrationOptions),D2D1::RectF(r.left+54*scale_,r.top+8*scale_,r.right-16*scale_,r.bottom),
                    compositor_->SmallFormat(),theme.text_secondary);
            }
            // The two optional extras; they only mean something while the switch is on.
            auto item=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int action,float title_right) {
                fluent::ControlState state{};
                state.checked=on;state.enabled=master_on;
                state.hovered=master_on && IsHovered(vm,H::SettingsIntegration,action);
                if(state.hovered) hover_fill(D2D1::RectF(r.left+40*scale_,r.top,r.right-6*scale_,r.bottom));
                const float x=r.left+54*scale_, text_left=r.left+112*scale_;
                painter_.DrawCheckBox(D2D1::RectF(x,r.top+11*scale_,x+20*scale_,r.top+31*scale_),L"",state);
                DrawIconText(x+30*scale_,r.top+11*scale_,20*scale_,20*scale_,icon,L"",master_on ? theme.text_secondary : WithAlpha(theme.text_secondary,theme.text_secondary.a*0.5f),0.85f);
                painter_.DrawText(l10n::Get(title),D2D1::RectF(text_left,r.top+9*scale_,title_right,r.top+31*scale_),
                    compositor_->TextFormat(),master_on ? theme.text : theme.text_secondary);
                painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(text_left,r.top+31*scale_),
                    (std::max)(40*scale_,r.right-16*scale_-text_left),theme.text_secondary);
            };
            item(lay.recycle_bin_row,I::IntegrationRecycleOption,I::SettingsRecycleBinDesc,L"\xE74D",
                vm.settings_integration_recycle_bin,7,lay.recycle_bin_row.right-16*scale_);
            item(lay.explorer_windows_row,I::IntegrationExperimental,I::IntegrationExperimentalDesc,L"\xE8A7",
                vm.settings_integration_experimental,4,lay.explorer_windows_row.right-16*scale_);
            item(lay.folders_compat_row,I::IntegrationCompat,I::IntegrationCompatDesc,L"\xE90F",
                vm.settings_integration_folders_compat,9,lay.folders_compat_row.right-16*scale_);
            if(lay.integration_hint.bottom>lay.integration_hint.top) {
                const auto r=lay.integration_hint;
                divider(D2D1::RectF(r.left,r.top-1,r.right,r.top-1));
                if(lay.integration_try.bottom>lay.integration_try.top)
                    button(lay.integration_try,l10n::Get(I::IntegrationTry),H::SettingsIntegration,8,true);
                const auto t=lay.integration_hint_text;
                if(vm.settings_integration_just_enabled)
                    DrawIconText(t.left-24*scale_,t.top+1*scale_,16*scale_,16*scale_,L"\xE73E",L"",theme.accent,0.95f);
                painter_.DrawWrappedCaption(IntegrationHint(vm),D2D1::Point2F(t.left,t.top),
                    (std::max)(40*scale_,t.right-t.left),vm.settings_integration_just_enabled ? theme.text : theme.text_secondary);
            }
        }
        toggle(lay.startup_row[0],I::SettingsLaunch,I::SettingsLaunchDesc,L"\xEC57",vm.settings_launch_on_startup,1);
        {
            const bool dim=dim_begin(!vm.settings_launch_on_startup);
            toggle(lay.start_in_tray_row,I::SettingsStartInTray,I::SettingsStartInTrayDesc,L"\xE896",vm.settings_start_in_tray,27);
            dim_end(dim);
        }
        toggle(lay.startup_row[1],I::SettingsKeepRunning,I::SettingsKeepRunningDesc,L"\xE9F5",vm.settings_keep_running,2);
        const I notify_icon[]={I::NotifyIconAlways,I::NotifyIconBackground,I::NotifyIconNever};const int notify_values[]={0,1,2};
        segmented(lay.notify_icon_card,lay.notify_icon_row,notify_icon,notify_values,vm.settings_notify_icon,H::SettingsNotifyIcon,I::SettingsNotifyIcon,I::SettingsNotifyIconDesc,3,L"\xECAA");
        toggle(lay.shell_tags_row,I::SettingsShellTags,I::SettingsShellTagsDesc,L"\xE838",vm.settings_shell_tags,21);
        }
        if(page==10) {
        {
            // Default location: buttons sit beside the text, or below it when narrow.
            const bool below=lay.home_folder_choose.top>lay.home_folder_card.top+40*scale_;
            label(lay.home_folder_card,l10n::Get(I::SettingsHomeFolder),
                vm.settings_home_folder.empty() ? l10n::Get(I::ThisPc) : vm.settings_home_folder,L"\xE80F",
                below ? lay.home_folder_card.right-16*scale_ : lay.home_folder_choose.left-12*scale_);
            button(lay.home_folder_choose,l10n::Get(I::SettingsHomeFolderPick),H::SettingsHomeFolder,0);
            button(lay.home_folder_reset,l10n::Get(I::ThisPc),H::SettingsHomeFolder,1,false,!vm.settings_home_folder.empty());
            
        }
        const I startup_open[]={I::StartupOpenLastTabs,I::OpenDefaultLocation};const int two_values[]={0,1};
        segmented(lay.startup_open_card,lay.startup_open_row,startup_open,two_values,vm.settings_startup_open,H::SettingsStartupOpen,I::SettingsStartupOpen,I::SettingsStartupOpenDesc,2,L"\xE81C");
        const I new_tab_open[]={I::NewTabOpenCurrent,I::OpenDefaultLocation};
        segmented(lay.new_tab_open_card,lay.new_tab_open_row,new_tab_open,two_values,vm.settings_new_tab_open,H::SettingsNewTabOpen,I::SettingsNewTabOpen,I::SettingsNewTabOpenDesc,2,L"\xECCD");
        const I external_open[]={I::ExternalOpenSmart,I::ExternalOpenTab,I::ExternalOpenWindow};const int external_values[]={0,1,2};
        segmented(lay.external_open_card,lay.external_open_row,external_open,external_values,vm.settings_external_open,H::SettingsExternalOpen,I::SettingsExternalOpen,I::SettingsExternalOpenDesc,3,L"\xE8AD");
        toggle(lay.win_e_activate_row,I::SettingsWinEActivate,I::SettingsWinEActivateDesc,L"\xE7C4",vm.settings_win_e_activate,47);
        toggle(lay.vertical_tabs_row,I::SettingsVerticalTabs,I::SettingsVerticalTabsDesc,L"\xE8A0",vm.settings_vertical_tabs,23);
        toggle(lay.pinned_names_row,I::PinnedNames,I::PinnedNamesDesc,L"\xE718",vm.show_pinned_tab_names,6);
        toggle(lay.close_tab_double_click_row,I::SettingsCloseTabDoubleClick,I::SettingsCloseTabDoubleClickDesc,L"\xEA39",vm.settings_close_tab_double_click,35);
        toggle(lay.close_last_tab_row,I::SettingsCloseLastTab,I::SettingsCloseLastTabDesc,L"\xE89F",vm.settings_close_last_tab,26);
        }
        if(page==9) {
        const I density[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy};const int heights[]={28,34,40};
        segmented(lay.density_card,lay.density_row,density,heights,vm.settings_row_height,H::SettingsDensity,I::SettingsRowHeight,I::SettingsRowHeightDesc,3,L"\xECE7");
        {
            // 鼠标与选择: the name band and hover selection only mean something with single-click.
            const I open_modes[]={I::ClickOpenDouble,I::ClickOpenSingle};const int open_values[]={0,1};
            segmented(lay.click_open_card,lay.click_open_row,open_modes,open_values,vm.settings_single_click ? 1 : 0,H::SettingsClickMode,I::SettingsClickOpen,I::SettingsClickOpenDesc,2,L"\xE8B0");
            if(vm.settings_single_click) {
                const I bands[]={I::NameHighlightHover,I::NameHighlightAlways};const int band_values[]={0,1};
                segmented(lay.name_highlight_card,lay.name_highlight_row,bands,band_values,vm.settings_name_highlight,H::SettingsNameBand,I::SettingsNameHighlight,I::SettingsNameHighlightDesc,2,L"\xE7E6");
                toggle(lay.hover_select_row,I::SettingsHoverSelect,I::SettingsHoverSelectDesc,L"\xE815",vm.settings_hover_select,77);
            }
            toggle(lay.item_checkbox_row,I::SettingsItemCheckboxes,I::SettingsItemCheckboxesDesc,L"\xE73A",vm.settings_item_checkboxes,78);
            const I blank_click[]={I::SettingsBlankClickOff,I::Back,I::Up};const int blank_click_values[]={0,1,2};
            segmented(lay.blank_click_row,lay.blank_click_choice,blank_click,blank_click_values,vm.settings_blank_click_action,H::SettingsBlankClick,I::SettingsBlankClickBack,I::SettingsBlankClickBackDesc,3,L"\xE72B");
        }
        toggle(lay.hidden_files_row,I::SettingsShowHidden,I::SettingsShowHiddenDesc,L"\xED1A",vm.settings_show_hidden_files,5);
        // Hidden + system entries: File Explorer keeps these behind a second option.
        toggle(lay.protected_files_row,I::SettingsShowProtected,I::SettingsShowProtectedDesc,L"\xEA18",vm.settings_show_protected_os_files,16);
        {
            const I folder_sorts[]={I::FolderSortTop,I::FolderSortFollow,I::FolderSortMixed};const int folder_sort_values[]={0,1,2};
            segmented(lay.folder_sort_card,lay.folder_sort_row,folder_sorts,folder_sort_values,vm.settings_folder_sort,H::SettingsFolderSort,I::SettingsFolderSort,I::SettingsFolderSortDesc,3,L"\xE8CB");
        }
        toggle(lay.list_style_row[0],I::ListSmartDate,I::ListSmartDateDesc,L"\xE787",vm.settings_list_smart_date,17);
        toggle(lay.list_style_row[1],I::ListZebraRows,I::ListZebraRowsDesc,L"\xE7BC",vm.settings_list_zebra_rows,18);
        toggle(lay.list_style_row[2],I::ListSizeBar,I::ListSizeBarDesc,L"\xE9D2",vm.settings_list_size_bar,19);
        toggle(lay.list_style_row[3],I::ListTagNameColor,I::ListTagNameColorDesc,L"\xE8EC",vm.settings_list_tag_names,22);
        toggle(lay.list_style_row[4],I::ListSelectionOutline,I::ListSelectionOutlineDesc,L"\xE8B3",vm.settings_list_selection_outline,33);
        toggle(lay.list_style_row[5],I::ListThumbnailBadges,I::ListThumbnailBadgesDesc,L"\xE8B9",vm.settings_list_thumbnail_badges,34);
        if(lay.thumb_cache_row.bottom>lay.thumb_cache_row.top) {
            // 缩略图缓存: usage bar against the 512 MB budget, then 清除缓存.
            const auto& r=lay.thumb_cache_row;
            const auto& b=lay.thumb_cache_button;
            const uint8_t phase=vm.settings_thumb_cache_phase;   // ThumbCacheState::Phase
            const bool unavailable=phase==4;
            const float text_right=unavailable ? r.right-16*scale_ : b.left-16*scale_;
            const bool two_lines=r.bottom-r.top>96*scale_;
            label(r,l10n::Get(I::ThumbCache),l10n::Get(I::ThumbCacheDesc),L"\xEDA2",text_right,two_lines ? 43.0f : 21.0f);
            const float line_top=r.bottom-30*scale_, x=r.left+54*scale_;
            std::wstring usage;
            wchar_t buffer[160]{};
            const uint64_t budget=512ull<<20;
            if(unavailable) usage=l10n::Get(I::ThumbCacheUnavailable);
            else if(phase==0) usage=l10n::Get(I::ThumbCacheCounting);
            else if(phase==2) usage=l10n::Get(I::ThumbCacheClearing);
            else if(phase==3) {
                swprintf_s(buffer,l10n::Get(I::ThumbCacheCleared).c_str(),pulse::format::ByteSize(vm.settings_thumb_cache_cleared).c_str());
                usage=buffer;
            } else if(vm.settings_thumb_cache_bytes==0) usage=l10n::Get(I::ThumbCacheEmpty);
            else {
                swprintf_s(buffer,l10n::Get(I::ThumbCacheUsage).c_str(),pulse::format::ByteSize(vm.settings_thumb_cache_bytes).c_str());
                usage=buffer;
            }
            float usage_left=x;
            if(!unavailable) {
                const float bar_w=(std::min)(200*scale_,(std::max)(0.0f,(text_right-x)*0.4f));
                const float bar_h=4*scale_, bar_y=line_top+9*scale_-bar_h/2;
                MakeBrush(dc,WithAlpha(theme.text_secondary,theme.text_secondary.a*0.22f),brTagDot_);
                FillRoundedRect(dc,brTagDot_.get(),x,bar_y,bar_w,bar_h,bar_h/2);
                const uint64_t used=phase==1 ? vm.settings_thumb_cache_bytes : 0;
                if(used) {
                    // At least a dot, so a few KB still shows something.
                    const float fill=(std::max)(bar_h,bar_w*static_cast<float>((std::min)(1.0,static_cast<double>(used)/budget)));
                    MakeBrush(dc,theme.accent,brTagDot_);
                    FillRoundedRect(dc,brTagDot_.get(),x,bar_y,fill,bar_h,bar_h/2);
                }
                usage_left=x+bar_w+12*scale_;
            }
            painter_.DrawText(usage,D2D1::RectF(usage_left,line_top,text_right,line_top+18*scale_),
                compositor_->SmallFormat(),phase==3 ? theme.text : theme.text_secondary);
            if(!unavailable) {
                const I caption=phase==2 ? I::ThumbCacheClearing : phase==3 ? I::ThumbCacheDone : I::ThumbCacheClear;
                button(b,l10n::Get(caption),H::SettingsThumbCache,0,false,phase==1 && vm.settings_thumb_cache_bytes>0);
            }
        }
        toggle(lay.alias_mark_row,I::AliasMark,I::AliasMarkDesc,L"\xE8AC",vm.settings_alias_mark,39);
        toggle(lay.tree_sticky_row,I::SettingsTreeSticky,I::SettingsTreeStickyDesc,L"\xE840",vm.settings_list_tree_sticky,79);
        if(lay.mtime_style_card.bottom>lay.mtime_style_card.top) {
            // 修改时间提示: style, then range / colors / switches while it is on.
            const auto& mt=vm.settings_mtime;
            const I mt_styles[]={I::MtimeStyleOff,I::MtimeStyleDot,I::MtimeStyleBar,I::MtimeStyleText};const int mt_style_values[]={0,1,2,3};
            segmented(lay.mtime_style_card,lay.mtime_style_row,mt_styles,mt_style_values,mt.style,H::SettingsMtimeStyle,I::ListMtimeHint,I::ListMtimeHintDesc,4,L"\xE916");
            if(mt.style!=0) {
                const I mt_ranges[]={I::MtimeRangeToday,I::MtimeRange3,I::MtimeRange7,I::MtimeRange30};const int mt_range_values[]={1,3,7,30};
                segmented(lay.mtime_range_card,lay.mtime_range_row,mt_ranges,mt_range_values,mt.range_days,H::SettingsMtimeRange,I::ListMtimeRange,I::ListMtimeRangeDesc,4,L"\xEC92");
                const I mt_palettes[]={I::MtimePaletteAccent,I::MtimePaletteHeat,I::MtimePaletteTeal,I::MtimePaletteAmber,I::MtimePaletteAurora,I::MtimePaletteGraphite};
                const int mt_palette_values[]={0,1,2,3,4,5};
                segmented(lay.mtime_palette_card,lay.mtime_palette_row,mt_palettes,mt_palette_values,mt.palette,H::SettingsMtimePalette,I::ListMtimePalette,I::ListMtimePaletteDesc,6,L"\xE9CA");
                {
                    // Live swatch after the title: newest to oldest in the chosen colors.
                    const auto& card=lay.mtime_palette_card;
                    const float title_w=typography::MeasureLine(compositor_,compositor_->TextFormat(),l10n::Get(I::ListMtimePalette));
                    const float mark=7.0f*scale_, step=13.0f*scale_;
                    float mx=card.left+54*scale_+title_w+12*scale_+mark/2;
                    const float my=card.top+22*scale_;
                    const float limit=lay.mtime_palette_row[0].left<card.left+100*scale_ ? card.right-16*scale_ : lay.mtime_palette_row[0].left-12*scale_;
                    for(int b=0;b<kMtimeBucketNone && mx+mark/2<limit;++b,mx+=step) {
                        const auto swatch=MtimeColor(mt.palette,b,theme.bg.r<0.5f,theme.accent,IsHighContrast());
                        MakeBrush(dc,swatch,brTagDot_);
                        if(mt.style==1) DrawMtimeMark(dc,brTagDot_.get(),mx,my,mark,b,swatch);
                        else {
                            FillRoundedRect(dc,brTagDot_.get(),mx-mark/2,my-1.5f*scale_,mark+3*scale_,3*scale_,1.5f*scale_);
                        }
                    }
                }
                
                toggle(lay.mtime_toggle_row[0],I::ListMtimeFade,I::ListMtimeFadeDesc,L"\xE793",mt.fade,70);
                toggle(lay.mtime_toggle_row[1],I::ListMtimeBold,I::ListMtimeBoldDesc,L"\xE8DD",mt.bold,71);
                if(vm.settings_change_tracking) toggle(lay.mtime_toggle_row[2],I::ListMtimeTree,I::ListMtimeTreeDesc,L"\xE8B7",mt.tree,72);
                else {
                    // Folders follow change tracking: shown off and disabled until it is on.
                    const auto& r=lay.mtime_toggle_row[2];
                    label(r,l10n::Get(I::ListMtimeTree),l10n::Get(I::ListMtimeTreeNeeds),L"\xE8B7");
                    fluent::ControlState state{}; state.checked=false; state.enabled=false;
                    painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
                }
                
                toggle(lay.mtime_toggle_row[3],I::ListMtimeOtherViews,I::ListMtimeOtherViewsDesc,L"\xE8A9",mt.other_views,73);
                toggle(lay.mtime_toggle_row[4],I::ListMtimeFlash,I::ListMtimeFlashDesc,L"\xE945",mt.flash,74);
            }
            toggle(lay.sync_row[0],I::SyncOneDrive,I::SyncOneDriveDesc,L"\xE753",vm.settings_sync_onedrive,75);
            toggle(lay.sync_row[1],I::SyncSvn,I::SyncSvnDesc,L"\xE8AB",vm.settings_sync_svn,76);
        }
        }
        if(page==12) {
        toggle(lay.confirm_delete_row,I::SettingsConfirmDelete,I::SettingsConfirmDeleteDesc,L"\xE897",vm.settings_confirm_delete,32);
        {
            const I sizes[]={I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};const int icons[]={40,48,56};
            segmented(lay.tray_icon_card,lay.tray_icon_row,sizes,icons,vm.settings_tray_icon,H::SettingsTrayIcon,I::SettingsTrayIcon,I::SettingsTrayIconDesc,3,L"\xED1E");
        }
        toggle(lay.tray_hide_row,I::TrayHideWhenEmpty,I::TrayHideWhenEmptyDesc,L"\xE8F6",vm.settings_tray_hide_empty,37);
        toggle(lay.change_tracking_row,I::SettingsChangeTracking,I::SettingsChangeTrackingDesc,L"\xE823",vm.settings_change_tracking,8);
        {
            const I days[]={I::ChangeToday,I::ChangeLast3Days,I::ChangeLast7Days};const int day_values[]={1,3,7};
            const bool dim=dim_begin(!vm.settings_change_tracking);
            segmented(lay.change_days_row,lay.change_days,days,day_values,vm.settings_change_days,H::SettingsChangeDays,I::SettingsChangeDays,I::SettingsChangeDaysDesc,3,L"\xE9D5");
            dim_end(dim);
        }
        // 网络位置 (pulse_network_design.html §06).
        toggle(lay.lan_discovery_row,I::SettingsLanDiscovery,I::SettingsLanDiscoveryDesc,L"\xE701",vm.settings_lan_discovery,40);
        {
            const I modes[]={I::LanCache10Min,I::LanCache1Hour,I::LanCacheSession};const int mode_values[]={0,1,2};
            const bool dim=dim_begin(!vm.settings_lan_discovery);
            segmented(lay.lan_cache_card,lay.lan_cache_row,modes,mode_values,vm.settings_lan_cache_mode,H::SettingsLanCache,I::SettingsLanCache,I::SettingsLanCacheDesc,3,L"\xE916");
            dim_end(dim);
        }
        toggle(lay.network_recent_row,I::SettingsNetworkRecent,I::SettingsNetworkRecentDesc,L"\xE81C",vm.settings_network_recent,41);
        toggle(lay.hidden_shares_row,I::NetworkShowHiddenShares,I::SettingsHiddenSharesDesc,L"\xED1A",vm.settings_hidden_shares,42);
        label(lay.network_clear_row,l10n::Get(I::SettingsClearNetwork),l10n::Get(I::SettingsClearNetworkDesc),L"\xE74D",lay.network_clear_button.left-8*scale_);
        button(lay.network_clear_button,l10n::Get(I::Clear),H::SettingsToggle,46);
        }
        text(l10n::Get(I::SettingsImmediate),lay.footer,true);
    } else if(!vm.settings_search_enabled) {
        text(l10n::Get(I::Search),lay.search_master_section);
        draw_card(lay.search_master_row);
        toggle(lay.search_master_row,I::SearchSwitch,I::SearchSwitchDesc,L"\xE71E",false,38);
    } else {
        text(l10n::Get(I::Search),lay.search_master_section);
        draw_card(lay.search_master_row);
        toggle(lay.search_master_row,I::SearchSwitch,I::SearchSwitchDesc,L"\xE71E",true,38);
        section(0,I::SearchModeName);section(1,I::SearchModeContent);
        draw_card(lay.group[0]);draw_card(lay.group[1]);
        toggle(lay.global_search_row,I::GlobalSearch,I::GlobalSearchDesc,L"\xE721",vm.settings_global_search_enabled,15);divider(lay.global_search_row);
        label(lay.global_search_hotkey_row,l10n::Get(I::GlobalSearchHotkey),
            vm.settings_global_search_error.empty() ? l10n::Get(I::GlobalSearchHotkeyDesc) : vm.settings_global_search_error,
            L"\xE765",lay.global_search_hotkey_row.right-16*scale_);
        button(lay.global_search_hotkey_button,vm.settings_global_search_capturing ? l10n::Get(I::GlobalSearchRecording) : vm.settings_global_search_hotkey,H::SettingsGlobalSearchHotkey,0);
        divider(lay.global_search_hotkey_row);
        {
            // Faded and inert while what they depend on is off.
            auto dim_begin=[&](bool dim) {
                if(dim) dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(),0.42f,nullptr,D2D1_LAYER_OPTIONS1_NONE),nullptr);
                return dim;
            };
            auto dim_end=[&](bool dim) { if(dim) dc->PopLayer(); };
            const bool dim=dim_begin(!vm.settings_global_search_enabled);
            toggle(lay.global_search_double_ctrl_row,I::GlobalSearchDoubleCtrl,I::GlobalSearchDoubleCtrlDesc,L"\xE945",vm.settings_global_search_double_ctrl,40);
            divider(lay.global_search_double_ctrl_row);
            dim_end(dim);
            const bool dim_gap=dim_begin(!vm.settings_global_search_enabled || !vm.settings_global_search_double_ctrl);
            const I gaps[]={I::DoubleCtrlFast,I::DoubleCtrlNormal,I::DoubleCtrlSlow};const int gap_values[]={200,300,400};
            segmented(lay.global_search_gap_card,lay.global_search_gap,gaps,gap_values,vm.settings_global_search_double_ctrl_ms,H::SettingsDoubleCtrlGap,
                I::GlobalSearchDoubleCtrlGap,I::GlobalSearchDoubleCtrlGapDesc,3,L"\xE916");
            divider(lay.global_search_gap_card);
            dim_end(dim_gap);
            const bool dim_apps=dim_begin(!vm.settings_global_search_enabled);
            toggle(lay.global_search_apps_row,I::GlobalSearchApps,I::GlobalSearchAppsDesc,L"\xE71D",vm.settings_global_search_apps,43);
            divider(lay.global_search_apps_row);
            dim_end(dim_apps);
            toggle(lay.dialog_jump_row,I::DialogJumpBar,I::DialogJumpBarDesc,L"\xE8E5",vm.settings_dialog_jump_bar,41);divider(lay.dialog_jump_row);
            const bool dim_auto=dim_begin(!vm.settings_dialog_jump_bar);
            toggle(lay.dialog_jump_auto_row,I::DialogJumpAuto,I::DialogJumpAutoDesc,L"\xE72A",vm.settings_dialog_jump_auto,42);
            divider(lay.dialog_jump_auto_row);
            dim_end(dim_auto);
        }
        toggle(lay.search_pinyin_row,I::SearchPinyin,I::SearchPinyinDesc,L"\xE721",vm.settings_search_pinyin,9);divider(lay.search_pinyin_row);
        toggle(lay.jump_history_row,I::JumpHistory,I::JumpHistoryDesc,L"\xE823",vm.settings_jump_history,44,lay.jump_history_clear.left-12*scale_);
        button(lay.jump_history_clear,l10n::Get(I::JumpHistoryClear),H::SettingsToggle,45,false,!vm.settings_jump_history_empty);
        divider(lay.jump_history_row);
        label(lay.filename_status,l10n::Get(I::SettingsFilenameIndex),vm.settings_index_status,L"\xE8A5",lay.filename_status.right-16*scale_);divider(lay.filename_status);
        disclosure(lay.disclosure[1],I::SettingsMaintenance,I::SettingsMaintenanceDesc,L"\xE713",1,false);
        label(lay.content_header,l10n::Get(I::SettingsContentFolders),vm.settings_content_summary,L"\xE8B7",
            lay.content_header.right-16*scale_);
        divider(lay.content_header);
        label(lay.content_types,l10n::Get(I::SettingsContentTypes),l10n::Get(I::SettingsContentTypesDesc),L"\xE8A5",
            lay.content_types.right-16*scale_,(lay.content_types.bottom-lay.content_types.top)/scale_-45);
        divider(lay.content_types);
        if(vm.settings_content_folders.empty()) {
            label(lay.content_empty,l10n::Get(I::ContentIndexEmpty),l10n::Get(I::SettingsContentEmptyHelp),L"\xE8B7",lay.content_empty.right-16*scale_);
        }
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) button(lay.content_pause,l10n::Get(vm.settings_content_paused ? I::ContentIndexResume : I::ContentIndexPause),H::SettingsContentAction,1);
        label(lay.content_options,l10n::Get(I::SettingsContentOptions),l10n::Get(I::SettingsContentOptionsDesc),L"\xE8A5");
        DrawIconText(lay.content_options.right-38*scale_,lay.content_options.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        text(l10n::Get(vm.settings_content_instant ? I::ContentInstantReady : I::SettingsContentHelp),lay.footer,true);
        if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) {
        divider(lay.content_options);label(lay.content_rebuild,l10n::Get(I::ContentIndexRebuild),l10n::Get(I::SettingsContentRebuildHelp),L"\xE72C");
        DrawIconText(lay.content_rebuild.right-38*scale_,lay.content_rebuild.top+22*scale_,18*scale_,18*scale_,L"\xE76C",L"",theme.text_secondary,0.75f);
        }
    }
}
} // namespace pulse::ui
