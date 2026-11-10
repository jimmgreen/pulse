#include "settings_pages_host.h"
#include "settings_pages_adapter.h"
#include "session.h"
#include "../runtime.h"
#include "../../app_runtime.h"
#include "../../../ui/extension_panel.h"
#include "../../../ui/ui_renderer.h"
#include "../../../ui/settings_page_ids.h"
#include <charconv>
#include <map>
#include <memory>

namespace pulse::extensions::native {
namespace {
std::string Utf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string value(static_cast<size_t>(n), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), n, nullptr, nullptr);
    return value;
}
struct Entry {
    size_t package; uint64_t page; std::wstring title;
    bool operator==(const Entry&) const = default;
};
struct Host {
    ui::ExtensionPanel selector;
    std::vector<Entry> entries;
    size_t selected = 0;
    bool visible = false, syncing = false;
};
auto& hosts = *new std::map<HWND, std::unique_ptr<Host>>;
void HidePages(HWND owner) {
    for (size_t i = 0; i < PackageCount(); ++i) if (auto* pages = PackageSettings(owner, i)) pages->Hide();
}
}
void CloseSettingsPagesHost(HWND owner) {
    HidePages(owner);
    hosts.erase(owner);
}
void SyncSettingsPagesHost(AppState& owner, const ui::WindowViewModel& vm) {
    const bool visible = vm.settings_open && vm.settings_page == ui::kSettingsPageNativeExtensions;
    auto found = hosts.find(owner.hwnd);
    if (!visible) {
        if (found != hosts.end() && found->second->visible) {
            HidePages(owner.hwnd);
            ShowWindow(found->second->selector.Hwnd(), SW_HIDE);
            found->second->visible = false;
        }
        return;
    }
    if (found == hosts.end()) found = hosts.emplace(owner.hwnd, std::make_unique<Host>()).first;
    auto& host = *found->second;
    if (host.syncing) return;
    host.syncing = true;
    struct Unlock { bool& value; ~Unlock() { value = false; } } unlock{host.syncing};
    if (!host.visible) PrepareCommandPackages(owner);
    const auto area = owner.renderer.NativeSettingsRect(static_cast<float>(owner.compositor.Width()),
        static_cast<float>(owner.compositor.Height()), vm);
    RECT bounds{static_cast<LONG>(area.left), static_cast<LONG>(area.top), static_cast<LONG>(area.right), static_cast<LONG>(area.bottom)};
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        HidePages(owner.hwnd); ShowWindow(host.selector.Hwnd(), SW_HIDE); host.visible = false; return;
    }
    std::vector<Entry> entries;
    for (size_t i = 0; i < PackageCount(); ++i) if (auto* pages = PackageSettings(owner.hwnd, i))
        if (auto page = pages->Page()) entries.push_back({i, page->id, page->title});
    const bool changed = entries != host.entries;
    if (changed || !host.selector.IsOpen()) {
        const auto previous = host.selected < host.entries.size() ? host.entries[host.selected] : Entry{SIZE_MAX, 0, {}};
        host.entries = std::move(entries); host.selected = 0;
        for (size_t i = 0; i < host.entries.size(); ++i)
            if (host.entries[i].package == previous.package && host.entries[i].page == previous.page) host.selected = i;
        UiTree tree; tree.root.id = "settings-selector"; tree.root.gap = 4;
        UiNode label; label.id = "settings-label"; label.type = UiNodeType::Text;
        label.text = Utf8(l10n::Get(host.entries.empty() ? l10n::StringId::NativeExtensionSettingsEmpty : l10n::StringId::NativeExtensionSettingsChoose));
        tree.root.children.push_back(std::move(label));
        if (!host.entries.empty()) {
            UiNode select; select.id = "settings-package"; select.type = UiNodeType::Select; select.state = "package";
            for (const auto& entry : host.entries) select.options.push_back(Utf8(entry.title));
            tree.root.children.push_back(std::move(select));
            tree.state.push_back({"package", std::to_string(host.selected)});
        }
        host.selector.Close();
        {
            ui::ExtensionPanelSpec spec; spec.content_only = true; spec.tree = std::move(tree);
            const HWND window = owner.hwnd;
            spec.tree_callbacks.on_state_changed = [window](uint64_t, const std::vector<ui::ExtensionStateChange>& changes) {
                const auto it = hosts.find(window); if (it == hosts.end()) return;
                for (const auto& change : changes) if (change.key == "package") {
                    size_t index = 0; const auto result = std::from_chars(change.value.data(), change.value.data() + change.value.size(), index);
                    if (result.ec == std::errc{} && result.ptr == change.value.data() + change.value.size() && index < it->second->entries.size()) {
                        it->second->selected = index; InvalidateRect(window, nullptr, FALSE);
                    }
                }
            };
            host.selector.ShowDocked(window, bounds, std::move(spec), owner.darkMode, owner.accentColor, owner.scale);
        }
    }
    // One settings page needs no "choose a package" header: the page gets the whole area.
    const bool single = host.entries.size() == 1;
    const LONG header_bottom = single ? bounds.top : std::min(bounds.bottom, bounds.top + static_cast<LONG>(104 * owner.scale));
    RECT header{bounds.left, bounds.top, bounds.right, header_bottom};
    host.selector.UpdateDocked(header, owner.darkMode, owner.accentColor, owner.scale);
    ShowWindow(host.selector.Hwnd(), single ? SW_HIDE : SW_SHOWNOACTIVATE);
    for (size_t i = 0; i < PackageCount(); ++i)
        if (host.selected >= host.entries.size() || host.entries[host.selected].package != i || bounds.bottom <= header_bottom)
            if (auto* pages = PackageSettings(owner.hwnd, i)) pages->Hide();
    if (host.selected < host.entries.size() && bounds.bottom > header_bottom) {
        const auto& entry = host.entries[host.selected];
        if (auto* pages = PackageSettings(owner.hwnd, entry.package)) {
            RECT body{bounds.left, header_bottom, bounds.right, bounds.bottom};
            pages->Show(entry.page, body, owner.darkMode, owner.accentColor, owner.scale);
        }
    }
    host.visible = true;
}
}
