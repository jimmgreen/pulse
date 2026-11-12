#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
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
    void Tile(StoreIcon icon,D2D1_RECT_F r,D2D1_COLOR_F color,bool solid=false) {
        if(!Present(r))return;
        // Product tiles (archive_ux_mockup.html): known products carry their own artwork,
        // others a white line icon on their catalog colour. High contrast keeps flat glyphs.
        if(solid&&!IsHighContrast()) { DrawProductTile(dc,r,ProductArtFor(icon),color,icon); return; }
        const float rad=((r.right-r.left)>64*s?16:(r.right-r.left)>50*s?14:8)*s;
        p.FillRoundedRect(r,rad,solid?color:WithAlpha(color,.09f));
        const float pad=(r.right-r.left)*.25f;
        Icon(icon,Inset(r,pad,pad),solid?t.text:color);
    }
    void Button(D2D1_RECT_F r,std::wstring_view v,A a,bool primary=false,bool enabled=true) {
        if(!Present(r)||v.empty())return;
        fluent::ControlState state{};state.enabled=enabled;state.hovered=enabled&&Hot(a);
        p.DrawButton({r,v,{},primary?fluent::ButtonKind::Primary:fluent::ButtonKind::Standard,state});
    }
    void Switch(D2D1_RECT_F r,bool checked,A a) {
        if(!Present(r))return;
        fluent::ControlState state{};state.checked=checked;state.hovered=Hot(a);p.DrawSwitch(r,L"",state);
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
    void Menu(D2D1_RECT_F r,const std::vector<SevenZipMenuRow>& rows,bool submenu,bool expanded,std::wstring_view name,
              bool full=false,bool archive=true) {
        if(!Present(r))return;
        p.DrawMenuSurface(r);
        dc->PushAxisAlignedClip(r,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float y=r.top+8*s,left=r.left;
        auto separator=[&] { Line(r.left+8*s,r.right-8*s,y+4*s,t.stroke_divider);y+=9*s; };
        auto item=[&](StoreIcon icon,std::wstring_view title,std::wstring_view detail={},std::wstring_view shortcut={},bool child=false,bool primary=false) {
            const float height=(detail.empty()?28:40)*s;
            fluent::MenuItemSpec spec{};spec.bounds={left+4*s,y,r.right-4*s,y+height};
            spec.text=title;spec.detail=detail;spec.shortcut=shortcut;spec.vector_icon=icon;spec.has_submenu=child;spec.primary=primary;
            p.DrawMenuItem(spec);y+=height;
        };
        if(full) {
            const StoreIcon actions[]={StoreIcon::Cut,StoreIcon::Copy,StoreIcon::Rename,StoreIcon::Trash};
            const float width=(r.right-r.left-16*s)/4;
            for(int i=0;i<4;++i) {
                const float x=r.left+8*s+(i+.5f)*width;
                Icon(actions[i],{x-8*s,y+5*s,x+8*s,y+21*s},t.text_secondary);
            }
            y+=32*s;Line(r.left+8*s,r.right-8*s,y,t.stroke_divider);
            item(StoreIcon::Open,l10n::Get(I::Open),{},L"Enter");
            if(archive) {
                item(StoreIcon::Eye,l10n::Get(I::QuickPreview),{},L"Space");
                item(StoreIcon::OpenWith,l10n::Get(I::ContextOpenWith),{},{},true);
            }
            separator();
        }
        const float group_top=y;
        float group_height=(submenu?30:26)*s;
        for(const auto& row:rows)group_height+=(row.detail.empty()?30:42)*s;
        if(submenu&&expanded)group_height+=37*s;
        p.FillRoundedRect({r.left+4*s,y,r.right-4*s,y+group_height},6*s,WithAlpha(t.accent,.04f));
        if(submenu) {
            item(StoreIcon::Box,name,{},{},true);y+=2*s;
            if(expanded) {
                left+=10*s;
                p.FillRoundedRect({left,y,left+2*s,group_top+group_height-4*s},1*s,WithAlpha(t.accent,.5f));
            }
        } else {
            auto group_color=MixColor(t.accent,t.text_secondary,.35f);
            Text(name,{r.left+12*s,y+2*s,r.right-28*s,y+24*s},true,&group_color);
            Icon(StoreIcon::Gear,{r.right-24*s,y+6*s,r.right-12*s,y+18*s},t.text_secondary);
            y+=26*s;
        }
        if(!submenu||expanded) {
            for(size_t ri=0;ri<rows.size();++ri) { const auto& row=rows[ri]; item(row.vector_icon,row.title,row.detail,{},false,ri==0&&!row.detail.empty());y+=2*s; }
            if(submenu) {separator();item(StoreIcon::Gear,l10n::Get(I::Settings));}
        }
        left=r.left;
        if(full) {
            separator();
            item(StoreIcon::Path,l10n::Get(I::CopyPath),{},L"Ctrl+Shift+C");
            item(StoreIcon::Star,l10n::Get(I::PinQuickAccess));
            separator();
            item(StoreIcon::Properties,l10n::Get(I::Properties),{},L"Alt+Enter");
        }
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
        Text(title,{r.left+62*s,r.top+10*s,r.right-reserve*s,r.top+34*s});
        Caption(desc,{r.left+62*s,r.top+36*s,r.right-reserve*s,r.bottom-8*s},t.text_secondary);
    }
};
A Offset(A a,size_t i){return static_cast<A>(static_cast<int>(a)+static_cast<int>(i));}
}
D2D1_RECT_F MainRenderer::SettingsStoreField(int field) const { return field>=0&&field<7?settings_store_fields_[field]:D2D1_RECT_F{}; }
D2D1_RECT_F MainRenderer::SettingsStoreClip() const {return settings_store_clip_;}

void MainRenderer::DrawSettingsStore(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    StorePainter p{painter_,compositor_->Dc(),compositor_,theme,vm,scale_};
    const float s=scale_;const auto& v=vm.settings_store_view;const auto& z=v.seven_zip;
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
        p.Caption(l10n::Get(I::StoreFailedDesc),{l.store_state.left,l.store_state.bottom+4*s,l.store_state.right,Present(l.store_notice)?l.store_notice.top-6*s:l.store_failed.bottom-14*s},theme.text_secondary);
        p.Mono(v.notice,l.store_notice);
        p.Button(l.store_refresh,l10n::Get(I::RecoveryRetry),A::StoreRetry,true,store_text::CanRefresh(v));
        p.Button(l.store_failed_sources,l10n::Get(I::StoreDownloadSources),A::StoreSources);
        p.Button(l.store_install_file,l10n::Get(I::StoreInstallFromFile),A::StoreInstallFromFile,false,store_text::CanInstallFromFile(v));
    } else {
        auto state_rect=l.store_state;
        if(v.state==ExtensionStoreView::State::Loading&&Present(state_rect)) {p.Icon(StoreIcon::Refresh,{state_rect.left,state_rect.top+7*s,state_rect.left+16*s,state_rect.top+23*s},theme.accent);state_rect.left+=22*s;}
        p.Text(store_text::State(v),state_rect,true);
        p.Note(v.state==ExtensionStoreView::State::Offline?l10n::Get(I::StoreOfflineDesc):v.notice,l.store_notice,v.state==ExtensionStoreView::State::Offline?fluent::InfoBarKind::Warning:fluent::InfoBarKind::Informational);
    }
    if(Present(l.store_restart)) {
        auto banner=l.store_restart;
        if(Present(l.store_restart_button)) {
            banner.left=(std::min)(banner.left,l.store_restart_button.left);
            banner.top=(std::min)(banner.top,l.store_restart_button.top);
            banner.right=(std::max)(banner.right,l.store_restart_button.right);
            banner.bottom=(std::max)(banner.bottom,l.store_restart_button.bottom);
        }
        auto color=painter_.StatusToneColor(fluent::InfoBarKind::Warning);
        painter_.FillRoundedRect(Inset(banner,-12*s,-6*s),6*s,WithAlpha(color,.08f));
        p.Caption(l10n::Get(I::StoreRestart),l.store_restart,theme.text_secondary);
        p.Button(l.store_restart_button,l10n::Get(I::StoreRestartNow),A::StoreRestartNow);
    }
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
        if(c.feature) {
            const D2D1_GRADIENT_STOP stops[]={{0,WithAlpha(theme.accent,.10f)},{1,WithAlpha(theme.accent,0)}};
            ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> brush;
            if(SUCCEEDED(p.dc->CreateGradientStopCollection(stops,2,&collection))&&SUCCEEDED(p.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({c.bounds.left,c.bounds.top},{c.bounds.right,c.publisher.bottom}),collection.get(),&brush)))p.dc->FillRoundedRectangle(D2D1::RoundedRect(c.bounds,12*s,12*s),brush.get());
        }
        p.Tile(item.icon,c.icon,HexColor(item.icon_color),true);
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
        if(c.feature&&z.formats_available)sub+=L" · "+store_view::Fill(l10n::Get(I::SzEngineFormats),L"{n}",std::to_wstring(z.formats_rw.size()+z.formats_ro.size()));
        else if(c.feature&&!Present(c.settings)&&!item.version.empty())sub+=L" · "+item.version+(item.size.empty()?L"":L" · "+item.size);
        p.Text(sub,publisher,true);p.Summary(item.summary,c.summary);
        p.Text(item.status,c.status,true);p.Text(item.version+(item.size.empty()?L"":L" · "+item.size),c.version,true);
        p.Button(c.button,item.button_label,Offset(A::StoreItemBase,i),!c.feature||!Present(c.settings),item.action_enabled);
        if(c.feature&&Present(c.settings)) {
            p.Button(c.settings,l10n::Get(I::StoreAllSettings)+L"   ",Offset(A::StoreItemSettingsBase,i));
            const float cy=(c.settings.top+c.settings.bottom)/2;
            p.Icon(StoreIcon::ChevronRight,{c.settings.right-20*s,cy-6*s,c.settings.right-8*s,cy+6*s},theme.text_secondary);
        } else p.Button(c.settings,l10n::Get(I::Settings),Offset(A::StoreItemSettingsBase,i));
    };
    if(l.store_feature_item>=0)draw_catalog_card(l.store_feature,static_cast<size_t>(l.store_feature_item));
    for(size_t i=0;i<l.store_items.size()&&i<v.items.size();++i)draw_catalog_card(l.store_items[i],i);
    const auto& q=l.store_quick;
    if(q.on) {
        p.Line(q.section.left,q.section.right,q.section.top,theme.stroke_divider);
        p.Heading(l10n::Get(I::StoreQuickTitle),q.title);
        auto saved_color=z.status.empty()?(v.saved?green:theme.text_secondary):(z.status==L"已保存。"||z.status==L"Saved."?green:painter_.StatusToneColor(fluent::InfoBarKind::Error));
        auto saved_rect=q.saved;
        if(z.status.empty()&&Present(saved_rect)&&(saved_rect.right-saved_rect.left)>32*s) {
            const float cy=(saved_rect.top+saved_rect.bottom)/2;
            p.Icon(StoreIcon::Check,{saved_rect.left,cy-6.5f*s,saved_rect.left+13*s,cy+6.5f*s},saved_color);
            saved_rect.left+=17*s;
        }
        p.Text(z.status.empty()?l10n::Get(v.saved?I::StoreSaved:I::StoreAutoSaved):z.status,saved_rect,true,&saved_color);
        if(!z.available) p.Caption(l10n::Get(z.item>=0&&z.item<static_cast<int>(v.items.size())&&v.items[z.item].enabled?I::SzLoading:I::SzUnavailable),q.rows[0],theme.text_secondary);
        else {
            const I labels[]={I::SzModeTitle,I::StoreQuickCompress,I::SzRememberTitle};
            const D2D1_RECT_F controls[]={q.mode,q.format,q.remember};
            for(int i=0;i<3;++i) {
                auto r=q.rows[i];const bool stacked=controls[i].top>=r.top+32*s;
                const float right=stacked?r.right:controls[i].left-12*s;
                p.Text(l10n::Get(labels[i]),{r.left,r.top+5*s,right,r.top+28*s});
                auto desc=i==0?store_view::Fill(l10n::Get(I::StoreMenuShows),L"{name}",z.archive_rows.empty()?store_view::ModeTitle(z.mode):z.archive_rows.front().title)
                    :i==1?store_view::Fill(l10n::Get(I::SzFormatDesc),L"{name}",store_view::FormatName(z.format))
                    :l10n::Get(I::SzRememberDesc);
                p.Text(desc,{r.left,r.top+28*s,right,r.top+49*s},true);
                if(i<2)p.Line(r.left,r.right,r.bottom,theme.stroke_divider);
            }
            p.Combo(q.mode,store_view::ModeTitle(z.mode),A::StoreQuickMode);
            p.Combo(q.format,store_view::FormatName(z.format),A::StoreQuickFormat);
            p.Combo(q.level,z.format==2?l10n::Get(I::SzLevelTar):store_view::LevelName(z.level),A::StoreQuickLevel,z.format!=2);
            p.Switch(q.remember,z.remember,A::StoreQuickRemember);
            painter_.FillRoundedRect(q.preview,0,WithAlpha(theme.surface_sheet,.75f));
            auto cap=q.preview_caption;
            if(Present(cap)&&(cap.right-cap.left)>36*s) {
                const float cy=(cap.top+cap.bottom)/2;
                p.Icon(StoreIcon::Lines,{cap.left,cy-6.5f*s,cap.left+13*s,cy+6.5f*s},theme.text_secondary);
                cap.left+=18*s;
            }
            p.Text(l10n::Get(I::StorePreviewArchiveCaption),cap,true);
            const std::wstring_view menu_group=z.item>=0&&z.item<static_cast<int>(v.items.size())&&v.items[z.item].id=="pulse.7zip"?std::wstring_view{L"7-Zip"}:
                (z.item>=0&&z.item<static_cast<int>(v.items.size())?std::wstring_view{v.items[z.item].name}:std::wstring_view{});
            p.Menu(q.preview_menu,z.archive_rows,z.submenu,true,menu_group);
        }
    }
    for(const auto& c:l.store_packs) {
        const auto data=store_view::Pack(vm,c.pack);p.Card(c.bounds);p.Tile(data.icon,c.icon,HexColor(data.color),true);
        p.Heading(data.name,c.name);p.Text(l10n::Get(I::StoreOfficial),c.publisher,true);p.Summary(data.summary,c.summary);
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
void MainRenderer::DrawSevenZipDetail(const WindowViewModel& vm,const D2D1_RECT_F& rect,const Theme& theme) {
    const auto l=MakeSettingsLayout(vm,rect,scale_,title_bar_height_,status_height_,&painter_);
    StorePainter p{painter_,compositor_->Dc(),compositor_,theme,vm,scale_};
    const auto& d=l.sz;const auto& v=vm.settings_store_view;const auto& z=v.seven_zip;const float s=scale_;
    if(z.item<0||z.item>=static_cast<int>(v.items.size()))return;
    const auto& item=v.items[z.item];auto green=painter_.StatusToneColor(fluent::InfoBarKind::Success);
    p.Icon(StoreIcon::ChevronLeft,{d.crumb.left+4*s,d.crumb.top+7*s,d.crumb.left+18*s,d.crumb.bottom-7*s},theme.text_secondary);
    auto crumb=d.crumb;crumb.left+=24*s;p.Text(l10n::Get(I::StoreTitle),crumb,true);
    p.Icon(StoreIcon::ChevronRight,{d.crumb_name.left,d.crumb_name.top+7*s,d.crumb_name.left+12*s,d.crumb_name.bottom-7*s},theme.text_disabled);
    auto crumb_name=d.crumb_name;crumb_name.left+=22*s;p.Text(item.name,crumb_name,true);
    p.Tile(item.icon,d.icon,HexColor(item.icon_color),true);
    if(Present(d.name)) {
        ComPtr<IDWriteTextLayout> title;
        if(SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(item.name.c_str(),static_cast<UINT32>(item.name.size()),compositor_->HeaderFormat(),d.name.right-d.name.left,d.name.bottom-d.name.top,&title))) {
            title->SetFontSize(26*s,{0,static_cast<UINT32>(item.name.size())});title->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            MakeBrush(p.dc,theme.text,brText_);compositor_->DrawTextLayout({d.name.left,d.name.top},title.get(),brText_.get());
        }
    }
    p.Icon(StoreIcon::Badge,{d.sub.left,d.sub.top+2*s,d.sub.left+14*s,d.sub.top+16*s},theme.accent);
    auto sub=d.sub;sub.left+=20*s;
    auto subtitle=item.official?l10n::Get(I::StoreOfficial):item.publisher;
    if(z.formats_available) subtitle+=L" · "+store_view::Fill(l10n::Get(I::SzEngineFormats),L"{n}",std::to_wstring(z.formats_rw.size()+z.formats_ro.size()));
    p.Text(subtitle,sub,true);
    const auto enabled=l10n::Get(item.enabled?I::StoreExtensionEnabled:I::StoreExtensionDisabled);
    p.Pill(d.badges[0],enabled,item.enabled?green:theme.text_secondary,false,item.enabled);
    p.Pill(d.badges[1],l10n::Get(I::StoreNativeCode),theme.text_secondary);
    p.Pill(d.badges[2],item.version+(item.size.empty()?L"":L" · "+item.size),theme.text_secondary);
    p.Text(enabled,d.enable_label,true);p.Switch(d.enable,item.enabled,A::StoreDetailEnable);
    p.Button(d.more,L" ",A::StoreDetailMore);p.Icon(StoreIcon::More,Inset(d.more,8*s,8*s),theme.text_secondary);
    if(Present(d.disabled_banner)) {
        auto banner=d.disabled_banner;
        if(Present(l.store_restart_button)) {
            banner.left=(std::min)(banner.left,l.store_restart_button.left);
            banner.top=(std::min)(banner.top,l.store_restart_button.top);
            banner.right=(std::max)(banner.right,l.store_restart_button.right);
            banner.bottom=(std::max)(banner.bottom,l.store_restart_button.bottom);
        }
        auto color=painter_.StatusToneColor(!item.enabled?fluent::InfoBarKind::Warning:fluent::InfoBarKind::Informational);
        painter_.FillRoundedRect(Inset(banner,-12*s,-6*s),6*s,WithAlpha(color,.08f));
        p.Caption(l10n::Get(!item.enabled?I::SzDisabledNote:I::StoreRestart),d.disabled_banner,theme.text_secondary);
        p.Button(l.store_restart_button,l10n::Get(I::StoreRestartNow),A::StoreRestartNow);
    }
    constexpr I tabs[]={I::SzTabSettings,I::SzTabMenu,I::SzTabFormats,I::SzTabAbout};
    for(int i=0;i<4;++i) {
        auto r=d.tabs[i];p.Line(r.left,r.right,r.bottom-s,theme.stroke_divider);
        if(p.Hot(Offset(A::StoreDetailTabBase,i)))painter_.FillRoundedRect(Inset(r,0,4*s),6*s,theme.fill_hover);
        painter_.DrawText(l10n::Get(tabs[i]),r,i==v.detail_tab?compositor_->HeaderFormat():compositor_->TextFormat(),i==v.detail_tab?theme.text:theme.text_secondary,fluent::HorizontalAlignment::Center);
        if(i==v.detail_tab)painter_.FillRoundedRect({r.left+12*s,r.bottom-3*s,r.right-12*s,r.bottom},1.5f*s,theme.accent);
    }
    for(auto r:d.groups)p.Card(r);
    if(!z.TabAvailable(v.detail_tab)&&v.detail_tab!=3) {
        const auto fallback=(z.available||z.needs_update)?I::SzExtensionUpdateRequired:z.needs_restart?I::ExtensionStoreInstalledRestart:item.enabled?I::SzLoading:I::SzUnavailable;
        p.Caption(l10n::Get(fallback),Inset(d.groups[0],20*s,20*s),theme.text_secondary);
        return;
    }
    if(v.detail_tab==0) {
        constexpr I sections[]={I::SzSectionExtract,I::SzSectionCompress,I::SzSectionPassword};
        for(int i=0;i<3;++i)p.Heading(l10n::Get(sections[i]),d.section_label[i]);
        if(!z.status.empty()&&Present(d.section_label[0])&&(d.section_label[0].right-d.section_label[0].left)>180*s) {
            const bool ok_status=z.status==L"已保存。"||z.status==L"Saved.";
            const auto st_color=ok_status?green:painter_.StatusToneColor(fluent::InfoBarKind::Error);
            painter_.DrawText(z.status,{d.section_label[0].left+100*s,d.section_label[0].top,d.section_label[0].right-2*s,d.section_label[0].bottom},compositor_->SmallFormat(),st_color,fluent::HorizontalAlignment::Right);
        }
        p.Row(d.mode_row,StoreIcon::Extract,l10n::Get(I::SzModeTitle),l10n::Get(I::SzModeDesc));
        for(int i=0;i<3;++i)p.Option(d.mode_opts[i],store_view::ModeTitle(i),store_view::ModeDesc(i),z.mode==i,Offset(A::SzModeBase,i));
        p.Line(d.groups[0].left+16*s,d.groups[0].right-16*s,d.location_row.top-2*s,theme.stroke_divider);
        p.Row(d.location_row,StoreIcon::Folder,l10n::Get(I::SzLocationTitle),l10n::Get(I::SzLocationDesc));
        for(int i=0;i<2;++i)p.Option(d.location_opts[i],l10n::Get(i?I::SzLocationFixed:I::SzLocationBeside),l10n::Get(i?I::SzLocationFixedDesc:I::SzLocationBesideDesc),z.location==i,Offset(A::SzLocationBase,i));
        p.Input(d.folder);p.Button(d.browse,l10n::Get(I::Browse),A::SzBrowse);
        p.Row(d.format_row,StoreIcon::Compress,l10n::Get(I::SzFormatTitle),store_view::Fill(l10n::Get(I::SzFormatDesc),L"{name}",store_view::FormatName(z.format)));
        for(int i=0;i<6;++i)p.Option(d.format_opts[i],store_view::FormatName(i),store_view::FormatDesc(i),z.format==i,Offset(A::SzFormatBase,i));
        p.Line(d.groups[1].left+16*s,d.groups[1].right-16*s,d.level_row.top-2*s,theme.stroke_divider);
        p.Row(d.level_row,StoreIcon::Gear,l10n::Get(I::SzLevelTitle),l10n::Get(I::SzLevelDesc));
        const bool dragging=v.level_drag>=0.0f;
        const float level_position=dragging?v.level_drag:static_cast<float>(z.level);
        const int shown_level=std::clamp(static_cast<int>(std::lround(level_position)),0,5);
        auto level_color=z.format==2?theme.text_disabled:theme.accent;
        p.Text(z.format==2?l10n::Get(I::SzLevelTar):store_view::LevelName(shown_level),d.level_value,true,&level_color);
        fluent::StepSliderSpec slider;
        slider.bounds=d.level_track;
        slider.stops=6;
        slider.position=level_position;
        slider.state.pressed=dragging;
        slider.state.enabled=z.format!=2;
        for(int i=0;i<6;++i)slider.state.hovered=slider.state.hovered||p.Hot(Offset(A::SzLevelBase,i));
        painter_.DrawStepSlider(slider);
        for(int i=0;i<6;++i) {
            const auto r=d.level_stops[i];
            const float cx=fluent::StepSliderStopX(d.level_track,6,static_cast<float>(i),s);
            const float half=(r.right-r.left)/2;
            const bool selected=shown_level==i&&slider.state.enabled;
            painter_.DrawText(store_view::LevelName(i),{cx-half,d.level_track.bottom+2*s,cx+half,r.bottom},
                compositor_->SmallFormat(),!slider.state.enabled?theme.text_disabled:selected?theme.accent:theme.text_secondary,
                fluent::HorizontalAlignment::Center);
        }
        p.Icon(StoreIcon::Info,{d.compress_note.left-22*s,d.compress_note.top+2*s,d.compress_note.left-6*s,d.compress_note.top+18*s},theme.text_secondary);
        p.Caption(l10n::Get(I::SzCompressNote),d.compress_note,theme.text_secondary);
        p.Row(d.remember_row,StoreIcon::Lock,l10n::Get(I::SzRememberTitle),l10n::Get(I::SzRememberDesc),76);
        p.Switch(d.remember,z.remember,A::SzRemember);
        p.Icon(StoreIcon::Info,{d.rar_note.left-22*s,d.rar_note.top+2*s,d.rar_note.left-6*s,d.rar_note.top+18*s},theme.text_secondary);
        p.Caption(l10n::Get(I::SzRarNote),d.rar_note,theme.text_secondary);
    } else if(v.detail_tab==1) {
        p.Heading(l10n::Get(I::SzPlacementTitle),d.section_label[0]);
        for(int i=0;i<2;++i)p.Option(d.placement_opts[i],l10n::Get(i?I::SzPlacementSub:I::SzPlacementTop),l10n::Get(i?I::SzPlacementSubDesc:I::SzPlacementTopDesc),z.submenu==(i==1),Offset(A::SzPlacementBase,i));
        p.Text(l10n::Get(I::SzCommandsTitle)+L"  ·  "+l10n::Get(I::SzCommandsDesc),d.commands_label,true);
        const StoreIcon icons[]={StoreIcon::Extract,StoreIcon::ExtractTo,StoreIcon::Test,StoreIcon::Compress,StoreIcon::AddArchive,StoreIcon::Hash};
        for(int i=0;i<6;++i){p.Row(d.command_rows[i],icons[i],store_view::CommandName(i),store_view::CommandTag(i),76);p.Switch(d.command_switch[i],(z.commands&(1u<<i))!=0,Offset(A::SzCommandBase,i));if(i<5)p.Line(d.command_rows[i].left+16*s,d.command_rows[i].right-16*s,d.command_rows[i].bottom,theme.stroke_divider);}
    } else if(v.detail_tab==2) {
        p.Heading(l10n::Get(I::SzFormatsWritable),d.formats_rw_label);p.Heading(l10n::Get(I::SzFormatsReadOnly),d.formats_ro_label);
        auto format_chip=[&](D2D1_RECT_F bounds,std::wstring_view name,bool writable) {
            if(!Present(bounds))return;
            auto color=writable?(vm.dark?MixColor(theme.accent,theme.text,.45f):theme.accent):theme.text;
            painter_.FillRoundedRect(bounds,6*s,writable?WithAlpha(theme.accent,.095f):theme.fill_hover);
            p.Mono(name,Inset(bounds,7*s,0),&color,true);
        };
        for(size_t i=0;i<d.chips_rw.size()&&i<z.formats_rw.size();++i)format_chip(d.chips_rw[i],z.formats_rw[i],true);
        for(size_t i=0;i<d.chips_ro.size()&&i<z.formats_ro.size();++i)format_chip(d.chips_ro[i],z.formats_ro[i],false);
        p.Icon(StoreIcon::Eye,{d.formats_note.left-22*s,d.formats_note.top+2*s,d.formats_note.left-6*s,d.formats_note.top+18*s},theme.text_secondary);
        p.Caption(l10n::Get(I::SzFormatsNote),d.formats_note,theme.text_secondary);
    } else {
        const I keys[]={I::Publisher,I::SzAboutVersion,I::SzAboutEngine,I::SzAboutLicense,I::SzAboutPermissions,I::SzAboutSource};
        for(int i=0;i<6;++i) {
            if(i)p.Line(d.about_rows[i].left,d.about_rows[i].right,d.about_rows[i].top-4*s,theme.stroke_divider);
            p.Text(l10n::Get(keys[i]),d.about_keys[i],true);
            auto value=d.about_values[i];
            if(i==0&&item.official) {p.Icon(StoreIcon::Badge,{value.left,value.top+3*s,value.left+14*s,value.top+17*s},theme.accent);value.left+=19*s;}
            p.Caption(store_view::AboutValue(v,item,i),value,(i==0&&item.official)||(i==5&&Present(d.about_source))?theme.accent:theme.text);
        }
        p.Heading(l10n::Get(I::SzAboutHistory),d.history_label);p.Card(d.history_card);
        for(size_t i=0;i<d.history_rows.size();++i) {
            p.Mono(store_view::HistoryVersion(item,i),d.history_versions[i],&theme.text);
            p.Caption(store_view::HistoryNote(item,i),d.history_notes[i],theme.text_secondary);
            p.Pill(d.history_current[i],l10n::Get(I::SzAboutCurrent),green);
            if(i)p.Line(d.history_rows[i].left,d.history_rows[i].right,d.history_rows[i].top,theme.stroke_divider);
        }
    }
    p.Card(d.preview_card);
    auto preview_title=d.preview_title;
    if(Present(preview_title)&&(preview_title.right-preview_title.left)>36*s) {
        const float cy=(preview_title.top+preview_title.bottom)/2;
        p.Icon(StoreIcon::Eye,{preview_title.left,cy-7*s,preview_title.left+14*s,cy+7*s},theme.text_secondary);
        preview_title.left+=19*s;
    }
    p.Text(l10n::Get(I::SzPreviewTitle),preview_title,true);
    if(Present(d.preview_kinds[0])) {
        painter_.DrawSegmentedTrack({d.preview_kinds[0].left,d.preview_kinds[0].top,d.preview_kinds[1].right,d.preview_kinds[1].bottom});
        for(int i=0;i<2;++i){fluent::SegmentedItemSpec spec{};spec.bounds=d.preview_kinds[i];spec.text=l10n::Get(i?I::SzPreviewFiles:I::SzPreviewArchive);spec.state.checked=v.preview_kind==i;spec.state.hovered=p.Hot(Offset(A::SzPreviewBase,i));spec.shared_track=true;spec.position=i?fluent::SegmentPosition::Last:fluent::SegmentPosition::First;painter_.DrawSegmentedItem(spec);}
    }
    const std::wstring_view detail_group=item.id=="pulse.7zip"?std::wstring_view{L"7-Zip"}:std::wstring_view{item.name};
    p.Menu(d.preview_menu,v.preview_kind==0?z.archive_rows:z.file_rows,z.submenu,true,detail_group,true,v.preview_kind==0);
    p.Card(d.result_card);
    const auto result_icon=z.mode==2?StoreIcon::Warn:StoreIcon::Check;
    const auto result_tone=z.mode==2?painter_.StatusToneColor(fluent::InfoBarKind::Warning):green;
    p.Icon(result_icon,{d.result_card.left+14*s,d.result_card.top+15*s,d.result_card.left+30*s,d.result_card.top+31*s},result_tone);
    auto result=Inset(d.result_card,14*s,14*s);result.left+=28*s;p.Caption(l10n::Get(z.mode==1?I::SzResultFolder:z.mode==2?I::SzResultHere:I::SzResultSmart),result,theme.text_secondary);
}
} // namespace pulse::ui
