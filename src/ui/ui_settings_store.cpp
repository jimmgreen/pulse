#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "ui_settings_store_status.h"
#include "store_icons.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
using I = l10n::StringId;
using A = PackAction;
bool Present(D2D1_RECT_F r) { return r.right > r.left && r.bottom > r.top; }
D2D1_RECT_F Inset(D2D1_RECT_F r, float x, float y) { return {r.left+x,r.top+y,r.right-x,r.bottom-y}; }
struct StorePainter {
    fluent::Painter& p;
    ID2D1DeviceContext* dc;
    Compositor* comp;
    const Theme& t;
    const WindowViewModel& vm;
    float s;
    bool Hot(A a) const { return IsHovered(vm, HitTestResult::SettingsPackAction, static_cast<int>(a)); }
    void Text(std::wstring_view v,D2D1_RECT_F r,bool small_text=false,D2D1_COLOR_F* color=nullptr) {
        if (Present(r)) p.DrawText(v,r,small_text?comp->SmallFormat():comp->TextFormat(),color?*color:small_text?t.text_secondary:t.text);
    }
    void Caption(std::wstring_view v,D2D1_RECT_F r,D2D1_COLOR_F color) {
        if (!Present(r)) return;
        dc->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        p.DrawWrappedCaption(v,{r.left,r.top},r.right-r.left,color);
        dc->PopAxisAlignedClip();
    }
    void Summary(std::wstring_view value,D2D1_RECT_F r) {
        if(!Present(r))return;
        const float width=r.right-r.left,height=r.bottom-r.top;
        if(p.MeasureWrappedCaptionHeight(value,width)<=height+.5f) {
            p.DrawWrappedCaption(value,{r.left,r.top},width,t.text_secondary);return;
        }
        size_t low=0,high=value.size();
        while(low<high) {
            const size_t middle=low+(high-low+1)/2;
            const auto candidate=std::wstring(value.substr(0,middle))+L"…";
            if(p.MeasureWrappedCaptionHeight(candidate,width)<=height+.5f)low=middle;else high=middle-1;
        }
        if(low&&value[low-1]>=0xD800&&value[low-1]<=0xDBFF)--low;
        p.DrawWrappedCaption(std::wstring(value.substr(0,low))+L"…",{r.left,r.top},width,t.text_secondary);
    }
    void Mono(std::wstring_view v,D2D1_RECT_F r,const D2D1_COLOR_F* color=nullptr,bool center=false) {
        if(!Present(r)||v.empty())return;
        ComPtr<IDWriteTextLayout> layout;
        if(SUCCEEDED(comp->DwriteFactory()->CreateTextLayout(v.data(),static_cast<UINT32>(v.size()),comp->SmallFormat(),r.right-r.left,r.bottom-r.top,&layout))) {
            layout->SetFontFamilyName(L"Consolas",{0,static_cast<UINT32>(v.size())});
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if(center)layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            ComPtr<ID2D1SolidColorBrush> brush;dc->CreateSolidColorBrush(color?*color:t.text_secondary,&brush);
            if(brush.get()){dc->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);comp->DrawTextLayout({r.left,r.top},layout.get(),brush.get());dc->PopAxisAlignedClip();}
        }
    }
    void Heading(std::wstring_view v,D2D1_RECT_F r) {
        if (!Present(r)) return;
        dc->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        p.DrawWrappedSection(v,{r.left,r.top},r.right-r.left,t.text);
        dc->PopAxisAlignedClip();
    }
    void Card(D2D1_RECT_F r,float radius=10) {
        if (!Present(r)) return;
        p.FillRoundedRect(r,radius*s,t.surface_card);p.StrokeRoundedRect(r,radius*s,t.stroke_card);
    }
    void Line(float left,float right,float y,D2D1_COLOR_F color) { if(right>left) p.FillRoundedRect({left,y,right,y+s},0,color); }
    void Icon(StoreIcon icon,D2D1_RECT_F r,D2D1_COLOR_F color) { if(Present(r)) DrawStoreIcon(dc,icon,r,color); }
    void Tile(StoreIcon icon,D2D1_RECT_F r,D2D1_COLOR_F color,bool solid=false,uint32_t custom_icon=0) {
        if(!Present(r))return;
        // Product tiles (archive_ux_mockup.html): known products carry their own artwork,
        // custom vector packages render inside the gradient tile, others render a white line icon.
        if(solid&&!IsHighContrast()) { DrawProductTile(dc,r,ProductArtFor(icon),color,icon,custom_icon,&t); return; }
        const float rad=((r.right-r.left)>64*s?16:(r.right-r.left)>50*s?14:8)*s;
        p.FillRoundedRect(r,rad,solid?color:WithAlpha(color,.09f));
        const float pad=(r.right-r.left)*.25f;
        const auto inner=Inset(r,pad,pad);
        const D2D1_COLOR_F fg=solid?t.text:color;
        if(!IsCustomExtensionIcon(custom_icon)||!DrawExtensionIcon(dc,custom_icon,inner,fg,&t))
            Icon(icon,inner,fg);
    }
    void Button(D2D1_RECT_F r,std::wstring_view v,A a,bool primary=false,bool enabled=true) {
        if(!Present(r)||v.empty())return;
        fluent::ControlState state{};state.enabled=enabled;state.hovered=enabled&&Hot(a);
        p.DrawButton({r,v,{},primary?fluent::ButtonKind::Primary:fluent::ButtonKind::Standard,state});
    }
    void Switch(D2D1_RECT_F r,bool checked,A a,bool enabled=true) {
        if(!Present(r))return;
        fluent::ControlState state{};state.checked=checked;state.enabled=enabled;state.hovered=enabled&&Hot(a);p.DrawSwitch(r,L"",state);
    }
    void Pill(D2D1_RECT_F r,std::wstring_view value,D2D1_COLOR_F color,bool selected=false,bool dot=false) {
        if(!Present(r))return;
        p.FillRoundedRect(r,(r.bottom-r.top)/2,WithAlpha(color,.095f));
        if(selected)p.StrokeRoundedRect(r,(r.bottom-r.top)/2,WithAlpha(color,.4f));
        if(dot&&(r.right-r.left)>24*s) {
            const float cy=(r.top+r.bottom)/2;
            p.FillRoundedRect({r.left+9*s,cy-3*s,r.left+15*s,cy+3*s},3*s,color);
            p.DrawText(value,{r.left+12*s,r.top,r.right,r.bottom},comp->SmallFormat(),color,fluent::HorizontalAlignment::Center);
        } else {
            p.DrawText(value,r,comp->SmallFormat(),color,fluent::HorizontalAlignment::Center);
        }
    }
    void Input(D2D1_RECT_F r) {
        if(!Present(r))return;
        fluent::ControlState state{};
        p.DrawTextFieldFrame(r, state, true);
    }
    void Combo(D2D1_RECT_F r,std::wstring_view label,A a,bool enabled=true) {
        Input(r);if(!Present(r))return;
        if(Hot(a)&&enabled)p.FillRoundedRect(r,7*s,t.fill_hover);
        auto color=enabled?t.text:t.text_disabled;
        Text(label,{r.left+10*s,r.top,r.right-27*s,r.bottom},true,&color);
        Icon(StoreIcon::ChevronDown,{r.right-22*s,(r.top+r.bottom)/2-7*s,r.right-8*s,(r.top+r.bottom)/2+7*s},color);
    }
    void Note(std::wstring_view value,D2D1_RECT_F r,fluent::InfoBarKind kind) {
        if(!Present(r))return;
        auto color=p.StatusToneColor(kind);p.FillRoundedRect(Inset(r,-12*s,-6*s),6*s,WithAlpha(color,.08f));Caption(value,r,t.text_secondary);
    }
    // The host right-click menu around the plugin's rows, drawn with the real menu's
    // pieces (surface, rows, group tint, accent rail) at store_view::MenuPreviewHeight.
    void Menu(D2D1_RECT_F r,const std::vector<SevenZipMenuRow>& rows,bool submenu,bool expanded,std::wstring_view name) {
        if(!Present(r))return;
        // A soft drop shadow, so the menu reads as floating over the stage.
        const float radius=static_cast<float>(t.radius_flyout)*s;
        for(int i=4;i>=1;--i) {
            const float k=static_cast<float>(i);
            p.FillRoundedRect({r.left-k*s,r.top-k*.5f*s+2*s,r.right+k*s,r.bottom+k*s+2*s},radius+k*s,
                              D2D1::ColorF(0.0f,0.0f,0.0f,vm.dark?.10f:.022f));
        }
        p.DrawMenuSurface(r);
        dc->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float y=r.top+8*s;
        auto separator=[&] { Line(r.left+8*s,r.right-8*s,y+4*s,t.stroke_divider);y+=9*s; };
        auto item=[&](StoreIcon icon,std::wstring_view title,std::wstring_view detail,std::wstring_view shortcut,
                      float left,bool child,bool grouped,bool primary,uint32_t custom_icon) {
            const float height=(detail.empty()?28:40)*s;
            fluent::MenuItemSpec spec{};spec.bounds={left,y,r.right,y+height};
            spec.text=title;spec.detail=detail;spec.shortcut=shortcut;
            if(IsCustomExtensionIcon(custom_icon)) spec.custom_icon=custom_icon;
            else spec.vector_icon=icon;
            spec.has_submenu=child;spec.primary=primary;spec.group_tint=grouped;spec.state.enabled=true;
            p.DrawMenuItem(spec);y+=height+(grouped?2*s:0);
        };
        auto host=[&](StoreIcon icon,std::wstring_view title,std::wstring_view shortcut) {
            item(icon,title,{},shortcut,r.left,false,false,false,0);
        };
        const StoreIcon actions[]={StoreIcon::Cut,StoreIcon::Copy,StoreIcon::Rename,StoreIcon::Trash};
        const float width=(r.right-r.left-16*s)/4;
        for(int i=0;i<4;++i) {
            const float x=r.left+8*s+(i+.5f)*width;
            Icon(actions[i],{x-8*s,y+5*s,x+8*s,y+21*s},t.text_secondary);
        }
        y+=32*s;Line(r.left+8*s,r.right-8*s,y,t.stroke_divider);
        host(StoreIcon::Open,l10n::Get(I::Open),L"Enter");
        separator();
        // The group: one tint (the list selection pair at half strength, like the live menu).
        const float group_top=y,group_h=store_view::MenuPreviewGroupHeight(rows,submenu&&expanded)*s;
        auto from=t.list_selected_start,to=t.list_selected_end;from.a*=.5f;to.a*=.5f;
        p.FillHorizontalGradientRect({r.left+4*s,y,r.right-4*s,y+group_h-4*s},7*s,from,to);
        if(submenu) {
            item(StoreIcon::Box,name,{},{},r.left,true,true,false,0);
            if(expanded) {
                p.FillRoundedRect({r.left+12*s,y,r.left+14*s,group_top+group_h-10*s},1*s,WithAlpha(t.accent,.5f));
                for(const auto& row:rows) item(row.vector_icon,row.title,row.detail,{},r.left+14*s,false,true,false,row.icon);
                separator();
                item(StoreIcon::Gear,l10n::Get(I::Settings),{},{},r.left+14*s,false,true,false,0);
            }
        } else {
            const auto color=MixColor(t.accent,t.text_secondary,.35f);
            p.DrawText(name,{r.left+16*s,y+2*s,r.right-34*s,y+24*s},p.RampFormat(fluent::Painter::TextRamp::Section),color);
            Icon(StoreIcon::Gear,{r.right-28*s,y+7*s,r.right-16*s,y+19*s},t.text_secondary);
            y+=26*s;
            for(size_t ri=0;ri<rows.size();++ri) {
                const auto& row=rows[ri];
                item(row.vector_icon,row.title,row.detail,{},r.left,false,true,ri==0&&!row.detail.empty(),row.icon);
            }
        }
        y=group_top+group_h;
        separator();
        host(StoreIcon::Properties,l10n::Get(I::Properties),L"Alt+Enter");
        dc->PopAxisAlignedClip();
    }
    void Option(D2D1_RECT_F r,std::wstring_view title,std::wstring_view desc,bool selected,A a) {
        if(!Present(r))return;
        p.FillRoundedRect(r,8*s,selected?WithAlpha(t.accent,.095f):Hot(a)?t.fill_hover:t.surface_card);
        p.StrokeRoundedRect(r,8*s,selected?WithAlpha(t.accent,.6f):t.stroke_card);
        const D2D1_RECT_F radio{r.left+12*s,r.top+15*s,r.left+26*s,r.top+29*s};
        fluent::ControlState state{};state.checked=selected;state.hovered=Hot(a);
        p.DrawRadioButton(radio,L"",state,14.0f);
        p.DrawText(title,{r.left+36*s,r.top+8*s,r.right-10*s,r.top+32*s},comp->HeaderFormat(),t.text);
        Caption(desc,{r.left+36*s,r.top+34*s,r.right-10*s,r.bottom-6*s},t.text_secondary);
    }
    void Row(D2D1_RECT_F r,StoreIcon icon,std::wstring_view title,std::wstring_view desc,float reserve=16) {
        if(!Present(r))return;
        Tile(icon,{r.left+16*s,r.top+16*s,r.left+48*s,r.top+48*s},t.text_secondary);
        // The caption starts under the title at the UI text size (36 DIP at 100%).
        const float title_h=24*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        Text(title,{r.left+62*s,r.top+10*s,r.right-reserve*s,r.top+10*s+title_h});
        Caption(desc,{r.left+62*s,r.top+12*s+title_h,r.right-reserve*s,r.bottom-6*s},t.text_secondary);
    }
};
A Offset(A a,size_t i){return static_cast<A>(static_cast<int>(a)+static_cast<int>(i));}
void DrawExtensionDetailHeader(StorePainter& p, const SettingsLayout::DetailHeader& d,
                               const WindowViewModel::NativeSettingsHeader& h, A back) {
    const float s = p.s;
    auto green=p.p.StatusToneColor(fluent::InfoBarKind::Success);
    if(p.Hot(back))p.p.FillRoundedRect(d.crumb,5*s,p.t.fill_hover);
    p.Icon(StoreIcon::ChevronLeft,{d.crumb.left+4*s,d.crumb.top+7*s,d.crumb.left+18*s,d.crumb.bottom-7*s},p.t.text_secondary);
    auto crumb=d.crumb;crumb.left+=24*s;p.Text(l10n::Get(I::StoreTitle),crumb,true);
    p.Icon(StoreIcon::ChevronRight,{d.crumb_name.left,d.crumb_name.top+7*s,d.crumb_name.left+12*s,d.crumb_name.bottom-7*s},p.t.text_disabled);
    auto crumb_name=d.crumb_name;crumb_name.left+=22*s;p.Text(h.name,crumb_name,true);
    p.Tile(h.icon,d.icon,HexColor(h.icon_color),true,h.custom_icon);
    if(Present(d.name)) {
        ComPtr<IDWriteTextLayout> title;
        if(SUCCEEDED(p.comp->DwriteFactory()->CreateTextLayout(h.name.c_str(),static_cast<UINT32>(h.name.size()),p.comp->HeaderFormat(),d.name.right-d.name.left,d.name.bottom-d.name.top,&title))) {
            title->SetFontSize(26*s,{0,static_cast<UINT32>(h.name.size())});title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<ID2D1SolidColorBrush> brush; p.dc->CreateSolidColorBrush(p.t.text, &brush);
            if (brush.get()) { p.dc->PushAxisAlignedClip(d.name, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); p.comp->DrawTextLayout({d.name.left,d.name.top},title.get(),brush.get()); p.dc->PopAxisAlignedClip(); }
        }
    }
    auto sub=d.sub;
    if(h.official){p.Icon(StoreIcon::Badge,{sub.left,sub.top+2*s,sub.left+14*s,sub.top+16*s},p.t.accent);sub.left+=20*s;}
    const auto subtitle=ExtensionDetailSubtitle(h.official,h.summary);
    p.Summary(subtitle,sub);
    if(!h.installed) {
        p.Pill(d.badges[0],l10n::Get(I::StoreDetailNotInstalled),p.t.text_secondary,false,false);
    } else {
        p.Pill(d.badges[0],l10n::Get(h.enabled?I::StoreExtensionEnabled:I::StoreExtensionDisabled),h.enabled?green:p.t.text_secondary,false,h.enabled);
    }
    if(!h.version.empty())p.Pill(d.badges[2],h.version,p.t.text_secondary);
    if(!h.status_text.empty()&&Present(d.status_pill)) {
        const auto tone=h.status_kind==2?green
                       :h.status_kind==3?p.p.StatusToneColor(fluent::InfoBarKind::Warning)
                       :h.status_kind==4?p.p.StatusToneColor(fluent::InfoBarKind::Error)
                                        :p.t.text_secondary;
        p.Pill(d.status_pill,h.status_text,tone,false,h.status_kind>=2);
    }
    if(!h.action_label.empty()) p.Button(d.install_button,h.action_label,A::StoreDetailInstall,true,h.action_enabled);
    if(h.installed) {
        p.Text(l10n::Get(h.enabled ? I::StoreExtensionEnabled : I::StoreExtensionDisabled),d.enable_label,true);
        p.Switch(d.enable,h.enabled,A::StoreDetailEnable);
        p.Button(d.more,L" ",A::StoreDetailMore);p.Icon(StoreIcon::More,Inset(d.more,8*s,8*s),p.t.text_secondary);
    }
}
void DrawExtensionDetailTabs(StorePainter& p, const SettingsLayout::DetailHeader& d, int count, int selected, A action) {
    if (count <= 0) return;
    selected = (std::clamp)(selected, 0, count - 1);
    const float s = p.s;
    constexpr I tabs[]={I::SzTabSettings,I::SzTabMenu,I::SzTabFormats,I::SzTabAbout};
    for(int i=0;i<count&&i<SettingsLayout::DetailHeader::kMaxTabs;++i) {
        auto r=d.tabs[i];p.Line(r.left,r.right,r.bottom-s,p.t.stroke_divider);
        const A tab_action = d.tab_count > i ? static_cast<A>(d.tab_actions[i]) : Offset(action,i);
        const std::wstring& title = (d.tab_count > i && !d.tab_titles[i].empty())
            ? d.tab_titles[i]
            : l10n::Get(d.tab_count > i ? d.tab_labels[i] : tabs[i]);
        if(p.Hot(tab_action))p.p.FillRoundedRect(Inset(r,0,4*s),6*s,p.t.fill_hover);
        p.p.DrawText(title,r,i==selected?p.comp->HeaderFormat():p.comp->TextFormat(),i==selected?p.t.text:p.t.text_secondary,fluent::HorizontalAlignment::Center);
        if(i==selected)p.p.FillRoundedRect({r.left+12*s,r.bottom-3*s,r.right-12*s,r.bottom},1.5f*s,p.t.accent);
    }
}
}
D2D1_RECT_F MainRenderer::SettingsStoreField(int field) const { return field>=0&&field<7?settings_store_fields_[field]:D2D1_RECT_F{}; }
D2D1_RECT_F MainRenderer::SettingsStoreQuickControlRect(int action) const {
    const auto it = settings_store_quick_controls_.find(action);
    return it == settings_store_quick_controls_.end() ? D2D1_RECT_F{} : it->second;
}
D2D1_RECT_F MainRenderer::SettingsStoreClip() const {return settings_store_clip_;}

// A plugin page opened for one package: the 7-Zip detail header, drawn with the same pieces.
void MainRenderer::DrawNativeSettingsHeader(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    const auto& d=l.native_head;
    if(!d.on)return;
    StorePainter p{painter_,compositor_->Dc(),compositor_,theme,vm,scale_};
    const auto& h=vm.native_settings_header;
    settings_store_fields_[5] = d.more;
    DrawExtensionDetailHeader(p, d, h, A::StoreBrowse);
    int selected_slot = h.tab == 0 ? (std::max)(0, h.active_plugin_tab) : h.tab;
    if (d.tab_count > 0 && h.tab != 0) {
        const int target_action = static_cast<int>(A::NativeTabBase) + h.tab;
        selected_slot = 0;
        for (int i = 0; i < d.tab_count; ++i) {
            if (d.tab_actions[i] == target_action) { selected_slot = i; break; }
        }
    }
    DrawExtensionDetailTabs(p, d, d.tab_count > 0 ? d.tab_count : (h.has_menu_tab ? 2 : 1), selected_slot, A::NativeTabBase);
}

void MainRenderer::DrawSettingsStore(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    StorePainter p{painter_,compositor_->Dc(),compositor_,theme,vm,scale_};
    const float s=scale_;const auto& v=vm.settings_store_view;
    auto green=painter_.StatusToneColor(fluent::InfoBarKind::Success);
    p.Input(l.store_search);p.Icon(StoreIcon::Search,{l.store_search.left+10*s,l.store_search.top+9*s,l.store_search.left+26*s,l.store_search.top+25*s},theme.text_secondary);
    constexpr StoreIcon chip_icons[]={StoreIcon::None,StoreIcon::Box,StoreIcon::Image,StoreIcon::TextSearch,StoreIcon::Sparkle};
    for(int i=0;i<5;++i) {
        auto r=l.store_chips[i];const bool active=v.category==i;
        if(active) {
            painter_.FillRoundedRect(r,(r.bottom-r.top)/2,WithAlpha(theme.accent,.11f));
            painter_.StrokeRoundedRect(r,(r.bottom-r.top)/2,WithAlpha(theme.accent,.42f));
        } else {
            painter_.FillRoundedRect(r,(r.bottom-r.top)/2,theme.surface_card);
            painter_.StrokeRoundedRect(r,(r.bottom-r.top)/2,theme.stroke_card);
            if(p.Hot(Offset(A::StoreChipBase,i)))painter_.FillRoundedRect(r,(r.bottom-r.top)/2,theme.fill_hover);
        }
        auto tr=r;
        if(chip_icons[i]!=StoreIcon::None&&(r.right-r.left)>38*s) {
            const float cy=(r.top+r.bottom)/2;
            p.Icon(chip_icons[i],{r.left+11*s,cy-6.5f*s,r.left+24*s,cy+6.5f*s},active?theme.accent:theme.text_secondary);
            tr.left+=15*s;
        }
        painter_.DrawText(store_view::Chip(i),tr,compositor_->SmallFormat(),active?theme.accent:theme.text_secondary,fluent::HorizontalAlignment::Center);
    }
    const bool failed=v.state==ExtensionStoreView::State::Failed;
    p.Button(l.store_refresh,l10n::Get(failed?I::RecoveryRetry:I::Refresh),failed?A::StoreRetry:A::StoreRefresh,failed,store_text::CanRefresh(v));
    p.Button(l.store_install_file,l10n::Get(I::StoreInstallFromFile),A::StoreInstallFromFile,false,store_text::CanInstallFromFile(v));
    p.Button(l.store_cancel,l10n::Get(I::Cancel),A::StoreCancel);
    if(failed) {
        p.Card(l.store_failed); // Buttons are drawn again above the error surface.
        auto tile=D2D1::RectF(l.store_failed.left+16*s,l.store_failed.top+18*s,l.store_failed.left+60*s,l.store_failed.top+62*s);
        p.Tile(StoreIcon::CloudOff,tile,theme.danger);
        p.Text(l10n::Get(I::StoreFailed),l.store_state);
        p.Caption(l10n::Get(I::StoreFailedDesc),{l.store_state.left,l.store_state.bottom+4*s,l.store_state.right,l.store_failed.bottom-14*s},theme.text_secondary);
        p.Button(l.store_refresh,l10n::Get(I::RecoveryRetry),A::StoreRetry,true,store_text::CanRefresh(v));
        p.Button(l.store_failed_sources,l10n::Get(I::StoreDownloadSources),A::StoreSources);
        p.Button(l.store_install_file,l10n::Get(I::StoreInstallFromFile),A::StoreInstallFromFile,false,store_text::CanInstallFromFile(v));
    } else {
        auto state_rect=l.store_state;
        if(v.state==ExtensionStoreView::State::Loading&&Present(state_rect)) {p.Icon(StoreIcon::Refresh,{state_rect.left,state_rect.top+7*s,state_rect.left+16*s,state_rect.top+23*s},theme.accent);state_rect.left+=22*s;}
        p.Text(store_text::State(v),state_rect,true);
    }
    store_notify::DrawStatusBanners(painter_, l, v, theme, true, false, p.Hot(A::StoreRestartNow));
    for(auto r:l.store_skeleton)if(Present(r)) {
        p.Card(r);painter_.FillRoundedRect({r.left+18*s,r.top+18*s,r.left+64*s,r.top+64*s},9*s,theme.fill_hover);
        p.Line(r.left+80*s,r.right-24*s,r.top+27*s,theme.fill_hover);
        painter_.FillRoundedRect({r.left+80*s,r.top+45*s,r.right-42*s,r.top+53*s},4*s,theme.fill_hover);
        painter_.FillRoundedRect({r.left+18*s,r.bottom-35*s,r.right-28*s,r.bottom-27*s},4*s,theme.fill_hover);
    }
    p.Text(l10n::Get(I::StoreOfficial),l.store_section);p.Text(l10n::Get(I::StoreOfficialHint),l.store_official_hint,true);
    p.Text(l10n::Get(I::StoreCommunity),l.store_community_section);p.Text(l10n::Get(I::StoreNoMatch),l.store_no_match,true);
    auto draw_catalog_card = [&](const SettingsLayout::StoreCard& c,size_t i) {
        if(i>=v.items.size()||!Present(c.bounds))return;
        const auto& item=v.items[i];
        p.Card(c.bounds,12);
        if(!c.feature&&p.Hot(Offset(A::StoreItemDetailBase,i)))
            painter_.StrokeRoundedRect(c.bounds,12*s,WithAlpha(theme.accent,.48f));
        if(c.feature) {
            const D2D1_GRADIENT_STOP stops[]={{0,WithAlpha(theme.accent,.10f)},{1,WithAlpha(theme.accent,0)}};
            ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> brush;
            if(SUCCEEDED(p.dc->CreateGradientStopCollection(stops,2,&collection))&&SUCCEEDED(p.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({c.bounds.left,c.bounds.top},{c.bounds.right,c.publisher.bottom}),collection.get(),&brush)))p.dc->FillRoundedRectangle(D2D1::RoundedRect(c.bounds,12*s,12*s),brush.get());
        }
        p.Tile(item.icon,c.icon,HexColor(item.icon_color),true,item.custom_icon);
        if(c.feature&&Present(c.name)) {
            ComPtr<IDWriteTextLayout> hero_title;
            if(SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(item.name.c_str(),static_cast<UINT32>(item.name.size()),compositor_->HeaderFormat(),c.name.right-c.name.left,c.name.bottom-c.name.top,&hero_title))) {
                hero_title->SetFontSize(20*s,{0,static_cast<UINT32>(item.name.size())});
                hero_title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                MakeBrush(p.dc,theme.text,brText_);
                compositor_->DrawTextLayout({c.name.left,c.name.top},hero_title.get(),brText_.get());
            }
        } else p.Heading(item.name,c.name);
        p.Pill(c.enabled_badge,l10n::Get(item.enabled?I::StoreExtensionEnabled:I::StoreExtensionDisabled),item.enabled?green:theme.text_secondary,false,item.enabled);
        auto publisher=c.publisher;
        if(item.official){p.Icon(StoreIcon::Badge,{publisher.left,publisher.top+2*s,publisher.left+14*s,publisher.top+16*s},theme.accent);publisher.left+=19*s;}
        std::wstring sub=item.official?l10n::Get(I::StoreOfficial):item.publisher;
        if(c.feature&&!item.version.empty())sub+=L" · "+item.version+(item.size.empty()?L"":L" · "+item.size);
        p.Text(sub,publisher,true);p.Summary(item.summary,c.summary);
        p.Text(item.status,c.status,true);p.Text(item.version+(item.size.empty()?L"":L" · "+item.size),c.version,true);
        p.Button(c.button,item.button_label,Offset(A::StoreItemBase,i),!c.feature||!Present(c.settings),item.action_enabled);
        if(c.feature&&Present(c.settings)) {
            p.Button(c.settings,l10n::Get(I::StoreAllSettings)+L"   ",Offset(A::StoreItemSettingsBase,i));
            const float cy=(c.settings.top+c.settings.bottom)/2;
            p.Icon(StoreIcon::ChevronRight,{c.settings.right-20*s,cy-6*s,c.settings.right-8*s,cy+6*s},theme.text_secondary);
        } else p.Button(c.settings,l10n::Get(I::Settings),Offset(A::StoreItemSettingsBase,i));
    };
    for(size_t i=0;i<l.store_items.size()&&i<v.items.size();++i)draw_catalog_card(l.store_items[i],i);
    settings_store_quick_controls_.clear();
    for(const auto& gq:l.store_generic_quick) {
        if(gq.item_index<0||static_cast<size_t>(gq.item_index)>=v.items.size())continue;
        if(gq.quick_index<0||static_cast<size_t>(gq.quick_index)>=v.quick_cards.size())continue;
        const auto& qc=v.quick_cards[static_cast<size_t>(gq.quick_index)];
        const auto& item=v.items[static_cast<size_t>(gq.item_index)];
        draw_catalog_card(gq.card,static_cast<size_t>(gq.item_index));
        p.Line(gq.section.left,gq.section.right,gq.section.top,theme.stroke_divider);
        p.Heading(qc.title.empty()?l10n::Get(I::StoreQuickTitle):qc.title,gq.title);
        auto saved_color=qc.status.empty()?(v.saved?green:theme.text_secondary):green;
        auto saved_rect=gq.saved;
        if(qc.status.empty()&&Present(saved_rect)&&(saved_rect.right-saved_rect.left)>32*s) {
            const float cy=(saved_rect.top+saved_rect.bottom)/2;
            p.Icon(StoreIcon::Check,{saved_rect.left,cy-6.5f*s,saved_rect.left+13*s,cy+6.5f*s},saved_color);
            saved_rect.left+=17*s;
        }
        p.Text(qc.status.empty()?l10n::Get(v.saved?I::StoreSaved:I::StoreAutoSaved):qc.status,saved_rect,true,&saved_color);
        if(!qc.available) {
            if(!gq.rows.empty())p.Caption(l10n::Get(item.enabled?I::SzLoading:I::SzUnavailable),gq.rows[0],theme.text_secondary);
        } else {
            for(size_t ri=0;ri<gq.rows.size()&&ri<qc.rows.size();++ri) {
                const auto& r=gq.rows[ri];const auto& rv=qc.rows[ri];
                float text_right=r.right;bool stacked_row=false;
                for(const auto& ctrl:gq.controls) {
                    if(ctrl.row!=static_cast<int>(ri))continue;
                    if(ctrl.bounds.top>=r.top+32*s)stacked_row=true;
                    else text_right=(std::min)(text_right,ctrl.bounds.left-12*s);
                }
                if(stacked_row)text_right=r.right;
                // The label block is centred in the row's 56 DIP text band (controls drop below it when stacked).
                const float band=(std::min)(r.bottom-r.top,56*s*static_cast<float>(vm.settings_ui_font_scale)/100);
                const bool has_desc=!rv.description.empty();
                const float ty=r.top+(band-(has_desc?44*s:23*s))/2;
                p.Text(rv.title,{r.left,ty,text_right,ty+23*s});
                if(has_desc)p.Text(rv.description,{r.left,ty+23*s,text_right,ty+44*s},true);
                if(ri+1<gq.rows.size())p.Line(r.left,r.right,r.bottom,theme.stroke_divider);
            }
            for(const auto& cr:gq.controls) {
                settings_store_quick_controls_[cr.action]=cr.bounds;
                for(const auto& rv:qc.rows) {
                    for(const auto& cv:rv.controls) {
                        if(cv.action!=cr.action)continue;
                        if(cv.kind==0)p.Switch(cr.bounds,cv.checked,static_cast<A>(cr.action),!cv.disabled);
                        else if(cv.kind==1)p.Combo(cr.bounds,cv.label,static_cast<A>(cr.action),!cv.disabled);
                        else if(cv.kind==2)p.Button(cr.bounds,cv.label,static_cast<A>(cr.action),false,!cv.disabled);
                    }
                }
            }
            if(qc.has_menu_preview&&Present(gq.preview_menu)) {
                // A quiet rounded stage for the menu; no full-height strip pinned to the card edge.
                painter_.FillRoundedRect(gq.preview,8*s,WithAlpha(theme.text,vm.dark?.045f:.03f));
                painter_.StrokeRoundedRect(gq.preview,8*s,theme.stroke_divider);
                auto cap=gq.preview_caption;
                if(Present(cap)&&(cap.right-cap.left)>36*s) {
                    const float cy=(cap.top+cap.bottom)/2;
                    p.Icon(StoreIcon::Lines,{cap.left,cy-6.5f*s,cap.left+13*s,cy+6.5f*s},theme.text_secondary);
                    cap.left+=18*s;
                }
                p.Text(qc.preview_caption.empty()?l10n::Get(I::StorePreviewArchiveCaption):qc.preview_caption,cap,true);
                p.Menu(gq.preview_menu,qc.preview_rows,qc.preview_submenu,true,item.name);
            }
        }
    }
    for(const auto& c:l.store_packs) {
        const auto data=store_view::Pack(vm,c.pack);p.Card(c.bounds);p.Tile(data.icon,c.icon,HexColor(data.color),true);
        p.Heading(data.name,c.name);
        // Pulse's own packs carry the same verified badge as the catalog's official extensions.
        auto pack_publisher=c.publisher;
        if(Present(pack_publisher)){p.Icon(StoreIcon::Badge,{pack_publisher.left,pack_publisher.top+2*s,pack_publisher.left+14*s,pack_publisher.top+16*s},theme.accent);pack_publisher.left+=19*s;}
        p.Text(l10n::Get(I::StoreOfficial),pack_publisher,true);p.Summary(data.summary,c.summary);
        if(!data.state.empty()) {
            auto color=data.installed?green:theme.text_secondary;auto state=c.state;
            if(data.installed&&Present(state)){p.Icon(StoreIcon::Check,{state.left,state.top+8*s,state.left+14*s,state.top+22*s},color);state.left+=19*s;}
            p.Text(data.state,state,true,&color);
        } else p.Text(data.meta,c.meta,true);
        p.Button(c.button,data.button,data.action,true,data.action_enabled);p.Button(c.secondary,data.secondary,data.secondary_action);
        if(data.progress>=0&&Present(c.progress)){painter_.FillRoundedRect(c.progress,1.5f*s,theme.fill_hover);auto r=c.progress;r.right=r.left+(r.right-r.left)*std::clamp(data.progress,0.f,1.f);painter_.FillRoundedRect(r,1.5f*s,theme.accent);}
    }
    if(Present(l.store_community_card)) {
        auto r=l.store_community_card;
        ComPtr<ID2D1Factory> factory; p.dc->GetFactory(&factory);
        ComPtr<ID2D1StrokeStyle> dashed;auto style=D2D1::StrokeStyleProperties();style.dashStyle=D2D1_DASH_STYLE_DASH;
        ComPtr<ID2D1SolidColorBrush> border;p.dc->CreateSolidColorBrush(theme.stroke_card,&border);
        if(SUCCEEDED(factory->CreateStrokeStyle(style,nullptr,0,&dashed))&&border.get())p.dc->DrawRoundedRectangle(D2D1::RoundedRect(r,10*s,10*s),border.get(),s,dashed.get());
        p.Tile(StoreIcon::Puzzle,{r.left+20*s,r.top+24*s,r.left+64*s,r.top+68*s},theme.text_secondary);
        p.Text(l10n::Get(I::StoreCommunityEmpty),l.store_empty);p.Caption(l10n::Get(I::StoreCommunityEmptyDesc),{l.store_empty.left,l.store_empty.bottom+4*s,r.right-20*s,r.bottom-12*s},theme.text_secondary);
    } else p.Text(l10n::Get(I::StoreEmpty),l.store_empty,true);
    if(Present(l.store_trust)){p.Line(l.store_trust.left-30*s,l.store_trust.right+20*s,l.store_trust.top-12*s,theme.stroke_divider);p.Icon(StoreIcon::Shield,{l.store_trust.left-26*s,l.store_trust.top,l.store_trust.left-10*s,l.store_trust.top+18*s},theme.text_secondary);p.Caption(l10n::Get(I::StoreTrust),l.store_trust,theme.text_secondary);}
    p.Caption(l10n::Get(I::StoreOfficialManage),l.store_manage,theme.accent);
}
void MainRenderer::DrawExtensionDetail(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    StorePainter p{painter_, compositor_->Dc(), compositor_, theme, vm, scale_};
    const auto& d = l.ext_detail;
    const auto& v = vm.settings_store_view;
    const float s = scale_;
    const int item_index = store_view::ActiveDetailItem(v);
    if (item_index < 0 || item_index >= static_cast<int>(v.items.size())) return;
    const auto& item = v.items[static_cast<size_t>(item_index)];
    const auto green = painter_.StatusToneColor(fluent::InfoBarKind::Success);

    WindowViewModel::NativeSettingsHeader header;
    header.name = item.name;
    header.summary = item.summary;
    header.version = item.version + (item.size.empty() ? L"" : L" · " + item.size);
    header.icon = item.icon;
    header.icon_color = item.icon_color;
    header.custom_icon = item.custom_icon;
    header.official = item.official;
    header.enabled = item.enabled;
    header.installed = item.installed;
    if (!item.button_label.empty() && (!item.installed || item.action_enabled)) {
        header.action_label = item.button_label;
        header.action_enabled = item.action_enabled;
    }
    DrawExtensionDetailHeader(p, d, header, A::StoreDetailBack);
    store_notify::DrawStatusBanners(painter_, l, v, theme, false, false, p.Hot(A::StoreRestartNow));
    const int selected_tab = (std::max)(0, d.tab_count - 2) + (v.detail_tab == 1 ? 1 : 0);
    DrawExtensionDetailTabs(p, d, d.tab_count, selected_tab, A::StoreDetailTabBase);

    if (v.detail_tab == 1) {
        p.Card(d.groups[0]);
        const I keys[] = {I::Publisher, I::SzAboutVersion, I::SzAboutEngine, I::SzAboutLicense, I::SzAboutPermissions, I::SzAboutSource};
        for (int i = 0; i < 6; ++i) {
            // Grid cells: a quiet key over its value; a hairline between the cell rows.
            if (i && d.about_rows[i].left <= d.about_rows[0].left + 1 && d.about_rows[i].top > d.about_rows[0].top)
                p.Line(d.groups[0].left + 20*s, d.groups[0].right - 20*s, d.about_rows[i].top - 9*s, theme.stroke_divider);
            p.Text(l10n::Get(keys[i]), d.about_keys[i], true);
            auto value = d.about_values[i];
            if (i == 0 && item.official) {
                p.Icon(StoreIcon::Badge, {value.left, value.top + 3*s, value.left + 14*s, value.top + 17*s}, theme.accent);
                value.left += 19*s;
            }
            auto text_val = store_view::AboutValue(v, item, i);
            if (i == 5 && text_val.empty() && !item.homepage.empty()) text_val = item.homepage;
            const bool is_link = i == 5 && (Present(d.about_source) || Present(d.about_homepage));
            p.Text(text_val, value, false, (i == 0 && item.official) || is_link ? const_cast<D2D1_COLOR_F*>(&theme.accent) : nullptr);
        }
        p.Heading(l10n::Get(I::SzAboutHistory), d.history_label);
        p.Card(d.history_card);
        const size_t history = d.history_rows.size();
        if (history) {
            // Timeline: a rail through one dot per release, the current release filled with the accent.
            const float rail_x = d.history_card.left + 26*s;
            const float first = (d.history_versions.front().top + d.history_versions.front().bottom) / 2;
            const float last = (d.history_versions.back().top + d.history_versions.back().bottom) / 2;
            if (last > first) painter_.FillRoundedRect({rail_x - s, first, rail_x + s, last}, s, WithAlpha(theme.text_secondary, .35f));
            for (size_t i = 0; i < history; ++i) {
                const float cy = (d.history_versions[i].top + d.history_versions[i].bottom) / 2;
                const bool current = Present(d.history_current[i]);
                const float r = (current || i == 0 ? 6.0f : 4.5f) * s;
                const D2D1_RECT_F dot{rail_x - r, cy - r, rail_x + r, cy + r};
                painter_.FillRoundedRect(dot, r, current ? theme.accent : theme.surface_card);
                painter_.StrokeRoundedRect(dot, r, current ? theme.accent : theme.text_secondary);
                p.Mono(store_view::HistoryVersion(item, i), d.history_versions[i], &theme.text);
                p.Caption(store_view::HistoryNote(item, i), d.history_notes[i], theme.text_secondary);
                p.Pill(d.history_current[i], l10n::Get(I::SzAboutCurrent), green);
            }
        }
        return;
    }

    // Introduction card; the right-click menu sits on a tinted stage inside it.
    p.Card(d.desc_card);
    p.Heading(l10n::Get(I::StoreDetailDescription), d.desc_label);
    if (Present(d.desc_text)) {
        const std::wstring_view desc_str = !item.description.empty() ? std::wstring_view{item.description}
            : !item.summary.empty() ? std::wstring_view{item.summary}
            : std::wstring_view{l10n::Get(I::StoreDetailNoDescription)};
        painter_.DrawWrappedBody(desc_str, {d.desc_text.left, d.desc_text.top}, d.desc_text.right - d.desc_text.left, theme.text);
    }

    if (d.showcase_quick >= 0 && d.showcase_quick < static_cast<int>(v.quick_cards.size())) {
        const auto& qc = v.quick_cards[static_cast<size_t>(d.showcase_quick)];
        painter_.FillRoundedRect(d.showcase_card, 8*s, WithAlpha(theme.accent, vm.dark ? .10f : .07f));
        painter_.StrokeRoundedRect(d.showcase_card, 8*s, WithAlpha(theme.accent, .16f));
        auto cap = d.showcase_caption;
        if (Present(cap) && (cap.right - cap.left) > 36*s) {
            const float cy = (cap.top + cap.bottom) / 2;
            p.Icon(StoreIcon::Lines, {cap.left, cy - 6.5f*s, cap.left + 13*s, cy + 6.5f*s}, theme.accent);
            cap.left += 18*s;
        }
        p.Text(qc.preview_caption.empty() ? l10n::Get(I::StoreDetailMenuShowcase) : qc.preview_caption, cap, true);
        p.Menu(d.showcase_menu, qc.preview_rows, qc.preview_submenu, true, item.name);
    }

    if (!item.highlights.empty()) {
        // Feature tiles: a tinted round check badge beside the text, vertically centred.
        p.Heading(l10n::Get(I::StoreDetailHighlights), d.highlights_label);
        for (size_t i = 0; i < d.highlight_rows.size() && i < item.highlights.size(); ++i) {
            const auto& r = d.highlight_rows[i];
            p.Card(r);
            const float cy = (r.top + r.bottom) / 2;
            const D2D1_RECT_F badge{r.left + 16*s, cy - 16*s, r.left + 48*s, cy + 16*s};
            painter_.FillRoundedRect(badge, 16*s, WithAlpha(green, .14f));
            p.Icon(StoreIcon::Check, {badge.left + 9*s, badge.top + 9*s, badge.right - 9*s, badge.bottom - 9*s}, green);
            const float tw = (std::max)(1.0f, r.right - 16*s - (r.left + 64*s));
            const float th = painter_.MeasureWrappedCaptionHeight(item.highlights[i], tw);
            p.Caption(item.highlights[i], {r.left + 64*s, cy - th / 2, r.right - 16*s, cy + th / 2 + 2*s}, theme.text);
        }
    }

    if (!item.tags.empty()) {
        p.Heading(l10n::Get(I::StoreDetailTags), d.tags_label);
        p.Card(d.tags_card);
        for (size_t i = 0; i < d.tag_chips.size() && i < item.tags.size(); ++i) {
            p.Pill(d.tag_chips[i], item.tags[i], theme.text_secondary);
        }
    }

    p.Card(d.facts_card);
    p.Heading(l10n::Get(I::StoreDetailQuickFacts), d.facts_title);
    {
        struct Fact { l10n::StringId key; std::wstring value; int link_kind; };
        std::vector<Fact> facts;
        const auto pub = store_view::AboutValue(v, item, 0);
        if (!pub.empty()) facts.push_back({I::Publisher, pub, 0});
        const auto ver_fact = store_view::AboutValue(v, item, 1);
        if (!ver_fact.empty()) facts.push_back({I::SzAboutVersion, ver_fact, 0});
        if (!item.engine.empty()) facts.push_back({I::SzAboutEngine, item.engine, 0});
        if (!item.license.empty()) facts.push_back({I::SzAboutLicense, item.license, 0});
        if (!item.homepage.empty()) facts.push_back({I::StoreDetailHomepage, item.homepage, 1});
        if (!item.source.empty() && item.source != item.homepage) facts.push_back({I::SzAboutSource, item.source, 2});
        for (size_t i = 0; i < facts.size() && i < d.fact_keys.size(); ++i) {
            if (i) p.Line(d.fact_rows[i].left, d.fact_rows[i].right, d.fact_rows[i].top - 4*s, theme.stroke_divider);
            p.Text(l10n::Get(facts[i].key), d.fact_keys[i], true);
            const bool is_link = (facts[i].link_kind == 1 && Present(d.fact_homepage)) ||
                                 (facts[i].link_kind == 2 && Present(d.fact_source));
            p.Caption(facts[i].value, d.fact_values[i], is_link ? theme.accent : theme.text);
        }
    }

    p.Card(d.perms_card);
    p.Heading(l10n::Get(I::StoreDetailPermissionsTitle), d.perms_title);
    if (item.permissions.empty()) {
        if (!d.perm_rows.empty()) {
            p.Caption(l10n::Get(I::StorePermissionsNone), d.perm_rows[0], theme.text_secondary);
        }
    } else {
        for (size_t i = 0; i < d.perm_rows.size() && i < item.permissions.size(); ++i) {
            const auto& r = d.perm_rows[i];
            p.Icon(StoreIcon::Shield, {r.left, r.top + 3*s, r.left + 12*s, r.top + 15*s}, theme.text_secondary);
            p.Caption(item.permissions[i], {r.left + 18*s, r.top, r.right, r.bottom}, theme.text_secondary);
        }
    }
}
} // namespace pulse::ui