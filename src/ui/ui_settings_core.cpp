#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"

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
        const float right=text_right>0 ? text_right : r.right-72*scale_;
        DrawIconText(r.left+16*scale_,r.top+17*scale_,24*scale_,24*scale_,icon,L"",theme.text_secondary,0.85f);
        text(title,D2D1::RectF(r.left+54*scale_,r.top+10*scale_,right,r.top+34*scale_));
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
                    dc->DrawTextLayout(D2D1::Point2F(bounds.left,bounds.top),description.get(),brTextSecondary_.get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int hit) {
        if(IsHovered(vm,H::SettingsToggle,hit)) {
            MakeBrush(dc,theme.fill_hover,brFillHover_);
            FillRoundedRect(dc,brFillHover_.get(),r.left+2*scale_,r.top+2*scale_,r.right-r.left-4*scale_,r.bottom-r.top-4*scale_,6*scale_);
        }
        label(r,l10n::Get(title),l10n::Get(desc),icon,0.0f,hit==15 ? 43.0f : 21.0f);
        fluent::ControlState state{}; state.checked=on; state.hovered=IsHovered(vm,H::SettingsToggle,hit);
        painter_.DrawSwitch(D2D1::RectF(r.right-60*scale_,r.top+16*scale_,r.right-16*scale_,r.top+48*scale_),L"",state);
    };
    auto button=[&](D2D1_RECT_F r,const std::wstring& value,H::Region region,int index,bool primary=false,bool enabled=true,const wchar_t* glyph=L"") {
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
                    H::Region hit,I title,I desc,const wchar_t* unit) {
        const bool stacked=cells[0].left<card.left+100*scale_;
        label(card,l10n::Get(title),l10n::Get(desc),L"\xE8A4",stacked ? card.right-16*scale_ : cells[0].left-12*scale_);
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
            const auto lr=ticks[i]==0 ? D2D1::RectF(g.left-9*scale_,g.cy+8*scale_,g.left+60*scale_,g.cy+22*scale_)
                                      : D2D1::RectF(tx-34*scale_,g.cy+8*scale_,tx+34*scale_,g.cy+22*scale_);
            painter_.DrawText(l10n::Get(tick_labels[i]),lr,compositor_->SmallFormat(),
                value==ticks[i] ? theme.text : theme.text_secondary,
                ticks[i]==0 ? fluent::HorizontalAlignment::Left : fluent::HorizontalAlignment::Center);
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
    if(vm.settings_page==0) {
        section(0,I::SettingsAppearance);section(1,I::SettingsStartupShutdown);section(2,I::SettingsFileList);
        for(const auto& group:lay.group) draw_card(group);
        const bool small_theme=lay.theme_tile[0].left<lay.theme_row.left+100*scale_;
        label(lay.theme_row,l10n::Get(I::SettingsTheme),small_theme ? L"" : l10n::Get(I::SettingsThemeDesc),L"\xE790",
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
        divider(lay.theme_row);
        label(lay.accent_card,l10n::Get(I::SettingsThemeColor),l10n::Get(I::SettingsThemeColorDesc),L"\xE790",lay.accent_picker.left-16*scale_);
        if(vm.settings_bloom) {vm.settings_bloom->SetDisk(lay.accent_picker);vm.settings_bloom->Draw(dc,theme);}
        divider(lay.accent_card);
        const I effects[]={I::EffectNone,I::EffectAcrylic,I::EffectMica,I::EffectMicaAlt};
        const I languages[]={I::LanguageSystem,I::LanguageZhCN,I::LanguageZhTW,I::LanguageEnUS};
        const auto dropdown=[&](D2D1_RECT_F r,D2D1_RECT_F c,I title,I desc,const std::wstring& value,int id) {
            label(r,l10n::Get(title),l10n::Get(desc),id==0 ? L"\xE790" : L"\xE8C1",c.left<r.left+100*scale_ ? r.right-16*scale_ : c.left-12*scale_);
            fluent::ButtonSpec control{};
            control.bounds=c; control.text=value; control.drop_down=true;
            control.state.hovered=IsHovered(vm,H::SettingsDropdown,id);
            painter_.DrawButton(control);
        };
        label(lay.effect_card,l10n::Get(I::SettingsWindowEffect),l10n::Get(compat::ModernWindows() ? I::SettingsWindowEffectDesc : I::EffectUnavailable),L"\xE790",lay.effect_card.right-16*scale_);
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
        divider(lay.effect_card);
        dropdown(lay.language_card,lay.language_choice,I::SettingsLanguage,I::SettingsLanguageDesc,l10n::Get(languages[vm.settings_language]),1);divider(lay.language_card);
        const I text_render[]={I::TextRenderAuto,I::TextRenderSharp,I::TextRenderSmooth};const int text_render_values[]={0,1,2};
        segmented(lay.text_render_card,lay.text_render_row,text_render,text_render_values,vm.settings_text_render,H::SettingsTextRender,I::SettingsTextRender,I::SettingsTextRenderDesc);divider(lay.text_render_card);
        const I ui_font_size[]={I::UiFontSmall,I::UiFontDefault,I::UiFontLarge,I::UiFontLarger};const int ui_font_size_values[]={90,100,112,125};
        segmented(lay.ui_font_size_card,lay.ui_font_size_row,ui_font_size,ui_font_size_values,vm.settings_ui_font_scale,H::SettingsUiFontSize,I::SettingsUiFontSize,I::SettingsUiFontSizeDesc,4,L"\xE8D2");
        {
            text(l10n::Get(I::SettingsIntegration),lay.integration_section);
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
                const auto message=IntegrationProblemMessage(vm);
                fluent::InfoBarSpec bar{};
                bar.bounds=lay.integration_bar;
                bar.title=l10n::Get(failed ? I::IntegrationFailTitle : I::IntegrationDriftTitle);
                bar.message=message;
                bar.kind=failed ? fluent::InfoBarKind::Error : fluent::InfoBarKind::Warning;
                bar.show_close=false;
                painter_.DrawInfoBar(bar);
                if(lay.integration_retry.bottom>lay.integration_retry.top)
                    button(lay.integration_retry,l10n::Get(failed ? I::IntegrationRetry : I::IntegrationReapply),H::SettingsIntegration,5,true);
                if(lay.integration_restore.bottom>lay.integration_restore.top)
                    button(lay.integration_restore,l10n::Get(I::IntegrationRestore),H::SettingsIntegration,6);
            }
            {
                const auto r=lay.integration_list_head;
                painter_.DrawText(l10n::Get(I::IntegrationScope),D2D1::RectF(r.left+54*scale_,r.top+8*scale_,r.right-16*scale_,r.bottom),
                    compositor_->SmallFormat(),theme.text_secondary);
            }
            auto item=[&](D2D1_RECT_F r,I title,I desc,const wchar_t* icon,bool on,int action) {
                fluent::ControlState state{};
                state.checked=on;state.hovered=IsHovered(vm,H::SettingsIntegration,action);
                if(state.hovered) hover_fill(D2D1::RectF(r.left+40*scale_,r.top,r.right-6*scale_,r.bottom));
                const float x=r.left+54*scale_, text_left=r.left+112*scale_;
                painter_.DrawCheckBox(D2D1::RectF(x,r.top+11*scale_,x+20*scale_,r.top+31*scale_),L"",state);
                DrawIconText(x+30*scale_,r.top+11*scale_,20*scale_,20*scale_,icon,L"",theme.text_secondary,master_on ? 0.85f : 0.6f);
                painter_.DrawText(l10n::Get(title),D2D1::RectF(text_left,r.top+9*scale_,r.right-16*scale_,r.top+31*scale_),
                    compositor_->TextFormat(),master_on ? theme.text : theme.text_secondary);
                painter_.DrawWrappedCaption(l10n::Get(desc),D2D1::Point2F(text_left,r.top+31*scale_),
                    (std::max)(40*scale_,r.right-16*scale_-text_left),theme.text_secondary);
            };
            item(lay.startup_row[2],I::IntegrationFolders,I::IntegrationFoldersDesc,L"\xE8B7",vm.settings_integration_folders,1);
            item(lay.this_pc_row,I::SettingsThisPc,I::SettingsThisPcDesc,L"\xE7F4",vm.settings_integration_this_pc,3);
            item(lay.win_e_row,I::SettingsWinE,I::SettingsWinEDesc,L"\xE765",vm.settings_integration_win_e,2);
            item(lay.explorer_windows_row,I::IntegrationExperimental,I::IntegrationExperimentalDesc,L"\xE8A7",vm.settings_integration_experimental,4);
            for(int i=0;i<4;++i) {
                fluent::BadgeSpec chip{};
                chip.bounds=lay.integration_chip[i];chip.text=l10n::Get(kIntegrationChips[i]);
                chip.kind=i==0 ? fluent::BadgeKind::Warning : fluent::BadgeKind::Neutral;
                painter_.DrawBadge(chip);
            }
            if(lay.integration_hint.bottom>lay.integration_hint.top) {
                const auto r=lay.integration_hint;
                divider(D2D1::RectF(r.left,r.top-1,r.right,r.top-1));
                DrawIconText(r.left+54*scale_,r.top+5*scale_,16*scale_,16*scale_,L"\xE946",L"",theme.text_secondary,0.8f);
                painter_.DrawWrappedCaption(IntegrationHint(vm),D2D1::Point2F(r.left+78*scale_,r.top+4*scale_),
                    (std::max)(40*scale_,r.right-16*scale_-r.left-78*scale_),theme.text_secondary);
            }
        }
        toggle(lay.startup_row[0],I::SettingsLaunch,I::SettingsLaunchDesc,L"\xE7E8",vm.settings_launch_on_startup,1);divider(lay.startup_row[0]);
        toggle(lay.start_in_tray_row,I::SettingsStartInTray,I::SettingsStartInTrayDesc,L"\xE921",vm.settings_start_in_tray,27);divider(lay.start_in_tray_row);
        toggle(lay.startup_row[1],I::SettingsKeepRunning,I::SettingsKeepRunningDesc,L"\xE737",vm.settings_keep_running,2);divider(lay.startup_row[1]);
        const I notify_icon[]={I::NotifyIconAlways,I::NotifyIconBackground,I::NotifyIconNever};const int notify_values[]={0,1,2};
        segmented(lay.notify_icon_card,lay.notify_icon_row,notify_icon,notify_values,vm.settings_notify_icon,H::SettingsNotifyIcon,I::SettingsNotifyIcon,I::SettingsNotifyIconDesc,3,L"\xE8A1");divider(lay.notify_icon_card);
        {
            // Default location: buttons sit beside the text, or below it when narrow.
            const bool below=lay.home_folder_choose.top>lay.home_folder_card.top+40*scale_;
            label(lay.home_folder_card,l10n::Get(I::SettingsHomeFolder),
                vm.settings_home_folder.empty() ? l10n::Get(I::ThisPc) : vm.settings_home_folder,L"\xE80F",
                below ? lay.home_folder_card.right-16*scale_ : lay.home_folder_choose.left-12*scale_);
            button(lay.home_folder_choose,l10n::Get(I::SettingsHomeFolderPick),H::SettingsHomeFolder,0);
            button(lay.home_folder_reset,l10n::Get(I::ThisPc),H::SettingsHomeFolder,1,false,!vm.settings_home_folder.empty());
            divider(lay.home_folder_card);
        }
        const I startup_open[]={I::StartupOpenLastTabs,I::OpenDefaultLocation};const int two_values[]={0,1};
        segmented(lay.startup_open_card,lay.startup_open_row,startup_open,two_values,vm.settings_startup_open,H::SettingsStartupOpen,I::SettingsStartupOpen,I::SettingsStartupOpenDesc,2,L"\xE81C");divider(lay.startup_open_card);
        const I new_tab_open[]={I::NewTabOpenCurrent,I::OpenDefaultLocation};
        segmented(lay.new_tab_open_card,lay.new_tab_open_row,new_tab_open,two_values,vm.settings_new_tab_open,H::SettingsNewTabOpen,I::SettingsNewTabOpen,I::SettingsNewTabOpenDesc,2,L"\xE710");divider(lay.new_tab_open_card);
        toggle(lay.close_last_tab_row,I::SettingsCloseLastTab,I::SettingsCloseLastTabDesc,L"\xE711",vm.settings_close_last_tab,26);divider(lay.close_last_tab_row);
        toggle(lay.close_tab_double_click_row,I::SettingsCloseTabDoubleClick,I::SettingsCloseTabDoubleClickDesc,L"\xE962",vm.settings_close_tab_double_click,35);
        const I density[]={I::SettingsDensityCompact,I::SettingsDensityStandard,I::SettingsDensityRoomy};const int heights[]={28,34,40};
        segmented(lay.density_card,lay.density_row,density,heights,vm.settings_row_height,H::SettingsDensity,I::SettingsRowHeight,I::SettingsRowHeightDesc);divider(lay.density_card);
        toggle(lay.performance_row,I::SettingsShowPerformance,I::SettingsShowPerformanceDesc,L"\xE946",vm.settings_show_performance,4);divider(lay.performance_row);
        toggle(lay.list_style_row[0],I::ListSmartDate,I::ListSmartDateDesc,L"\xE787",vm.settings_list_smart_date,17);divider(lay.list_style_row[0]);
        toggle(lay.list_style_row[1],I::ListZebraRows,I::ListZebraRowsDesc,L"\xE8FD",vm.settings_list_zebra_rows,18);divider(lay.list_style_row[1]);
        toggle(lay.list_style_row[2],I::ListSizeBar,I::ListSizeBarDesc,L"\xE9D2",vm.settings_list_size_bar,19);divider(lay.list_style_row[2]);
        toggle(lay.list_style_row[3],I::ListTagNameColor,I::ListTagNameColorDesc,L"\xE8EC",vm.settings_list_tag_names,22);divider(lay.list_style_row[3]);
        toggle(lay.list_style_row[4],I::ListSelectionOutline,I::ListSelectionOutlineDesc,L"\xE73E",vm.settings_list_selection_outline,33);divider(lay.list_style_row[4]);
        toggle(lay.list_style_row[5],I::ListThumbnailBadges,I::ListThumbnailBadgesDesc,L"\xE8B9",vm.settings_list_thumbnail_badges,34);divider(lay.list_style_row[5]);
        const I folder_sort[]={I::FolderSortTop,I::FolderSortFollow,I::FolderSortMixed};const int folder_sort_values[]={0,1,2};
        segmented(lay.folder_sort_card,lay.folder_sort_row,folder_sort,folder_sort_values,vm.settings_folder_sort,H::SettingsFolderSort,I::SettingsFolderSort,I::SettingsFolderSortDesc);divider(lay.folder_sort_card);
        toggle(lay.confirm_delete_row,I::SettingsConfirmDelete,I::SettingsConfirmDeleteDesc,L"\xE74D",vm.settings_confirm_delete,32);
        disclosure(lay.disclosure[0],I::SettingsAdvanced,I::SettingsAdvancedDesc,L"\xE713",0,true);
        if(vm.settings_expanded & 1u) {
        draw_card(lay.wallpaper_card);
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


            const I looks[]={I::WallpaperLookSubtle,I::WallpaperLookBalanced,I::WallpaperLookVivid};const int look_ticks[]={25,50,75};
            draw_card(lay.wallpaper_look_card);slider(lay.wallpaper_look_card,lay.wallpaper_look_row,vm.wallpaper_look,kPanelTransparencyMax,look_ticks,looks,H::SettingsWallpaperLook,I::SettingsWallpaperLook,I::SettingsWallpaperLookDesc,L"%");
            const I blurs[]={I::WallpaperBlurOff,I::WallpaperBlurLight,I::WallpaperBlurStrong};const int blur_ticks[]={0,14,28};
            draw_card(lay.wallpaper_blur_card);slider(lay.wallpaper_blur_card,lay.wallpaper_blur_row,vm.wallpaper_blur,kWallpaperBlurMax,blur_ticks,blurs,H::SettingsWallpaperBlur,I::SettingsWallpaperBlur,I::SettingsWallpaperBlurDesc,L" px");
            const I sizes[]={I::SettingsTraySmall,I::SettingsTrayStandard,I::SettingsTrayLarge};const int icons[]={40,48,56};
            draw_card(lay.tray_icon_card);segmented(lay.tray_icon_card,lay.tray_icon_row,sizes,icons,vm.settings_tray_icon,H::SettingsTrayIcon,I::SettingsTrayIcon,I::SettingsTrayIconDesc);
            draw_card(lay.shell_tags_row);toggle(lay.shell_tags_row,I::SettingsShellTags,I::SettingsShellTagsDesc,L"\xE8EC",vm.settings_shell_tags,21);
            draw_card(lay.hidden_files_row);toggle(lay.hidden_files_row,I::SettingsShowHidden,I::SettingsShowHiddenDesc,L"\xE890",vm.settings_show_hidden_files,5);
            // Hidden + system entries: File Explorer keeps these behind a second option.
            draw_card(lay.protected_files_row);toggle(lay.protected_files_row,I::SettingsShowProtected,I::SettingsShowProtectedDesc,L"\xE72E",vm.settings_show_protected_os_files,16);
            draw_card(lay.pinned_names_row);toggle(lay.pinned_names_row,I::PinnedNames,I::PinnedNamesDesc,L"\xE718",vm.show_pinned_tab_names,6);
            draw_card(lay.vertical_tabs_row);toggle(lay.vertical_tabs_row,I::SettingsVerticalTabs,I::SettingsVerticalTabsDesc,L"\xE7C4",vm.settings_vertical_tabs,23);
            draw_card(lay.hints_row);toggle(lay.hints_row,I::SettingsHints,I::SettingsHintsDesc,L"\xE82F",vm.settings_show_hints,24);
            draw_card(lay.hints_reset_row);label(lay.hints_reset_row,l10n::Get(I::SettingsHintsReset),
                l10n::Get(vm.settings_tips_seen ? I::SettingsHintsResetDesc : I::HintsResetDone),L"\xE72C",lay.hints_reset_button.left-8*scale_);
            button(lay.hints_reset_button,l10n::Get(I::HintsResetButton),H::SettingsToggle,25,false,vm.settings_tips_seen);
            {
                const I blank_click[]={I::SettingsBlankClickOff,I::Back,I::Up};const int blank_click_values[]={0,1,2};
                draw_card(lay.blank_click_row);segmented(lay.blank_click_row,lay.blank_click_choice,blank_click,blank_click_values,vm.settings_blank_click_action,H::SettingsBlankClick,I::SettingsBlankClickBack,I::SettingsBlankClickBackDesc,3,L"\xE72B");
            }
            draw_card(lay.change_tracking_row);toggle(lay.change_tracking_row,I::SettingsChangeTracking,I::SettingsChangeTrackingDesc,L"\xE823",vm.settings_change_tracking,8);
            const I days[]={I::ChangeToday,I::ChangeLast3Days,I::ChangeLast7Days};const int day_values[]={1,3,7};
            draw_card(lay.change_days_row);segmented(lay.change_days_row,lay.change_days,days,day_values,vm.settings_change_days,H::SettingsChangeDays,I::SettingsChangeDays,I::SettingsChangeTrackingDesc);
        }
        text(l10n::Get(I::SettingsImmediate),lay.footer,true);
    } else {
        section(0,I::SearchModeName);section(1,I::SearchModeContent);
        draw_card(lay.group[0]);draw_card(lay.group[1]);
        toggle(lay.global_search_row,I::GlobalSearch,I::GlobalSearchDesc,L"\xE721",vm.settings_global_search_enabled,15);divider(lay.global_search_row);
        label(lay.global_search_hotkey_row,l10n::Get(I::GlobalSearchHotkey),
            vm.settings_global_search_error.empty() ? l10n::Get(I::GlobalSearchHotkeyDesc) : vm.settings_global_search_error,
            L"\xE765",lay.global_search_hotkey_row.right-16*scale_);
        button(lay.global_search_hotkey_button,vm.settings_global_search_capturing ? l10n::Get(I::GlobalSearchRecording) : vm.settings_global_search_hotkey,H::SettingsGlobalSearchHotkey,0);
        divider(lay.global_search_hotkey_row);
        toggle(lay.search_pinyin_row,I::SearchPinyin,I::SearchPinyinDesc,L"\xE721",vm.settings_search_pinyin,9);divider(lay.search_pinyin_row);
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
