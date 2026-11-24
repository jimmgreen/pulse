#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "tab_shape.h"
#include "window_helpers.h"
#include "ai_panel_icons.h"
#include "bloom_accent_picker.h"
#include "layout_panel.h"
#include "ui_layout_icons.h"
#include "../common/localization.h"
#include <algorithm>
#include "../common/text_format.h"
#include <cmath>

namespace pulse::ui {
void MainRenderer::DrawSettingsSegmentPlate(int key, int selected, D2D1_RECT_F bounds, bool enabled) {
    auto& transition = settings_segment_motion_[key];
    if (!enabled || !motion::SystemAnimationsEnabled()) transition = {};
    fluent::SegmentedItemSpec plate{};
    plate.bounds = transition.Update(static_cast<uint64_t>(key), selected, bounds,
        motion_frame_, motion_now_, 200);
    plate.shared_track = true;
    plate.state.checked = true;
    plate.state.enabled = enabled;
    painter_.DrawSegmentedItem(plate);
    if (transition.Active(motion_now_)) motion_frame_wanted_ = true;
}

void MainRenderer::DrawUilIcon(wchar_t code, D2D1_RECT_F box, D2D1_COLOR_F color, D2D1_COLOR_F accent) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (code >= 0x100) {
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, color, brIconTone_);
        command_icons::Draw(dc, brIconTone_.get(), paneHeaderStroke_.get(), static_cast<command_icons::Icon>(code - 0x100), box);
        return;
    }
    DrawUiLayoutIcon(dc, static_cast<UilIcon>(code), box, color, accent);
}

namespace {
// 界面布局 group tiles: the mockup's icon and tone per group (--accent, --t-view ...).
bool UilGroupLook(l10n::StringId title, bool dark, D2D1_COLOR_F accent, D2D1_COLOR_F& hue, wchar_t& code) {
    using I = l10n::StringId;
    const auto hex = [&](uint32_t light, uint32_t night) { return HexColor(dark ? night : light); };
    switch (title) {
    case I::UilSecStart: hue = accent; code = static_cast<wchar_t>(UilIcon::Zen); return true;
    case I::UilSecCompare: hue = hex(0x6A4FD6, 0xAE9BFF); code = static_cast<wchar_t>(0x100 + static_cast<int>(command_icons::Icon::Compare)); return true;
    case I::UilSecWindow: hue = hex(0x6A4FD6, 0xAE9BFF); code = static_cast<wchar_t>(0x100 + static_cast<int>(command_icons::Icon::WindowParts)); return true;
    case I::UilSecList: hue = hex(0x18874F, 0x5ED29B); code = static_cast<wchar_t>(0x100 + static_cast<int>(command_icons::Icon::FileList)); return true;
    case I::UilSecToolbar: hue = hex(0xC2560F, 0xFF9B5E); code = static_cast<wchar_t>(UilIcon::Cmdbar); return true;
    case I::UilSecTitle: hue = hex(0xC7860A, 0xF3B744); code = static_cast<wchar_t>(0x100 + static_cast<int>(command_icons::Icon::TitleButtons)); return true;
    case I::UilSecProfile: hue = hex(0xC7860A, 0xF3B744); code = static_cast<wchar_t>(0x100 + static_cast<int>(command_icons::Icon::User)); return true;
    default: return false;
    }
}

// 标题栏按钮 drag: the held row floats under the pointer (kept inside the
// list) and lands on the nearest row; the rows between slide over.
struct UilTitleDrag { bool active=false; int held=-1; int slot=-1; std::array<float,12> dy{}; };
UilTitleDrag LayoutUilTitleDrag(const WindowViewModel& vm,const SettingsLayout& lay) {
    UilTitleDrag d;
    const int n=(std::min)(lay.uil_tb_count,12), h=vm.settings_uil_title_drag;
    if(h<0 || h>=n || n<2) return d;
    const float lo=lay.uil_tb_row[0].top, hi=lay.uil_tb_row[n-1].top;
    const float top=std::clamp(vm.settings_uil_title_drag_pos-vm.settings_uil_title_drag_grab,lo,(std::max)(lo,hi));
    float best=1e30f;
    for(int t=0;t<n;++t) {
        const float gap=std::fabs(lay.uil_tb_row[t].top-top);
        if(gap<best) {best=gap;d.slot=t;}
    }
    for(int i=0;i<n;++i) {
        if(i==h) {d.dy[static_cast<size_t>(i)]=top-lay.uil_tb_row[i].top;continue;}
        int v=i;
        if(h<d.slot && i>h && i<=d.slot) v=i-1;
        else if(h>d.slot && i>=d.slot && i<h) v=i+1;
        d.dy[static_cast<size_t>(i)]=lay.uil_tb_row[v].top-lay.uil_tb_row[i].top;
    }
    d.active=true;d.held=h;
    return d;
}
} // namespace

float MainRenderer::SettingsTitleRowTop(const WindowViewModel& vm,const D2D1_RECT_F& rect,int row) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    return row>=0 && row<l.uil_tb_count ? l.uil_tb_row[row].top : NAN;
}

int MainRenderer::SettingsTitleDropSlot(const WindowViewModel& vm,const D2D1_RECT_F& rect) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    const auto d=LayoutUilTitleDrag(vm,l);
    return d.active ? d.slot : -1;
}

void MainRenderer::DrawSettingsCore(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc=compositor_->Dc();
    const auto lay=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    auto text=[&](const std::wstring& value,D2D1_RECT_F r,bool is_small=false) {
        painter_.DrawText(value,r,is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),is_small ? theme.text_secondary : theme.text);
    };
    // General pages: a soft card that lets the window tint (and Mica/acrylic) through
    // instead of a flat white slab (chrome_surfaces_mockup.html, E); dark is soft already.
    const float card_soft=IsGeneralSettingsPage(vm.settings_page) && !vm.dark && !IsHighContrast() ? 0.55f : 1.0f;
    auto draw_card=[&](D2D1_RECT_F r) {
        if(r.bottom<=r.top) return;
        MakeBrush(dc,WithAlpha(theme.surface_card,card_alpha_*card_soft),brFillInput_); MakeBrush(dc,theme.stroke_card,brStrokeCard_);
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
        if(icon && icon[0]==L'\x1')
            DrawUilIcon(icon[1],D2D1::RectF(r.left+19*scale_,r.top+20*scale_,r.left+37*scale_,r.top+38*scale_),theme.text_secondary,theme.text_secondary);
        else if(vm.settings_page==11)
            DrawIconText(r.left+20*scale_,r.top+13*scale_,18*scale_,18*scale_,icon,L"",theme.text_secondary,1.0f);
        else
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
        if(hit==41 || hit==42 || hit==45) {
            const float width=(std::max)(40*scale_,r.right-r.left-126*scale_);
            const float title_h=painter_.MeasureWrappedBodyHeight(l10n::Get(title),width);
            painter_.DrawGlyph(icon,D2D1::RectF(r.left+18*scale_,r.top+19*scale_,r.left+36*scale_,r.top+37*scale_),theme.text_secondary);
            painter_.DrawWrappedBody(l10n::Get(title),D2D1::Point2F(r.left+54*scale_,r.top+12*scale_),width,theme.text);
            painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(r.left+54*scale_,r.top+18*scale_+title_h),width,theme.text_secondary);
        } else label(r,l10n::Get(title),l10n::Get(desc),icon,text_right,hit==15 ? 43.0f : 21.0f);
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
        label(card,l10n::Get(title),title==desc ? L"" : l10n::Get(desc),icon,stacked ? card.right-16*scale_ : choices[0].left-12*scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[count-1].right,choices[count-1].bottom));
        for (int i = 0; i < count; ++i) {
            if (current != values[i]) continue;
            DrawSettingsSegmentPlate(static_cast<int>(hit), current, choices[i]);
        }
        for(int i=0;i<count;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(labels[i]);
            item.suppress_plate = current == values[i];
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
        // Side column hues, one per section in page order; lifted on dark so the glyphs stay readable.
        const D2D1_COLOR_F hues[]={theme.accent,HexColor(0x2E8B7A),HexColor(0xB5700A),HexColor(0x7B5CC4),HexColor(0xC2476B)};
        for(int i=0;i<lay.section_count;++i) {
            const auto& r=lay.section[i];
            const bool side=lay.section_side && lay.section_name[i].right>lay.section_name[i].left;
            const auto name=side ? lay.section_name[i] : D2D1::RectF(r.left,r.top,r.right,r.bottom);
            if(side) {
                if(lay.section_rule[i]>0.0f) {
                    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
                    FillRect(dc,brStrokeDivider_.get(),lay.content.left+20*scale_,lay.section_rule[i],lay.content.right-lay.content.left-40*scale_,1);
                }
                auto hue=IsHighContrast() ? theme.text : vm.dark ? MixColor(hues[i%5],HexColor(0xFFFFFF),0.3f) : hues[i%5];
                wchar_t uil_code=0;
                D2D1_COLOR_F uil_hue{};
                const bool uil=page==kSettingsPageUiLayout &&
                    UilGroupLook(lay.section_title[i],vm.dark,vm.dark ? MixColor(theme.accent,HexColor(0xFFFFFF),0.3f) : theme.accent,uil_hue,uil_code);
                if(uil && !IsHighContrast()) hue=uil_hue;
                const auto tile=lay.section_tile[i];
                painter_.FillRoundedRect(tile,8*scale_,WithAlpha(hue,vm.dark ? 0.16f : 0.12f));
                painter_.StrokeRoundedRect(tile,8*scale_,WithAlpha(hue,0.18f));
                if(uil) DrawUilIcon(uil_code,D2D1::RectF(tile.left+7*scale_,tile.top+7*scale_,tile.right-7*scale_,tile.bottom-7*scale_),hue,hue);
                else DrawStoreIcon(dc,SectionInfo(lay.section_title[i]).icon,
                    D2D1::RectF(tile.left+8*scale_,tile.top+8*scale_,tile.right-8*scale_,tile.bottom-8*scale_),hue);
                if(uil && lay.section_hint[i].bottom>lay.section_hint[i].top) {
                    // 起点: the three switching keys; 标题栏按钮: each starting point's default buttons.
                    const auto h=lay.section_hint[i];
                    if(lay.section_title[i]==I::UilSecStart) {
                        float kx=h.left, ky=h.top;
                        for(const wchar_t* key:{L"Alt+Shift+1",L"Alt+Shift+2",L"Alt+Shift+3"}) {
                            const float kw=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->SmallFormat(),key)+12*scale_;
                            if(kx>h.left && kx+kw>h.right) { kx=h.left; ky+=24*scale_; }
                            const auto k=D2D1::RectF(kx,ky,kx+kw,ky+20*scale_);
                            painter_.FillRoundedRect(k,4*scale_,theme.fill_input);
                            painter_.StrokeRoundedRect(k,4*scale_,theme.stroke_card);
                            painter_.DrawText(key,k,compositor_->SmallFormat(),theme.text_secondary,fluent::HorizontalAlignment::Center);
                            kx+=kw+4*scale_;
                        }
                        painter_.DrawText(l10n::Get(I::UilHintStartKeys),D2D1::RectF(h.left,ky+26*scale_,h.right,ky+46*scale_),compositor_->SmallFormat(),theme.text_secondary);
                    } else {
                        float ly=h.top;
                        for(const I id:{I::UilHintTbZen,I::UilHintTbStandard,I::UilHintTbPro}) {
                            const auto line=l10n::Get(id);
                            painter_.DrawWrappedCaption(line,D2D1::Point2F(h.left,ly),h.right-h.left,theme.text_secondary);
                            ly+=painter_.MeasureWrappedCaptionHeight(line,h.right-h.left)+4*scale_;
                        }
                    }
                }
                painter_.DrawText(l10n::Get(lay.section_title[i]),name,compositor_->HeaderFormat(),theme.text);
                const auto desc=lay.section_desc[i];
                painter_.DrawWrappedCaption(l10n::Get(SectionInfo(lay.section_title[i]).desc),D2D1::Point2F(desc.left,desc.top),
                    desc.right-desc.left,theme.text_secondary);
            } else {
                painter_.DrawText(l10n::Get(lay.section_title[i]),D2D1::RectF(r.left+2*scale_,r.top+6*scale_,r.right,r.bottom),
                    compositor_->HeaderFormat(),theme.text);
            }
            if(lay.section_title[i]==I::StartupDialogsTitle) {
                const float title_width=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->HeaderFormat(),l10n::Get(I::StartupDialogsTitle));
                const auto experimental=l10n::Get(I::IntegrationExperimentalTag);
                const float badge_width=painter_.MeasureBadgeWidth(experimental);
                fluent::BadgeSpec badge{};badge.text=experimental;badge.kind=fluent::BadgeKind::Warning;
                // Beside the title; in the side column, under it when the title fills the line.
                const float bx=side && title_width+12*scale_+badge_width>name.right-name.left ? name.left : (side ? name.left : r.left)+title_width+12*scale_;
                const float by=side ? (bx==name.left ? name.bottom+2*scale_ : name.top+1*scale_) : r.top+7*scale_;
                badge.bounds=D2D1::RectF(bx,by,bx+badge_width,by+22*scale_);
                painter_.DrawBadge(badge);
            }
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
        label(lay.effect_card,l10n::Get(I::SettingsWindowEffect),l10n::Get(WindowEffectSupported(WindowEffect::Mica) ? I::SettingsWindowEffectDesc : I::EffectUnavailable),L"\xE81E",lay.effect_card.right-16*scale_);
        for(int i=0;i<kWindowEffectCount;++i) {
            const auto r=lay.effect_row[i];
            auto preview=r;preview.bottom-=22*scale_;
            const bool enabled=WindowEffectSupported(static_cast<WindowEffect>(i));
            const bool selected=i==static_cast<int>(SupportedWindowEffect(vm.window_effect));
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
        painter_.DrawWrappedCaption(l10n::Get(I::StartupIntegrationLead),D2D1::Point2F(lay.startup_lead.left,lay.startup_lead.top),lay.startup_lead.right-lay.startup_lead.left,theme.text_secondary);
        const I overview_titles[]={I::StartupFoldersSummary,I::StartupDialogsTitle,I::SettingsLaunch};
        const I overview_status[]={vm.settings_integration_state>=2 ? I::StartupNeedsAttention : vm.settings_integration_enabled ? I::StartupFoldersOn : I::StartupFoldersOff,
            vm.settings_dialog_conflict || vm.settings_dialog_failed ? I::StartupNeedsAttention : vm.settings_dialog_enabled ? I::StartupDialogsOn : I::StartupDialogsOff,
            vm.settings_launch_on_startup ? I::StartupLaunchOn : I::StartupLaunchOff};
        for(int i=0;i<3;++i) {
            const auto r=lay.startup_overview[i];draw_card(r);
            const wchar_t* glyphs[]={L"\xEC50",L"\xF101",L"\xEC57"};
            const bool attention=i==0 ? vm.settings_integration_state>=2 : i==1 && (vm.settings_dialog_conflict || vm.settings_dialog_failed);
            const bool on=i==0 ? vm.settings_integration_enabled : i==1 ? vm.settings_dialog_enabled : vm.settings_launch_on_startup;
            const auto ink=attention ? D2D1::ColorF(vm.dark ? 0xF7D154 : 0x8A5500) : on ? theme.accent : theme.text_secondary;
            MakeBrush(dc,on || attention ? WithAlpha(ink,0.10f) : theme.fill_hover,brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left+16*scale_,r.top+19*scale_,r.left+50*scale_,r.top+53*scale_),9*scale_,9*scale_),brFillHover_.get());
            painter_.DrawGlyph(glyphs[i],D2D1::RectF(r.left+24*scale_,r.top+27*scale_,r.left+42*scale_,r.top+45*scale_),ink);
            text(l10n::Get(overview_titles[i]),D2D1::RectF(r.left+62*scale_,r.top+13*scale_,r.right-30*scale_,r.top+35*scale_));
            painter_.DrawGlyph(L"\xE76C",D2D1::RectF(r.right-28*scale_,r.top+29*scale_,r.right-14*scale_,r.top+43*scale_),theme.text_secondary);
            painter_.DrawTagDot(D2D1::Point2F(r.left+65*scale_,r.top+48*scale_),3*scale_,ink);
            text(l10n::Get(overview_status[i]),D2D1::RectF(r.left+74*scale_,r.top+36*scale_,r.right-30*scale_,r.bottom-10*scale_),true);
        }
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
            const I cover_desc[]={I::StartupFolderDesc,I::StartupThisPcDesc,I::StartupWinEDesc};
            for(int i=0;i<3;++i) {
                const auto r=lay.integration_cover[i];
                const auto choice=lay.integration_choice[i];
                const bool stacked=choice.left<r.left+100*scale_;
                const float text_right=stacked ? r.right-16*scale_ : choice.left-16*scale_;
                const wchar_t* icons[]={L"\xE8B7",L"\xE7F4",L"\xE765"};
                painter_.DrawGlyph(icons[i],D2D1::RectF(r.left+20*scale_,r.top+15*scale_,r.left+38*scale_,r.top+33*scale_),theme.text_secondary);
                text(l10n::Get(kIntegrationCover[i]),D2D1::RectF(r.left+54*scale_,r.top+8*scale_,text_right,r.top+32*scale_));
                painter_.DrawWrappedCaption(l10n::Get(cover_desc[i]),D2D1::Point2F(r.left+54*scale_,r.top+34*scale_),text_right-r.left-54*scale_,theme.text_secondary);
                const bool on=i==0 ? vm.settings_integration_folders : i==1 ? vm.settings_integration_this_pc : vm.settings_integration_win_e;
                const int current=!on ? 0 : i==0 ? 1 : 1+(i==1 ? vm.settings_open_this_pc : vm.settings_open_win_e);
                const I labels[]={I::SettingsSecExplorer,i==0 ? I::IntegrationHandlerPulse : I::ExternalOpenWindow,I::ExternalOpenTab,I::OpenWindowActivate};
                const int count=IntegrationChoiceCount(i);
                const bool wrapped=count==4 && lay.integration_segments[i][2].top>lay.integration_segments[i][0].top;
                const int columns=wrapped ? 2 : count;
                for(int row=0;row<(wrapped ? 2 : 1);++row) {
                    const auto first=lay.integration_segments[i][row*columns];
                    const auto last=lay.integration_segments[i][row*columns+columns-1];
                    painter_.DrawSegmentedTrack(D2D1::RectF(first.left,first.top,last.right,last.bottom));
                }
                if(current>=0 && current<count) DrawSettingsSegmentPlate(130000+i*3+(wrapped ? 1+current/columns : 0),current,lay.integration_segments[i][current],master_on);
                for(int j=0;j<count;++j) {
                    fluent::SegmentedItemSpec item{};item.bounds=lay.integration_segments[i][j];item.text=l10n::Get(labels[j]);
                    item.state.enabled=master_on;item.state.checked=current==j;item.suppress_plate=item.state.checked;
                    item.state.hovered=master_on && IsHovered(vm,H::SettingsIntegration,20+i*3+j);
                    item.shared_track=true;item.position=j%columns==0 ? fluent::SegmentPosition::First : j%columns==columns-1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
                    painter_.DrawSegmentedItem(item);
                }
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
                const bool extra=action==4 || action==9;
                const float x=r.left+54*scale_, text_left=r.left+(extra?54.0f:84.0f)*scale_;
                if(extra) {
                    painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+12*scale_,r.right-16*scale_,r.top+44*scale_),L"",state);
                    title_right=r.right-84*scale_;
                } else painter_.DrawCheckBox(D2D1::RectF(x,r.top+11*scale_,x+20*scale_,r.top+31*scale_),L"",state);
                (void)icon;
                painter_.DrawText(l10n::Get(title),D2D1::RectF(text_left,r.top+9*scale_,title_right,r.top+31*scale_),
                    compositor_->TextFormat(),master_on ? theme.text : theme.text_secondary);
                const bool badge_stacked=action==4 && r.right-r.left<600*scale_;
                if(action==4) {
                    const auto experimental=l10n::Get(I::IntegrationExperimentalTag);
                    const float w=painter_.MeasureBadgeWidth(experimental);
                    const float tag_left=badge_stacked ? text_left : text_left+MeasureTextWidth(compositor_->DwriteFactory(),compositor_->TextFormat(),l10n::Get(title))+8*scale_;
                    const float y=r.top+(badge_stacked ? 31.0f : 9.0f)*scale_;
                    fluent::BadgeSpec tag{};tag.bounds=D2D1::RectF(tag_left,y,tag_left+w,y+20*scale_);tag.text=experimental;tag.kind=fluent::BadgeKind::Warning;
                    painter_.DrawBadge(tag);
                }
                painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(text_left,r.top+(badge_stacked ? 55.0f : 31.0f)*scale_),
                    (std::max)(40*scale_,title_right-text_left),theme.text_secondary);
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
                    button(lay.integration_try,l10n::Get(I::IntegrationTry),H::SettingsIntegration,8,false,true,L"\xE7F4");
                if(lay.integration_hint_action.bottom>lay.integration_hint_action.top)
                    button(lay.integration_hint_action,l10n::Get(vm.settings_keep_running ? I::IntegrationEnableLaunch : I::IntegrationEnableKeep),H::SettingsIntegration,vm.settings_keep_running ? 11 : 10,false);
                const auto t=lay.integration_hint_text;
                if(vm.settings_integration_just_enabled)
                    DrawIconText(t.left-24*scale_,t.top+1*scale_,16*scale_,16*scale_,L"\xE73E",L"",theme.accent,0.95f);
                painter_.DrawWrappedCaption(IntegrationHint(vm),D2D1::Point2F(t.left,t.top),
                    (std::max)(40*scale_,t.right-t.left),vm.settings_integration_just_enabled ? theme.text : theme.text_secondary);
            }
        }
        {
            const bool available=vm.settings_dialog_available && !vm.settings_dialog_busy && !vm.settings_integration_restore_busy;
            const auto r=lay.dialog_toggle_row;
            const float title_width=(std::max)(40*scale_,(lay.dialog_badge.top>r.top+44*scale_ ? r.right-78*scale_ : lay.dialog_badge.left-18*scale_)-r.left-54*scale_);
            DrawIconText(r.left+20*scale_,r.top+15*scale_,18*scale_,18*scale_,L"\xF101",L"",theme.text_secondary,1.0f);
            painter_.DrawWrappedBody(l10n::Get(I::StartupDialogsToggle),D2D1::Point2F(r.left+54*scale_,r.top+12*scale_),title_width,theme.text);
            const float title_h=painter_.MeasureWrappedBodyHeight(l10n::Get(I::StartupDialogsToggle),title_width);
            painter_.DrawWrappedCaption(l10n::Get(I::StartupDialogsDesc),D2D1::Point2F(r.left+54*scale_,r.top+18*scale_+title_h),title_width,theme.text_secondary);
            fluent::ControlState state{};state.enabled=available;state.checked=vm.settings_dialog_enabled;
            state.hovered=available && IsHovered(vm,H::SettingsDialogManagement,10);
            painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
            fluent::BadgeSpec dialog_badge{};dialog_badge.bounds=lay.dialog_badge;
            const auto dialog_badge_text=l10n::Get(vm.settings_dialog_conflict || vm.settings_dialog_failed ? I::StartupNeedsAttention : vm.settings_dialog_enabled ? I::StartupLaunchOn : I::StartupLaunchOff);
            dialog_badge.text=dialog_badge_text;
            dialog_badge.kind=vm.settings_dialog_conflict || vm.settings_dialog_failed ? fluent::BadgeKind::Warning : vm.settings_dialog_enabled ? fluent::BadgeKind::Success : fluent::BadgeKind::Neutral;
            painter_.DrawBadge(dialog_badge);
            divider(lay.dialog_toggle_row);
            const bool dim_adaptive=dim_begin(!vm.settings_dialog_enabled);
            toggle(lay.dialog_adaptive_row,I::DialogAdaptiveLegacy,I::DialogAdaptiveLegacyDesc,L"\xE943",vm.settings_dialog_adaptive_legacy,45);
            dim_end(dim_adaptive);
            const auto status=lay.dialog_management_status;
            if(vm.settings_dialog_conflict || vm.settings_dialog_failed) {
                const auto title=l10n::Get(I::StartupNeedsAttention);
                fluent::InfoBarSpec bar{};bar.bounds=D2D1::RectF(status.left+16*scale_,status.top+8*scale_,status.right-16*scale_,status.bottom-8*scale_);
                bar.title=title;bar.message=vm.settings_dialog_status;bar.show_close=false;
                bar.kind=vm.settings_dialog_conflict ? fluent::InfoBarKind::Warning : fluent::InfoBarKind::Error;
                const float ca=card_alpha_;
                bar.backdrop=D2D1::ColorF(theme.bg.r+(theme.surface_card.r-theme.bg.r)*ca,
                    theme.bg.g+(theme.surface_card.g-theme.bg.g)*ca,theme.bg.b+(theme.surface_card.b-theme.bg.b)*ca,1.0f);
                if(lay.dialog_retry_button.bottom>lay.dialog_retry_button.top && lay.dialog_retry_button.left>bar.bounds.left+60*scale_)
                    bar.trailing_width=(bar.bounds.right-lay.dialog_retry_button.left)/scale_+2.0f;
                painter_.DrawInfoBar(bar);
            } else if(status.bottom>status.top) painter_.DrawWrappedCaption(vm.settings_dialog_status,D2D1::Point2F(status.left+16*scale_,status.top+12*scale_),status.right-status.left-32*scale_,theme.text_secondary);
            if(lay.dialog_retry_button.bottom>lay.dialog_retry_button.top)
                button(lay.dialog_retry_button,l10n::Get(I::IntegrationRetry),H::SettingsDialogManagement,11,true,available);
            if(lay.dialog_scope_button.bottom>lay.dialog_scope_button.top) {
                const auto scope_rect=lay.dialog_scope_button;
                if(IsHovered(vm,H::SettingsDialogManagement,13)) {
                    MakeBrush(dc,theme.fill_hover,brFillHover_);dc->FillRectangle(scope_rect,brFillHover_.get());
                }
                const float inset=scope_rect.right-scope_rect.left<600*scale_ ? 20*scale_ : 56*scale_;
                painter_.DrawGlyph(vm.settings_dialog_scope_expanded ? L"\xE70E" : L"\xE70D",D2D1::RectF(scope_rect.left+inset,scope_rect.top+13*scale_,scope_rect.left+inset+16*scale_,scope_rect.top+29*scale_),theme.text_secondary);
                painter_.DrawText(l10n::Get(I::StartupDialogsScope),D2D1::RectF(scope_rect.left+inset+22*scale_,scope_rect.top+10*scale_,scope_rect.right-20*scale_,scope_rect.bottom-10*scale_),compositor_->SmallFormat(),theme.text_secondary);
            }
            startup_info::Draw(dc,painter_,theme,lay.startup_dialog_info,scale_,vm.settings_dialog_enabled);
            auto action_label=[&](D2D1_RECT_F row,D2D1_RECT_F control,I title,I desc,const wchar_t* glyph) {
                if(row.bottom<=row.top) return;
                divider(D2D1::RectF(row.left,row.top,row.right,row.top));
                const float right=control.top>row.top+40*scale_ ? row.right-26*scale_ : control.left-20*scale_;
                label(row,l10n::Get(title),L"",glyph,right);
                painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(row.left+54*scale_,row.top+35*scale_),right-row.left-54*scale_,theme.text_secondary);
            };
            const bool feedback_recording=vm.settings_dialog_feedback_recording;
            action_label(lay.dialog_feedback_row,lay.dialog_feedback_button,I::DialogFeedbackTitle,
                feedback_recording ? I::DialogFeedbackRecording : I::DialogFeedbackDesc,L"\xE8F2");
            button(lay.dialog_feedback_button,l10n::Get(feedback_recording ? I::DialogFeedbackFinish : I::DialogFeedbackStart),
                H::SettingsDialogManagement,18,true,available && !vm.settings_dialog_api_test_running);
            action_label(lay.dialog_api_test_row,lay.dialog_api_test_button,I::DialogApiTestTitle,I::DialogApiTestDesc,L"\xE9D9");
            button(lay.dialog_api_test_button,l10n::Get(I::DialogApiTestLaunch),H::SettingsDialogManagement,17,false,
                available && !vm.settings_dialog_api_test_running && !feedback_recording);
            const bool clear_shown=lay.dialog_clear_button.right>lay.dialog_clear_button.left;
            action_label(lay.dialog_history_heading,clear_shown && lay.dialog_clear_button.left<lay.dialog_refresh_button.left ? lay.dialog_clear_button : lay.dialog_refresh_button,
                I::DialogHistoryTitle,I::DialogHistoryDesc,L"\xE9D9");
            button(lay.dialog_refresh_button,l10n::Get(I::DialogHistoryRefresh),H::SettingsDialogManagement,4,false,available);
            if(clear_shown) button(lay.dialog_clear_button,l10n::Get(I::DialogHistoryClear),H::SettingsDialogManagement,16,false,available);
            const auto note=lay.dialog_history_note;
            const auto& history_note=vm.settings_dialog_history_note.empty() ? l10n::Get(I::DialogHistoryEmpty) : vm.settings_dialog_history_note;
            painter_.DrawWrappedCaption(history_note,D2D1::Point2F(note.left+20*scale_,note.top+12*scale_),
                (std::max)(40*scale_,note.right-note.left-40*scale_),theme.text_secondary);
            // dialog_records_mockup.html: icon, name and file name, status chip, one
            // summary line; what happened / what to do in a box; a later success.
            namespace R=dialog_records;
            for(const auto& [caption_row,group]:lay.dialog_history_groups) {
                divider(D2D1::RectF(caption_row.left,caption_row.top,caption_row.right,caption_row.top));
                painter_.FillRoundedRect(caption_row,0,theme.surface_sheet);
                painter_.DrawText(l10n::Get(group==1 ? I::DialogRecordGroupRestart : group==3 ? I::DialogRecordGroupPaused : I::DialogRecordGroupRecent),
                    D2D1::RectF(caption_row.left+R::kSide*scale_,caption_row.top,caption_row.right-R::kSide*scale_,caption_row.bottom),
                    compositor_->SmallFormat(),theme.text_secondary);
            }
            const bool dark_theme=(theme.bg.r+theme.bg.g+theme.bg.b)<1.5f;
            const D2D1_COLOR_F success=dark_theme ? HexColor(0x5FD4A0) : HexColor(0x0F7B4F);
            const auto measure_text=[&](std::wstring_view text){return typography::MeasureLine(compositor_,compositor_->TextFormat(),text);};
            const auto measure_small=[&](std::wstring_view text){return typography::MeasureLine(compositor_,compositor_->SmallFormat(),text);};
            for(size_t i=0;i<lay.dialog_history_rows.size() && i<vm.settings_dialog_records.size();++i) {
                const auto row=lay.dialog_history_rows[i];const auto& record=vm.settings_dialog_records[i];
                divider(D2D1::RectF(row.left,row.top,row.right,row.top));
                const float top=row.top+R::kPad*scale_;
                // Program icon: drawn 1:1 when the worker extracted it at this size.
                const float edge=std::round(R::kIcon*scale_);
                const float icon_x=std::round(row.left+R::kSide*scale_),icon_y=std::round(top+(R::kHead*scale_-edge)*0.5f);
                const auto icon=D2D1::RectF(icon_x,icon_y,icon_x+edge,icon_y+edge);
                if(ID2D1Bitmap* bitmap=record.icon ? AppIconBitmap(record.icon) : nullptr) {
                    compositor_->Dc()->DrawBitmap(bitmap,&icon,1.0f,D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                } else {
                    painter_.FillRoundedRect(icon,7*scale_,WithAlpha(theme.text,0.06f));
                    DrawStoreIcon(compositor_->Dc(),StoreIcon::Square,D2D1::RectF(icon.left+8*scale_,icon.top+8*scale_,icon.right-8*scale_,icon.bottom-8*scale_),
                        theme.text_secondary,1.3f);
                }
                // Delete (or, for a paused program, retry): a subtle button at the far right of the name line.
                const auto remove=i<lay.dialog_history_delete_buttons.size() ? lay.dialog_history_delete_buttons[i] : D2D1_RECT_F{};
                const bool removable=remove.right>remove.left;
                if(removable) {
                    const bool hovered=available && IsHovered(vm,H::SettingsDialogManagement,100+static_cast<int>(i));
                    if(hovered) painter_.FillRoundedRect(remove,4*scale_,theme.fill_hover);
                    const float cx=(remove.left+remove.right)*0.5f,cy=(remove.top+remove.bottom)*0.5f,half=8*scale_;
                    DrawStoreIcon(compositor_->Dc(),record.retry ? StoreIcon::Refresh : StoreIcon::Trash,D2D1::RectF(cx-half,cy-half,cx+half,cy+half),
                        available ? (hovered ? theme.text : theme.text_secondary) : theme.text_disabled,1.3f);
                }
                const float chip_right=removable ? remove.left-6*scale_ : row.right-R::kSide*scale_;
                // Status chip, centered on the name line.
                const float chip_w=record.status.empty() ? 0.0f : painter_.MeasureBadgeWidth(record.status);
                const float chip_top=top+(R::kTitleH-22.0f)*0.5f*scale_;
                const auto chip=D2D1::RectF(chip_right-chip_w,chip_top,chip_right,chip_top+22*scale_);
                if(chip_w>0) painter_.DrawBadge({chip,record.status,record.tone==1 ? fluent::BadgeKind::Success :
                    record.tone==2 ? fluent::BadgeKind::Warning : record.tone==3 ? fluent::BadgeKind::Danger : fluent::BadgeKind::Neutral});
                // Name, then the executable name in the caption color while there is room.
                const float text_left=row.left+R::TextLeft()*scale_;
                const float name_right=(chip_w>0 ? chip.left-12*scale_ : chip_right);
                const auto name=FitTextEnd(record.application,(std::max)(0.0f,name_right-text_left),measure_text);
                const float name_w=measure_text(name);
                painter_.DrawText(name,D2D1::RectF(text_left,top,name_right,top+R::kTitleH*scale_),compositor_->TextFormat(),theme.text);
                const float file_left=text_left+name_w+6*scale_;
                if(!record.file.empty() && record.file!=record.application && name_right-file_left>=40*scale_)
                    painter_.DrawText(FitTextEnd(record.file,name_right-file_left,measure_small),
                        D2D1::RectF(file_left,top+1*scale_,name_right,top+R::kTitleH*scale_),compositor_->SmallFormat(),theme.text_secondary);
                // Summary: operation, count and the latest event in the list's date style.
                std::wstring meta=record.meta;
                if(record.last_event) meta+=(meta.empty() ? L"" : L" · ")+SmartListDate(record.last_event);
                painter_.DrawText(FitTextEnd(meta,(std::max)(0.0f,row.right-R::kSide*scale_-text_left),measure_small),
                    D2D1::RectF(text_left,top+R::kTitleH*scale_,row.right-R::kSide*scale_,top+R::kHead*scale_),compositor_->SmallFormat(),theme.text_secondary);
                float y=top+R::kHead*scale_;
                if(const float box_h=R::BoxHeight(painter_,record,row.right-row.left,scale_);box_h>0) {
                    const auto box=D2D1::RectF(text_left,y+R::kBoxTop*scale_,row.right-R::kSide*scale_,y+R::kBoxTop*scale_+box_h);
                    painter_.FillRoundedRect(box,6*scale_,WithAlpha(theme.text,dark_theme ? 0.08f : 0.05f));
                    const auto [what,todo]=R::SplitDetail(record.detail);
                    const float width=R::BoxTextWidth(row.right-row.left,scale_);
                    const auto origin=D2D1::Point2F(box.left+R::kBoxPadX*scale_,box.top+R::kBoxPadY*scale_);
                    painter_.DrawWrappedCaption(what,origin,width,theme.text);
                    if(!todo.empty()) painter_.DrawWrappedCaption(todo,D2D1::Point2F(origin.x,origin.y+painter_.MeasureWrappedCaptionHeight(what,width)+2*scale_),
                        width,theme.text_secondary);
                    y=box.bottom;
                }
                if(!record.later.empty()) {
                    const float note_top=y+R::kLaterTop*scale_;
                    DrawStoreIcon(compositor_->Dc(),StoreIcon::Check,D2D1::RectF(text_left,note_top+2*scale_,text_left+12*scale_,note_top+14*scale_),success,1.6f);
                    painter_.DrawWrappedCaption(record.later,D2D1::Point2F(text_left+R::kLaterIcon*scale_,note_top),
                        R::LaterTextWidth(row.right-row.left,scale_),success);
                }
            }
            action_label(lay.dialog_application_row,lay.dialog_export_button,I::StartupDiagnostics,I::StartupDiagnosticsDesc,L"\xE8A5");
            button(lay.dialog_export_button,l10n::Get(I::StartupDiagnosticsExport),H::SettingsDialogManagement,12,false,available);
            toggle(lay.dialog_jump_row,I::DialogJumpBar,I::DialogJumpBarDesc,L"\xE8E5",vm.settings_dialog_jump_bar,41);
            divider(lay.dialog_jump_row);
            const bool dim_auto=dim_begin(!vm.settings_dialog_jump_bar);
            toggle(lay.dialog_jump_auto_row,I::DialogJumpAuto,I::DialogJumpAutoDesc,L"\xE72A",vm.settings_dialog_jump_auto,42);
            dim_end(dim_auto);

        }
        toggle(lay.startup_row[0],I::SettingsLaunch,I::SettingsLaunchDesc,L"\xEC57",vm.settings_launch_on_startup,1);
        {
            const bool dim=dim_begin(!vm.settings_launch_on_startup);
            toggle(lay.start_in_tray_row,I::SettingsStartInTray,I::SettingsStartInTrayDesc,L"\xE8A1",vm.settings_start_in_tray,27);
            dim_end(dim);
        }
        toggle(lay.startup_row[1],I::SettingsKeepRunning,I::SettingsKeepRunningDesc,L"\xF103",vm.settings_keep_running,2);
        const I notify_icon[]={I::NotifyIconAlways,I::NotifyIconBackground,I::NotifyIconNever};const int notify_values[]={0,1,2};
        segmented(lay.notify_icon_card,lay.notify_icon_row,notify_icon,notify_values,vm.settings_notify_icon,H::SettingsNotifyIcon,I::SettingsNotifyIcon,I::SettingsNotifyIconDesc,3,L"\xE8A1");
        {
            const auto r=lay.shell_tags_row;
            label(r,l10n::Get(I::SettingsShellTags),l10n::Get(I::SettingsShellTagsDesc),L"\xE8EC",r.right-72*scale_,(r.bottom-r.top)/scale_-47);
            fluent::ControlState state{};state.checked=vm.settings_shell_tags;state.hovered=IsHovered(vm,H::SettingsToggle,21);
            painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
        }
        label(lay.startup_restore_row,l10n::Get(I::StartupRestoreTitle),L"",L"\xE7A7",(lay.startup_restore_button.top>lay.startup_restore_row.top+40*scale_ ? lay.startup_restore_row.right-16*scale_ : lay.startup_restore_button.left-20*scale_));
        painter_.DrawWrappedCaption(l10n::Get(I::StartupRestoreDesc),D2D1::Point2F(lay.startup_restore_row.left+54*scale_,lay.startup_restore_row.top+36*scale_),(lay.startup_restore_button.top>lay.startup_restore_row.top+40*scale_ ? lay.startup_restore_row.right-54*scale_ : lay.startup_restore_button.left-20*scale_)-lay.startup_restore_row.left-54*scale_,theme.text_secondary);
        button(lay.startup_restore_button,l10n::Get(I::StartupRestoreAction),H::SettingsDialogManagement,15,false,
            vm.settings_dialog_available && !vm.settings_dialog_busy && !vm.settings_integration_restore_busy &&
            (vm.settings_integration_enabled || vm.settings_integration_state>=2 || vm.settings_dialog_enabled || vm.settings_dialog_conflict || vm.settings_dialog_failed || vm.settings_shell_tags));
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
        const I open_pulse_icon_labels[]={I::ExternalOpenWindow,I::ExternalOpenTab,I::OpenWindowActivate}; const int open_pulse_icon_values[]={0,1,2};
        segmented(lay.open_pulse_icon_card,lay.open_pulse_icon_row,open_pulse_icon_labels,open_pulse_icon_values,vm.settings_open_pulse_icon,H::SettingsOpenPulseIcon,I::SettingsOpenPulseIcon,I::SettingsOpenPulseIconDesc,3);
        const I new_window_shows_labels[]={I::OpenWindowStartup,I::OpenWindowThisPc,I::NewTabOpenCurrent}; const int new_window_shows_values[]={0,1,2};
        segmented(lay.new_window_shows_card,lay.new_window_shows_row,new_window_shows_labels,new_window_shows_values,vm.settings_new_window_shows,H::SettingsNewWindowShows,I::SettingsNewWindowShows,I::SettingsNewWindowShowsDesc,3);
        toggle(lay.arrange_minimized_row,I::SettingsArrangeMinimized,I::SettingsArrangeMinimizedDesc,L"\xE921",vm.settings_arrange_include_minimized,81);
        const I arrange_gap_labels[]={I::ArrangeGapNone,I::ArrangeGapSix}; const int arrange_gap_values[]={0,1};
        segmented(lay.arrange_gap_card,lay.arrange_gap_row,arrange_gap_labels,arrange_gap_values,vm.settings_arrange_gap,H::SettingsArrangeGap,I::SettingsArrangeGap,I::SettingsArrangeGapDesc,2);
        label(lay.tab_active_style_card,l10n::Get(I::SettingsTabActiveStyle),l10n::Get(I::SettingsTabActiveStyleDesc),L"\xE8A0");
        const I style_labels[]={I::TabActiveStyleLift,I::TabActiveStyleGradient};
        for(int i=0;i<2;++i) {
            const auto r=lay.tab_active_style_choices[i];
            const bool selected=vm.tab_active_style==i;
            const auto preview_accent=vm.dark ? MixColor(theme.accent,HexColor(0xFFFFFF),0.3f) : theme.accent;
            const bool hovered=IsHovered(vm,H::SettingsTabActiveStyle,i);
            const auto foreground=selected ? theme.text : theme.text_secondary;
            painter_.FillRoundedRect(r,8*scale_,hovered ? theme.fill_hover : theme.fill_input);
            MakeBrush(dc,selected ? preview_accent : theme.stroke_card,brStrokeCard_);
            const float stroke=(selected ? 2.0f : 1.0f)*scale_;
            const auto border=D2D1::RectF(r.left+stroke/2,r.top+stroke/2,r.right-stroke/2,r.bottom-stroke/2);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(border,8*scale_,8*scale_),brStrokeCard_.get(),stroke);
            const float s=scale_;
            const auto ill=D2D1::RectF(r.left+9*s,r.top+9*s,r.right-9*s,r.top+67*s);
            const float strip_bottom=std::round(ill.top+31*s);
            const auto strip_fill=vm.dark ? HexColor(0x1A1A1A) : HexColor(0xF1F3EE);
            const auto sheet_fill=theme.surface_sheet;
            painter_.FillRoundedRect(ill,6*s,strip_fill);
            painter_.FillRoundedRect(D2D1::RectF(ill.left,strip_bottom,ill.right,ill.bottom),5*s,sheet_fill);
            MakeBrush(dc,sheet_fill,brFillSelected_);
            FillRect(dc,brFillSelected_.get(),ill.left,strip_bottom,(std::max)(0.0f,ill.right-ill.left),(std::min)(6*s,(std::max)(0.0f,ill.bottom-strip_bottom)));
            const float inner_w=(std::max)(0.0f,ill.right-ill.left-16*s);
            const float gap=4*s;
            const float side_w=(std::max)(0.0f,(inner_w-gap*2)*0.27f);
            const float mid_w=(std::max)(0.0f,inner_w-side_w*2-gap*2);
            const float tab_top=ill.top+7*s;
            const float left_x0=ill.left+8*s;
            const float mid_x0=left_x0+side_w+gap;
            const float right_x0=mid_x0+mid_w+gap;
            auto draw_inactive=[&](float x0,float tw) {
                if(tw<8*s) return;
                const float mid_y=(tab_top+strip_bottom-2*s)*0.5f;
                painter_.FillRoundedRect(D2D1::RectF(x0+3*s,mid_y-2*s,x0+7*s,mid_y+2*s),1*s,WithAlpha(theme.text_secondary,0.6f));
                if(tw>18*s) painter_.FillRoundedRect(D2D1::RectF(x0+10*s,mid_y-1.25f*s,x0+tw-5*s,mid_y+1.25f*s),1*s,WithAlpha(theme.text_secondary,0.42f));
            };
            draw_inactive(left_x0,side_w);
            draw_inactive(right_x0,side_w);
            if(mid_w>12*s) {
                const D2D1_RECT_F active_rc=D2D1::RectF(mid_x0,tab_top,mid_x0+mid_w,strip_bottom);
                ChromeTabShape shape{};
                shape.top_radius=5.5f*s;
                shape.bottom_radius=8.0f*s;
                shape.connect_bottom=true;
                auto vertical=[&](const D2D1_GRADIENT_STOP* stops,UINT32 n) {
                    ComPtr<ID2D1GradientStopCollection> collection;
                    ComPtr<ID2D1LinearGradientBrush> brush;
                    if(FAILED(dc->CreateGradientStopCollection(stops,n,&collection)) ||
                       FAILED(dc->CreateLinearGradientBrush(
                           D2D1::LinearGradientBrushProperties({0.0f,active_rc.top},{0.0f,active_rc.bottom}),
                           collection.get(),&brush)))
                        return ComPtr<ID2D1LinearGradientBrush>();
                    return brush;
                };
                if(i==0 && !IsHighContrast()) {
                    dc->PushAxisAlignedClip(D2D1::RectF(ill.left,ill.top,ill.right,strip_bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    for(int k=3;k>=1;--k) {
                        const float grow=static_cast<float>(k)*0.85f*s;
                        ChromeTabShape halo=shape;
                        halo.top_radius+=grow;
                        MakeBrush(dc,vm.dark ? WithAlpha(HexColor(0x000000),0.12f)
                                             : WithAlpha(HexColor(0x303B50),0.03f),brFillPressed_);
                        FillChromeTab(dc,brFillPressed_.get(),
                                      D2D1::RectF(active_rc.left-grow,active_rc.top-grow,active_rc.right+grow,active_rc.bottom),halo);
                    }
                    dc->PopAxisAlignedClip();
                }
                MakeBrush(dc,sheet_fill,brFillSelected_);
                FillChromeTab(dc,brFillSelected_.get(),active_rc,shape);
                if(!IsHighContrast()) {
                    const float edge_w=(std::max)(1.0f,std::round(s));
                    if(i==1) {
                        const D2D1_GRADIENT_STOP stops[]={{0.0f,WithAlpha(theme.accent,vm.dark ? 0.30f : 0.24f)},
                                                            {1.0f,WithAlpha(theme.accent,0.0f)}};
                        if(auto wash=vertical(stops,2); wash.get()) FillChromeTab(dc,wash.get(),active_rc,shape);
                        const D2D1_GRADIENT_STOP edge_stops[]={{0.0f,WithAlpha(theme.accent,0.55f)},
                                                                 {0.55f,WithAlpha(theme.accent,0.40f)},
                                                                 {0.95f,WithAlpha(theme.accent,0.0f)}};
                        if(auto fade=vertical(edge_stops,3); fade.get()) StrokeChromeTabEdge(dc,fade.get(),active_rc,shape,edge_w);
                    } else {
                        if(vm.dark) {
                            const D2D1_GRADIENT_STOP stops[]={{0.0f,D2D1::ColorF(1.0f,1.0f,1.0f,0.05f)},
                                                                {0.9f,D2D1::ColorF(1.0f,1.0f,1.0f,0.0f)}};
                            if(auto light=vertical(stops,2); light.get()) FillChromeTab(dc,light.get(),active_rc,shape);
                        }
                        const D2D1_GRADIENT_STOP stops[]={{0.0f,WithAlpha(theme.accent,vm.dark ? 0.14f : 0.10f)},
                                                            {0.6f,WithAlpha(theme.accent,0.0f)}};
                        if(auto wash=vertical(stops,2); wash.get()) FillChromeTab(dc,wash.get(),active_rc,shape);
                        const D2D1_COLOR_F edge=vm.dark ? HexColor(0xFFFFFF) : HexColor(0x303B50);
                        const float edge_a=vm.dark ? 0.13f : 0.16f;
                        const D2D1_GRADIENT_STOP edge_stops[]={{0.0f,WithAlpha(edge,edge_a)},
                                                                 {0.55f,WithAlpha(edge,edge_a*0.85f)},
                                                                 {0.95f,WithAlpha(edge,0.0f)}};
                        if(auto fade=vertical(edge_stops,3); fade.get()) StrokeChromeTabEdge(dc,fade.get(),active_rc,shape,edge_w);
                    }
                }
                const float mid_y=(tab_top+strip_bottom-2*s)*0.5f;
                painter_.FillRoundedRect(D2D1::RectF(active_rc.left+5*s,mid_y-2.5f*s,active_rc.left+10*s,mid_y+2.5f*s),1.2f*s,theme.icon_folder);
                if(mid_w>28*s) {
                    const float line_right=active_rc.right-(mid_w>44*s ? 14*s : 5*s);
                    painter_.FillRoundedRect(D2D1::RectF(active_rc.left+13*s,mid_y-1.6f*s,line_right,mid_y+1.6f*s),1.2f*s,WithAlpha(theme.text,0.85f));
                }
                if(mid_w>44*s) painter_.FillRoundedRect(D2D1::RectF(active_rc.right-9*s,mid_y-1.5f*s,active_rc.right-6*s,mid_y+1.5f*s),1.5f*s,WithAlpha(theme.text_secondary,0.55f));
                painter_.FillRoundedRect(D2D1::RectF(active_rc.left+5*s,strip_bottom-3.5f*s,active_rc.left+16*s,strip_bottom-1.5f*s),1*s,preview_accent);
            }
            const auto addr=D2D1::RectF(ill.left+8*s,strip_bottom+5*s,ill.right-8*s,ill.bottom-5*s);
            if(addr.bottom>addr.top+6*s && addr.right>addr.left+16*s) {
                painter_.FillRoundedRect(addr,4*s,theme.surface_card);
                painter_.StrokeRoundedRect(addr,4*s,theme.stroke_card,1);
                const float am=(addr.top+addr.bottom)*0.5f;
                const float seg_w=(std::min)(26*s,(addr.right-addr.left-14*s)*0.28f);
                painter_.FillRoundedRect(D2D1::RectF(addr.left+6*s,am-1.2f*s,addr.left+6*s+seg_w,am+1.2f*s),1*s,WithAlpha(theme.text_secondary,0.45f));
                painter_.FillRoundedRect(D2D1::RectF(addr.left+12*s+seg_w,am-1.2f*s,addr.left+12*s+seg_w*1.8f,am+1.2f*s),1*s,WithAlpha(theme.text_secondary,0.30f));
            }
            painter_.StrokeRoundedRect(ill,6*s,theme.stroke_card,1);
            painter_.DrawText(l10n::Get(style_labels[i]),D2D1::RectF(r.left+7*scale_,r.top+72*scale_,r.right-7*scale_,r.bottom-6*scale_),compositor_->SmallFormat(),foreground,fluent::HorizontalAlignment::Center);
        }
        label(lay.tab_layout_card,l10n::Get(I::SettingsTabLayout),l10n::Get(I::SettingsTabLayoutDesc),L"\xE8A0");
        const I tab_labels[]={I::TabLayoutScroll,I::TabLayoutGrid,I::TabLayoutFit};
        for(int i=0;i<3;++i) {
            const auto r=lay.tab_layout_choices[i];
            const bool enabled=!vm.settings_vertical_tabs;
            const bool selected=vm.tab_layout==i;
            const auto preview_accent=vm.dark ? MixColor(theme.accent,HexColor(0xFFFFFF),0.3f) : theme.accent;
            const bool hovered=enabled && IsHovered(vm,H::SettingsTabLayout,i);
            const auto foreground=enabled ? (selected ? theme.text : theme.text_secondary) : theme.text_disabled;
            painter_.FillRoundedRect(r,8*scale_,hovered ? theme.fill_hover : theme.fill_input);
            MakeBrush(dc,selected && enabled ? preview_accent : theme.stroke_card,brStrokeCard_);
            const float stroke=(selected ? 2.0f : 1.0f)*scale_;
            const auto border=D2D1::RectF(r.left+stroke/2,r.top+stroke/2,r.right-stroke/2,r.bottom-stroke/2);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(border,8*scale_,8*scale_),brStrokeCard_.get(),stroke);
            // A tiny Pulse window: the tab strip on the title colour, each tab a folder dot
            // and a title line; the active tab joins the content below with the accent
            // underline real tabs have; faint rows of files under it.
            const auto ill=D2D1::RectF(r.left+9*scale_,r.top+9*scale_,r.right-9*scale_,r.top+63*scale_);
            const float s=scale_;
            const auto strip_fill=vm.dark ? HexColor(0x1C1C1C) : HexColor(0xE9EDE6);
            const auto content_fill=vm.dark ? HexColor(0x2B2B2B) : HexColor(0xFFFFFF);
            const auto tab_fill=WithAlpha(theme.text,vm.dark ? 0.07f : 0.06f);
            const auto ink=enabled ? theme.text_secondary : theme.text_disabled;
            const auto accent=enabled ? preview_accent : theme.text_disabled;
            const auto folder=enabled ? theme.icon_folder : theme.text_disabled;
            painter_.FillRoundedRect(ill,6*s,strip_fill);
            painter_.StrokeRoundedRect(ill,6*s,theme.stroke_card,1);
            const int rows=i==0 ? 1 : 2;
            const float tab_h=11*s,row_gap=2*s,strip_top=ill.top+5*s;
            const float strip_bottom=strip_top+rows*tab_h+(rows-1)*row_gap;
            const auto content=D2D1::RectF(ill.left+4*s,strip_bottom,ill.right-4*s,ill.bottom-4*s);
            painter_.FillRoundedRect(content,3*s,content_fill);
            for(int k=0;k<3;++k) {
                const float y=content.top+(5+k*5.5f)*s;
                if(y+2*s>content.bottom-2*s) break;
                const float widths[]={0.62f,0.44f,0.53f};
                painter_.FillRoundedRect(D2D1::RectF(content.left+5*s,y,content.left+5*s+(content.right-content.left-10*s)*widths[k],y+2*s),
                    1*s,WithAlpha(ink,0.28f));
            }
            auto tab=[&](float x0,float x1,float y0,bool active) {
                if(x1-x0<6*s) return;
                const auto t=D2D1::RectF(x0,y0,x1,y0+tab_h);
                painter_.FillRoundedRect(t,3*s,active ? content_fill : tab_fill);
                const float mid=(t.top+t.bottom)*0.5f;
                painter_.FillRoundedRect(D2D1::RectF(x0+3*s,mid-1.75f*s,x0+6.5f*s,mid+1.75f*s),0.8f*s,WithAlpha(folder,active ? 0.95f : 0.6f));
                if(x1-x0>14*s) painter_.FillRoundedRect(D2D1::RectF(x0+8.5f*s,mid-1*s,x1-3*s,mid+1*s),1*s,WithAlpha(ink,active ? 0.75f : 0.45f));
                if(active) painter_.FillRoundedRect(D2D1::RectF(x0+2*s,t.bottom-1.6f*s,x1-2*s,t.bottom),0.8f*s,accent);
            };
            const float x_left=ill.left+4*s,x_right=ill.right-4*s;
            if(i==0) {
                // One row: fixed-width tabs run past the edge; a chevron scrolls on.
                const float chevron_w=12*s,tab_w=34*s;
                dc->PushAxisAlignedClip(D2D1::RectF(x_left,strip_top,x_right-chevron_w,strip_top+tab_h),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                for(int col=0;col<6;++col) tab(x_left+col*(tab_w+2*s),x_left+col*(tab_w+2*s)+tab_w,strip_top,col==1);
                dc->PopAxisAlignedClip();
                const float cx=x_right-chevron_w*0.5f,cy=strip_top+tab_h*0.5f;
                DrawStoreIcon(dc,StoreIcon::ChevronRight,D2D1::RectF(cx-4.5f*s,cy-4.5f*s,cx+4.5f*s,cy+4.5f*s),ink,1.4f);
            } else {
                // Two rows: equal columns, or each tab as wide as its title.
                const float weights[2][3]={{3,2,4},{4,3,0}};
                for(int row=0;row<2;++row) {
                    const float y=strip_top+row*(tab_h+row_gap);
                    const int count=i==1 ? 3 : (row==0 ? 3 : 2);
                    float total=0;for(int c=0;c<count;++c) total+=i==1 ? 1.0f : weights[row][c];
                    const float room=x_right-x_left-(count-1)*2*s;
                    float x=x_left;
                    for(int col=0;col<count;++col) {
                        const float w=room*(i==1 ? 1.0f : weights[row][col])/total;
                        tab(x,x+w,y,row==1 && col==(i==1 ? 1 : 0));
                        x+=w+2*s;
                    }
                }
            }
            painter_.DrawText(l10n::Get(tab_labels[i]),D2D1::RectF(r.left+7*scale_,r.top+69*scale_,r.right-7*scale_,r.bottom-7*scale_),compositor_->SmallFormat(),foreground,fluent::HorizontalAlignment::Center);
        }
        const I row_labels[]={I::TabRowsTwo,I::TabRowsThree,I::TabRowsFour};
        const bool rows_enabled=!vm.settings_vertical_tabs && vm.tab_layout!=0;
        const auto rr=lay.tab_rows_card;
        const auto* choices=lay.tab_rows_choices;
        label(rr,l10n::Get(I::SettingsTabRows),l10n::Get(I::SettingsTabRowsDesc),L"\xE8A0",choices[0].left<rr.left+100*scale_ ? rr.right-16*scale_ : choices[0].left-12*scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left,choices[0].top,choices[2].right,choices[2].bottom));
        const int selected=std::clamp(vm.tab_max_rows-2,0,2);
        DrawSettingsSegmentPlate(static_cast<int>(H::SettingsTabRows),selected,choices[selected],rows_enabled);
        for(int i=0;i<3;++i) {
            fluent::SegmentedItemSpec item{};item.bounds=choices[i];item.text=l10n::Get(row_labels[i]);
            item.shared_track=true;item.state.checked=i==selected;item.suppress_plate=item.state.checked;
            item.state.enabled=rows_enabled;item.state.hovered=rows_enabled && IsHovered(vm,H::SettingsTabRows,i);
            painter_.DrawSegmentedItem(item);
        }
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
            if(!vm.settings_single_click) {
                const auto r=lay.double_click_interval_card, c=lay.double_click_interval_choice;
                label(r,l10n::Get(I::SettingsDoubleClickInterval),l10n::Get(I::SettingsDoubleClickIntervalDesc),L"\xE917",c.left<r.left+100*scale_ ? r.right-16*scale_ : c.left-12*scale_);
                fluent::ButtonSpec control{};
                control.bounds=c; control.drop_down=true;
                control.text=vm.settings_double_click_interval_ms ? std::to_wstring(vm.settings_double_click_interval_ms)+L" ms" : l10n::Get(I::DoubleClickSystem);
                control.state.hovered=IsHovered(vm,H::SettingsDropdown,13);
                painter_.DrawButton(control);
            }
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
        toggle(lay.list_style_row[6],I::MediaColumnsTitle,I::MediaColumnsDesc,L"\xE714",vm.settings_media_columns,80);
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
            segmented(lay.mtime_style_card,lay.mtime_style_row,mt_styles,mt_style_values,mt.style,H::SettingsMtimeStyle,I::ListMtimeHint,I::ListMtimeHintDesc,4,L"\xEC92");  // SmartDate; Timer stays with the durations
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
        if(page==kSettingsPageUiLayout) {
            static constexpr I kModeNames[]={I::SetupModeZen,I::SetupModeStandard,I::SetupModePro};
            static constexpr I kModeLines[]={I::SetupModeZenLine,I::SetupModeStandardLine,I::SetupModeProLine};
            static constexpr const wchar_t* kModeKeys[]={L"Alt+Shift+1",L"Alt+Shift+2",L"Alt+Shift+3"};
            const int mode=std::clamp(vm.settings_uil_mode,0,2);
            const bool changed=!vm.settings_uil_diff.empty();
            const D2D1_COLOR_F accent_fg=IsHighContrast() ? theme.text : vm.dark ? MixColor(theme.accent,HexColor(0xFFFFFF),0.3f) : theme.accent;
            auto with_mode=[&](I id) {
                std::wstring t=l10n::Get(id);
                if(const auto at=t.find(L"{mode}"); at!=std::wstring::npos) t.replace(at,6,l10n::Get(kModeNames[mode]));
                return t;
            };
            auto icon_button=[&](D2D1_RECT_F r,const wchar_t* glyph,int index,bool enabled) {
                if(r.bottom<=r.top) return;
                fluent::ButtonSpec spec{};spec.bounds=r;spec.glyph=glyph;spec.icon_only=true;
                spec.state.enabled=enabled;spec.state.hovered=enabled && IsHovered(vm,H::SettingsUiLayout,index);
                painter_.DrawButton(spec);
            };
            button(lay.uil_back,l10n::Get(I::UilBackToFolder),H::SettingsUiLayout,4);
            // 起点 cards: the current one in the accent (lighter while options differ from it).
            for(int i=0;i<3;++i) {
                const auto c=lay.uil_card[i];
                if(c.right<=c.left) continue;
                const bool current=i==mode, hot=IsHovered(vm,H::SettingsUiLayout,i);
                if(current) painter_.FillRoundedRect(c,8*scale_,WithAlpha(theme.accent,vm.dark ? 0.14f : 0.07f));
                else if(hot) painter_.FillRoundedRect(c,8*scale_,theme.fill_hover);
                if(current) painter_.StrokeRoundedRect(D2D1::RectF(c.left+1,c.top+1,c.right-1,c.bottom-1),8*scale_,WithAlpha(theme.accent,changed ? 0.5f : 1.0f),2.0f);
                else painter_.StrokeRoundedRect(c,8*scale_,theme.stroke_card);
                MakeBrush(dc,theme.accent,brFillHover_);
                DrawLayoutMiniPreview(dc,brFillHover_.get(),lay.uil_preview[i],i,theme,vm.dark,scale_);
                const auto pv=lay.uil_preview[i];
                const bool beside=pv.right<c.right-120*scale_;
                const float tl=beside ? pv.right+14*scale_ : c.left+12*scale_;
                const float tt=beside ? c.top+12*scale_ : pv.bottom+10*scale_;
                const float tr=c.right-12*scale_;
                {
                    // Title: the mode's own icon (not 侧边栏's or 列视图's), its name, the kbd chip.
                    static constexpr wchar_t kCardIcon[]={static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModeZen)),static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModeStandard)),
                        static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModePro))};
                    DrawUilIcon(kCardIcon[i],D2D1::RectF(tl,tt+3*scale_,tl+16*scale_,tt+19*scale_),current ? accent_fg : theme.text,current ? accent_fg : theme.text);
                    painter_.DrawText(l10n::Get(kModeNames[i]),D2D1::RectF(tl+22*scale_,tt,tr-30*scale_,tt+24*scale_),compositor_->TextFormat(),theme.text);
                    const float kw=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->SmallFormat(),kModeKeys[i])+12*scale_;
                    const float kr=tr-(current ? 28*scale_ : 0.0f);
                    const auto k=D2D1::RectF(kr-kw,tt+2*scale_,kr,tt+22*scale_);
                    painter_.FillRoundedRect(k,4*scale_,theme.fill_input);
                    painter_.StrokeRoundedRect(k,4*scale_,theme.stroke_card);
                    painter_.DrawText(kModeKeys[i],k,compositor_->SmallFormat(),theme.text_secondary,fluent::HorizontalAlignment::Center);
                }
                painter_.DrawWrappedCaption(l10n::Get(kModeLines[i]),D2D1::Point2F(tl,tt+26*scale_),tr-tl,theme.text_secondary);
                if(current) {
                    const auto mark=D2D1::RectF(tr-20*scale_,tt+2*scale_,tr,tt+22*scale_);
                    if(changed) painter_.StrokeRoundedRect(mark,10*scale_,accent_fg,1.5f);
                    else painter_.FillRoundedRect(mark,10*scale_,theme.accent);
                    painter_.DrawGlyph(L"\xE73E",D2D1::RectF(mark.left+4*scale_,mark.top+4*scale_,mark.right-4*scale_,mark.bottom-4*scale_),changed ? accent_fg : theme.accent_text);
                }
            }
            {
                const auto d=lay.uil_diff_row;
                if(d.right>d.left) {
                    if(!changed) {
                        painter_.DrawGlyph(L"\xE73E",D2D1::RectF(d.left+18*scale_,d.top+17*scale_,d.left+36*scale_,d.top+35*scale_),accent_fg);
                        text(with_mode(I::UilCurrentIs),D2D1::RectF(d.left+46*scale_,d.top+14*scale_,d.right-16*scale_,d.top+38*scale_));
                    } else {
                        painter_.DrawGlyph(L"\xE70F",D2D1::RectF(d.left+18*scale_,d.top+15*scale_,d.left+36*scale_,d.top+33*scale_),theme.text_secondary);
                        const float tr=lay.uil_restore.top<d.top+20*scale_ ? lay.uil_restore.left-12*scale_ : d.right-16*scale_;
                        text(with_mode(I::UilCustomBased),D2D1::RectF(d.left+46*scale_,d.top+12*scale_,tr,d.top+36*scale_));
                        for(size_t k=0;k<lay.uil_chip.size() && k<vm.settings_uil_diff.size();++k) {
                            const auto c=lay.uil_chip[k];
                            painter_.FillRoundedRect(c,12*scale_,WithAlpha(theme.accent,vm.dark ? 0.2f : 0.1f));
                            painter_.DrawText(l10n::Get(static_cast<I>(vm.settings_uil_diff[k])),D2D1::RectF(c.left,c.top+3*scale_,c.right,c.bottom-3*scale_),
                                compositor_->SmallFormat(),accent_fg,fluent::HorizontalAlignment::Center);
                        }
                        button(lay.uil_restore,with_mode(I::UilRestoreTo),H::SettingsUiLayout,3);
                    }
                }
            }
            static constexpr I kSidebarLabels[]={I::LayoutSegDocked,I::LayoutSegFloating,I::LayoutSegHidden};
            static constexpr I kCommandLabels[]={I::LayoutSegFullCommands,I::LayoutSegInlineCommands,I::LayoutSegCollapsed};
            static constexpr I kAddressLabels[]={I::LayoutSegOwnRow,I::LayoutSegInTitle};
            static constexpr I kDensityLabels[]={I::LayoutSegCompact,I::LayoutSegStandard,I::LayoutSegRoomy};
            static constexpr I kViewLabels[]={I::UilViewDetails,I::UilViewIcons};
            static constexpr int kValues[]={0,1,2};
            const auto& seg=vm.settings_uil_seg;
            const auto& on=vm.settings_uil_toggle;
            using CI = command_icons::Icon;
            static constexpr I kLiteLabels[]={I::UilSegLite,I::UilSegFull};
            static constexpr I kTrayLabels[]={I::UilSegTrayAlways,I::UilSegTrayOnUse};
            static constexpr I kTextLabels[]={I::UilSegShowText,I::UilSegIconOnly};
            static constexpr I kTabLayoutLabels[]={I::UilTabList,I::TabLayoutScroll,I::UilTabGrid,I::UilTabFit};
            static constexpr int kTabLayoutValues[]={0,1,2,3};
            static constexpr I kTabRowsLabels[]={I::TabRowsTwo,I::TabRowsThree,I::TabRowsFour};
            static constexpr int kRowValues[]={2,3,4};
            segmented(lay.uil_seg_card[0],lay.uil_seg[0],kSidebarLabels,kValues,seg[0],H::SettingsUiSidebar,I::LayoutRowSidebar,I::UilSidebarDesc,3,UilG(CI::Sidebar));
            const bool lite_dim=dim_begin(seg[0]==2);
            segmented(lay.uil_lite_card,lay.uil_lite,kLiteLabels,kValues,on[1] ? 0 : 1,H::SettingsUiLite,I::UilSidebarContent,I::UilSidebarContentDesc,2,UilG(CI::Star));
            dim_end(lite_dim);
            segmented(lay.uil_tray_card,lay.uil_tray,kTrayLabels,kValues,on[4] ? 0 : 1,H::SettingsUiTray,I::UilTrayEntry,I::UilTrayEntryDesc,2,UilG(CI::Tray));
            toggle(lay.uil_toggle_row[0],I::LayoutTglTabs,I::UilTabsDesc,UilG(CI::TabStrip),on[0],300);  // as the 界面 panel; Tabs is 排列方式's
            {
                const bool tabs_dim=dim_begin(!on[0] || vm.settings_vertical_tabs);
                segmented(lay.uil_tablayout_card,lay.uil_tablayout,kTabLayoutLabels,kTabLayoutValues,vm.tab_overflow_list ? 0 : vm.tab_layout+1,H::SettingsUiTabLayout,I::UilTabLayout,I::UilTabLayoutDesc,4,UilG(UilIcon::Tabs));
                dim_end(tabs_dim);
                const bool rows_dim=dim_begin(!on[0] || vm.settings_vertical_tabs || vm.tab_layout==0);
                segmented(lay.uil_tabrows_card,lay.uil_tabrows,kTabRowsLabels,kRowValues,vm.tab_max_rows,H::SettingsTabRows,I::SettingsTabRows,I::SettingsTabRowsDesc,3,UilG(UilIcon::Rows));
                dim_end(rows_dim);
            }
            segmented(lay.uil_seg_card[2],lay.uil_seg[2],kAddressLabels,kValues,seg[2],H::SettingsUiAddress,I::LayoutRowAddress,I::UilAddressDesc,2,UilG(UilIcon::Addr));
            segmented(lay.uil_seg_card[1],lay.uil_seg[1],kCommandLabels,kValues,seg[1],H::SettingsUiCommands,I::LayoutRowCommands,I::UilCommandsDesc,3,UilG(UilIcon::Cmdbar));
            const bool labels_dim=dim_begin(seg[1]!=0);
            segmented(lay.uil_labels_card,lay.uil_labels,kTextLabels,kValues,on[8] ? 0 : 1,H::SettingsUiLabels,I::UilToolbarLabels,I::UilToolbarLabelsDesc,2,UilG(CI::TextName));
            dim_end(labels_dim);
            toggle(lay.uil_toggle_row[6],I::UilSelectionTitle,I::UilSelectionDesc,UilG(CI::SelectionBar),on[6],306);
            toggle(lay.uil_toggle_row[2],I::LayoutTglDetails,I::UilDetailsDesc,UilG(CI::Panel),on[2],302);
            toggle(lay.uil_toggle_row[3],I::LayoutTglStatus,I::UilStatusDesc,UilG(CI::StatusBar),on[3],303);  // as the 界面 panel
            segmented(lay.uil_seg_card[4],lay.uil_seg[4],kViewLabels,kValues,seg[4],H::SettingsUiView,I::UilView,I::UilViewDesc,2,UilG(CI::Grid));
            segmented(lay.uil_seg_card[3],lay.uil_seg[3],kDensityLabels,kValues,seg[3],H::SettingsUiDensity,I::LayoutRowDensity,I::UilDensityDesc,3,UilG(CI::Compact));  // as 文件列表's 行高
            toggle(lay.uil_toggle_row[5],I::LayoutTglZebra,I::UilZebraDesc,UilG(CI::Zebra),on[5],305);  // as 文件列表's 斑马纹
            toggle(lay.uil_toggle_row[7],I::UilExtTitle,I::UilExtDesc,UilG(CI::FileExt),on[7],307);
            // 三套有什么不同: the current starting point's column tinted in the accent.
            if(lay.uil_cmp_card.bottom>lay.uil_cmp_card.top) {
                const auto& X=lay.uil_cmp_x; const auto& Y=lay.uil_cmp_y;
                const auto col=D2D1::RectF(X[1+mode]+2*scale_,Y[0],X[2+mode]-2*scale_,Y[12]);
                painter_.FillRoundedRect(col,6*scale_,WithAlpha(theme.accent,vm.dark ? 0.16f : 0.08f));
                static constexpr wchar_t kHeadIcon[]={static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModeZen)),static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModeStandard)),
                    static_cast<wchar_t>(0x100+static_cast<int>(command_icons::Icon::ModePro))};
                for(int m=0;m<3;++m) {
                    const auto fg=m==mode ? accent_fg : theme.text;
                    DrawUilIcon(kHeadIcon[m],D2D1::RectF(X[1+m]+10*scale_,Y[0]+11*scale_,X[1+m]+26*scale_,Y[0]+27*scale_),fg,fg);
                    painter_.DrawText(l10n::Get(kModeNames[m]),D2D1::RectF(X[1+m]+32*scale_,Y[0]+8*scale_,X[2+m]-8*scale_,Y[0]+30*scale_),compositor_->TextFormat(),fg);
                }
                MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
                for(int r=0;r<11;++r) {
                    FillRect(dc,brStrokeDivider_.get(),X[0]+8*scale_,Y[r+1]-1,X[4]-X[0]-16*scale_,1);
                    for(int k=0;k<4;++k) {
                        const float l=X[k]+10*scale_, w=X[k+1]-X[k]-20*scale_;
                        painter_.DrawWrappedCaption(l10n::Get(kUilCompare[r][k]),D2D1::Point2F(l,Y[r+1]+8*scale_),w,
                            k==0 ? theme.text_secondary : k-1==mode ? theme.text : theme.text_secondary);
                    }
                }
            }
            // 命令栏按钮
            if(lay.uil_tbc_row.bottom>lay.uil_tbc_row.top) {
                label(lay.uil_tbc_row,l10n::Get(I::UilTbcCustomize),l10n::Get(I::UilTbcCustomizeDesc),UilG(CI::Wrench),lay.uil_tbc_button.left-12*scale_);
                button(lay.uil_tbc_button,l10n::Get(I::UilTbcOpen),H::SettingsUiLayout,5);
                const bool tbc_dim=dim_begin(seg[1]==2);
                toggle(lay.uil_tbc_toggle[0],I::UilTbcSort,I::UilTbcSortDesc,UilG(CI::Sort),vm.settings_uil_tb_shown[0],310);
                toggle(lay.uil_tbc_toggle[1],I::UilTbcFilter,I::UilTbcFilterDesc,UilG(CI::Filter),vm.settings_uil_tb_shown[1],311);
                toggle(lay.uil_tbc_toggle[2],I::UilTbcGroup,I::UilTbcGroupDesc,UilG(CI::GroupBy),vm.settings_uil_tb_shown[2],312);
                dim_end(tbc_dim);
            }
            // 界面方案
            if(lay.uil_pf_row[0].bottom>lay.uil_pf_row[0].top) {
                label(lay.uil_pf_row[0],l10n::Get(I::UilPfExport),l10n::Get(I::UilPfExportDesc),UilG(UilIcon::Export),lay.uil_pf_button[0].left-12*scale_);
                button(lay.uil_pf_button[0],l10n::Get(I::UilPfExportBtn),H::SettingsUiLayout,60);
                label(lay.uil_pf_row[1],l10n::Get(I::UilPfImport),l10n::Get(I::UilPfImportDesc),UilG(UilIcon::Import),lay.uil_pf_button[1].left-12*scale_);
                button(lay.uil_pf_button[1],l10n::Get(I::UilPfImportBtn),H::SettingsUiLayout,61);
                label(lay.uil_pf_row[2],l10n::Get(I::UilPfContent),l10n::Get(I::UilPfContentDesc),UilG(CI::Copy),lay.uil_pf_button[2].left-12*scale_);
                button(lay.uil_pf_button[2],l10n::Get(vm.settings_uil_show_profile ? I::UilPfHide : I::UilPfView),H::SettingsUiLayout,62);
                button(lay.uil_pf_button[3],l10n::Get(I::UilPfCopy),H::SettingsUiLayout,63,true);
                const auto t=lay.uil_pf_text;
                if(t.bottom>t.top && !vm.settings_uil_profile.empty()) {
                    const auto box=D2D1::RectF(t.left+12*scale_,t.top+8*scale_,t.right-12*scale_,t.bottom-8*scale_);
                    painter_.FillRoundedRect(box,6*scale_,theme.fill_input);
                    float ly=box.top+4*scale_;
                    size_t from=0;
                    while(from<=vm.settings_uil_profile.size()) {
                        const size_t nl=vm.settings_uil_profile.find(L'\n',from);
                        const std::wstring line=vm.settings_uil_profile.substr(from,nl==std::wstring::npos ? std::wstring::npos : nl-from);
                        painter_.DrawText(line,D2D1::RectF(box.left+10*scale_,ly,box.right-8*scale_,ly+17*scale_),compositor_->SmallFormat(),theme.text_secondary);
                        ly+=17*scale_;
                        if(nl==std::wstring::npos) break;
                        from=nl+1;
                    }
                }
            }
            // 标题栏按钮 (mockup .tblist): grip, icon, name over its description,
            // key, switch; a divider row is its label, a hairline and ×. A held row
            // follows the pointer like the 右键菜单 preview's (grip, ghost card,
            // accent insertion line) while the rows between make room.
            static constexpr I kTbNames[]={I::WindowManage,I::TitleBtnLayout,I::Settings,I::TooltipToggleTheme,I::AiAsstTitle};
            static constexpr I kTbDescs[]={I::UilTbDescWin,I::UilTbDescLayout,I::UilTbDescSettings,I::UilTbDescTheme,I::UilTbDescAi};
            static constexpr const wchar_t* kTbKeys[]={L"",L"Alt+L",L"Ctrl+,",L"",L""};
            // The title bar's own glyphs: WinSwitch, UiLayout, gear, contrast, sparkle.
            static constexpr const wchar_t* kTbIcons[]={command_icons::kGlyphWinSwitch,command_icons::kGlyphUiLayout,L"\xE713",L"\xE793",L"\xE99A"};
            const UilTitleDrag tb_drag=LayoutUilTitleDrag(vm,lay);
            auto grip_dots=[&](float x,float cy,const D2D1_COLOR_F& color) {
                for(int col=0;col<2;++col) for(int k=-1;k<=1;++k) {
                    const float gx=x+col*3.5f*scale_, gy=cy+k*4.5f*scale_;
                    painter_.FillRoundedRect(D2D1::RectF(gx-1.1f*scale_,gy-1.1f*scale_,gx+1.1f*scale_,gy+1.1f*scale_),1.1f*scale_,color);
                }
            };
            auto moved=[](D2D1_RECT_F r,float dy) {r.top+=dy;r.bottom+=dy;return r;};
            auto draw_tb_row=[&](int i,float dy,bool held) {
                const auto r=moved(lay.uil_tb_row[i],dy);
                const auto sw=moved(lay.uil_tb_switch[i],dy);
                const int kind=vm.settings_uil_title[static_cast<size_t>(i)].first;
                const bool shown=vm.settings_uil_title[static_cast<size_t>(i)].second;
                const int base=100+i*4;
                const float cy=(r.top+r.bottom)*0.5f;
                const auto card=D2D1::RectF(r.left+2*scale_,r.top+2*scale_,r.right-2*scale_,r.bottom-2*scale_);
                if(held) {
                    painter_.FillRoundedRect(D2D1::RectF(card.left-1*scale_,card.top+3*scale_,card.right+1*scale_,card.bottom+6*scale_),7*scale_,D2D1::ColorF(0.16f,0.2f,0.35f,0.08f));
                    painter_.FillRoundedRect(D2D1::RectF(card.left,card.top+1*scale_,card.right,card.bottom+3*scale_),6*scale_,D2D1::ColorF(0.16f,0.2f,0.35f,0.08f));
                    painter_.FillRoundedRect(card,6*scale_,theme.surface_flyout);
                    painter_.StrokeRoundedRect(card,6*scale_,theme.stroke_card,1.0f);
                } else if(!tb_drag.active && (IsHovered(vm,H::SettingsUiLayout,base) || IsHovered(vm,H::SettingsUiLayout,base+1))) {
                    painter_.FillRoundedRect(card,6*scale_,theme.fill_hover);
                }
                grip_dots(r.left+14*scale_,cy,theme.text_secondary);
                if(kind<0) {
                    const std::wstring label=l10n::Get(I::UilTbDivider);
                    const float lw=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->SmallFormat(),label);
                    painter_.DrawText(label,D2D1::RectF(r.left+34*scale_,cy-9*scale_,r.left+34*scale_+lw+2*scale_,cy+9*scale_),compositor_->SmallFormat(),theme.text_secondary);
                    const float l0=r.left+34*scale_+lw+12*scale_, l1=sw.left-12*scale_;
                    if(l1>l0) painter_.FillRoundedRect(D2D1::RectF(l0,cy-0.5f*scale_,l1,cy+0.5f*scale_),0.5f*scale_,theme.stroke_divider);
                    icon_button(sw,L"\xE711",base+3,true);
                    return;
                }
                const int k=std::clamp(kind,0,4);
                const D2D1_COLOR_F ink=shown ? theme.text : theme.text_secondary;
                painter_.DrawGlyph(kTbIcons[k],D2D1::RectF(r.left+32*scale_,cy-10*scale_,r.left+52*scale_,cy+10*scale_),ink);
                float right=sw.left-12*scale_;
                if(kTbKeys[k][0]) {
                    const float kw=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->SmallFormat(),kTbKeys[k]);
                    painter_.DrawText(kTbKeys[k],D2D1::RectF(right-kw-2*scale_,cy-9*scale_,right+2*scale_,cy+9*scale_),compositor_->SmallFormat(),theme.text_secondary);
                    right-=kw+14*scale_;
                }
                painter_.DrawText(l10n::Get(kTbNames[k]),D2D1::RectF(r.left+64*scale_,r.top+5*scale_,right,r.top+25*scale_),compositor_->TextFormat(),ink);
                painter_.DrawText(l10n::Get(kTbDescs[k]),D2D1::RectF(r.left+64*scale_,r.top+25*scale_,right,r.top+43*scale_),compositor_->SmallFormat(),theme.text_secondary);
                fluent::ControlState st{};st.checked=shown;st.hovered=!tb_drag.active && IsHovered(vm,H::SettingsUiLayout,base);
                painter_.DrawSwitch(sw,L"",st);
            };
            const int tb_rows=(std::min)(lay.uil_tb_count,static_cast<int>(vm.settings_uil_title.size()));
            for(int i=0;i<tb_rows;++i)
                if(!tb_drag.active || i!=tb_drag.held) draw_tb_row(i,tb_drag.active ? tb_drag.dy[static_cast<size_t>(i)] : 0.0f,false);
            if(tb_drag.active && tb_drag.held<tb_rows) {
                const auto gap=lay.uil_tb_row[tb_drag.slot];
                const float gy=tb_drag.slot>tb_drag.held ? gap.bottom-1*scale_ : gap.top+1*scale_;
                painter_.FillRoundedRect(D2D1::RectF(gap.left+8*scale_,gy-1*scale_,gap.right-8*scale_,gy+1*scale_),1*scale_,theme.accent);
                draw_tb_row(tb_drag.held,tb_drag.dy[static_cast<size_t>(tb_drag.held)],true);
            }
            if(lay.uil_tb_warn.bottom>lay.uil_tb_warn.top) {
                const auto w=lay.uil_tb_warn;
                const D2D1_COLOR_F warn=IsHighContrast() ? theme.text : HexColor(vm.dark ? 0xF3B744 : 0xC7860A);
                painter_.DrawGlyph(L"\xE7BA",D2D1::RectF(w.left+18*scale_,w.top+16*scale_,w.left+36*scale_,w.top+34*scale_),warn);
                painter_.DrawWrappedCaption(l10n::Get(I::UilTbWarn),D2D1::Point2F(w.left+54*scale_,w.top+14*scale_),w.right-w.left-70*scale_,theme.text);
            }
            if(lay.uil_tb_actions.bottom>lay.uil_tb_actions.top) {
                const auto a=lay.uil_tb_actions;
                text(with_mode(vm.settings_uil_title_default ? I::UilTbIsDefault : I::UilTbNotDefault),
                    D2D1::RectF(a.left+16*scale_,a.top+19*scale_,lay.uil_tb_add.left-12*scale_,a.top+41*scale_),true);
                button(lay.uil_tb_add,l10n::Get(I::UilTbAddDivider),H::SettingsUiLayout,90);
                button(lay.uil_tb_reset,l10n::Get(I::UilTbReset),H::SettingsUiLayout,91,false,!vm.settings_uil_title_default);
            }
        }
        if(page==12) {
        toggle(lay.confirm_delete_row,I::SettingsConfirmDelete,I::SettingsConfirmDeleteDesc,L"\xE897",vm.settings_confirm_delete,32);
        toggle(lay.shelf_shake_row,I::SettingsShelfShake,I::SettingsShelfShakeDesc,L"\xE7C2",vm.settings_shelf_shake,82);
        {
            const I sens[]={I::SettingsShelfSensLow,I::SettingsShelfSensMed,I::SettingsShelfSensHigh};const int sens_vals[]={0,1,2};
            const bool dim=dim_begin(!vm.settings_shelf_shake);
            segmented(lay.shelf_sensitivity_card,lay.shelf_sensitivity_row,sens,sens_vals,vm.settings_shelf_sensitivity,H::SettingsShelfSensitivity,I::SettingsShelfSensitivity,I::SettingsShelfSensitivityDesc,3,L"\xE8A4");
            dim_end(dim);
        }
        {
            const I pos[]={I::SettingsShelfPosCursor,I::SettingsShelfPosLast,I::SettingsShelfPosRightEdge};const int pos_vals[]={0,1,2};
            segmented(lay.shelf_position_card,lay.shelf_position_row,pos,pos_vals,vm.settings_shelf_position,H::SettingsShelfPosition,I::SettingsShelfPosition,I::SettingsShelfPositionDesc,3,L"\xE8A7");
        }
        toggle(lay.shelf_auto_hide_row,I::SettingsShelfAutoHide,I::SettingsShelfAutoHideDesc,L"\xE718",vm.settings_shelf_auto_hide,83);
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
        // 网络位置.
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
            toggle(lay.global_search_double_ctrl_row,I::GlobalSearchDoubleCtrl,I::GlobalSearchDoubleCtrlDesc,L"\xE765",vm.settings_global_search_double_ctrl,40);
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