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
#include "../../../ui/store_icons.h"
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
    std::vector<SettingsTabDescriptor> tabs;
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
    int requested_plugin_tab = -1;
    int mode = 0;                // 0: plugin settings, 1: context menu
    int active_plugin_tab = 0;
    // Opened for one package (its store card, its own 设置 command): that package's page only, no chooser.
    bool focused = false;
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
// The real context-menu rows of one package (ContextMenuCommandRows, with the user's placement and
// hidden commands applied) as menuPreview items: a heading starts a tinted group, a tinted row with
// children is the collapsed submenu, plain rows stay flat.
std::string PreviewGlyph(const ui::FluentMenuItem& item) {
    if (item.custom_icon) return "icon:#" + std::to_string(item.custom_icon);
    if (item.vector_icon != ui::StoreIcon::None) return "icon:#" + std::to_string(ui::StoreIconToId(item.vector_icon));
    return {};
}
UiMenuItem PreviewItem(const ui::FluentMenuItem& item, const std::string& group) {
    UiMenuItem out;
    out.text = Utf8(item.text);
    out.detail = Utf8(item.detail);
    out.shortcut = Utf8(item.shortcut);
    out.glyph = PreviewGlyph(item);
    out.primary = item.primary;
    out.checked = item.checked;
    out.group = group;
    for (const auto& child : item.children) {
        out.children.push_back(PreviewItem(child, {}));
        if (child.separator_after) { UiMenuItem sep; sep.separator = true; out.children.push_back(std::move(sep)); }
    }
    if (!out.children.empty() && out.children.back().separator) out.children.pop_back();
    return out;
}
std::vector<UiMenuItem> PreviewMenu(AppState& owner, const std::string& package_id) {
    std::vector<UiMenuItem> menu;
    std::string group;
    for (const auto& row : ContextMenuCommandRows(owner, nullptr, package_id, true)) {
        if (row.heading) { group = Utf8(row.text); continue; }
        if (!row.group_tint) group.clear();
        const std::string row_group = !row.children.empty() && row.group_tint ? Utf8(row.text) : group;
        menu.push_back(PreviewItem(row, row_group));
        if (row.separator_after && row_group.empty()) { UiMenuItem sep; sep.separator = true; menu.push_back(std::move(sep)); }
    }
    while (!menu.empty() && menu.back().separator) menu.pop_back();
    return menu;
}

// 右键菜单 tab: how the package joins the menu (four option cards) and its commands (icon rows with a
// switch) on the left; the live right-click menu on the right, which drops below on a narrow window.
UiTree BuildContextMenuTree(AppState& owner, const Entry& entry) {
    using I = l10n::StringId;
    UiTree tree;
    tree.root.id = "ext-ctx-root";
    tree.root.gap = 12;
    const auto text = [](const char* id, const std::wstring& value, const char* role) {
        UiNode node; node.id = id; node.type = UiNodeType::Text; node.text_role = role; node.text = Utf8(value);
        return node;
    };
    const auto card = [](const char* id, const std::wstring& title, int gap) {
        UiNode node; node.id = id; node.type = UiNodeType::Group; node.bordered = true; node.gap = gap;
        node.text = Utf8(title);
        return node;
    };

    // How the commands join the menu.
    const int current_placement = static_cast<int>(owner.ctxMenuPrefs.PlacementForExtension(entry.package_id));
    UiNode placement_card = card("ext-ctx-placement-card", l10n::Get(I::NativeExtMenuPlacementTitle), 10);
    placement_card.children.push_back(text("ext-ctx-placement-desc", l10n::Get(I::NativeExtMenuPlacementDesc), "caption"));
    UiNode choices; choices.id = "ext-ctx-placement-grid"; choices.type = UiNodeType::Grid; choices.columns = 2; choices.gap = 8;
    struct Choice { I title, detail; ui::StoreIcon icon; };
    const Choice kChoices[4] = {
        // 默认 = the way the extension itself puts its commands: the extension (puzzle) mark, not a sparkle.
        {I::NativeExtMenuPlacementDefault, I::NativeExtMenuPlacementDefaultDesc, ui::StoreIcon::Puzzle},
        {I::NativeExtMenuPlacementFlat, I::NativeExtMenuPlacementFlatDesc, ui::StoreIcon::Lines},
        {I::NativeExtMenuPlacementSubmenu, I::NativeExtMenuPlacementSubmenuDesc, ui::StoreIcon::ListView},
        {I::NativeExtMenuPlacementHidden, I::NativeExtMenuPlacementHiddenDesc, ui::StoreIcon::Eye},
    };
    for (int i = 0; i < 4; ++i) {
        UiNode option;
        option.id = "ext-ctx-placement-" + std::to_string(i);
        option.type = UiNodeType::OptionCard;
        option.option_kind = "radio";
        option.state = "placement" + std::to_string(i);
        option.glyph = "icon:#" + std::to_string(ui::StoreIconToId(kChoices[i].icon));
        option.text = Utf8(l10n::Get(kChoices[i].title));
        option.detail = Utf8(l10n::Get(kChoices[i].detail));
        choices.children.push_back(std::move(option));
        tree.state.push_back({"placement" + std::to_string(i), i == current_placement ? "true" : "false"});
    }
    placement_card.children.push_back(std::move(choices));

    // Each command, with its own icon, on a row with a switch.
    UiNode commands_card = card("ext-ctx-commands-card", l10n::Get(I::NativeExtMenuCommandsTitle), 2);
    commands_card.children.push_back(text("ext-ctx-commands-desc", l10n::Get(I::NativeExtMenuCommandsDesc), "caption"));
    const auto commands = KnownPackageCommands(owner, entry.package);
    bool any_hidden_override = current_placement != 0;
    if (commands.empty()) {
        commands_card.children.push_back(text("ext-ctx-commands-empty", l10n::Get(I::NativeExtMenuCommandsEmpty), "body"));
    }
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmd = commands[i];
        const bool visible = owner.ctxMenuPrefs.ExtensionCommandVisible(cmd.identity);
        if (!visible) any_hidden_override = true;
        UiNode row;
        row.id = "ext-ctx-cmd-row-" + std::to_string(i);
        row.type = UiNodeType::SettingsRow;
        row.bordered = false;
        row.glyph = "icon:#" + std::to_string(ui::IsCustomExtensionIcon(cmd.icon)
            ? cmd.icon : ui::StoreIconToId(ui::StoreCommandIconFor(cmd.icon, cmd.identity)));
        row.text = cmd.title;
        if (cmd.hidden_by_plugin) row.detail = Utf8(l10n::Get(I::NativeExtMenuHiddenByPlugin));
        const std::string state_key = "cmd:" + cmd.identity;
        UiNode toggle;
        toggle.id = "ext-ctx-cmd-sw-" + std::to_string(i);
        toggle.type = UiNodeType::Switch;
        toggle.state = state_key;
        toggle.min_width = toggle.max_width = 48;
        row.children.push_back(std::move(toggle));
        tree.state.push_back({state_key, visible ? "true" : "false"});
        commands_card.children.push_back(std::move(row));
    }

    UiNode settings_column; settings_column.id = "ext-ctx-settings"; settings_column.type = UiNodeType::Stack;
    settings_column.gap = 12; settings_column.flex = 1; settings_column.min_width = 420;
    settings_column.children.push_back(std::move(placement_card));
    settings_column.children.push_back(std::move(commands_card));
    if (any_hidden_override) {
        UiNode reset_btn;
        reset_btn.id = "ext-ctx-reset";
        reset_btn.type = UiNodeType::Button;
        reset_btn.command = "reset-menu";
        reset_btn.glyph = "icon:#" + std::to_string(ui::StoreIconToId(ui::StoreIcon::Undo));
        reset_btn.text = Utf8(l10n::Get(I::NativeExtMenuResetOverrides));
        settings_column.children.push_back(std::move(reset_btn));
    }

    // The live menu: the host frame around this package's rows as they are right now.
    UiNode preview_card = card("ext-ctx-preview-card", l10n::Get(I::NativeExtMenuPreviewTitle), 10);
    preview_card.min_width = preview_card.max_width = 320;
    preview_card.children.push_back(text("ext-ctx-preview-desc", l10n::Get(I::NativeExtMenuPreviewDesc), "caption"));
    UiNode menu; menu.id = "ext-ctx-preview-menu"; menu.type = UiNodeType::MenuPreview; menu.menu_kind = "context";
    menu.menu = PreviewMenu(owner, entry.package_id);
    const bool hidden = menu.menu.empty();
    preview_card.children.push_back(std::move(menu));
    if (hidden) preview_card.children.push_back(text("ext-ctx-preview-hidden", l10n::Get(I::NativeExtMenuPreviewHidden), "caption"));

    UiNode columns; columns.id = "ext-ctx-columns"; columns.type = UiNodeType::Stack; columns.orientation = "horizontal";
    columns.gap = 16;
    columns.children.push_back(std::move(settings_column));
    columns.children.push_back(std::move(preview_card));
    tree.root.children.push_back(std::move(columns));
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
    if (found->second->requested != package) found->second->requested_plugin_tab = 0;
    found->second->requested = package;
    found->second->requested_mode = 0;
}
std::wstring FocusedSettingsTitle(HWND owner) {
    const auto found = hosts.find(owner);
    if (found == hosts.end()) return {};
    const auto& host = *found->second;
    if (host.requested == SIZE_MAX)
        return host.focused && host.selected < host.entries.size() ? host.entries[host.selected].title : std::wstring{};
    if (auto* pages = PackageSettings(owner, host.requested))
        if (const auto page = pages->Page(); page && !page->title.empty()) return page->title;
    const auto* manifest = PackageManifest(host.requested);
    return manifest ? Wide(manifest->name) : std::wstring{};
}
void BindSettingsPagesPath(HWND owner, std::wstring_view path) {
    constexpr std::wstring_view prefix = L"native-extensions/";
    if (!path.starts_with(prefix)) return;
    const auto id = Utf8(std::wstring(path.substr(prefix.size())));
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || !PackageEnabled(package) || manifest->id != id) continue;
        const auto current = FocusedSettings(owner);
        if (current.package_id != id) RequestSettingsPagesPackage(owner, package);
        return;
    }
}
FocusedSettingsInfo FocusedSettings(HWND owner) {
    FocusedSettingsInfo info;
    const auto found = hosts.find(owner);
    if (found == hosts.end()) return info;
    const auto& host = *found->second;
    size_t package = SIZE_MAX;
    if (host.requested != SIZE_MAX) package = host.requested;
    else if (host.focused && host.selected < host.entries.size()) package = host.entries[host.selected].package;
    if (package == SIZE_MAX) return info;
    if (const auto* manifest = PackageManifest(package)) info.package_id = manifest->id;
    info.mode = host.requested != SIZE_MAX && host.requested_mode >= 0 ? host.requested_mode : host.mode;
    for (const auto& entry : host.entries)
        if (entry.package == package) info.has_menu = entry.has_page && entry.has_commands;
    if (const auto* pages = PackageSettings(owner, package)) {
        for (const auto& t : pages->Tabs()) {
            info.plugin_tabs.push_back({t.id, t.tab_id, t.title});
        }
        const int req_tab = host.requested_plugin_tab >= 0 ? host.requested_plugin_tab : host.active_plugin_tab;
        if (!info.plugin_tabs.empty()) {
            info.active_plugin_tab = (std::clamp)(req_tab, 0, static_cast<int>(info.plugin_tabs.size()) - 1);
            const auto st = pages->PageStatus(info.plugin_tabs[static_cast<size_t>(info.active_plugin_tab)].page);
            info.status_kind = st.kind;
            info.status_text = st.text;
        }
    }
    return info;
}
std::vector<std::wstring> PackageSettingsExtraTabTitles(HWND owner, size_t package) {
    std::vector<std::wstring> titles;
    if (const auto* pages = PackageSettings(owner, package)) {
        const auto tabs = pages->Tabs();
        for (size_t k = 1; k < tabs.size(); ++k) titles.push_back(tabs[k].title);
    }
    return titles;
}
void SetFocusedSettingsMode(HWND owner, int mode) {
    const auto found = hosts.find(owner);
    if (found == hosts.end()) return;
    // As RequestSettingsPagesContextMenu does: the next sync rebuilds the page in that mode.
    found->second->requested_mode = mode == 1 ? 1 : 0;
    InvalidateRect(owner, nullptr, FALSE);
}
void SetFocusedSettingsPluginTab(HWND owner, int tab_index) {
    const auto found = hosts.find(owner);
    if (found == hosts.end()) return;
    found->second->requested_plugin_tab = (std::max)(0, tab_index);
    InvalidateRect(owner, nullptr, FALSE);
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
    const bool was_focused = host.focused;
    if (host.requested != SIZE_MAX) host.focused = true;
    if (!host.visible) {
        PrepareCommandPackages(owner);
        for (size_t i = 0; i < PackageCount(); ++i) {
            const auto* manifest = PackageManifest(i);
            if (!manifest || !manifest->native || !PackageEnabled(i) || PackageActive(owner.hwnd, i)) continue;
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
        std::vector<SettingsTabDescriptor> tabs;
        if (auto* pages = PackageSettings(owner.hwnd, i)) tabs = pages->Tabs();
        const bool has_cmds = !KnownPackageCommands(owner, i).empty();
        if (tabs.empty() && !has_cmds) continue;
        Entry item;
        item.package = i;
        item.package_id = manifest->id;
        item.has_page = !tabs.empty();
        item.page = tabs.empty() ? 0 : tabs.front().id;
        item.title = !tabs.empty() && !tabs.front().title.empty() ? tabs.front().title : Wide(manifest->name);
        item.tabs = std::move(tabs);
        item.has_commands = has_cmds;
        entries.push_back(std::move(item));
    }
    const bool changed = entries != host.entries;
    const size_t requested = std::exchange(host.requested, SIZE_MAX);
    const int req_mode = std::exchange(host.requested_mode, -1);
    const int req_plugin_tab = std::exchange(host.requested_plugin_tab, -1);
    const auto prev_pkg_before = host.selected < host.entries.size() ? host.entries[host.selected].package : SIZE_MAX;
    if (requested != SIZE_MAX && requested != prev_pkg_before) {
        host.active_plugin_tab = 0;
    }
    if (req_plugin_tab >= 0) {
        host.active_plugin_tab = req_plugin_tab;
    }
    if (req_mode >= 0) {
        host.mode = req_mode;
        host.menu_dirty = true;
    }
    if (changed || was_focused != host.focused || !host.selector.IsOpen() || requested != SIZE_MAX || req_mode >= 0) {
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
        if (!host.focused && (host.entries.size() != 1 || host.entries.empty()))
            tree.root.children.push_back(std::move(label));
        if (!host.focused && host.entries.size() > 1) {
            UiNode select;
            select.id = "settings-package";
            select.type = UiNodeType::Select;
            select.state = "package";
            for (const auto& entry : host.entries) select.options.push_back(Utf8(entry.title));
            tree.root.children.push_back(std::move(select));
            tree.state.push_back({"package", std::to_string(host.selected)});
        }
        if (has_both_tabs && !host.focused) {
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
    const bool single = host.focused || host.entries.size() == 1;
    // Opened for one package: its page fills the area (its 右键菜单 view opens from that package's own entry).
    const bool hide_header = host.focused || (single && !host.entries.empty() && !has_both_tabs);
    const LONG header_dip = host.entries.empty() ? 64
        : single ? 52 : (has_both_tabs ? 148 : 104);
    const LONG header_bottom = hide_header ? bounds.top : std::min(bounds.bottom, bounds.top + static_cast<LONG>(header_dip * owner.scale));
    RECT header{bounds.left, bounds.top, bounds.right, header_bottom};
    const D2D1_COLOR_F wash = owner.renderer.EmbeddedBackdropWash();
    host.selector.SetEmbeddedWash(wash);
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
                if (!entry.tabs.empty()) {
                    host.active_plugin_tab = (std::clamp)(host.active_plugin_tab, 0, static_cast<int>(entry.tabs.size()) - 1);
                } else {
                    host.active_plugin_tab = 0;
                }
                const uint64_t show_page = !entry.tabs.empty() ? entry.tabs[static_cast<size_t>(host.active_plugin_tab)].id : entry.page;
                pages->Show(show_page, body, owner.darkMode, owner.accentColor, owner.scale, wash);
            }
        } else {
            if (host.menu_dirty && host.menu_panel.IsOpen() && host.menu_package == entry.package) {
                // Same package: refresh in place so the scroll position and focus survive.
                host.menu_dirty = false;
                const auto tree = BuildContextMenuTree(owner, entry);
                host.menu_panel.UpdateTree(tree);
                host.menu_panel.SetTreeStates(tree.state);
                host.menu_panel.UpdateDocked(body, owner.darkMode, owner.accentColor, owner.scale);
            } else if (host.menu_dirty || !host.menu_panel.IsOpen() || host.menu_package != entry.package) {
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
                    bool mutated = false;
                    for (const auto& change : changes) {
                        if (change.key.size() == 10 && change.key.starts_with("placement") && change.value == "true") {
                            const int idx = change.key[9] - '0';
                            if (idx >= 0 && idx <= 3) {
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
                        // The preview, the radio choice and the reset button all follow the change.
                        if (const auto it = hosts.find(window); it != hosts.end()) it->second->menu_dirty = true;
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
            host.menu_panel.SetEmbeddedWash(wash);
            ShowWindow(host.menu_panel.Hwnd(), SW_SHOWNOACTIVATE);
        }
    } else if (host.menu_panel.Hwnd()) {
        ShowWindow(host.menu_panel.Hwnd(), SW_HIDE);
    }
    host.visible = true;
}
#ifdef PULSE_WITH_SELFTEST
bool ProbeSettingsPagesFocusRetention(AppState& owner, const ui::WindowViewModel& detail) {
    if (!owner.isolatedTest || hosts.contains(owner.hwnd) || PackageCount() != 0) return false;
    RequestSettingsPagesPackage(owner.hwnd, 0);
    SyncSettingsPagesHost(owner, detail);
    auto& host = *hosts.at(owner.hwnd);
    const bool entered = host.focused && host.visible;
    auto hidden = detail;
    hidden.settings_open = false;
    SyncSettingsPagesHost(owner, hidden);
    const bool left = !host.visible;
    SyncSettingsPagesHost(owner, detail);
    const bool returned = host.focused && host.visible;
    CloseSettingsPagesHost(owner.hwnd);
    return entered && left && returned;
}
#endif
}