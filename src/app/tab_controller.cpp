#include "tab_controller.h"

#include "context_menu.h"

#include <algorithm>
#include <memory>

namespace pulse::app {

namespace {

constexpr uint32_t kPalette[] = {
    0xE74856, 0xF7630C, 0xFFB900, 0x6CCB5F,
    0x00B7C3, 0x0078D4, 0x8661C5, 0xE3008C,
};

ui::FluentMenuItem MenuItem(int command, const wchar_t* text,
                            const wchar_t* glyph = nullptr) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = text;
    if (glyph) item.glyph = glyph;
    return item;
}

} // namespace

const uint32_t* TabController::Palette() noexcept {
    return kPalette;
}

TabGroup* TabController::FindGroup(WindowTabs& tabs, int id) const {
    for (auto& group : tabs.tab_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

void TabController::Changed() const {
    if (callbacks_.invalidate) callbacks_.invalidate();
}

void TabController::LayoutChanged() const {
    if (callbacks_.layout_changed) callbacks_.layout_changed();
}

void TabController::WillChangeLayout() const {
    if (callbacks_.will_change_layout) callbacks_.will_change_layout();
}

void TabController::ToggleGroupCollapse(WindowTabs& tabs, int group_id) {
    TabGroup* group = FindGroup(tabs, group_id);
    if (!group) return;
    group->collapsed = !group->collapsed;
    if (group->collapsed && tabs.active < tabs.items.size() &&
        tabs.items[tabs.active]->tab_group == group_id) {
        const auto visible = [&](size_t index) {
            const int owner = tabs.items[index]->tab_group;
            if (owner == 0) return true;
            const TabGroup* candidate = FindGroup(tabs, owner);
            return !candidate || !candidate->collapsed;
        };
        const size_t current = tabs.active;
        size_t target = tabs.items.size();
        for (size_t i = current + 1; i < tabs.items.size(); ++i) {
            if (visible(i)) { target = i; break; }
        }
        if (target == tabs.items.size()) {
            for (size_t i = current; i-- > 0;) {
                if (visible(i)) { target = i; break; }
            }
        }
        if (target < tabs.items.size()) {
            WillChangeLayout();
            tabs.SwitchTab(target);
            LayoutChanged();
        }
    }
    Changed();
}

uint32_t TabController::FirstUnusedColor(const WindowTabs& tabs) const {
    for (uint32_t color : kPalette) {
        const bool used = std::any_of(tabs.tab_groups.begin(), tabs.tab_groups.end(),
            [color](const TabGroup& group) { return group.color_rgb == color; });
        if (!used) return color;
    }
    return kPalette[tabs.tab_groups.size() % std::size(kPalette)];
}

void TabController::CreateGroupAndEdit(WindowTabs& tabs, int tab_index, POINT screen_pt,
                                       ui::FluentMenu& menu) {
    if (tab_index < 0 || tab_index >= static_cast<int>(tabs.items.size())) return;
    TabGroup group;
    group.id = tabs.next_tab_group_id++;
    group.color_rgb = FirstUnusedColor(tabs);
    tabs.tab_groups.push_back(group);
    tabs.items[static_cast<size_t>(tab_index)]->tab_group = group.id;
    Changed();
    ShowGroupMenu(tabs, group.id, screen_pt, menu);
}

void TabController::ShowGroupMenu(WindowTabs& tabs, int group_id, POINT screen_pt,
                                  ui::FluentMenu& menu) {
    TabGroup* group = FindGroup(tabs, group_id);
    if (!group) return;
    const int id = group->id;
    menu.SetFilterPlaceholder(L"标签组名称…");
    menu.SetInitialFilterText(group->name);
    menu.SetFilterMinWidth(260.0f);
    const auto build = [&](const std::wstring& query) {
        TabGroup* current = FindGroup(tabs, id);
        if (current && current->name != query) {
            current->name = query;
            Changed();
        }
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem colors;
        for (int i = 0; i < static_cast<int>(std::size(kPalette)); ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = CmdTabColorBase + i;
            swatch.color = ui::HexColor(kPalette[i]);
            swatch.checked = current && current->color_rgb == kPalette[i];
            colors.quick_swatches.push_back(swatch);
        }
        colors.separator_after = true;
        items.push_back(std::move(colors));
        items.push_back(MenuItem(CmdTabGroupNewTab, L"在组中新建标签页"));
        items.push_back(MenuItem(CmdTabGroupUngroup, L"取消组合"));
        items.push_back(MenuItem(CmdTabGroupClose, L"关闭分组标签页"));
        return items;
    };
    const int command = menu.TrackPopup(screen_pt, build(group->name), build);
    menu.SetFilterPlaceholder(L"搜索命令、文件夹…");

    if (command >= CmdTabColorBase &&
        command < CmdTabColorBase + static_cast<int>(std::size(kPalette))) {
        if (TabGroup* current = FindGroup(tabs, id)) {
            current->color_rgb = kPalette[command - CmdTabColorBase];
        }
    } else if (command == CmdTabGroupNewTab) {
        int last_member = -1;
        for (int i = 0; i < static_cast<int>(tabs.items.size()); ++i) {
            if (tabs.items[static_cast<size_t>(i)]->tab_group == id) last_member = i;
        }
        if (last_member >= 0) {
            const Tab* folder = tabs.items[static_cast<size_t>(last_member)]->ActiveFolder();
            const std::wstring path = folder ? folder->current_path : L"C:\\";
            WillChangeLayout();
            tabs.NewTabAt(static_cast<size_t>(last_member + 1), path);
            tabs.Active()->tab_group = id;
            LayoutChanged();
            if (callbacks_.load_tab) {
                if (Tab* created = tabs.Active()->ActiveFolder())
                    callbacks_.load_tab(*created);
            }
        }
    } else if (command == CmdTabGroupUngroup) {
        RemoveGroup(tabs, id);
    } else if (command == CmdTabGroupClose) {
        WillChangeLayout();
        for (int i = static_cast<int>(tabs.items.size()) - 1; i >= 0; --i) {
            if (tabs.items[static_cast<size_t>(i)]->tab_group == id) {
                tabs.CloseTab(static_cast<size_t>(i));
            }
        }
        RemoveGroup(tabs, id);
        LayoutChanged();
    }
    Changed();
}

void TabController::RemoveGroup(WindowTabs& tabs, int group_id) const {
    for (auto& tab : tabs.items) {
        if (tab->tab_group == group_id) tab->tab_group = 0;
    }
    tabs.tab_groups.erase(std::remove_if(tabs.tab_groups.begin(), tabs.tab_groups.end(),
        [group_id](const TabGroup& group) { return group.id == group_id; }),
        tabs.tab_groups.end());
}

void TabController::PruneEmptyGroups(WindowTabs& tabs) const {
    tabs.tab_groups.erase(std::remove_if(tabs.tab_groups.begin(), tabs.tab_groups.end(),
        [&](const TabGroup& group) {
            return std::none_of(tabs.items.begin(), tabs.items.end(),
                [&](const std::unique_ptr<LayoutTab>& tab) {
                    return tab->tab_group == group.id;
                });
        }), tabs.tab_groups.end());
}

void TabController::CloseTabs(WindowTabs& tabs, int first, int last, int except) const {
    WillChangeLayout();
    for (int i = last; i >= first; --i) {
        if (i != except) tabs.CloseTab(static_cast<size_t>(i));
    }
    PruneEmptyGroups(tabs);
    LayoutChanged();
}

void TabController::TogglePin(WindowTabs& tabs, int index) {
    if (index < 0 || index >= static_cast<int>(tabs.items.size())) return;
    LayoutTab& tab = *tabs.items[static_cast<size_t>(index)];
    size_t first_unpinned = 0;
    while (first_unpinned < tabs.items.size() && tabs.items[first_unpinned]->pinned) {
        ++first_unpinned;
    }
    const bool pin = !tab.pinned;
    if (pin) tab.tab_group = 0;
    tab.pinned = pin;
    tabs.MoveTab(static_cast<size_t>(index),
                 pin ? first_unpinned : (first_unpinned > 0 ? first_unpinned - 1 : 0));
    PruneEmptyGroups(tabs);
    Changed();
}

void TabController::ShowTabMenu(WindowTabs& tabs, int tab_index, POINT screen_pt,
                                ui::FluentMenu& menu) {
    if (tab_index < 0 || tab_index >= static_cast<int>(tabs.items.size())) return;
    LayoutTab& tab = *tabs.items[static_cast<size_t>(tab_index)];
    std::vector<ui::FluentMenuItem> items;
    items.push_back(MenuItem(CmdTabNewRight, L"在右侧新建标签页", L"\xE710"));
    items.push_back(MenuItem(CmdTabDuplicate, L"复制标签页", L"\xE8C8"));
    items.push_back(MenuItem(CmdTabPin,
        tab.pinned ? L"取消固定标签页" : L"固定标签页", L"\xE718"));
    items.back().separator_after = true;
    if (tab.tab_group == 0) {
        items.push_back(MenuItem(CmdTabAddToNewGroup,
            tabs.tab_groups.empty() ? L"创建新组" : L"将标签页添加到新组"));
        if (!tabs.tab_groups.empty()) {
            ui::FluentMenuItem join;
            join.text = L"将标签页添加到";
            for (size_t i = 0; i < tabs.tab_groups.size(); ++i) {
                auto child = MenuItem(CmdTabJoinGroupBase + static_cast<int>(i),
                    tabs.tab_groups[i].name.empty() ? L"(未命名组)"
                                                    : tabs.tab_groups[i].name.c_str());
                join.children.push_back(std::move(child));
            }
            items.push_back(std::move(join));
        }
        items.back().separator_after = true;
    } else {
        items.push_back(MenuItem(CmdTabRemoveFromGroup, L"从组中移除该标签页"));
        items.back().separator_after = true;
    }
    items.push_back(MenuItem(CmdTabClose, L"关闭标签页", L"\xE711"));
    items.back().enabled = !tab.pinned && tabs.items.size() > 1;
    items.push_back(MenuItem(CmdTabCloseOthers, L"关闭其他标签页"));
    items.push_back(MenuItem(CmdTabCloseRight, L"关闭右侧标签页"));

    const int command = menu.TrackPopup(screen_pt, std::move(items));
    if (command == CmdTabNewRight || command == CmdTabDuplicate) {
        const Tab* folder = tab.ActiveFolder();
        const Tab* current = tabs.Active() ? tabs.Active()->ActiveFolder() : nullptr;
        const std::wstring path = command == CmdTabDuplicate
            ? (folder ? folder->current_path : L"C:\\")
            : (current ? current->current_path : L"C:\\");
        WillChangeLayout();
        tabs.NewTabAt(static_cast<size_t>(tab_index) + 1, path);
        LayoutChanged();
        if (callbacks_.load_tab) {
            if (Tab* created = tabs.Active()->ActiveFolder())
                callbacks_.load_tab(*created);
        }
    } else if (command == CmdTabPin) {
        TogglePin(tabs, tab_index);
        return;
    } else if (command == CmdTabAddToNewGroup) {
        CreateGroupAndEdit(tabs, tab_index, screen_pt, menu);
        return;
    } else if (command == CmdTabRemoveFromGroup) {
        tab.tab_group = 0;
        NormalizeGroupRuns(tabs);
        PruneEmptyGroups(tabs);
    } else if (command == CmdTabClose) {
        CloseTabs(tabs, tab_index, tab_index);
    } else if (command == CmdTabCloseOthers) {
        CloseTabs(tabs, 0, static_cast<int>(tabs.items.size()) - 1, tab_index);
    } else if (command == CmdTabCloseRight) {
        CloseTabs(tabs, tab_index + 1, static_cast<int>(tabs.items.size()) - 1);
    } else if (command >= CmdTabJoinGroupBase &&
               command < CmdTabJoinGroupBase + static_cast<int>(tabs.tab_groups.size())) {
        tab.tab_group = tabs.tab_groups[static_cast<size_t>(command - CmdTabJoinGroupBase)].id;
        NormalizeGroupRuns(tabs);
    }
    Changed();
}

} // namespace pulse::app
