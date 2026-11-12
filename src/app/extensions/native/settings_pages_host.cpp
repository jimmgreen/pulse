#include "settings_pages_host.h"
#include "settings_pages_adapter.h"
#include "command_contributions.h"
#include "session.h"
#include "../runtime.h"
#include "../../app_runtime.h"
#include "../../../common/localization.h"
#include "../../../ui/extension_panel.h"
#include "../../../ui/ui_renderer.h"
#include "../../../ui/settings_page_ids.h"
#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {
std::string Utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string value(static_cast<size_t>(n), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), n, nullptr, nullptr);
    return value;
}
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
struct Entry {
    size_t package = 0;
    std::string package_id;
    std::wstring title;
    bool has_page = false;
    uint64_t page = 0;
    bool has_commands = false;
    bool operator==(const Entry&) const = default;
};
struct Host {
    ui::ExtensionPanel selector;
    ui::ExtensionPanel menu_panel;
    std::vector<Entry> entries;
    size_t selected = 0;
    size_t requested = SIZE_MAX; // runtime package to select at the next sync
    int requested_mode = -1;     // -1: keep, 0: plugin settings, 1: context menu
    int mode = 0;                // 0: plugin settings, 1: context menu
    bool menu_dirty = true;
    size_t menu_package = SIZE_MAX;
    bool visible = false, syncing = false;
};
auto& hosts = *new std::map<HWND, std::unique_ptr<Host>>;
void HidePages(HWND owner) {
    for (size_t i = 0; i < PackageCount(); ++i) if (auto* pages = PackageSettings(owner, i)) pages->Hide();
}
int EffectiveMode(const Entry& entry, int desired) {
    if (entry.has_page && entry.has_commands) return desired == 1 ? 1 : 0;
    return entry.has_page ? 0 : 1;
}
UiTree BuildContextMenuTree(AppState& owner, const Entry& entry) {
    UiTree tree;
    tree.root.id = "ext-ctx-root";
    tree.root.gap = 12;

    // Card 1: Menu placement selector
    UiNode placement_card;
    placement_card.id = "ext-ctx-placement-card";
    placement_card.type = UiNodeType::Group;
    placement_card.bordered = true;
    placement_card.gap = 8;

    UiNode placement_title;
    placement_title.id = "ext-ctx-placement-title";
    placement_title.type = UiNodeType::Text;
    placement_title.text_role = "section";
    placement_title.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementTitle));
    placement_card.children.push_back(std::move(placement_title));

    UiNode placement_desc;
    placement_desc.id = "ext-ctx-placement-desc";
    placement_desc.type = UiNodeType::Text;
    placement_desc.text_role = "caption";
    placement_desc.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementDesc));
    placement_card.children.push_back(std::move(placement_desc));

    UiNode seg;
    seg.id = "ext-ctx-placement-seg";
    seg.type = UiNodeType::Segmented;
    seg.state = "placement";
    seg.options = {
        Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementDefault)),
        Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementFlat)),
        Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementSubmenu)),
        Utf8(l10n::Get(l10n::StringId::NativeExtMenuPlacementHidden)),
    };
    placement_card.children.push_back(std::move(seg));
    const int current_placement = static_cast<int>(owner.ctxMenuPrefs.PlacementForExtension(entry.package_id));
    tree.state.push_back({"placement", std::to_string(current_placement)});
    tree.root.children.push_back(std::move(placement_card));

    // Card 2: Per-command visibility switches
    UiNode commands_card;
    commands_card.id = "ext-ctx-commands-card";
    commands_card.type = UiNodeType::Group;
    commands_card.bordered = true;
    commands_card.gap = 8;

    UiNode commands_title;
    commands_title.id = "ext-ctx-commands-title";
    commands_title.type = UiNodeType::Text;
    commands_title.text_role = "section";
    commands_title.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuCommandsTitle));
    commands_card.children.push_back(std::move(commands_title));

    UiNode commands_desc;
    commands_desc.id = "ext-ctx-commands-desc";
    commands_desc.type = UiNodeType::Text;
    commands_desc.text_role = "caption";
    commands_desc.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuCommandsDesc));
    commands_card.children.push_back(std::move(commands_desc));

    const auto commands = KnownPackageCommands(owner, entry.package);
    bool any_hidden_override = current_placement != 0;
    if (commands.empty()) {
        UiNode empty;
        empty.id = "ext-ctx-commands-empty";
        empty.type = UiNodeType::Text;
        empty.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuCommandsEmpty));
        commands_card.children.push_back(std::move(empty));
    } else {
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto& cmd = commands[i];
            const bool visible = owner.ctxMenuPrefs.ExtensionCommandVisible(cmd.identity);
            if (!visible) any_hidden_override = true;

            UiNode row;
            row.id = "ext-ctx-cmd-row-" + std::to_string(i);
            row.type = UiNodeType::Stack;
            row.orientation = "horizontal";
            row.alignment = "center";
            row.gap = 8;

            UiNode left;
            left.id = "ext-ctx-cmd-left-" + std::to_string(i);
            left.type = UiNodeType::Stack;
            left.orientation = "vertical";
            left.gap = 2;
            left.flex = 1;

            UiNode label;
            label.id = "ext-ctx-cmd-label-" + std::to_string(i);
            label.type = UiNodeType::Text;
            label.glyph = "icon:#" + std::to_string(cmd.icon);
            label.text = cmd.title;
            left.children.push_back(std::move(label));

            if (cmd.hidden_by_plugin) {
                UiNode note;
                note.id = "ext-ctx-cmd-note-" + std::to_string(i);
                note.type = UiNodeType::Text;
                note.text_role = "caption";
                note.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuHiddenByPlugin));
                left.children.push_back(std::move(note));
            }
            row.children.push_back(std::move(left));

            const std::string state_key = "cmd:" + cmd.identity;
            UiNode toggle;
            toggle.id = "ext-ctx-cmd-sw-" + std::to_string(i);
            toggle.type = UiNodeType::Switch;
            toggle.state = state_key;
            row.children.push_back(std::move(toggle));
            tree.state.push_back({state_key, visible ? "true" : "false"});
            commands_card.children.push_back(std::move(row));
        }
    }
    tree.root.children.push_back(std::move(commands_card));

    if (any_hidden_override) {
        UiNode reset_btn;
        reset_btn.id = "ext-ctx-reset";
        reset_btn.type = UiNodeType::Button;
        reset_btn.command = "reset-menu";
        reset_btn.glyph = "icon:undo";
        reset_btn.text = Utf8(l10n::Get(l10n::StringId::NativeExtMenuResetOverrides));
        tree.root.children.push_back(std::move(reset_btn));
    }
    return tree;
}
}
void CloseSettingsPagesHost(HWND owner) {
    HidePages(owner);
    hosts.erase(owner);
}
void RequestSettingsPagesPackage(HWND owner, size_t package) {
    auto found = hosts.find(owner);
    if (found == hosts.end()) found = hosts.emplace(owner, std::make_unique<Host>()).first;
    found->second->requested = package;
    found->second->requested_mode = 0;
}
void RequestSettingsPagesContextMenu(HWND owner, size_t package) {
    auto found = hosts.find(owner);
    if (found == hosts.end()) found = hosts.emplace(owner, std::make_unique<Host>()).first;
    found->second->requested = package;
    found->second->requested_mode = 1;
}
void SyncSettingsPagesHost(AppState& owner, const ui::WindowViewModel& vm) {
    const bool visible = vm.settings_open && vm.settings_page == ui::kSettingsPageNativeExtensions;
    auto found = hosts.find(owner.hwnd);
    if (!visible) {
        if (found != hosts.end() && found->second->visible) {
            HidePages(owner.hwnd);
            if (found->second->selector.Hwnd()) ShowWindow(found->second->selector.Hwnd(), SW_HIDE);
            if (found->second->menu_panel.Hwnd()) ShowWindow(found->second->menu_panel.Hwnd(), SW_HIDE);
            found->second->visible = false;
        }
        return;
    }
    if (found == hosts.end()) found = hosts.emplace(owner.hwnd, std::make_unique<Host>()).first;
    auto& host = *found->second;
    if (host.syncing) return;
    host.syncing = true;
    struct Unlock { bool& value; ~Unlock() { value = false; } } unlock{host.syncing};
    if (!host.visible) {
        PrepareCommandPackages(owner);
        for (size_t i = 0; i < PackageCount(); ++i) {
            const auto* manifest = PackageManifest(i);
            if (!manifest || !manifest->native || !PackageEnabled(i) || PackageSessionLoaded(owner.hwnd, i)) continue;
            if (std::find(manifest->resources.begin(), manifest->resources.end(), "settings.own") != manifest->resources.end())
                EnsureCommandPackageStarted(owner, i);
        }
        host.menu_dirty = true;
    }
    const auto area = owner.renderer.NativeSettingsRect(static_cast<float>(owner.compositor.Width()),
        static_cast<float>(owner.compositor.Height()), vm);
    RECT bounds{static_cast<LONG>(area.left), static_cast<LONG>(area.top), static_cast<LONG>(area.right), static_cast<LONG>(area.bottom)};
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        HidePages(owner.hwnd);
        if (host.selector.Hwnd()) ShowWindow(host.selector.Hwnd(), SW_HIDE);
        if (host.menu_panel.Hwnd()) ShowWindow(host.menu_panel.Hwnd(), SW_HIDE);
        host.visible = false;
        return;
    }
    std::vector<Entry> entries;
    for (size_t i = 0; i < PackageCount(); ++i) {
        const auto* manifest = PackageManifest(i);
        if (!manifest || !manifest->native || !PackageEnabled(i)) continue;
        std::optional<SettingsPageDescriptor> page_info;
        if (auto* pages = PackageSettings(owner.hwnd, i)) page_info = pages->Page();
        const bool has_cmds = !KnownPackageCommands(owner, i).empty();
        if (!page_info && !has_cmds) continue;
        Entry item;
        item.package = i;
        item.package_id = manifest->id;
        item.has_page = page_info.has_value();
        item.page = page_info ? page_info->id : 0;
        item.title = page_info && !page_info->title.empty() ? page_info->title : Wide(manifest->name);
        item.has_commands = has_cmds;
        entries.push_back(std::move(item));
    }
    const bool changed = entries != host.entries;
    const size_t requested = std::exchange(host.requested, SIZE_MAX);
    const int req_mode = std::exchange(host.requested_mode, -1);
    if (req_mode >= 0) {
        host.mode = req_mode;
        host.menu_dirty = true;
    }
    if (changed || !host.selector.IsOpen() || requested != SIZE_MAX || req_mode >= 0) {
        const auto previous_pkg = host.selected < host.entries.size() ? host.entries[host.selected].package : SIZE_MAX;
        host.entries = std::move(entries);
        host.selected = 0;
        for (size_t i = 0; i < host.entries.size(); ++i)
            if (host.entries[i].package == previous_pkg) host.selected = i;
        for (size_t i = 0; i < host.entries.size(); ++i)
            if (host.entries[i].package == requested) host.selected = i;
        host.menu_dirty = true;

        const bool has_both_tabs = host.selected < host.entries.size() &&
            host.entries[host.selected].has_page && host.entries[host.selected].has_commands;
        UiTree tree;
        tree.root.id = "settings-selector";
        tree.root.gap = 6;
        UiNode label;
        label.id = "settings-label";
        label.type = UiNodeType::Text;
        label.text = Utf8(l10n::Get(host.entries.empty() ? l10n::StringId::NativeExtensionSettingsEmpty : l10n::StringId::NativeExtensionSettingsChoose));
        if (host.entries.size() != 1 || host.entries.empty())
            tree.root.children.push_back(std::move(label));
        if (host.entries.size() > 1) {
            UiNode select;
            select.id = "settings-package";
            select.type = UiNodeType::Select;
            select.state = "package";
            for (const auto& entry : host.entries) select.options.push_back(Utf8(entry.title));
            tree.root.children.push_back(std::move(select));
            tree.state.push_back({"package", std::to_string(host.selected)});
        }
        if (has_both_tabs) {
            UiNode mode_seg;
            mode_seg.id = "settings-mode";
            mode_seg.type = UiNodeType::Segmented;
            mode_seg.state = "mode";
            mode_seg.options = {
                Utf8(l10n::Get(l10n::StringId::NativeExtTabSettings)),
                Utf8(l10n::Get(l10n::StringId::NativeExtTabContextMenu)),
            };
            tree.root.children.push_back(std::move(mode_seg));
            tree.state.push_back({"mode", host.mode == 1 ? "1" : "0"});
        }
        host.selector.Close();
        {
            ui::ExtensionPanelSpec spec;
            spec.content_only = true;
            spec.tree = std::move(tree);
            const HWND window = owner.hwnd;
            spec.tree_callbacks.on_state_changed = [window](uint64_t, const std::vector<ui::ExtensionStateChange>& changes) {
                const auto it = hosts.find(window);
                if (it == hosts.end()) return;
                for (const auto& change : changes) {
                    if (change.key == "package") {
                        size_t index = 0;
                        const auto result = std::from_chars(change.value.data(), change.value.data() + change.value.size(), index);
                        if (result.ec == std::errc{} && result.ptr == change.value.data() + change.value.size() && index < it->second->entries.size()) {
                            it->second->selected = index;
                            it->second->requested = it->second->entries[index].package;
                            it->second->menu_dirty = true;
                            InvalidateRect(window, nullptr, FALSE);
                        }
                    } else if (change.key == "mode") {
                        it->second->mode = change.value == "1" ? 1 : 0;
                        it->second->menu_dirty = true;
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
            };
            host.selector.ShowDocked(window, bounds, std::move(spec), owner.darkMode, owner.accentColor, owner.scale);
        }
    }

    const bool has_both_tabs = host.selected < host.entries.size() &&
        host.entries[host.selected].has_page && host.entries[host.selected].has_commands;
    const bool hide_header = host.entries.size() == 1 && !has_both_tabs;
    const LONG header_dip = host.entries.empty() ? 64
        : (host.entries.size() > 1 && has_both_tabs ? 148 : (host.entries.size() > 1 ? 104 : 52));
    const LONG header_bottom = hide_header ? bounds.top : std::min(bounds.bottom, bounds.top + static_cast<LONG>(header_dip * owner.scale));
    RECT header{bounds.left, bounds.top, bounds.right, header_bottom};
    host.selector.UpdateDocked(header, owner.darkMode, owner.accentColor, owner.scale);
    ShowWindow(host.selector.Hwnd(), hide_header ? SW_HIDE : SW_SHOWNOACTIVATE);

    const int active_mode = host.selected < host.entries.size() ? EffectiveMode(host.entries[host.selected], host.mode) : 0;
    for (size_t i = 0; i < PackageCount(); ++i) {
        const bool show_this_page = active_mode == 0 && host.selected < host.entries.size() &&
            host.entries[host.selected].package == i && bounds.bottom > header_bottom;
        if (!show_this_page) {
            if (auto* pages = PackageSettings(owner.hwnd, i)) pages->Hide();
        }
    }

    if (host.selected < host.entries.size() && bounds.bottom > header_bottom) {
        const auto& entry = host.entries[host.selected];
        RECT body{bounds.left, header_bottom, bounds.right, bounds.bottom};
        if (active_mode == 0) {
            if (host.menu_panel.Hwnd()) ShowWindow(host.menu_panel.Hwnd(), SW_HIDE);
            if (auto* pages = PackageSettings(owner.hwnd, entry.package)) {
                pages->Show(entry.page, body, owner.darkMode, owner.accentColor, owner.scale);
            }
        } else {
            if (host.menu_dirty || !host.menu_panel.IsOpen() || host.menu_package != entry.package) {
                host.menu_package = entry.package;
                host.menu_dirty = false;
                const std::string pkg_id = entry.package_id;
                const size_t pkg_index = entry.package;
                ui::ExtensionPanelSpec spec;
                spec.content_only = true;
                spec.tree = BuildContextMenuTree(owner, entry);
                const HWND window = owner.hwnd;
                spec.tree_callbacks.on_state_changed = [window, pkg_id](uint64_t, const std::vector<ui::ExtensionStateChange>& changes) {
                    auto* state = IsWindow(window) ? reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
                    if (!state) return;
                    const std::string prefix = "native:" + pkg_id + ":";
                    const auto has_override = [&] {
                        if (state->ctxMenuPrefs.PlacementForExtension(pkg_id) != app::ExtensionMenuPlacement::Default) return true;
                        return std::any_of(state->ctxMenuPrefs.extension_hidden_commands.begin(),
                                           state->ctxMenuPrefs.extension_hidden_commands.end(),
                                           [&](const std::string& id) { return id.starts_with(prefix); });
                    };
                    const bool before = has_override();
                    bool mutated = false;
                    for (const auto& change : changes) {
                        if (change.key == "placement") {
                            int idx = 0;
                            const auto res = std::from_chars(change.value.data(), change.value.data() + change.value.size(), idx);
                            if (res.ec == std::errc{} && idx >= 0 && idx <= 3) {
                                state->ctxMenuPrefs.SetExtensionPlacement(pkg_id, static_cast<app::ExtensionMenuPlacement>(idx));
                                mutated = true;
                            }
                        } else if (change.key.starts_with("cmd:")) {
                            state->ctxMenuPrefs.SetExtensionCommandVisible(change.key.substr(4), change.value == "true");
                            mutated = true;
                        }
                    }
                    if (mutated) {
                        if (!state->isolatedTest) state->ctxMenuPrefs.Save();
                        if (before != has_override()) {
                            if (const auto it = hosts.find(window); it != hosts.end()) it->second->menu_dirty = true;
                        }
                        InvalidateRect(window, nullptr, FALSE);
                    }
                };
                spec.tree_callbacks.on_command = [window, pkg_id, pkg_index](const std::string& cmd) {
                    if (cmd != "reset-menu") return;
                    auto* state = IsWindow(window) ? reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA)) : nullptr;
                    if (!state) return;
                    state->ctxMenuPrefs.SetExtensionPlacement(pkg_id, app::ExtensionMenuPlacement::Default);
                    const std::string prefix = "native:" + pkg_id + ":";
                    std::erase_if(state->ctxMenuPrefs.extension_hidden_commands, [&](const std::string& id) {
                        return id.starts_with(prefix);
                    });
                    if (!state->isolatedTest) state->ctxMenuPrefs.Save();
                    if (const auto it = hosts.find(window); it != hosts.end()) {
                        it->second->menu_dirty = true;
                        it->second->requested = pkg_index;
                        it->second->requested_mode = 1;
                    }
                    InvalidateRect(window, nullptr, FALSE);
                };
                host.menu_panel.Close();
                host.menu_panel.ShowDocked(window, body, std::move(spec), owner.darkMode, owner.accentColor, owner.scale);
            } else {
                host.menu_panel.UpdateDocked(body, owner.darkMode, owner.accentColor, owner.scale);
            }
            ShowWindow(host.menu_panel.Hwnd(), SW_SHOWNOACTIVATE);
        }
    } else if (host.menu_panel.Hwnd()) {
        ShowWindow(host.menu_panel.Hwnd(), SW_HIDE);
    }
    host.visible = true;
}
}
