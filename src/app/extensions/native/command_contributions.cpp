#include "command_contributions.h"
#include "commands_adapter.h"
#include "session.h"
#include "settings_pages_adapter.h"
#include "settings_pages_host.h"
#include "../icon_registry.h"
#include "../command_projection_cache.h"
#include "../runtime.h"
#include "../resource_settings.h"
#include "../../../../sdk/native/include/pulse/icons.h"
#include "../seven_zip_menu_projection.h"
#include "../seven_zip_menu_settings.h"
#include "../../ai_assistant.h"
#include "../../app_navigation.h"
#include "../../open_routing.h"
#include "../../../ops/archive_open_fallback.h"
#include "../../session.h"
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
constexpr std::string_view kDesktopAiCommand = "native:pulse.desktop:desktop-ai-organize";
constexpr std::wstring_view kDesktopAiCommandWide = L"native:pulse.desktop:desktop-ai-organize";
bool CommandVisible(AppState& owner, std::string_view identity) {
    return identity != kDesktopAiCommand || AiAssistantEnabled(owner);
}
bool InvokeLiveShortcut(AppState& owner, uint32_t key, uint32_t modifiers) {
    for (const auto& info : CommandsAdapter::Entries(owner.hwnd)) {
        if (info.enabled && info.key == key && info.modifiers == modifiers && CommandVisible(owner, info.identity))
            return CommandsAdapter::Invoke(owner.hwnd, info.token);
    }
    return false;
}
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
std::string CurrentLanguageTag() {
    switch (l10n::effective_language()) {
    case l10n::Language::ZhTW: return "zh-TW";
    case l10n::Language::EnUS: return "en-US";
    case l10n::Language::ZhCN:
    case l10n::Language::System:
    default:
        return "zh-CN";
    }
}
std::string ProjectionRevision(const Manifest& manifest) {
    return manifest.native ? manifest.native->sha256 : std::string{};
}
std::wstring ResolveProjectionRoot(const AppState& owner) {
    if (owner.isolatedTest) {
        wchar_t value[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", value, ARRAYSIZE(value));
        if (!size || size >= ARRAYSIZE(value) || !std::filesystem::path(value).is_absolute()) return {};
        return std::wstring(value, size);
    }
    return app::GetPulseDataDir();
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
// 7-Zip used to hide menu commands with its own switches; those now live in the host's 右键菜单 tab.
// Bits a user turned off there become host overrides once, so the menu looks the same after upgrade.
void MigrateSevenZipHiddenCommands(AppState& owner, const std::string& package, unsigned commands) {
    const std::string marker = "migrated:" + package + ":commands";
    auto& hidden = owner.ctxMenuPrefs.extension_hidden_commands;
    if (commands == seven_zip::kAllMenuCommands || hidden.contains(marker)) return;
    for (size_t i = 0; i < seven_zip::kMenuCommands.size(); ++i) {
        if ((commands & (1u << i)) == 0)
            owner.ctxMenuPrefs.SetExtensionCommandVisible("native:" + package + ":" + std::string(seven_zip::kMenuCommands[i]), false);
    }
    hidden.insert(marker);
    if (!owner.isolatedTest) owner.ctxMenuPrefs.Save();
}
std::vector<CommandInfo> AllCommandEntries(AppState& owner, bool* waiting_for_seven_zip = nullptr,
                                          std::string_view only_package = {}) {
    if (waiting_for_seven_zip) *waiting_for_seven_zip = false;
    PrepareCommandPackages(owner);
    WarmCommandProjectionCache(owner);
    WarmSevenZipMenuPreferences(owner);
    auto live = CommandsAdapter::Entries(owner.hwnd);
    std::vector<bool> used(live.size(), false);
    std::vector<CommandInfo> result;
    CommandSelection selection;
    bool selection_captured = false;
    const StaticCommandLanguage language = CurrentStaticLanguage();
    const std::string language_tag = CurrentLanguageTag();
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg)) continue;
        if (!only_package.empty() && manifest->id != only_package) continue;
        const bool loaded = PackageSessionLoaded(owner.hwnd, pkg);
        if (!loaded) {
            if (const auto cached = command_projection::Lookup(
                    manifest->id, ProjectionRevision(*manifest), language_tag)) {
                if (!selection_captured) {
                    selection = CaptureWindowCommandSelection(owner);
                    selection_captured = true;
                }
                for (const auto& cmd : cached->commands) {
                    result.push_back(CommandsAdapter::EvaluateProjectedCommand(
                        owner.hwnd, pkg, manifest->id, cmd, selection));
                }
                continue;
            }
        }
        if (manifest->id == "pulse.7zip" && manifest->command_contributions_declared) {
            // These commands depend on persistent settings, even when the DLL is cold
            // or has started but is still waiting for its own settings callback.
            for (size_t i = 0; i < live.size(); ++i)
                if (live[i].package == manifest->id) used[i] = true;
            const auto encoded = seven_zip_menu::Snapshot();
            if (!encoded) {
                if (waiting_for_seven_zip) *waiting_for_seven_zip = true;
                continue;
            }
            const auto prefs = seven_zip::ParseCommandPreferences(*encoded).value_or(seven_zip::CommandPreferences{});
            MigrateSevenZipHiddenCommands(owner, manifest->id, prefs.commands);
            if (!selection_captured) {
                selection = CaptureWindowCommandSelection(owner);
                selection_captured = true;
            }
            for (auto contrib : manifest->command_contributions) {
                if (seven_zip_menu::ApplyPreferences(contrib, prefs))
                    result.push_back(CommandsAdapter::EvaluateStaticContribution(owner.hwnd, pkg, manifest->id, contrib, language, selection));
            }
            continue;
        }
        if (loaded) {
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
        if (!used[i] && (only_package.empty() || live[i].package == only_package))
            result.push_back(std::move(live[i]));
    }
    std::erase_if(result, [&owner](const CommandInfo& info) { return !CommandVisible(owner, info.identity); });
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
void WarmCommandProjectionCache(AppState& owner) {
    const std::wstring root = ResolveProjectionRoot(owner);
    if (root.empty()) return;
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || manifest->native->sha256.empty() || !PackageEnabled(pkg)) continue;
        uint64_t generation = 0;
        if (!command_projection::BeginWarmRead(manifest->id, generation)) continue;
        bool queued = false;
        try {
            queued = owner.worker.EnqueueIo([root, id = manifest->id, generation] {
                try {
                    command_projection::PublishWarmRead(id, generation, ReadCommandProjectionFile(root, id));
                } catch (...) {
                    command_projection::PublishWarmRead(id, generation, std::nullopt);
                }
            });
        } catch (...) {}
        if (!queued) {
            command_projection::PublishWarmRead(manifest->id, generation, std::nullopt);
        }
    }
}

void WarmSevenZipMenuPreferences(AppState& owner) {
    bool enabled = false;
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (manifest && manifest->id == "pulse.7zip" && manifest->native && PackageEnabled(pkg)) {
            enabled = true;
            break;
        }
    }
    if (!enabled) return;
    std::wstring isolated_root;
    if (owner.isolatedTest) {
        wchar_t value[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", value, ARRAYSIZE(value));
        if (!size || size >= ARRAYSIZE(value) || !std::filesystem::path(value).is_absolute()) return;
        isolated_root.assign(value, size);
    }
    if (!seven_zip_menu::BeginRead()) return;
    try {
        if (owner.worker.EnqueueIo([root = std::move(isolated_root)] {
            try {
                seven_zip_menu::Publish(ReadOwnSettings(root.empty() ? app::GetPulseDataDir() : root, "pulse.7zip"), true);
            } catch (...) {
                seven_zip_menu::Publish({}, true);
            }
        })) return;
    } catch (...) {}
    // A stopped worker rejected the job; no completion will release the read.
    seven_zip_menu::Publish({}, true);
}

bool SevenZipMenuPreferencesReady() {
    return static_cast<bool>(seven_zip_menu::Snapshot());
}

std::vector<PackageCommandSummary> KnownPackageCommands(AppState& owner, size_t package) {
    const auto* manifest = PackageManifest(package);
    if (!manifest || !manifest->native) return {};
    std::vector<PackageCommandSummary> out;
    std::set<std::string> seen;
    const auto visible_and_new = [&owner, &seen](const std::string& identity) {
        return CommandVisible(owner, identity) && seen.insert(identity).second;
    };
    const bool loaded = PackageSessionLoaded(owner.hwnd, package);
    if (loaded) {
        for (auto& item : CommandsAdapter::LivePackageCommands(owner.hwnd, manifest->id)) {
            if (visible_and_new(item.identity)) out.push_back(std::move(item));
        }
    } else if (const auto cached = command_projection::Lookup(
                   manifest->id, ProjectionRevision(*manifest), CurrentLanguageTag())) {
        for (const auto& cmd : cached->commands) {
            const std::string identity = "native:" + manifest->id + ":" + cmd.id;
            if (!visible_and_new(identity)) continue;
            uint32_t icon = cmd.icon < PULSE_ICON_CUSTOM_MIN ? cmd.icon : 0u;
            if (!cmd.icon_name.empty()) {
                if (const auto resolved = icons::GlobalRegistry().ResolveName(manifest->id, cmd.icon_name))
                    icon = *resolved;
            }
            out.push_back(PackageCommandSummary{
                identity,
                cmd.title,
                icon,
                (cmd.flags & PULSE_COMMAND_HIDDEN) != 0,
            });
        }
        return out;
    }
    if (manifest->command_contributions_declared) {
        const StaticCommandLanguage language = CurrentStaticLanguage();
        for (const auto& contrib : manifest->command_contributions) {
            const std::string identity = "native:" + manifest->id + ":" + contrib.id;
            if (!visible_and_new(identity)) continue;
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
std::vector<ui::FluentMenuItem> ContextMenuCommandRows(AppState& owner, bool* waiting_for_seven_zip,
                                                     std::string_view only_package, bool any_selection) {
    std::vector<CommandInfo> commands;
    for (auto& info : AllCommandEntries(owner, waiting_for_seven_zip, only_package)) {
        if (any_selection) info.enabled = true;
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
namespace {
std::string LowerAsciiFilenameUtf8(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::string out;
    out.reserve(name.size());
    for (const wchar_t c : name) {
        if (c >= L'A' && c <= L'Z') out.push_back(static_cast<char>(c - L'A' + 'a'));
        else if (c < 0x80) out.push_back(static_cast<char>(c));
        else out.push_back('?'); // Preserve length so multi-byte stem doesn't break suffix match.
    }
    return out;
}
size_t MatchHandlerExtensionLength(const ManifestFileHandlerContribution& handler,
                                   std::string_view lower_filename) {
    size_t best = 0;
    for (const auto& ext : handler.extensions) {
        if (ext.size() > best && lower_filename.size() >= ext.size() &&
            lower_filename.ends_with(ext)) {
            best = ext.size();
        }
    }
    return best;
}
} // namespace

std::vector<std::wstring> FallbackOpenExtensions() {
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg)) continue;
        for (const auto& handler : manifest->file_handlers) {
            if (!handler.fallback_on_unassociated) continue;
            for (const auto& ext : handler.extensions) {
                std::wstring wide = Wide(ext);
                if (!wide.empty() && seen.insert(wide).second) {
                    out.push_back(std::move(wide));
                }
            }
        }
    }
    return out;
}

bool HandleExtensionFileOpen(AppState& owner,
                             std::string_view package_id,
                             std::string_view handler_id,
                             const std::wstring& path,
                             bool force_preview) {
    if (package_id.empty() || handler_id.empty() || path.empty()) return false;
    const std::string lower_filename = LowerAsciiFilenameUtf8(path);
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg) || manifest->id != package_id) continue;
        for (const auto& handler : manifest->file_handlers) {
            if (handler.id != handler_id) continue;
            if (MatchHandlerExtensionLength(handler, lower_filename) == 0) return false;
            if (force_preview || handler.action == FileHandlerAction::Preview) {
                EnsureCommandPackageStarted(owner, pkg);
                PreviewArchiveWhenSelected(owner, path);
                return true;
            }
            CommandSelection selection;
            const std::filesystem::path file(path);
            CommandSelectionEntry entry;
            if (!TryUtf8(path, entry.path) || !TryUtf8(file.filename().wstring(), entry.name) ||
                !TryUtf8(file.parent_path().wstring(), selection.directory)) {
                return false;
            }
            selection.entries.push_back(std::move(entry));
            selection.valid = true;
            const std::string identity = "native:" + manifest->id + ":" + handler.command;
            if (!CommandVisible(owner, identity) || !EnsureCommandPackageStarted(owner, pkg)) return false;
            return CommandsAdapter::InvokeIdentityWithSelection(owner.hwnd, identity, selection);
        }
        return false;
    }
    return false;
}

bool HandleArchiveOpenFallback(AppState& owner, const std::wstring& path) {
    if (ops::archive_open::Supported(path)) {
        for (size_t package = 0; package < PackageCount(); ++package) {
            const auto* manifest = PackageManifest(package);
            if (!manifest || manifest->id != "pulse.7zip" || !manifest->native || !PackageEnabled(package)) continue;
            WarmSevenZipMenuPreferences(owner);
            if (seven_zip_menu::PreviewOnOpen()) {
                EnsureCommandPackageStarted(owner, package);
                PreviewArchiveWhenSelected(owner, path);
                return true;
            }
            CommandSelection selection;
            const std::filesystem::path file(path);
            CommandSelectionEntry entry;
            if (!TryUtf8(path, entry.path) || !TryUtf8(file.filename().wstring(), entry.name) ||
                !TryUtf8(file.parent_path().wstring(), selection.directory)) return false;
            selection.entries.push_back(std::move(entry));
            selection.valid = true;
            if (!EnsureCommandPackageStarted(owner, package)) return false;
            return CommandsAdapter::InvokeHiddenIdentity(owner.hwnd, "native:pulse.7zip:extract-auto", selection);
        }
    }
    const std::string lower_filename = LowerAsciiFilenameUtf8(path);
    std::string best_package;
    std::string best_handler;
    size_t best_len = 0;
    for (size_t package = 0; package < PackageCount(); ++package) {
        const auto* manifest = PackageManifest(package);
        if (!manifest || !manifest->native || !PackageEnabled(package)) continue;
        for (const auto& handler : manifest->file_handlers) {
            if (!handler.fallback_on_unassociated) continue;
            const size_t len = MatchHandlerExtensionLength(handler, lower_filename);
            if (len > best_len) {
                best_len = len;
                best_package = manifest->id;
                best_handler = handler.id;
            }
        }
    }
    if (best_len > 0) {
        return HandleExtensionFileOpen(owner, best_package, best_handler, path, false);
    }
    return false;
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
    if (!CommandVisible(owner, CommandsAdapter::Identity(owner.hwnd, command))) return true;
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
    if (InvokeLiveShortcut(owner, vk, modifiers)) return true;

    CommandSelection selection;
    bool selection_captured = false;
    const StaticCommandLanguage language = CurrentStaticLanguage();
    const std::string language_tag = CurrentLanguageTag();
    WarmCommandProjectionCache(owner);
    for (size_t pkg = 0; pkg < PackageCount(); ++pkg) {
        const auto* manifest = PackageManifest(pkg);
        if (!manifest || !manifest->native || !PackageEnabled(pkg) || PackageSessionLoaded(owner.hwnd, pkg)) continue;
        if (const auto cached = command_projection::Lookup(
                manifest->id, ProjectionRevision(*manifest), language_tag)) {
            for (const auto& cmd : cached->commands) {
                if (cmd.virtual_key != vk || cmd.modifiers != modifiers) continue;
                if (!selection_captured) {
                    selection = CaptureWindowCommandSelection(owner);
                    selection_captured = true;
                }
                const auto info = CommandsAdapter::EvaluateProjectedCommand(
                    owner.hwnd, pkg, manifest->id, cmd, selection);
                if (!info.enabled || !CommandVisible(owner, info.identity)) continue;
                if (!EnsureCommandPackageStarted(owner, pkg)) return false;
                if (CommandsAdapter::InvokeIdentity(owner.hwnd, info.identity)) return true;
                return InvokeLiveShortcut(owner, vk, modifiers);
            }
            continue;
        }
        if (!manifest->command_contributions_declared) continue;
        for (const auto& contrib : manifest->command_contributions) {
            if (contrib.virtual_key != vk || contrib.modifiers != modifiers) continue;
            if (!selection_captured) {
                selection = CaptureWindowCommandSelection(owner);
                selection_captured = true;
            }
            const auto info = CommandsAdapter::EvaluateStaticContribution(
                owner.hwnd, pkg, manifest->id, contrib, language, selection);
            if (!info.enabled || !CommandVisible(owner, info.identity)) continue;
            if (!EnsureCommandPackageStarted(owner, pkg)) return false;
            if (CommandsAdapter::InvokeIdentity(owner.hwnd, info.identity)) return true;
            return InvokeLiveShortcut(owner, vk, modifiers);
        }
    }
    return false;
}
std::wstring CommandIdentity(AppState& owner, int token) {
    const auto identity = CommandsAdapter::Identity(owner.hwnd, token);
    return CommandVisible(owner, identity) ? Wide(identity) : std::wstring{};
}
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState& owner) {
    const auto commands = AllCommandEntries(owner);
    ApplyToolbarSuggestions(owner, commands);
    std::vector<ui::FluentMenuItem> rows;
    for (const auto& id : owner.appPrefs.toolbar_tools) {
        if (!id.starts_with(L"native:")) continue;
        if (id == kDesktopAiCommandWide && !AiAssistantEnabled(owner)) continue;
        ui::FluentMenuItem row; row.text = id; row.enabled = false; row.tooltip = id;
        for (const auto& command : commands) if (Wide(command.identity) == id) { row = Row(command); break; }
        rows.push_back(std::move(row));
    }
    return rows;
}
}