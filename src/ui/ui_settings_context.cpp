#include "ui_renderer.h"
#include "ui_renderer_internal.h"

#include "../app/keymap.h"
#include <algorithm>
#include <string>
#include <utility>

namespace pulse::ui {
namespace {

bool PulseMenuDark(const Theme& theme) {
    return theme.bg.r * 0.30f + theme.bg.g * 0.59f + theme.bg.b * 0.11f < 0.5f;
}

// Switch rows of a surface as the list draws them, and how many are off.
std::pair<int,int> PulseMenuCounts(const WindowViewModel& vm,app::BuiltinMenuSurface surface) {
    int total=0,off=0;
    for(const auto& row:app::BuiltinMenuOrderedRows(surface,{})) {
        if(row.fixed!=app::BuiltinFixedRow::None || row.item>=app::BuiltinMenuItem::Count) continue;
        const size_t i=vm.settings_builtin_first+static_cast<size_t>(row.item);
        if(i>=vm.settings_items.size()) continue;
        ++total;
        if(!vm.settings_items[i].on) ++off;
    }
    return {total,off};
}

} // namespace

void MainRenderer::DrawSettingsContext(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    using I=l10n::StringId;
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    // Intro and the layer cards (MakeSettingsLayout), as on 系统右键菜单.
    painter_.DrawWrappedCaption(l10n::Get(I::ContextMenuDesc),D2D1::Point2F(l.ctx_intro.left,l.ctx_intro.top+6*scale_),
                                l.ctx_intro.right-l.ctx_intro.left,theme.text_secondary);
    DrawCtxLayerCards(l.ctx_layer[0],l.ctx_layer[1],false,IsHovered(vm,HitTestResult::SettingsCtxPivot,6),theme);
    DrawSettingsPulseMenu(vm,rect,theme);
}

// Same cards, rows, chips and tabs as DrawSettingsSysCtx.
void MainRenderer::DrawSettingsPulseMenu(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    using I=l10n::StringId;
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    if(l.pulse_card.bottom<=l.pulse_card.top) return;
    auto* dc=compositor_->Dc();
    auto fill=[&](const D2D1_RECT_F& r,float radius,const D2D1_COLOR_F& c) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(c,&b);
        if(b) dc->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),b.Get());
    };
    auto stroke=[&](const D2D1_RECT_F& r,float radius,const D2D1_COLOR_F& c,float w) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(c,&b);
        if(b) dc->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),b.Get(),w);
    };
    auto hline=[&](float x0,float x1,float y,const D2D1_COLOR_F& c) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(c,&b);
        if(b) dc->DrawLine(D2D1::Point2F(x0,y),D2D1::Point2F(x1,y),b.Get(),1);
    };
    const float s=scale_;
    const bool dark=PulseMenuDark(theme);
    const D2D1_COLOR_F amber=dark ? D2D1::ColorF(0xE8B04A) : D2D1::ColorF(0xB7791F);
    const D2D1_COLOR_F subtle=WithAlpha(theme.text,dark ? 0.06f : 0.05f);
    IDWriteTextFormat* title_format=SysCtxTitleFormat();
    auto card=[&](const D2D1_RECT_F& r) {
        painter_.FillRoundedRect(r,10*s,theme.fill_input);
        painter_.StrokeRoundedRect(r,10*s,theme.stroke_card,1);
    };
    auto hot_hit=[&](int index) {return IsHovered(vm,HitTestResult::SettingsToggle,index);};
    static constexpr I kTabs[]={I::ContextTabItem,I::ContextTabBackground,I::ContextTabRow};
    static constexpr const wchar_t* kTabGlyph[]={L"\xE8A5",L"\xE7C4",L"\xE8FD"};

    // ---- left card: the three surfaces, then Windows and other software ----
    if(l.pulse_tabs.right>l.pulse_tabs.left) {
        card(l.pulse_tabs);
        auto group=[&](const D2D1_RECT_F& r,const wchar_t* text) {
            painter_.DrawText(text,D2D1::RectF(r.left+10*s,r.top+6*s,r.right,r.bottom),compositor_->SmallFormat(),theme.text_secondary);
        };
        group(l.pulse_group[0],l10n::Get(I::ContextPulseMenu).c_str());
        for(int i=0;i<3;++i) {
            const auto& r=l.pulse_tab[i];
            const bool active=vm.settings_context_tab==i;
            if(active) {
                painter_.FillRoundedRect(r,6*s,theme.fill_selected);
                painter_.FillRoundedRect(D2D1::RectF(r.left,r.top+8*s,r.left+3*s,r.bottom-8*s),1.5f*s,theme.accent);
            } else if(hot_hit(64+i)) {
                painter_.FillRoundedRect(r,6*s,theme.fill_hover);
            }
            const float mid=(r.top+r.bottom)*0.5f;
            DrawIconText(r.left+10*s,mid-8*s,16*s,16*s,kTabGlyph[i],L"",theme.text_secondary,0.75f);
            // "12 · 7": the switched-off count in amber; a dot when reordered.
            const auto surface=static_cast<app::BuiltinMenuSurface>(i);
            const auto [total,off]=PulseMenuCounts(vm,surface);
            float cr=r.right-10*s;
            if(PulseMenuOrderCustom(vm,surface)) {
                painter_.FillRoundedRect(D2D1::RectF(cr-6*s,mid-3*s,cr,mid+3*s),3*s,theme.accent);
                cr-=10*s;
            }
            if(off>0) {
                const std::wstring text=L" · "+std::to_wstring(off);
                const float w=CellTextWidth(text,true);
                painter_.DrawText(text,D2D1::RectF(cr-w-2*s,r.top,cr+2*s,r.bottom),compositor_->SmallFormat(),amber,
                                  fluent::HorizontalAlignment::Right);
                cr-=w;
            }
            const std::wstring count=std::to_wstring(total);
            const float cw=CellTextWidth(count,true);
            painter_.DrawText(count,D2D1::RectF(cr-cw-2*s,r.top,cr+2*s,r.bottom),compositor_->SmallFormat(),theme.text_secondary,
                              fluent::HorizontalAlignment::Right);
            painter_.DrawText(l10n::Get(kTabs[i]),D2D1::RectF(r.left+36*s,r.top,cr-cw-6*s,r.bottom),compositor_->TextFormat(),theme.text);
        }
        hline(l.pulse_group[1].left+6*s,l.pulse_group[1].right-6*s,l.pulse_group[1].top-6*s,theme.stroke_divider);
        group(l.pulse_group[1],l10n::Get(l10n::StringId::WindowsAndOtherApps).c_str());
        // Opens 系统右键菜单 (page 6), where both switches per entry live.
        if(const auto r=l.context_system_hint;r.bottom>r.top) {
            const bool hover=IsHovered(vm,HitTestResult::SettingsNav,6);
            if(hover) painter_.FillRoundedRect(r,6*s,theme.fill_hover);
            const float mid=(r.top+r.bottom)*0.5f;
            DrawIconText(r.left+10*s,mid-8*s,16*s,16*s,L"\xE7F4",L"",theme.text_secondary,0.75f);
            DrawIconText(r.right-24*s,mid-7*s,14*s,14*s,L"\xE72A",L"",hover ? theme.accent : theme.text_secondary,0.7f);
            painter_.DrawText(l10n::Get(l10n::StringId::WindowsAppCommands).c_str(),D2D1::RectF(r.left+36*s,r.top,r.right-28*s,r.bottom),
                              compositor_->TextFormat(),theme.text);
        }
    }

    // ---- middle card: head, presets, rows ----
    const auto card_rc=l.pulse_card;
    card(card_rc);
    const auto surface=PulseMenuSurface(vm);
    {
        const auto& h=l.pulse_head;
        const auto& title=l10n::Get(kTabs[std::clamp(vm.settings_context_tab,0,2)]);
        const float title_w=CellTextWidth(title,false)*1.08f+4*s;
        painter_.DrawText(title,D2D1::RectF(h.left+14*s,h.top,h.left+14*s+title_w,h.bottom),title_format,theme.text);
        const auto [total,off]=PulseMenuCounts(vm,surface);
        const std::wstring keys=l10n::Get(l10n::StringId::Text3350).c_str()+std::to_wstring(total)+l10n::Get(l10n::StringId::Items_2).c_str()+
                                std::to_wstring(total-off)+l10n::Get(l10n::StringId::Shown).c_str()+std::to_wstring(off)+l10n::Get(l10n::StringId::Hidden_3).c_str();
        painter_.DrawText(keys,D2D1::RectF(h.left+24*s+title_w,h.top,l.pulse_restore.left-8*s,h.bottom),compositor_->SmallFormat(),theme.text_secondary);
        // 恢复默认: the page's quiet head button, live once something changed.
        bool any=vm.settings_builtin_hidden!=0;
        for(int i=0;i<static_cast<int>(app::BuiltinMenuSurface::Count);++i) any|=PulseMenuOrderCustom(vm,static_cast<app::BuiltinMenuSurface>(i));
        const auto& r=l.pulse_restore;
        if(any && hot_hit(67)) painter_.FillRoundedRect(r,6*s,theme.fill_hover);
        const D2D1_COLOR_F color=any ? theme.text_secondary : theme.text_disabled;
        const float mid=(r.top+r.bottom)*0.5f;
        DrawIconText(r.left+6*s,mid-8*s,16*s,16*s,L"\xE777",L"",color,0.62f);
        painter_.DrawText(l10n::Get(I::RestoreDefaults),D2D1::RectF(r.left+24*s,r.top,r.right-4*s,r.bottom),compositor_->SmallFormat(),color);
        hline(card_rc.left,card_rc.right,h.bottom,theme.stroke_divider);
    }
    {
        // Presets as filter chips; 自定义 only reports a hand-made mix.
        const auto active=app::BuiltinMenuPresetFor(vm.settings_builtin_hidden);
        static constexpr I kPresetName[]={I::ContextPresetSlim,I::ContextPresetStandard,I::ContextPresetFull,I::ContextPresetCustom};
        const auto& first=l.pulse_preset[0];
        painter_.DrawText(l10n::Get(l10n::StringId::Preset).c_str(),D2D1::RectF(card_rc.left+12*s,first.top,first.left,first.bottom),
                          compositor_->SmallFormat(),theme.text_secondary);
        for(int i=0;i<4;++i) {
            const auto& r=l.pulse_preset[i];
            const bool on=static_cast<int>(active)==i;
            const bool hover=i<3 && !on && hot_hit(60+i);
            painter_.FillRoundedRect(r,13*s,on ? WithAlpha(theme.accent,0.16f) : hover ? theme.fill_hover : WithAlpha(theme.text,0.0f));
            painter_.StrokeRoundedRect(r,13*s,on ? WithAlpha(theme.accent,0.55f) : theme.stroke_card,1);
            painter_.DrawText(l10n::Get(kPresetName[i]),r,compositor_->SmallFormat(),
                              on ? theme.text : i==3 ? theme.text_disabled : theme.text_secondary,fluent::HorizontalAlignment::Center);
        }
        hline(card_rc.left,card_rc.right,l.pulse_rows_top.top,theme.stroke_divider);
    }
    // Rows (list order = menu order of the selected surface).
    const auto rows=app::BuiltinMenuOrderedRows(surface,PulseMenuOrder(vm));
    for(size_t k=0;k<rows.size();++k) {
        const auto& row=rows[k];
        D2D1_RECT_F r{};
        bool on=true,hot=false;int hit=-1;
        if(row.fixed!=app::BuiltinFixedRow::None) {
            r=l.pulse_fixed[row.fixed==app::BuiltinFixedRow::Open ? 0 : 1];
        } else {
            const size_t i=vm.settings_builtin_first+static_cast<size_t>(row.item);
            if(i>=l.context_rows.size()||i>=vm.settings_items.size()) continue;
            r=l.context_rows[i];on=vm.settings_items[i].on;hit=100+static_cast<int>(i);
            hot=hot_hit(hit);
        }
        if(r.bottom<=r.top || r.bottom<l.content.top || r.top>l.content.bottom) continue;
        if(hot) painter_.FillRoundedRect(r,8*s,theme.fill_hover);
        const float mid=(r.top+r.bottom)*0.5f;
        const float dim=on ? 1.0f : 0.5f;
        const auto plate=D2D1::RectF(r.left+10*s,mid-14*s,r.left+38*s,mid+14*s);
        painter_.FillRoundedRect(plate,7*s,subtle);
        const wchar_t* glyph=row.glyph ? row.glyph : row.fixed==app::BuiltinFixedRow::Strip ? L"\xE8C6" : L"\xE8A5";
        DrawIconText(plate.left+6*s,plate.top+6*s,16*s,16*s,glyph,L"",WithAlpha(theme.text_secondary,dim),0.8f);
        // Right side: the switch, or 始终显示 for the rows that never hide.
        float badge_right=r.right-12*s;
        if(hit<0) {
            const auto note=l10n::Get(I::ContextAlwaysShown);
            const float w=CellTextWidth(note,true);
            painter_.DrawText(note,D2D1::RectF(badge_right-w-2*s,r.top,badge_right+2*s,r.bottom),compositor_->SmallFormat(),theme.text_secondary);
            DrawIconText(badge_right-w-20*s,mid-8*s,16*s,16*s,L"\xE72E",L"",theme.text_secondary,0.7f);
            badge_right-=w+30*s;
        } else {
            fluent::ControlState state{};state.checked=on;state.hovered=hot;
            const auto sw=D2D1::RectF(badge_right-44*s,mid-11*s,badge_right,mid+11*s);
            painter_.DrawSwitch(sw,L"",state);
            badge_right=sw.left-10*s;
        }
        const float text_left=r.left+50*s;
        auto badge=[&](std::wstring_view text,fluent::BadgeKind kind) {
            const float w=painter_.MeasureBadgeWidth(text);
            if(badge_right-w<text_left+120*s) return;  // the title keeps priority
            fluent::BadgeSpec spec;
            spec.bounds=D2D1::RectF(badge_right-w,mid-10*s,badge_right,mid+10*s);
            spec.text=text;
            spec.kind=kind;
            painter_.DrawBadge(spec);
            badge_right-=w+6*s;
        };
        if(row.shortcut && *row.shortcut) {
            const std::wstring key=app::ShownShortcut(row.shortcut);  // the user's key
            if(!key.empty()) badge(key,fluent::BadgeKind::Keycap);
        }
        if(hit>=0 && vm.settings_context_tab!=2 && app::BuiltinMenuShared(row.item)) badge(l10n::Get(I::ContextShared),fluent::BadgeKind::Accent);
        if(row.submenu) badge(l10n::Get(l10n::StringId::Submenu).c_str(),fluent::BadgeKind::Neutral);
        const auto label=app::BuiltinMenuRowLabel(row);
        painter_.DrawText(label,D2D1::RectF(text_left,r.top,std::max(text_left+40*s,badge_right-6*s),r.bottom),compositor_->TextFormat(),
                          on ? theme.text : theme.text_secondary);
    }

    // ---- right card: live preview ----
    if(l.pulse_side.right>l.pulse_side.left) {
        card(l.pulse_side);
        const auto& t=l.pulse_side_tab;
        painter_.FillRoundedRect(t,6*s,theme.fill_selected);
        painter_.DrawText(sysctx_text::TabPreview(),t,compositor_->TextFormat(),theme.text,fluent::HorizontalAlignment::Center);
        hline(l.pulse_side.left,l.pulse_side.right,t.bottom+8*s,theme.stroke_divider);
        if(const auto& n=l.pulse_side_note;n.bottom>n.top) {
            painter_.FillRoundedRect(n,8*s,subtle);
            painter_.StrokeRoundedRect(n,8*s,theme.stroke_card,1);
            DrawIconText(n.left+12*s,n.top+12*s,16*s,16*s,L"\xE946",L"",theme.text_secondary,0.8f);
            painter_.DrawWrappedCaption(PulseMenuSideNote(),D2D1::Point2F(n.left+36*s,n.top+12*s),n.right-n.left-48*s,theme.text_secondary);
        }
    }
    // Live preview built from the same rows the real menu keeps. Movable
    // rows show a grip on hover and can be dragged to a new place.
    const auto pv=l.pulse_preview;
    if(pv.bottom<=pv.top) return;
    if(vm.settings_context_tab!=2)
        painter_.DrawText(l10n::Get(vm.settings_context_tab==1 ? I::ContextPreviewBackground : I::ContextPreviewItem),
                          D2D1::RectF(pv.left,pv.top,pv.right,pv.top+18*s),compositor_->SmallFormat(),theme.text_secondary);
    if(l.pulse_order_reset.right>l.pulse_order_reset.left) {
        const auto label=l10n::Get(I::ContextOrderReset);
        const float w=CellTextWidth(label,true);
        if(IsHovered(vm,HitTestResult::SettingsToggle,kSettingsMenuOrderReset))
            fill(D2D1::RectF(l.pulse_order_reset.right-w-8*s,l.pulse_order_reset.top,l.pulse_order_reset.right+4*s,l.pulse_order_reset.bottom),4*s,theme.fill_hover);
        painter_.DrawText(label,D2D1::RectF(l.pulse_order_reset.right-w-2*s,l.pulse_order_reset.top,l.pulse_order_reset.right+2*s,l.pulse_order_reset.bottom),
                          compositor_->SmallFormat(),theme.accent);
    }
    const auto box=PulsePreviewBox(pv,s);
    fill(D2D1::RectF(box.left+1*s,box.top+2*s,box.right+1*s,box.bottom+3*s),8*s,D2D1::ColorF(0,0,0,0.06f));
    fill(box,8*s,theme.surface_flyout);
    stroke(box,8*s,theme.stroke_card,1);
    painter_.DrawText(l10n::Get(vm.settings_context_tab==2 ? I::ContextOrderHintRow : I::ContextOrderHint),
                      D2D1::RectF(box.left+2*s,box.bottom+6*s,box.right,pv.bottom),compositor_->SmallFormat(),theme.text_secondary);
    const auto pl=LayoutPulsePreview(vm,box,s);
    auto hovered=[&](const app::BuiltinMenuRow& row) {
        return !pl.dragging && IsHovered(vm,HitTestResult::SettingsToggle,PulseMenuRowHit(row));
    };
    auto grip=[&](float x,float cy,const D2D1_COLOR_F& color) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(color,&b);
        if(!b) return;
        for(int col=0;col<2;++col) for(int k=-1;k<=1;++k)
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+col*3.5f*s,cy+k*4.5f*s),1.1f*s,1.1f*s),b.Get());
    };
    auto ghost_card=[&](const D2D1_RECT_F& g) {
        fill(D2D1::RectF(g.left-1*s,g.top+3*s,g.right+1*s,g.bottom+6*s),7*s,D2D1::ColorF(0.16f,0.2f,0.35f,0.08f));
        fill(D2D1::RectF(g.left,g.top+1*s,g.right,g.bottom+3*s),6*s,D2D1::ColorF(0.16f,0.2f,0.35f,0.08f));
        fill(g,6*s,theme.surface_flyout);
        stroke(g,6*s,theme.stroke_card,1);
    };
    auto insertion=[&](float x0,float y0,float x1,float y1) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(theme.accent,&b);
        if(!b) return;
        dc->DrawLine(D2D1::Point2F(x0,y0),D2D1::Point2F(x1,y1),b.Get(),2*s);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inner;dc->CreateSolidColorBrush(theme.surface_flyout,&inner);
        if(inner) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0,y0),3.5f*s,3.5f*s),inner.Get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x0,y0),3*s,3*s),b.Get(),1.5f*s);
    };
    if(vm.settings_context_tab==2) {
        // One list row with the hover buttons that stay on.
        const auto row=D2D1::RectF(box.left+8*s,box.top+12*s,box.right-8*s,box.bottom-12*s);
        fill(row,4*s,theme.fill_hover);
        DrawIconText(row.left+8*s,row.top+6*s,20*s,20*s,L"\xE8A5",L"",theme.text_secondary,0.8f);
        painter_.DrawText(L"notes.txt",D2D1::RectF(row.left+36*s,row.top,row.right-90*s,row.bottom),compositor_->TextFormat(),theme.text);
        for(const auto& v:pl.rows) {
            if(hovered(v.row)) fill(v.rect,4*s,theme.fill_pressed);
            DrawIconText(v.rect.left+4*s,v.rect.top+4*s,16*s,16*s,v.row.glyph,L"",theme.text_secondary,0.75f);
        }
        if(pl.dragging) {
            const float x=(pl.gap.left+pl.gap.right)*0.5f;
            insertion(x,pl.gap.top+1*s,x,pl.gap.bottom-1*s);
            ghost_card(pl.ghost);
            DrawIconText(pl.ghost.left+4*s,pl.ghost.top+4*s,16*s,16*s,pl.drag_row.glyph,L"",theme.text,0.8f);
        }
        return;
    }
    auto draw_row=[&](const app::BuiltinMenuRow& row,const D2D1_RECT_F& rect) {
        float y=rect.top;
        if(row.item==app::BuiltinMenuItem::Tags) {
            static constexpr D2D1_COLOR_F kSwatch[]={{0.93f,0.27f,0.27f,1},{0.96f,0.56f,0.13f,1},{0.13f,0.73f,0.38f,1},{0.92f,0.70f,0.03f,1},{0.66f,0.33f,0.97f,1},{0.23f,0.51f,0.96f,1},{0.58f,0.64f,0.72f,1}};
            float sx=rect.left+16*s;
            for(const auto& c:kSwatch) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(c,&b);
                if(b) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx+5*s,y+kPulsePreviewSwatches*s*0.5f),5*s,5*s),b.Get());
                sx+=28*s;
            }
            y+=kPulsePreviewSwatches*s;
        }
        const auto r=D2D1::RectF(rect.left,y,rect.right,y+kPulsePreviewRow*s);
        if(row.fixed==app::BuiltinFixedRow::Strip) {
            static constexpr const wchar_t* kStrip[]={L"\xE8C6",L"\xE8C8",L"\xE74D",L"\xE8AC"};
            float sx=r.left+28*s;
            for(const auto* g:kStrip) {DrawIconText(sx,r.top+9*s,16*s,16*s,g,L"",theme.text,0.8f);sx+=44*s;}
            return;
        }
        if(row.glyph) DrawIconText(r.left+10*s,r.top+9*s,16*s,16*s,row.glyph,L"",theme.text,0.8f);
        const auto label=row.item==app::BuiltinMenuItem::Tags ? l10n::Get(I::TagsEllipsis) : app::BuiltinMenuRowLabel(row);
        painter_.DrawText(label,D2D1::RectF(r.left+36*s,r.top,r.right-8*s,r.bottom),compositor_->SmallFormat(),theme.text);
        if(row.submenu) {
            DrawIconText(r.right-22*s,r.top+11*s,12*s,12*s,L"\xE76C",L"",theme.text_secondary,0.7f);
        } else if(row.shortcut) {
            const std::wstring key=app::ShownShortcut(row.shortcut);
            const float w=CellTextWidth(key.c_str(),true);
            painter_.DrawText(key.c_str(),D2D1::RectF(r.right-10*s-w,r.top,r.right-6*s,r.bottom),compositor_->SmallFormat(),theme.text_secondary);
        }
    };
    const float x0=box.left+6*s,x1=box.right-6*s;
    for(const auto& v:pl.rows) {
        const bool fixed=v.row.fixed!=app::BuiltinFixedRow::None;
        if(hovered(v.row)) {
            if(!fixed) {
                fill(v.rect,4*s,theme.fill_hover);
                grip(box.left+7*s,(v.rect.top+v.rect.bottom)*0.5f,theme.text_secondary);
            } else {
                // Open and the action strip stay on top: a lock where the grip would be.
                DrawIconText(box.left+3*s,(v.rect.top+v.rect.bottom)*0.5f-6*s,12*s,12*s,L"\xE72E",L"",theme.text_secondary,0.7f);
            }
        }
        draw_row(v.row,v.rect);
        if(v.separator_after)
            hline(x0+4*s,x1-4*s,v.rect.bottom+kPulsePreviewSeparator*s*0.5f,theme.stroke_divider);
    }
    if(pl.dragging) {
        const float y=(pl.gap.top+pl.gap.bottom)*0.5f;
        insertion(x0+2*s,y,x1-2*s,y);
        ghost_card(pl.ghost);
        draw_row(pl.drag_row,pl.ghost);
        grip(box.left+7*s,(pl.ghost.top+pl.ghost.bottom)*0.5f,theme.text_secondary);
    }
}

float MainRenderer::SettingsMenuRowStart(const WindowViewModel& vm,const D2D1_RECT_F& rect,int item) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    if(l.pulse_preview.bottom<=l.pulse_preview.top) return NAN;
    WindowViewModel still=vm;still.settings_menu_drag=-1;
    for(const auto& r:LayoutPulsePreview(still,PulsePreviewBox(l.pulse_preview,scale_),scale_).rows)
        if(r.row.fixed==app::BuiltinFixedRow::None && static_cast<int>(r.row.item)==item)
            return vm.settings_context_tab==2 ? r.rect.left : r.rect.top;
    return NAN;
}

app::BuiltinMenuOrder MainRenderer::SettingsMenuDropOrder(const WindowViewModel& vm,const D2D1_RECT_F& rect,float x,float y) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    const auto pv=l.pulse_preview;
    const float slack=40*scale_;
    if(pv.bottom<=pv.top || x<pv.left-slack || x>pv.right+slack || y<pv.top-slack || y>pv.bottom+slack) return {};
    const auto pl=LayoutPulsePreview(vm,PulsePreviewBox(pv,scale_),scale_);
    return pl.dragging ? pl.order : app::BuiltinMenuOrder{};
}
}
