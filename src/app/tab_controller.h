#pragma once

#include "app_model.h"
#include "../ui/fluent_menu.h"

#include <functional>

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
        // Closing the window's last tab closes the window: the controller holds
        // no window handle, so it asks its owner instead of doing it itself.
        std::function<void()> close_window;
    };

    explicit TabController(Callbacks callbacks = {}) : callbacks_(std::move(callbacks)) {}
    void SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

    void ToggleGroupCollapse(WindowTabs& tabs, int group_id);
    // Opens a new tab at the end of the group, carrying the last member's
    // folder. Shared by the group menu and the chip hover card.
    void NewTabInGroup(WindowTabs& tabs, int group_id);
    void ShowGroupMenu(WindowTabs& tabs, int group_id, POINT screen_pt, ui::FluentMenu& menu);
    // `multi_instance` gates the "open in a new window" entry: without the mode
    // every action stays inside the running window.
    void ShowTabMenu(WindowTabs& tabs, int tab_index, POINT screen_pt, ui::FluentMenu& menu,
                     bool multi_instance);

    static const uint32_t* Palette() noexcept;
    static constexpr size_t PaletteSize() noexcept { return 8; }

private:
    TabGroup* FindGroup(WindowTabs& tabs, int id) const;
    uint32_t FirstUnusedColor(const WindowTabs& tabs) const;
    void CreateGroupAndEdit(WindowTabs& tabs, int tab_index, POINT screen_pt,
                            ui::FluentMenu& menu);
    void RemoveGroup(WindowTabs& tabs, int group_id) const;
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
