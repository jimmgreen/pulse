#include "staging_shelf_controller.h"

#include "app_internal.h"
#include "app_messages.h"
#include "app_ops_ui.h"
#include "drag_shake_watcher.h"
#include "shelf_snippets.h"
#include "../common/windows_compat.h"
#include "../ui/drag_drop.h"
#include "../ui/confirm_dialog.h"
#include "../ui/fluent_menu.h"
#include "../ui/floating_shelf_window.h"
#include "../ui/ui_renderer.h"

#include <algorithm>

namespace pulse {
namespace {

using I = l10n::StringId;

std::wstring Label(I id, std::wstring_view name) {
    auto text = l10n::Get(id);
    if (const auto at = text.find(L"{name}"); at != std::wstring::npos) text.replace(at, 6, name);
    return text;
}

void InvalidateShelves(AppState& s) {
    app::ForEachWindowSharingCore(s, [](AppState& w) { InvalidateRect(w.hwnd, nullptr, FALSE); });
}

bool AtSidebar(AppState& s, POINT point) {
    if (!IsWindowVisible(s.hwnd) || IsIconic(s.hwnd)) return false;
    ScreenToClient(s.hwnd, &point);
    RECT client{};
    GetClientRect(s.hwnd, &client);
    const float width = static_cast<float>(client.right);
    if (s.renderer.SidebarRailVisible(width) && !s.renderer.SidebarPeekVisible(width)) return false;
    // The sidebar (docked or its overlay) starts under the title and address rows.
    const D2D1_RECT_F sidebar = s.renderer.SidebarRect(width, static_cast<float>(client.bottom));
    client.right = static_cast<LONG>(sidebar.right);
    client.top = static_cast<LONG>(sidebar.top);
    return PtInRect(&client, point) != FALSE;
}

std::vector<uint64_t> CheckedItems(AppState& s, bool expanded) {
    return expanded ? s.trayView.checked : std::vector<uint64_t>{};
}

bool CollectShelfItems(AppState& s, const std::vector<app::ShelfContentItem>& incoming, int target, bool move) {
    auto& tray = s.tray;
    const bool create = target == 2;
    uint64_t shelf = tray.active_shelf_id();
    if (target >= 10) {
        const auto i = static_cast<size_t>(target - 10);
        if (i >= tray.shelves().size()) return false;
        shelf = tray.shelves()[i].id;
    }
    std::vector<app::TrayItem> items;
    items.reserve(incoming.size());
    for (const auto& entry : incoming) {
        app::TrayItem item;
        item.path = entry.path;
        item.content_kind = static_cast<uint8_t>(entry.kind);
        item.owned_snippet = entry.owned_snippet;
        item.display_name = entry.display_name;
        item.domain = entry.domain;
        item.source_label = entry.source_label;
        items.push_back(std::move(item));
    }
    if (items.empty()) return false;
    if (create) shelf = tray.CreateShelf();
    if (!tray.CollectRichToShelf(shelf, items, move)) {
        if (create) tray.DeleteShelf(shelf);
        return false;
    }
    InvalidateShelves(s);
    return true;
}

} // namespace

class StagingShelfController {
public:
    explicit StagingShelfController(AppCore& core) : core_(core) {
        ui::FloatingShelfCallbacks cb;
        cb.build_view = [this](bool expanded) {
            auto* s = Owner();
            if (!s) return ui::TrayDeckView{};
            auto view = BuildFloatingShelfView(*s, expanded);
            if (const auto* tab = ActiveTab(*s); tab && !view.target.empty()) {
                // Display the same plan executed on click, not the number of cards.
                const auto plan = s->tray.PlanRelease(tab->current_path, release_mode_, CheckedItems(*s, expanded));
                view.release_copy_count = static_cast<int>(plan.copies.sources.size());
                view.release_move_count = static_cast<int>(plan.moves.sources.size());
            } else {
                view.release_copy_count = view.release_move_count = 0;
            }
            view.release_mode = static_cast<int>(release_mode_);
            view.dragging_out = dragging_out_;
            return view;
        };
        cb.open_switch_menu = [this](POINT p) { if (auto* s = Owner()) Menu(*s, p); };
        cb.open_more_menu = cb.open_switch_menu;
        cb.open_release_menu = [this](POINT p) { if (auto* s = Owner()) ReleaseMenu(*s, p); };
        cb.commit_rename = [this](const std::wstring& name) {
            if (auto* s = Owner(); s && s->tray.RenameShelf(s->tray.active_shelf_id(), name)) Refresh(*s);
        };
        cb.toggle_pin = [this] {
            if (auto* s = Owner()) {
                s->appPrefs.shelf_pinned = !s->appPrefs.shelf_pinned;
                s->appPrefs.Save();
                Refresh(*s);
            }
        };
        cb.dock_to_sidebar = [this] { if (auto* s = Owner()) Hide(*s); };
        cb.view_changed = [this](bool grid) {
            core_.appPrefs.shelf_view_grid = grid;
            core_.appPrefs.Save();
        };
        cb.filter_changed = [this](int filter) {
            if (auto* s = Owner()) s->trayView.filter = std::clamp(filter, 0, 4);
        };
        cb.toggle_check = [this](uint64_t id) {
            if (auto* s = Owner()) {
                auto& checks = s->trayView.checked;
                if (const auto at = std::find(checks.begin(), checks.end(), id); at != checks.end()) checks.erase(at);
                else checks.push_back(id);
            }
        };
        cb.clear_checks = [this] { if (auto* s = Owner()) s->trayView.checked.clear(); };
        cb.compare_checked = [this] {
            if (auto* s = Owner(); s && s->trayView.checked.size() == 2) {
                s->trayView.open = true;
                s->trayView.compare = true;
                s->trayView.compare_at = ui::motion::NowMs();
                Hide(*s, false);
            }
        };
        cb.remove_item = [this](uint64_t id) {
            if (auto* s = Owner(); s && s->tray.RemoveItemById(id)) Refresh(*s, true);
        };
        cb.remove_stale = [this] { if (auto* s = Owner()) { TrayRemoveStale(*s); Refresh(*s); } };
        cb.open_source_folder = [this](const std::wstring& path) {
            if (auto* s = Owner()) NavigateTo(*s, path);
        };
        cb.release_to_current = [this] {
            if (auto* s = Owner()) if (const auto* tab = ActiveTab(*s); tab && !tab->net_readonly)
                Release(*s, tab->current_path);
        };
        cb.send_to_folder = [this](const std::wstring& path) { if (auto* s = Owner()) Release(*s, path); };
        cb.pick_send_folder = [this] { if (auto* s = Owner()) PickDestination(*s); };
        cb.start_drag_out = [this](const std::vector<uint64_t>& ids) { if (auto* s = Owner()) DragOut(*s, ids); };
        cb.accept_drop = [this](IDataObject* obj, int target, bool shift) {
            auto* s = Owner();
            if (!s) return false;
            return dragging_out_ ? MoveToShelf(*s, target) : StageShelfDrop(*s, obj, target, shift);
        };
        cb.paste_clipboard = [this] {
            auto* s = Owner();
            if (!s) return;
            if (TrayClipLive(*s)) TakeTrayClip(*s);
            else {
                std::vector<app::ShelfContentItem> items;
                if (app::ExtractClipboardShelfItems(window_.Hwnd(), app::DefaultShelfSnippetsDir(), items))
                    CollectShelfItems(*s, items, 0, false);
            }
            Refresh(*s);
        };
        cb.toggle_shelf = [this] { if (auto* s = Owner()) Hide(*s); };
        cb.dragging_window = [this](POINT p) {
            AppState* next = SidebarAt(p);
            const HWND hit = next ? next->hwnd : nullptr;
            if (hit == dock_hot_) return;
            dock_hot_ = hit;
            if (auto* s = Owner()) InvalidateShelves(*s);
        };
        cb.finish_drag_window = [this](POINT p, POINT pos) {
            if (auto* s = Owner()) {
                auto& prefs = s->appPrefs;
                prefs.shelf_has_last_pos = true;
                prefs.shelf_last_x = pos.x;
                prefs.shelf_last_y = pos.y;
                prefs.Save();
                if (SidebarAt(p)) Hide(*s);
                else { dock_hot_ = nullptr; InvalidateShelves(*s); }
            }
        };
        cb.auto_hidden = [this] { if (auto* s = Owner()) Hide(*s); };
        window_.SetCallbacks(std::move(cb));
    }

    bool Visible() const { return window_.Visible(); }
    bool DockHot(HWND hwnd) const { return dock_hot_ == hwnd; }
    bool DragActive() const { return dragging_out_; }

    bool ReleaseDrop(AppState& s, const std::wstring& dest) {
        if (!dragging_out_ || s.tray.active_shelf_id() != drag_source_ || drag_ids_.empty()) return false;
        if (!ReleaseTrayTo(s, dest, release_mode_, drag_ids_)) return false;
        RememberTrayDest(s, dest);
        Refresh(s);
        return true;
    }

    void Start(AppState& s) {
        owner_ = s.hwnd;
        listener_ = s.hwnd;
        core_.tray.LoadNamedShelvesFile();
        saved_ = core_.tray.NamedShelvesToJson();
        UpdateWatcher();
    }

    void Tick(AppState& s) {
        if (s.hwnd != listener_) return;
        UpdateWatcher();
        const auto now = GetTickCount64();
        if (now >= save_at_) {
            save_at_ = now + 1500;
            Save();
        }
        if (now < refresh_at_ || !Visible()) return;
        refresh_at_ = now + 240;
        auto* owner = Owner();
        if (!owner) return;
        const auto& tray = owner->tray;
        size_t hash = std::hash<uint64_t>{}(tray.active_shelf_id());
        auto add = [&](size_t value) { hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2); };
        if (const auto* tab = ActiveTab(*owner)) add(std::hash<std::wstring>{}(tab->current_path));
        add(owner->darkMode);
        add(std::hash<std::wstring>{}(owner->appPrefs.window_effect));
        for (float c : {owner->accentColor.r, owner->accentColor.g, owner->accentColor.b})
            add(std::hash<float>{}(c));
        for (const auto& shelf : tray.shelves()) {
            add(shelf.id);
            add(shelf.color_rgb);
            add(std::hash<std::wstring>{}(shelf.name));
            for (const auto& batch : shelf.batches) {
                add(batch.move_intent);
                for (const auto& item : batch.items) {
                    add(item.id);
                    add(std::hash<std::wstring>{}(item.path));
                    add(item.exists);
                    add(item.inflight_task);
                    add(static_cast<size_t>(item.size));
                }
            }
        }
        if (hash == last_hash_) return;
        last_hash_ = hash;
        const bool emptied = last_total_ > 0 && tray.batches().empty();
        last_total_ = TrayItemTotalCount(tray);
        Appearance(*owner);
        Refresh(*owner, emptied);
    }

    void Close(AppState& s, AppState* next) {
        if (!next) {
            watcher_->Update(nullptr, 0, false, 1);
            core_.doubleCtrl.SetInputObserver({});
            input_observer_installed_ = false;
            window_.Shutdown();
            Save();
            core_.tray.ClearTempShelf();
            return;
        }
        if (owner_ == s.hwnd) owner_ = next->hwnd;
        if (dock_hot_ == s.hwnd) dock_hot_ = nullptr;
        if (listener_ == s.hwnd) { listener_ = next->hwnd; UpdateWatcher(); }
    }

    void Summon() {
        if (!app::IsSystemOleDragActive()) return;
        if (auto* s = core_.windows.MostRecent()) Show(*s, watcher_->LastTriggerPoint(), false, true);
    }

    void Show(AppState& s, POINT p, bool expanded, bool during_drag = false) {
        Bind(s);
        const auto& prefs = s.appPrefs;
        const HMONITOR monitor = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (!during_drag && prefs.shelf_summon_pos == 1 && prefs.shelf_has_last_pos)
            p = {prefs.shelf_last_x, prefs.shelf_last_y};
        else if (!during_drag && prefs.shelf_summon_pos == 2 && GetMonitorInfoW(monitor, &mi)) {
            p.x = mi.rcWork.right - 8;
            p.y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 4;
        } else if (!during_drag) {
            const LONG offset = MulDiv(18, compat::WindowDpi(s.hwnd), 96);
            const LONG y_offset = MulDiv(42, compat::WindowDpi(s.hwnd), 96);
            p.x += offset;
            p.y -= y_offset;
            if (GetMonitorInfoW(monitor, &mi)) {
                p.x = std::clamp(p.x, mi.rcWork.left, mi.rcWork.right - 1);
                p.y = std::clamp(p.y, mi.rcWork.top, mi.rcWork.bottom - 1);
            }
        }
        if (window_.ShowAt(p.x, p.y, expanded, during_drag)) InvalidateShelves(s);
    }

    void TearOff(AppState& s, POINT p) {
        Bind(s);
        if (window_.ShowTearOff(p)) InvalidateShelves(s);
    }

    void Hide(AppState& s, bool reset_view = true) {
        window_.Hide();
        dock_hot_ = nullptr;
        if (reset_view) s.trayView = {};
        InvalidateShelves(s);
    }

    void Menu(AppState& s, POINT p) {
        if (!EnsureMenu(s)) return;
        enum { kNew = 1, kRename, kPromote, kDelete, kClear, kFloat, kColor = 20, kSwitch = 100 };
        std::vector<ui::FluentMenuItem> menu;
        std::vector<uint64_t> ids;
        auto add = [&](int cmd, std::wstring text) -> ui::FluentMenuItem& {
            ui::FluentMenuItem item;
            item.command = cmd;
            item.text = std::move(text);
            return menu.emplace_back(std::move(item));
        };
        const uint64_t current = s.tray.active_shelf_id();
        const bool temp = s.tray.active_shelf().temp;
        for (const auto& shelf : s.tray.shelves()) {
            auto& item = add(kSwitch + static_cast<int>(ids.size()), s.tray.ShelfDisplayName(shelf));
            item.radio_group = true;
            item.radio = shelf.id == current;
            item.has_swatch = true;
            item.swatch_color = D2D1::ColorF(shelf.color_rgb);
            ids.push_back(shelf.id);
        }
        if (!menu.empty()) menu.back().separator_after = true;
        add(kNew, l10n::Get(I::ShelfMenuNew));
        add(kRename, Label(I::ShelfMenuRenameFormat, s.tray.ActiveShelfName()));
        if (temp) add(kPromote, l10n::Get(I::ShelfMenuSaveTemp));
        else add(kDelete, l10n::Get(I::ShelfMenuDelete));
        add(kClear, Label(I::ShelfMenuClearFormat, s.tray.ActiveShelfName()));
        add(kFloat, l10n::Get(Visible() ? I::ShelfDockTip : I::ShelfPopoutTip));
        auto& colors = add(0, l10n::Get(I::ShelfColorTip));
        for (size_t i = 0; i < std::size(app::kShelfPaletteRgb); ++i) {
            ui::FluentMenuItem item;
            item.command = kColor + static_cast<int>(i);
            item.text = std::to_wstring(i + 1);
            item.has_swatch = true;
            item.swatch_color = D2D1::ColorF(app::kShelfPaletteRgb[i]);
            colors.children.push_back(std::move(item));
        }
        const int cmd = s.menu->TrackPopup(p, menu);
        if (cmd >= kSwitch && static_cast<size_t>(cmd - kSwitch) < ids.size()) {
            if (s.tray.SwitchShelf(ids[cmd - kSwitch])) ResetViews(s);
        } else if (cmd == kNew) {
            s.tray.SwitchShelf(s.tray.CreateShelf());
            ResetViews(s);
        } else if (cmd == kRename) {
            if (!Visible()) Show(s, p, false);
            window_.BeginInlineRename(s.tray.ActiveShelfName());
        } else if (cmd == kPromote) {
            s.tray.SaveTempAsNamed(current);
        } else if (cmd == kDelete) {
            ui::ConfirmDialogSpec spec;
            spec.message = l10n::Get(I::ShelfMenuDelete) + L": " + s.tray.ActiveShelfName();
            if (ui::ShowConfirmDialog(Visible() ? window_.Hwnd() : s.hwnd, spec, s.darkMode, s.accentColor) &&
                s.tray.DeleteShelf(current)) ResetViews(s);
        } else if (cmd == kClear) {
            ClearTrayWithUndo(s);
        } else if (cmd == kFloat) {
            if (Visible()) Hide(s);
            else Show(s, p, false);
        } else if (cmd >= kColor && static_cast<size_t>(cmd - kColor) < std::size(app::kShelfPaletteRgb)) {
            s.tray.SetShelfColor(current, app::kShelfPaletteRgb[cmd - kColor]);
        }
        if (cmd) Refresh(s);
    }

private:
    AppState* Owner() const { return core_.windows.Lookup(owner_); }
    void Bind(AppState& s) {
        owner_ = s.hwnd;
        last_total_ = TrayItemTotalCount(s.tray);
        Appearance(s);
    }

    void Appearance(AppState& s) {
        window_.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
                              s.accentColor, s.appPrefs.shelf_view_grid);
    }

    void UpdateWatcher() {
        const bool shake = core_.appPrefs.shelf_shake_enabled;
        watcher_->Update(listener_, WM_STAGING_SHELF_SUMMON, shake, core_.appPrefs.shelf_shake_sensitivity);
        if (input_observer_installed_ && pointer_polling_enabled_ == shake) return;
        DoubleCtrlWatcher::InputPoll poll;
        if (shake) poll = [watcher = watcher_] { watcher->PollPointer(); };
        core_.doubleCtrl.SetInputObserver([watcher = watcher_](const RAWINPUT& raw) { watcher->OnInput(raw); },
                                          std::move(poll));
        input_observer_installed_ = true;
        pointer_polling_enabled_ = shake;
    }

    void Save() {
        auto next = core_.tray.NamedShelvesToJson();
        if (next != saved_ && core_.tray.SaveNamedShelvesFile()) saved_ = std::move(next);
    }

    void Refresh(AppState& s, bool emptied = false) {
        window_.Refresh(emptied && s.appPrefs.shelf_auto_hide);
        InvalidateShelves(s);
    }

    void ResetViews(AppState& s) {
        release_mode_ = app::TrayReleaseMode::Staged;
        app::ForEachWindowSharingCore(s, [](AppState& w) { w.trayView = {}; });
    }

    AppState* SidebarAt(POINT p) const {
        AppState* found = nullptr;
        core_.windows.ForEach([&](HWND, AppState& w) { if (!found && AtSidebar(w, p)) found = &w; });
        return found;
    }

    void Release(AppState& s, const std::wstring& path) {
        if (ReleaseTrayTo(s, path, release_mode_, CheckedItems(s, window_.Expanded()))) {
            RememberTrayDest(s, path);
            Refresh(s);
        }
    }

    void PickDestination(AppState& s) {
        std::wstring path;
        if (PickFolder(s, path, l10n::Get(I::ShelfReleaseMenuOther).c_str(), L"tray_send"))
            Release(s, path);
    }

    void ReleaseMenu(AppState& s, POINT p) {
        if (!EnsureMenu(s)) return;
        std::vector<ui::FluentMenuItem> menu;
        const auto add = [&](int cmd, std::wstring text, bool radio = false) {
            ui::FluentMenuItem item;
            item.command = cmd;
            item.text = std::move(text);
            item.radio_group = radio;
            item.radio = radio && cmd - 1 == static_cast<int>(release_mode_);
            menu.push_back(std::move(item));
        };
        add(1, l10n::Get(I::ShelfReleaseModeAuto), true);
        add(2, l10n::Get(I::ShelfReleaseModeCopy), true);
        add(3, l10n::Get(I::ShelfReleaseModeMove), true);
        menu.back().separator_after = true;
        const auto dests = TrayDestList(s);
        for (size_t i = 0; i < dests.size(); ++i) add(10 + static_cast<int>(i), dests[i]);
        add(4, l10n::Get(I::ShelfReleaseMenuOther));
        const int cmd = s.menu->TrackPopup(p, menu);
        if (cmd >= 1 && cmd <= 3) {
            release_mode_ = static_cast<app::TrayReleaseMode>(cmd - 1);
            Refresh(s);
        } else if (cmd == 4) PickDestination(s);
        else if (cmd >= 10 && static_cast<size_t>(cmd - 10) < dests.size()) Release(s, dests[cmd - 10]);
    }

    void DragOut(AppState& s, const std::vector<uint64_t>& ids) {
        const auto checks = CheckedItems(s, window_.Expanded());
        const auto& selected = ids.empty() || (ids.size() == 1 &&
            std::find(checks.begin(), checks.end(), ids.front()) != checks.end()) ? checks : ids;
        std::vector<std::wstring> paths;
        drag_ids_.clear();
        for (const auto& b : s.tray.batches()) for (const auto& item : b.items)
            if (item.exists && !item.inflight_task &&
                (selected.empty() || std::find(selected.begin(), selected.end(), item.id) != selected.end())) {
                paths.push_back(ClipboardPath(item.path));
                drag_ids_.push_back(item.id);
            }
        if (paths.empty()) return;
        drag_source_ = s.tray.active_shelf_id();
        const HWND source_window = s.hwnd;
        const bool dark = s.darkMode;
        dragging_out_ = true;
        s.trayDragOut = true;
        app::SetShelfOwnDragActive(true);
        Refresh(s);
        ui::DoFileDragDrop(paths, DROPEFFECT_COPY, {}, {}, dark);
        app::SetShelfOwnDragActive(false);
        dragging_out_ = false;
        drag_ids_.clear();
        if (auto* source = core_.windows.Lookup(source_window)) source->trayDragOut = false;
        if (auto* owner = Owner()) Refresh(*owner);
    }

    bool MoveToShelf(AppState& s, int target) {
        auto& tray = s.tray;
        const bool create = target == 2;
        uint64_t destination = 0;
        if (create) destination = tray.CreateShelf();
        else if (target >= 10 && static_cast<size_t>(target - 10) < tray.shelves().size())
            destination = tray.shelves()[target - 10].id;
        if (destination && tray.MoveItemsBetweenShelves(drag_source_, destination, drag_ids_)) {
            Refresh(s);
            return true;
        }
        if (create && destination) tray.DeleteShelf(destination);
        return false;
    }

    AppCore& core_;
    ui::FloatingShelfWindow window_;
    std::shared_ptr<app::DragShakeWatcher> watcher_ = std::make_shared<app::DragShakeWatcher>();
    bool input_observer_installed_ = false;
    bool pointer_polling_enabled_ = false;
    HWND owner_ = nullptr;
    HWND listener_ = nullptr;
    HWND dock_hot_ = nullptr;
    ULONGLONG refresh_at_ = 0;
    ULONGLONG save_at_ = 0;
    size_t last_hash_ = 0;
    int last_total_ = 0;
    bool dragging_out_ = false;
    uint64_t drag_source_ = 0;
    std::vector<uint64_t> drag_ids_;
    app::TrayReleaseMode release_mode_ = app::TrayReleaseMode::Staged;
    std::wstring saved_;
};

void StartStagingShelf(AppState& s) {
    if (s.core.staging_shelf) return;
    s.core.staging_shelf = std::make_shared<StagingShelfController>(s.core);
    s.core.staging_shelf->Start(s);
}

void TickStagingShelf(AppState& s) {
    if (auto& controller = s.core.staging_shelf) controller->Tick(s);
}

void CloseStagingShelfOwner(AppState& s, AppState* next) {
    if (auto& controller = s.core.staging_shelf) controller->Close(s, next);
}

void SummonStagingShelf(AppState& s) {
    if (auto& controller = s.core.staging_shelf) controller->Summon();
}

void ShowStagingShelf(AppState& s, POINT p, bool expanded) {
    if (auto& controller = s.core.staging_shelf) controller->Show(s, p, expanded);
}

void TearOffStagingShelf(AppState& s, POINT p) {
    if (auto& controller = s.core.staging_shelf) controller->TearOff(s, p);
}

void DockStagingShelf(AppState& s) {
    if (auto& controller = s.core.staging_shelf) controller->Hide(s);
}

void ToggleStagingShelf(AppState& s) {
    if (StagingShelfVisible(s)) DockStagingShelf(s);
    else {
        POINT p{};
        if (GetCursorPos(&p)) ShowStagingShelf(s, p, false);
    }
}

void ShowStagingShelfMenu(AppState& s, POINT p) {
    if (auto& controller = s.core.staging_shelf) controller->Menu(s, p);
}

bool StagingShelfVisible(const AppState& s) {
    const auto& controller = s.core.staging_shelf;
    return controller && controller->Visible();
}

bool StagingShelfDockHot(const AppState& s) {
    const auto& controller = s.core.staging_shelf;
    return controller && controller->DockHot(s.hwnd);
}

bool StageShelfDrop(AppState& s, IDataObject* obj, int target, bool shift_move) {
    std::vector<app::ShelfContentItem> items;
    bool content = false;
    if (!app::ExtractShelfDropItems(obj, app::DefaultShelfSnippetsDir(), items, &content)) return false;
    return CollectShelfItems(s, items, target, !content && (target == 1 || shift_move));
}

bool FloatingShelfDragActive(const AppState& s) {
    return s.core.staging_shelf && s.core.staging_shelf->DragActive();
}

bool ReleaseFloatingShelfDrop(AppState& s, const std::wstring& destination) {
    return s.core.staging_shelf && s.core.staging_shelf->ReleaseDrop(s, destination);
}
} // namespace pulse