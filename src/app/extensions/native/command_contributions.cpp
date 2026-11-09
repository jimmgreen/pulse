#include "command_contributions.h"
#include "commands_adapter.h"
#include "session.h"
#include "../../app_runtime.h"
#include "../../actions/action_registry.h"
namespace pulse::extensions::native {
namespace {
std::wstring Wide(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<size_t>(count), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}
ui::FluentMenuItem Row(const CommandInfo& info) {
    ui::FluentMenuItem row;
    row.command = info.token; row.text = Wide(info.title); row.tooltip = Wide(info.identity);
    row.enabled = info.enabled;
    row.glyph = info.icon == PULSE_COMMAND_ICON_ARCHIVE ? L"\xE8B7" : info.icon == PULSE_COMMAND_ICON_EXTRACT ? L"\xE896" : L"\xE8A5";
    if (info.key) {
        if (info.modifiers & PULSE_COMMAND_CTRL) row.shortcut += L"Ctrl+";
        if (info.modifiers & PULSE_COMMAND_SHIFT) row.shortcut += L"Shift+";
        if (info.modifiers & PULSE_COMMAND_ALT) row.shortcut += L"Alt+";
        if (info.key >= VK_F1) row.shortcut += L"F" + std::to_wstring(info.key - VK_F1 + 1);
        else row.shortcut += static_cast<wchar_t>(info.key);
    }
    return row;
}
}
std::vector<ui::FluentMenuItem> CommandRows(AppState& owner, std::wstring_view query) {
    PrepareCommandPackages(owner);
    std::vector<ui::FluentMenuItem> rows;
    for (const auto& info : CommandsAdapter::Entries(owner.hwnd)) {
        if (!info.enabled) continue;
        auto row = Row(info);
        app::actions::ActionView view; view.title = row.text; view.id = info.identity;
        if (!query.empty() && !app::actions::ActionMatchTier(view, query)) continue;
        rows.push_back(std::move(row));
    }
    return rows;
}
bool HandleContributedCommand(AppState& owner, int command) { return CommandsAdapter::Invoke(owner.hwnd, command); }
bool HandleContributedShortcut(AppState& owner, WPARAM key) {
    const uint32_t modifiers = (GetKeyState(VK_CONTROL) < 0 ? PULSE_COMMAND_CTRL : 0u) |
        (GetKeyState(VK_SHIFT) < 0 ? PULSE_COMMAND_SHIFT : 0u) | (GetKeyState(VK_MENU) < 0 ? PULSE_COMMAND_ALT : 0u);
    if (!modifiers && (key < VK_F1 || key > VK_F24)) return false;
    PrepareCommandPackages(owner);
    return CommandsAdapter::Shortcut(owner.hwnd, static_cast<uint32_t>(key), modifiers);
}
std::wstring CommandIdentity(AppState& owner, int token) {
    return Wide(CommandsAdapter::Identity(owner.hwnd, token));
}
std::vector<ui::FluentMenuItem> PinnedCommandRows(AppState& owner) {
    const auto commands = CommandsAdapter::Entries(owner.hwnd);
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
