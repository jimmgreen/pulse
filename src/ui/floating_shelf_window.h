#pragma once
#include "FluentTokens.h"
#include "shelf_shared.h"
#include "shell_icons.h"
#include "thumbnail_cache.h"
#include "ui_compositor.h"
#include "ui_view_model.h"
#include "window_material.h"
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

struct FloatingShelfCallbacks {
    // Queries fresh TrayDeckView for the active shelf.
    std::function<TrayDeckView(bool expanded)> build_view;
    // Menus anchored in screen coordinates.
    std::function<void(POINT anchor_screen)> open_switch_menu;
    std::function<void(POINT anchor_screen)> open_more_menu;
    std::function<void(POINT anchor_screen)> open_release_menu;
    // Shelf mutations & actions.
    std::function<void(const std::wstring& new_name)> commit_rename;
    std::function<void()> toggle_pin;
    std::function<void()> dock_to_sidebar;
    std::function<void(bool grid)> view_changed;
    std::function<void(int filter)> filter_changed;
    std::function<void(uint64_t item_id)> toggle_check;
    std::function<void()> clear_checks;
    std::function<void()> compare_checked;
    std::function<void(uint64_t item_id)> remove_item;
    std::function<void()> remove_stale;
    std::function<void(const std::wstring& folder_path)> open_source_folder;
    std::function<void()> release_to_current;
    std::function<void(const std::wstring& dest_dir)> send_to_folder;
    std::function<void()> pick_send_folder;
    // Drag out (`only_ids` empty => whole shelf or checked subset).
    std::function<void(const std::vector<uint64_t>& only_ids)> start_drag_out;
    // Drop `IDataObject` onto shelf (`drop_target`: 0=copy, 1=move, 2=new shelf, >=10=shelf index).
    std::function<bool(IDataObject* obj, int drop_target, bool shift_move)> accept_drop;
    // Paste (`Ctrl+V`) into active shelf.
    std::function<void()> paste_clipboard;
    // Toggle via `Ctrl+Shift+S`.
    std::function<void()> toggle_shelf;
    // Window move / dock-back notifications.
    std::function<void(POINT cursor_screen)> dragging_window;
    std::function<void(POINT cursor_screen, POINT win_pos)> finish_drag_window;
    std::function<void()> auto_hidden;
};

class FloatingShelfWindow {
public:
    FloatingShelfWindow();
    ~FloatingShelfWindow();
    FloatingShelfWindow(const FloatingShelfWindow&) = delete;
    FloatingShelfWindow& operator=(const FloatingShelfWindow&) = delete;

    void SetCallbacks(FloatingShelfCallbacks cb);
    void SetAppearance(bool dark, WindowEffect effect, D2D1_COLOR_F accent, bool view_grid);

    // Shows (or moves if already visible) at screen pixel coordinates (x, y),
    // clamped to the target monitor's work area. For a drag summon, coordinates
    // identify the cursor instead; the window opens around it without activation.
    bool ShowAt(int screen_x, int screen_y, bool expanded = false, bool enter_drop_ready = false);
    // Begins an interactive window drag immediately after tearing off from the sidebar.
    bool ShowTearOff(POINT cursor_screen);
    void Hide();
    void Shutdown();

    bool Visible() const noexcept;
    bool Expanded() const noexcept;
    void SetExpanded(bool expanded);
    HWND Hwnd() const noexcept;
    RECT WindowBounds() const noexcept;

    // Starts inline editing of the active shelf's name in the header.
    void BeginInlineRename(const std::wstring& current_name);

    // Repaints and resizes to match the latest shelf state; arms auto-hide if
    // `arm_auto_hide_when_empty` and the shelf just became empty while unpinned.
    void Refresh(bool arm_auto_hide_when_empty = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::ui
