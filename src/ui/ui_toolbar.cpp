#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"

namespace pulse::ui {
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = ToolbarLayoutAt(rect.right,NewButtonWidthPx(compact),vm.pane.filter_expand);
    const bool searchOverlay=vm.address_searching && rect.right-left<480*scale_;
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            // Up is disabled at the top of the tree (This PC with no history),
            // the way Back and Forward already are (#146).
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward
                                  : i==2 ? vm.can_go_up : true;
            fluent::ButtonSpec button;
            button.bounds = r; button.glyph = nav_glyphs[i];
            button.state.enabled = enabled;
            button.state.hovered = enabled && IsHovered(vm, nav_hits[i]);
            painter_.DrawCommandButton(button);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.focused = vm.address_editing && !vm.address_searching;
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!vm.address_editing || vm.address_searching) {
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if ((int)i == vm.breadcrumb_hover) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
                if (i > 0) {
                    // Chevron separator.
                    float chX = seg.rc.left - 14.0f * scale_;
                    DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.55f);
                }
                const bool search_crumb = vm.pane.is_query_search && vm.pane.has_search_origin &&
                    i + 1 == placed.size();
                if (search_crumb && (int)i != vm.breadcrumb_hover && (int)i != vm.breadcrumb_drop) {
                    auto tint = theme.accent;
                    tint.a *= 0.16f;
                    ComPtr<ID2D1SolidColorBrush> tint_brush;
                    dc->CreateSolidColorBrush(tint, &tint_brush);
                    if (tint_brush.get()) FillRoundedRect(dc, tint_brush.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top, theme.radius_control * scale_);
                }
                MakeBrush(dc, theme.text, brText_);
                // Width measured exactly; let the ink use the right padding as slack
                // so the trailing glyph is not shaved by the clip rect.
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                    seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                    seg.rc.bottom - seg.rc.top);
            }
            if (placed.empty() && !vm.pane.path.empty()) {
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                    addrRc.left + 12.0f * scale_, addrRc.top,
                    addrRc.right - addrRc.left - 18.0f * scale_,
                    addrRc.bottom - addrRc.top);
            }
        }
        dc->PopAxisAlignedClip();
    }
    DrawAddressSearchChrome(vm, rect.right, theme);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    // Embedded hosts (the file picker) keep the address row only.
    if (embedded_) return;
    const std::wstring label=l10n::Get(l10n::StringId::New);
    fluent::ButtonSpec create;
    create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
    create.glyph=kIconAdd; create.kind=fluent::ButtonKind::Primary;
    create.icon_only=compact; create.drop_down=!compact;
    create.state.hovered=IsHovered(vm,HitTestResult::NewButton);
    painter_.DrawButton(create);
    const wchar_t* glyphs[]={kIconCut,kIconCopy,kIconPaste,kIconRename,kIconDelete,kIconSplit,
        vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,L""};
    const HitTestResult::Region hits[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
        HitTestResult::Rename,HitTestResult::Delete,HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
    for(int i=0;i<8;++i) {
        if (layout.commands[i].right <= layout.commands[i].left) continue;
        const bool enabled=i==2 || i>=5 || vm.pane.selected_count>0;
        const bool selected=(i==5 && vm.pane_slots.size()>1) || (i==6 && vm.details_visible) ||
            (i==7 && vm.pane.column_strip.enabled);
        DrawButton(layout.commands[i],theme,enabled && IsHovered(vm,hits[i]) ? theme.fill_hover :
            selected ? theme.fill_selected : kTransparent,
            i==5 ? L"" : glyphs[i],L"",!enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,true,true);
        if (i==5) {
            // The split button shows the current layout (1 / 2 side by side /
            // 2 stacked / 3 / 4). A pane count that does not match the preset
            // (a custom split) is described from the pane rectangles instead.
            constexpr int kPaneCount[]={1,2,2,3,4};
            int preset=std::clamp(vm.layout_preset,0,4);
            const int panes=static_cast<int>(vm.pane_slots.size());
            if (panes>0 && kPaneCount[preset]!=panes) {
                if (panes==1) preset=0;
                else if (panes==2) preset=vm.pane_slots[1].rect.top>=vm.pane_slots[0].rect.bottom-1.0f ? 2 : 1;
                else preset=panes==3 ? 3 : 4;
            }
            constexpr PaneHeaderIcon kIcons[]={PaneHeaderIcon::SplitSingle,PaneHeaderIcon::Split,
                PaneHeaderIcon::SplitStacked,PaneHeaderIcon::SplitThree,PaneHeaderIcon::SplitFour};
            DrawPaneHeaderIcon(layout.commands[i],kIcons[preset],selected ? theme.accent : theme.text_secondary);
        }
        if (i==7) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Columns,selected ? theme.accent : theme.text_secondary);
    }
    if (layout.overflow.right > layout.overflow.left) {
        DrawButton(layout.overflow,theme,IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover : kTransparent,
            L"\xE712",L"",theme.text_secondary,true,true);
    }
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, l10n::StringId label,
                             HitTestResult::Region region, bool dropdown, float turn = 0.0f) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.chevron_turn=turn;
        button.state.hovered=IsHovered(vm,region);
        painter_.DrawButton(button);
    };
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),layout.sort.left-6*scale_,layout.sort.top+7*scale_,scale_,18*scale_);
    // The shared drum picker turns the chevron of the button that opened it,
    // in step with its open/close fade.
    const float picker_open=group_wheel_.OpenAmount();
    const bool sort_picker=group_wheel_.IsSortPicker();
    command(layout.sort,L"\xE8CB",l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true,
        sort_picker ? picker_open : 0.0f);
    if (vm.pane.filter_expand <= 0.015f && !vm.filter_editing) {
        command(layout.filter,kIconFilter,l10n::StringId::ToolbarFilter,HitTestResult::FilterBox,false);
    } else {
        fluent::TextFieldSpec field;
        field.bounds=layout.filter;
        field.placeholder=l10n::Get(l10n::StringId::FilterPlaceholder);
        field.text=vm.pane.filter_text;
        field.leading_glyph=kIconFilter;
        field.compact_leading_glyph=true;
        field.suppress_text=vm.filter_editing;
        field.state.focused=vm.filter_editing;
        field.state.hovered=IsHovered(vm,HitTestResult::FilterBox);
        if (!vm.pane.filter_text.empty()) field.trailing_width=30;
        painter_.DrawTextField(field);
        if (!vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f) {
            DrawButton(FilterClearRect(rect,vm.pane.filter_expand),theme,
                IsHovered(vm,HitTestResult::FilterClear) ? theme.fill_hover : kTransparent,
                L"\xE711",L"",theme.text_secondary,true,true,0.65f);
        }
    }
    // "Group" sits with Sort and Filter so grouping is findable and, once on,
    // always visible: the chip names the grouping and its x switches it off.
    if (layout.group.right > layout.group.left) {
        const D2D1_RECT_F g=layout.group;
        const bool icon_only=g.right-g.left < 60*scale_;
        if (toolbar_group_ <= 0) {
            command(g,L"\xF168",l10n::StringId::ToolbarGroup,HitTestResult::ToolbarGroup,true,
                sort_picker ? 0.0f : picker_open);
        } else if (icon_only) {
            DrawButton(g,theme,IsHovered(vm,HitTestResult::ToolbarGroup) ? theme.fill_hover : theme.fill_selected,
                L"\xF168",L"",theme.accent,true,true);
        } else {
            const float radius=(g.bottom-g.top)*0.5f;
            D2D1_COLOR_F fill=theme.accent;
            fill.a*=IsHovered(vm,HitTestResult::ToolbarGroup) ? 0.26f : 0.16f;
            painter_.FillRoundedRect(g,radius,fill);
            DrawIconText(g.left+6*scale_,g.top,24*scale_,g.bottom-g.top,L"\xF168",L"",theme.accent,0.8f);
            const D2D1_RECT_F x=ToolbarGroupClearRect(g,scale_);
            if (IsHovered(vm,HitTestResult::ToolbarGroupClear)) {
                D2D1_COLOR_F xf=theme.accent;
                xf.a*=0.28f;
                painter_.FillRoundedRect(x,(x.bottom-x.top)*0.5f,xf);
            }
            DrawIconText(x.left,x.top,x.right-x.left,x.bottom-x.top,kIconCloseSmall,L"x",theme.accent,0.5f);
            std::wstring group_label=ToolbarGroupLabel();
            const D2D1_RECT_F text_rc=D2D1::RectF(g.left+30*scale_,g.top,x.left-2*scale_,g.bottom);
            if (IDWriteFactory2* f=compositor_->DwriteFactory()) {
                group_label=FitEndEllipsis(group_label,std::max(0.0f,text_rc.right-text_rc.left),
                    [&](const std::wstring& t){ return MeasureTextWidth(f,compositor_->TextFormat(),t); });
            }
            painter_.DrawText(group_label,text_rc,compositor_->TextFormat(),theme.accent);
        }
    }
}
std::wstring MainRenderer::ToolbarGroupLabel() const {
    constexpr l10n::StringId names[]={l10n::StringId::GroupByName,l10n::StringId::GroupByDate,
        l10n::StringId::GroupByType,l10n::StringId::GroupBySize,l10n::StringId::GroupByTag,
        l10n::StringId::Location};
    std::wstring label=l10n::Get(l10n::StringId::ToolbarGroupActive);
    const size_t at=label.find(L"{g}");
    if (at!=std::wstring::npos) label.replace(at,3,l10n::Get(names[std::clamp(toolbar_group_,1,6)-1]));
    return label;
}
}