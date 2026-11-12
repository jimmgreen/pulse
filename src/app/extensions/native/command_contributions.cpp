#include "command_contributions.h"
#include "commands_adapter.h"
#include "session.h"
#include "settings_pages_adapter.h"
#include "settings_pages_host.h"
#include "../icon_registry.h"
#include "../runtime.h"
#include "../../app_navigation.h"
#include "../../../ui/settings_page_ids.h"
#include "../../../ui/store_icons.h"
#include "../../../common/localization.h"
#include "../../app_runtime.h"
#include "../../actions/action_registry.h"
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>
namespace pulse::extensions::native {
namespace {
constexpr int kSettingsCommand = 0x41000000;
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring output(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}
StaticCommandLanguage CurrentStaticLanguage() noexcept {
    switch (l10n::effective_language()) {
    case l10n::Language::ZhTW: return StaticCommandLanguage::ZhTW;
    case l10n::Language::EnUS: return StaticCommandLanguage::EnUS;
    case l10n::Language::ZhCN:
    case l10n::Language::System:
        return StaticCommandLanguage::ZhCN;
    }
    return StaticCommandLanguage::EnUS;
}
int SettingsCommand(AppState& owner, const std::string& id) {
    for (size_t i = 0; i < PackageCount() && i < 10000; ++i) {
        const auto* manifest = PackageManifest(i);
        if (!manifest || manifest->id != id || !manifest->native || !PackageEnabled(i)) continue;
        if (id == "pulse.7zip") return kSettingsCommand + static_cast<int>(i);
        const auto* page = PackageSettings(owner.hwnd, i);
        if ((page && page->Page()) || !KnownPackageCommands(owner, i).empty())
            return kSettingsCommand + static_cast<int>(i);
        return 0;
    }
    return 0;
}
std::wstring PackageDisplayName(const std::string& id) {
    for (size_t i = 0; i < PackageCount(); ++i) {
        const auto* manifest = PackageManifest(i);
        if (manifest && manifest->id == id && !manifest->name.empty()) return Wide(manifest->name);
    }
    return Wide(id);
}
bool TryUtf8(const std::wstring& value, std::string& out) {
    out.clear();
    if (value.empty()) return true;
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), out.data(), count, nullptr, nullptr) == count;
}
CommandSelection CaptureWindowCommandSelection(AppState& owner) {
    CommandSelection selection;
    const auto* tab = ActiveTab(owner);
    if (!tab || tab->loading || !tab->snapshot || tab->snapshot_path != tab->current_path) return selection;
    if (tab->content_results || tab->search_entries || !std::filesystem::path(tab->current_path).is_absolute())
        return selection;
    if (tab->SelectedCount() > 4096) return selection;
    if (!TryUtf8(tab->current_path, selection.directory)) return {};
    size_t bytes = selection.directory.size();
    const auto indices = tab->SelectedIndices();
    if (indices.size() > 4096) return {};
    selection.entries.reserve(indices.size());
    for (const int index : indices) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount() || !tab->IsSelected(index)) return {};
        const auto& entry = tab->EntryAt(static_cast<size_t>(index));
        const auto full_path = EntryFullPath(*tab, index);
        if (!std::filesystem::path(full_path).is_absolute()) return {};
        CommandSelectionEntry item;
        item.directory = entry.is_dir;
        if (!TryUtf8(full_path, item.path) || !TryUtf8(entry.name, item.name)) return {};
        bytes += item.path.size() + item.name.size();
        if (bytes > 4 * 1024 * 1024) return {};
        selection.entries.push_back(std::move(item));
    }
    selection.valid = true;
    return selection;
}
std::vector<CommandInfo> AllCommandEntries(AppState& owner) {
    PrepareCommandPackages(owner);
    auto live = CommandsAdapter::Entries(owner.hwnd);
    std::vector<bool> used(live.size(), false);
    std::vector<CommandInfo> result;
    CommandSelection selection;
    bool selection_captured = false;
    const StaticCommandLanguage language = CurrentStaticLanguage();
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg)) continue;
        if (PackageSessionLoaded(owner.hwnd, pkg)) {
            for (size_t i = 0; i < live.size(); ++i) {
                if (!used[i] && live[i].package == manifest->id) {
                    used[i] = true;
                    result.push_back(std::move(live[i]));
                }
            }
        } else if (manifest->command_contributions_declared) {
            if (!selection_captured) {
                selection = CaptureWindowCommandSelection(owner);
                selection_captured = true;
            }
            for (const auto& contrib : manifest->command_contributions) {
                result.push_back(CommandsAdapter::EvaluateStaticContribution(
                    owner.hwnd, pkg, manifest->id, contrib, language, selection));
            }
        }
    }
    for (size_t i = 0; i < live.size(); ++i) {
        if (!used[i]) result.push_back(std::move(live[i]));
    }
    return result;
}
ui::FluentMenuItem Row(const CommandInfo& info) {
    ui::FluentMenuItem row;
    row.command = info.token; row.text = Wide(info.title); row.tooltip = Wide(info.identity);
    row.enabled = info.enabled;
    if (ui::IsCustomExtensionIcon(info.icon)) {
        row.custom_icon = info.icon;
    } else {
        row.vector_icon = ui::StoreCommandIconFor(info.icon, info.identity);
    }
    row.primary = (info.flags & PULSE_COMMAND_PRIMARY) != 0 ||
        (info.package == "pulse.7zip" && (info.identity.ends_with(":extract") || info.identity.ends_with(":quick-compress")));
    row.checked = (info.flags & PULSE_COMMAND_CHECKED) != 0;
    row.separator_after = (info.flags & PULSE_COMMAND_SEPARATOR_AFTER) != 0;
    if (info.key) {
        if (info.modifiers & PULSE_COMMAND_CTRL) row.shortcut += L"Ctrl+";
        if (info.modifiers & PULSE_COMMAND_SHIFT) row.shortcut += L"Shift+";
        if (info.modifiers & PULSE_COMMAND_ALT) row.shortcut += L"Alt+";
        if (info.key >= VK_F1) row.shortcut += L"F" + std::to_wstring(info.key - VK_F1 + 1);
        else row.shortcut += static_cast<wchar_t>(info.key);
    }
    return row;
}
void AssignGroupIcon(ui::FluentMenuItem& parent, uint32_t group_icon, const std::string& package_id) {
    if (group_icon != 0) {
        if (ui::IsCustomExtensionIcon(group_icon)) parent.custom_icon = group_icon;
        else parent.vector_icon = ui::StoreIconFromId(group_icon);
        return;
    }
    parent.vector_icon = package_id == "pulse.7zip" ? ui::StoreIcon::Box : ui::StoreIcon::Puzzle;
}
}
std::vector<PackageCommandSummary> KnownPackageCommands(AppState& owner, size_t package) {
    const auto* manifest = PackageManifest(package);
    if (!manifest || !manifest->native) return {};
    std::vector<PackageCommandSummary> out;
    std::set<std::string> seen;
    if (PackageSessionLoaded(owner.hwnd, package)) {
        for (auto& item : CommandsAdapter::LivePackageCommands(owner.hwnd, manifest->id)) {
            if (seen.insert(item.identity).second) out.push_back(std::move(item));
        }
    }
    if (manifest->command_contributions_declared) {
        const StaticCommandLanguage language = CurrentStaticLanguage();
        for (const auto& contrib : manifest->command_contributions) {
            const std::string identity = "native:" + manifest->id + ":" + contrib.id;
            if (!seen.insert(identity).second) continue;
            uint32_t icon = contrib.icon;
            if (!contrib.icon_name.empty()) {
                if (const auto resolved = icons::GlobalRegistry().ResolveName(manifest->id, contrib.icon_name))
                    icon = *resolved;
            }
            out.push_back(PackageCommandSummary{
                identity,
                CommandsAdapter::ResolveLocalizedText(contrib.title, language),
                icon,
                (contrib.flags & PULSE_COMMAND_HIDDEN) != 0,
            });
        }
    }
    return out;
}
std::vector<ui::FluentMenuItem> CommandRows(AppState& owner, std::wstring_view query) {
    std::vector<ui::FluentMenuItem> rows;
    for (const auto& info : AllCommandEntries(owner)) {
        if (!info.enabled) continue;
        auto row = Row(info);
        row.separator_after = false;
        app::actions::ActionView view; view.title = row.text; view.id = info.identity;
        if (!query.empty() && !app::actions::ActionMatchTier(view, query)) continue;
        rows.push_back(std::move(row));
    }
    return rows;
}
std::vector<ui::FluentMenuItem> ContextMenuCommandRows(AppState& owner) {
    std::vector<CommandInfo> commands;
    for (auto& info : AllCommandEntries(owner)) {
        if (!info.enabled) continue;
        if (owner.ctxMenuPrefs.PlacementForExtension(info.package) == app::ExtensionMenuPlacement::Hidden) continue;
        if (!owner.ctxMenuPrefs.ExtensionCommandVisible(info.identity)) continue;
        commands.push_back(std::move(info));
    }
    std::vector<ui::FluentMenuItem> rows;
    auto push_with_separators = [](std::vector<ui::FluentMenuItem>& dst, ui::FluentMenuItem&& row, uint32_t flags) {
        if ((flags & PULSE_COMMAND_SEPARATOR_BEFORE) != 0 && !dst.empty()) dst.back().separator_after = true;
        dst.push_back(std::move(row));
    };
    // Rows keep registration order; a group gathers its package's commands at its first one.
    std::vector<bool> placed(commands.size(), false);
    for (size_t i = 0; i < commands.size(); ++i) {
        if (placed[i]) continue;
        const auto& head = commands[i];
        const auto placement = owner.ctxMenuPrefs.PlacementForExtension(head.package);
        if (placement == app::ExtensionMenuPlacement::Flat) {
            placed[i] = true;
            auto row = Row(head);
            row.detail = Wide(head.detail);
            push_with_separators(rows, std::move(row), head.flags);
            continue;
        }
        if (placement == app::ExtensionMenuPlacement::Submenu) {
            std::vector<ui::FluentMenuItem> members;
            uint32_t group_icon = 0;
            bool same_nonempty_group = !head.group.empty();
            for (size_t k = i; k < commands.size(); ++k) {
                if (placed[k] || commands[k].package != head.package) continue;
                placed[k] = true;
                if (commands[k].group != head.group) same_nonempty_group = false;
                if (group_icon == 0 && commands[k].group_icon != 0) group_icon = commands[k].group_icon;
                auto row = Row(commands[k]);
                row.detail = Wide(commands[k].detail);
                push_with_separators(members, std::move(row), commands[k].flags);
            }
            ui::FluentMenuItem parent;
            parent.text = same_nonempty_group ? Wide(head.group) : PackageDisplayName(head.package);
            AssignGroupIcon(parent, group_icon, head.package);
            parent.group_tint = parent.group_start = parent.group_end = true;
            if (const int command = SettingsCommand(owner, head.package)) {
                if (!members.empty()) members.back().separator_after = true;
                ui::FluentMenuItem settings;
                settings.command = command; settings.text = l10n::Get(l10n::StringId::Settings);
                settings.vector_icon = ui::StoreIcon::Gear;
                members.push_back(std::move(settings));
            }
            parent.children = std::move(members);
            rows.push_back(std::move(parent));
            continue;
        }
        if (head.group.empty()) {
            placed[i] = true;
            auto row = Row(head);
            row.detail = Wide(head.detail);
            push_with_separators(rows, std::move(row), head.flags);
            continue;
        }
        std::vector<ui::FluentMenuItem> members;
        bool submenu = false;
        uint32_t group_icon = 0;
        for (size_t k = i; k < commands.size(); ++k) {
            if (placed[k] || commands[k].package != head.package || commands[k].group != head.group) continue;
            placed[k] = true;
            submenu = submenu || (commands[k].flags & PULSE_COMMAND_GROUP_SUBMENU) != 0;
            if (group_icon == 0 && commands[k].group_icon != 0) group_icon = commands[k].group_icon;
            auto row = Row(commands[k]);
            row.detail = Wide(commands[k].detail);
            push_with_separators(members, std::move(row), commands[k].flags);
        }
        if (submenu) {
            ui::FluentMenuItem parent;
            parent.text = Wide(head.group);
            AssignGroupIcon(parent, group_icon, head.package);
            parent.group_tint = parent.group_start = parent.group_end = true;
            if (const int command = SettingsCommand(owner, head.package)) {
                if (!members.empty()) members.back().separator_after = true;
                ui::FluentMenuItem settings;
                settings.command = command; settings.text = l10n::Get(l10n::StringId::Settings);
                settings.vector_icon = ui::StoreIcon::Gear;
                members.push_back(std::move(settings));
            }
            parent.children = std::move(members);
            rows.push_back(std::move(parent));
        } else {
            ui::FluentMenuItem caption;
            caption.heading = true; caption.text = Wide(head.group);
            caption.trailing_command = SettingsCommand(owner, head.package);
            caption.enabled = caption.trailing_command != 0;
            caption.trailing_vector_icon = ui::StoreIcon::Gear;
            caption.group_tint = caption.group_start = true;
            for (auto& member : members) member.group_tint = true;
            if (!members.empty()) members.back().group_end = true;
            else caption.group_end = true;
            rows.push_back(std::move(caption));
            rows.insert(rows.end(), std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
        }
    }
    return rows;
}
bool HandleContributedCommand(AppState& owner, int command) {
    if (command >= kSettingsCommand && command < kSettingsCommand + 10000) {
        const size_t package = static_cast<size_t>(command - kSettingsCommand);
        const auto* manifest = PackageManifest(package);
        if (!manifest || SettingsCommand(owner, manifest->id) != command) return true;
        if (manifest->id == "pulse.7zip") {
            EnsureCommandPackageStarted(owner, package);
            owner.settings_store_browse = true; owner.settings_store_installed = false;
            owner.store_detail = true; owner.store_detail_tab = 0;
            OpenSettingsTab(owner, 8);
        } else {
            const auto* page = PackageSettings(owner.hwnd, package);
            if (page && page->Page()) RequestSettingsPagesPackage(owner.hwnd, package);
            else RequestSettingsPagesContextMenu(owner.hwnd, package);
            OpenSettingsTab(owner, ui::kSettingsPageNativeExtensions);
        }
        owner.settings.SetScroll(0, 0);
        InvalidateRect(owner.hwnd, nullptr, FALSE);
        return true;
    }
    if (const auto target = CommandsAdapter::LookupStaticToken(owner.hwnd, command)) {
        if (PackageEnabled(target->package) && EnsureCommandPackageStarted(owner, target->package))
            CommandsAdapter::InvokeIdentity(owner.hwnd, target->identity);
        return true;
    }
    return CommandsAdapter::Invoke(owner.hwnd, command);
}
bool HandleContributedShortcut(AppState& owner, WPARAM key) {
    const uint32_t modifiers = (GetKeyState(VK_CONTROL) < 0 ? PULSE_COMMAND_CTRL : 0u) |
        (GetKeyState(VK_SHIFT) < 0 ? PULSE_COMMAND_SHIFT : 0u) | (GetKeyState(VK_MENU) < 0 ? PULSE_COMMAND_ALT : 0u);
    if (!modifiers && (key < VK_F1 || key > VK_F24)) return false;
    const auto vk = static_cast<uint32_t>(key);
    PrepareCommandPackages(owner);
    if (CommandsAdapter::Shortcut(owner.hwnd, vk, modifiers)) return true;

    CommandSelection selection;
    bool selection_captured = false;
    const StaticCommandLanguage language = CurrentStaticLanguage();
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg)) continue;
        if (!manifest->command_contributions_declared || PackageSessionLoaded(owner.hwnd, pkg)) continue;
        for (const auto& contrib : manifest->command_contributions) {
            if (contrib.virtual_key != vk || contrib.modifiers != modifiers) continue;
            if (!selection_captured) {
                selection = CaptureWindowCommandSelection(owner);
                selection_captured = true;
            }
            const auto info = CommandsAdapter::EvaluateStaticContribution(
                owner.hwnd, pkg, manifest->id, contrib, language, selection);
            if (!info.enabled) continue;
            if (!EnsureCommandPackageStarted(owner, pkg)) return false;
            if (CommandsAdapter::InvokeIdentity(owner.hwnd, info.identity)) return true;
            return CommandsAdapter::Shortcut(owner.hwnd, vk, modifiers);
        }
    }
    return false;
}
std::wstring CommandIdentity(AppState& owner, int token) {
    return Wide(CommandsAdapter::Identity(owner.hwnd, token));
}
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState& owner) {
    const auto commands = AllCommandEntries(owner);
    std::vector<ui::FluentMenuItem> rows;
    for (const auto& id : owner.appPrefs.toolbar_tools) {
        if (!id.starts_with(L"native:")) continue;
        ui::FluentMenuItem row; row.text = id; row.enabled = false; row.tooltip = id;
        for (const auto& command : commands) if (Wide(command.identity) == id) { row = Row(command); break; }
        rows.push_back(std::move(row));
    }
    return rows;
}
}
