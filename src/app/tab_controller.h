#pragma once

#include "app_model.h"
#include "../ui/fluent_menu.h"

#include <functional>
#include <vector>

namespace pulse::app {

class TabController {
public:
    struct Callbacks {
        std::function<void(Tab&)> load_tab;
        std::function<void()> invalidate;
        std::function<void()> layout_changed;
        std::function<void()> will_change_layout;
        // Replaces `path` with the default location when new tabs open there.
        std::function<bool(std::wstring&)> default_new_tab;
        // Closing the only tab closes the window when this returns true.
        std::function<bool()> last_tab_closes_window;
        std::function<void()> close_window;
        // Right before the tabs at these indices are closed by hand (M7f:
        // Ctrl+Shift+T brings them back). Only the tabs that will close.
        std::function<void(const std::vector<size_t>&)> closing_tabs;
        std::function<void(size_t)> save_workspace;
    };

    explicit TabController(Callbacks callbacks = {}) : callbacks_(std::move(callbacks)) {}
    void SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

    void ToggleGroupCollapse(WindowTabs& tabs, int group_id);
    void ShowGroupMenu(WindowTabs& tabs, int group_id, POINT screen_pt, ui::FluentMenu& menu);
    void ShowTabMenu(WindowTabs& tabs, int tab_index, POINT screen_pt, ui::FluentMenu& menu);

    static const uint32_t* Palette() noexcept;
    static constexpr size_t PaletteSize() noexcept { return 8; }

private:
    friend struct TabControllerTestAccess;
    void DuplicateTab(WindowTabs& tabs, size_t index);
    TabGroup* FindGroup(WindowTabs& tabs, int id) const;
    uint32_t FirstUnusedColor(const WindowTabs& tabs) const;
    void CreateGroupAndEdit(WindowTabs& tabs, int tab_index, POINT screen_pt,
                            ui::FluentMenu& menu);
    void RemoveGroup(WindowTabs& tabs, int group_id) const;
    void ClosingTabs(const WindowTabs& tabs, std::vector<size_t> indices) const;
    void PruneEmptyGroups(WindowTabs& tabs) const;
    void CloseTabs(WindowTabs& tabs, int first, int last, int except = -1) const;
    void TogglePin(WindowTabs& tabs, int index);
    void Changed() const;
    void LayoutChanged() const;
    void WillChangeLayout() const;
    void OpenCreatedTab(WindowTabs& tabs, size_t index, std::wstring path,
                        int group_id = 0);

    Callbacks callbacks_;
};

} // namespace pulse::app
