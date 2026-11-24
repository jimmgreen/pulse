// extension_columns_ui.cpp - extension columns and icon badges (pulse.columns)
// in the file list. The providers live in extensions::columns::Registry; this
// file picks the ones a pane shows and hands the renderer lookups that never
// block (a miss is queued and the pane repaints when Revision() moves).
#include "extension_columns_ui.h"
#include "app_model.h"
#include "app_state.h"
#include "app_runtime.h"
#include "extensions/column_registry.h"
#include "../common/localization.h"
#include "../common/string_ids.h"
#include "../ui/fluent_menu.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>

namespace pulse::app {
namespace {

namespace cols = extensions::columns;

std::mutex g_mutex;
std::vector<std::wstring> g_hidden;          // provider keys
std::map<std::wstring, float> g_widths;      // provider key -> DIP
bool g_widths_dirty = false;

uint64_t Ticks(const FILETIME& t) noexcept {
    return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

std::wstring Prefix(const std::wstring& folder) {
    return !folder.empty() && folder.back() == L'\\' ? folder : folder + L"\\";
}

// Local folders only, as for media columns: on a share every read would
// cross the network, and virtual folders have no file paths.
bool Eligible(const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path) || fs::IsUncPath(path)) return false;
    return !path.starts_with(L"\\\\") || (path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\?\\UNC\\"));
}

bool Hidden(const std::vector<std::wstring>& hidden, const std::wstring& key) {
    return std::find(hidden.begin(), hidden.end(), key) != hidden.end();
}

// What one pane's lookups read: the listing and the providers it shows.
struct PaneColumns {
    fs::SnapshotPtr snapshot;
    std::wstring prefix;
    std::vector<uint64_t> columns;
    std::vector<uint64_t> badges;

    bool ItemAt(int source, cols::Item& item) const {
        if (!snapshot || source < 0 || static_cast<size_t>(source) >= snapshot->size()) return false;
        const fs::DirEntry& e = (*snapshot)[static_cast<size_t>(source)];
        item.path = prefix + e.name;
        item.is_dir = e.is_dir;
        item.size = e.is_dir ? 0 : e.size;
        item.mtime = Ticks(e.mtime);
        return true;
    }
};

void Decorate(const Tab& tab, ui::PaneViewModel& out) {
    if (tab.loading || !tab.snapshot || out.this_pc_columns || out.is_search || out.content_results ||
        !Eligible(tab.current_path))
        return;
    auto& registry = cols::Registry::Instance();
    const auto column_list = registry.List(cols::Kind::Column);
    const auto badge_list = registry.List(cols::Kind::Badge);
    if (column_list.empty() && badge_list.empty()) return;
    auto state = std::make_shared<PaneColumns>();
    {
        std::lock_guard lock(g_mutex);
        const bool details = tab.view_mode == ui::ViewMode::Details && !out.column_strip.Active();
        for (const auto& info : column_list) {
            if (!details || state->columns.size() >= ui::MainRenderer::kMaxExtensionColumns) break;
            const std::wstring key = info.Key();
            if (Hidden(g_hidden, key)) continue;
            ui::PaneViewModel::ExtensionColumnView view;
            view.key = key;
            view.title = info.title;
            const auto width = g_widths.find(key);
            view.width_dip = width != g_widths.end() && width->second >= 48.0f ? width->second : info.width_dip;
            view.align_end = info.align_end;
            out.extension_columns.push_back(std::move(view));
            state->columns.push_back(info.handle);
        }
        for (const auto& info : badge_list)
            if (!Hidden(g_hidden, info.Key())) state->badges.push_back(info.handle);
    }
    if (state->columns.empty() && state->badges.empty()) return;
    state->snapshot = tab.snapshot;
    state->prefix = Prefix(tab.current_path);
    if (!state->columns.empty()) {
        out.extension_cell = [state](int source, size_t column, std::wstring& text, uint32_t& tone) {
            if (column >= state->columns.size()) return false;
            cols::Item item;
            if (!state->ItemAt(source, item)) return false;
            cols::CellValue value;
            if (cols::Registry::Instance().Cell(state->columns[column], item, value) != cols::Lookup::Ready)
                return false;
            text = std::move(value.text);
            tone = value.tone;
            return true;
        };
    }
    if (!state->badges.empty()) {
        out.extension_badge = [state](int source, uint32_t& icon, uint32_t& tone) {
            cols::Item item;
            if (!state->ItemAt(source, item)) return false;
            // The first provider (in registration order) with a badge wins;
            // every provider is still asked, so the order settles quickly.
            bool found = false;
            for (const uint64_t handle : state->badges) {
                cols::BadgeValue value;
                if (cols::Registry::Instance().Badge(handle, item, value) != cols::Lookup::Ready ||
                    !value.shown || found)
                    continue;
                icon = value.icon;
                tone = value.tone;
                found = true;
            }
            return found;
        };
    }
}

std::wstring PackageText(l10n::StringId id, const std::string& package_id) {
    std::wstring text = l10n::Get(id);
    std::wstring package;
    cols::Utf8ToWide(package_id, package);
    const size_t at = text.find(L"{name}");
    if (at != std::wstring::npos) text.replace(at, 6, package);
    return text;
}

std::vector<cols::ProviderInfo> AllProviders() {
    auto& registry = cols::Registry::Instance();
    auto all = registry.List(cols::Kind::Column);
    auto badges = registry.List(cols::Kind::Badge);
    all.insert(all.end(), badges.begin(), badges.end());
    return all;
}

} // namespace

void InstallExtensionColumns() { SetExtensionPaneDecorator(&Decorate); }

void SetExtensionColumnPrefs(const std::vector<std::wstring>& hidden, const std::vector<std::wstring>& widths) {
    std::lock_guard lock(g_mutex);
    g_hidden = hidden;
    g_widths.clear();
    for (const auto& line : widths) {
        const size_t bar = line.rfind(L'|');
        if (bar == std::wstring::npos || bar == 0) continue;
        const float dip = static_cast<float>(std::wcstod(line.c_str() + bar + 1, nullptr));
        if (dip >= 48.0f && dip <= 480.0f) g_widths[line.substr(0, bar)] = dip;
    }
    g_widths_dirty = false;
}

std::vector<std::wstring> ExtensionColumnWidthPrefs() {
    std::lock_guard lock(g_mutex);
    std::vector<std::wstring> out;
    for (const auto& [key, dip] : g_widths) out.push_back(key + L"|" + std::to_wstring(static_cast<int>(dip + 0.5f)));
    return out;
}

void SetExtensionColumnWidth(const std::wstring& key, float dip) {
    std::lock_guard lock(g_mutex);
    if (key.empty()) return;
    if (dip < 48.0f) g_widths.erase(key);
    else g_widths[key] = std::min(dip, 480.0f);
    g_widths_dirty = true;
}

uint64_t ExtensionColumnsRevision() { return cols::Registry::Instance().Revision(); }

std::vector<ui::FluentMenuItem> BuildExtensionColumnMenuRows(const std::vector<std::wstring>& hidden,
                                                             const std::vector<std::wstring>& shown_keys) {
    std::vector<ui::FluentMenuItem> rows;
    const auto providers = AllProviders();
    int index = 0;
    for (const auto& info : providers) {
        const std::wstring key = info.Key();
        ui::FluentMenuItem item;
        item.command = kExtensionColumnToggleBase + index++;
        item.text = info.title;
        item.detail = PackageText(info.kind == cols::Kind::Column ? l10n::StringId::ExtensionColumnFrom
                                                                  : l10n::StringId::ExtensionBadgeFrom,
                                  info.package_id);
        item.checked = !Hidden(hidden, key);
        if (item.checked) item.glyph = L"\xE73E";   // CheckMark: TrackPopup paints checks as glyphs
        if (item.checked && info.kind == cols::Kind::Column &&
            std::find(shown_keys.begin(), shown_keys.end(), key) == shown_keys.end()) {
            item.badge_text = l10n::Get(l10n::StringId::DetailsColumnNoRoom);
            item.tooltip = l10n::Get(l10n::StringId::DetailsColumnNoRoomTip);
        }
        rows.push_back(std::move(item));
    }
    return rows;
}

bool ApplyExtensionColumnCommand(std::vector<std::wstring>& hidden, int command) {
    const int index = command - kExtensionColumnToggleBase;
    if (index < 0 || index >= 100) return false;
    const auto providers = AllProviders();
    if (static_cast<size_t>(index) >= providers.size()) return false;
    const std::wstring key = providers[static_cast<size_t>(index)].Key();
    const auto it = std::find(hidden.begin(), hidden.end(), key);
    if (it == hidden.end()) {
        hidden.push_back(key);
        if (hidden.size() > 128) hidden.erase(hidden.begin());
    } else {
        hidden.erase(it);
    }
    return true;
}

} // namespace pulse::app

namespace pulse {

void SaveExtensionColumnWidths(AppState& s) {
    {
        std::lock_guard lock(app::g_mutex);
        if (!app::g_widths_dirty) return;
        app::g_widths_dirty = false;
    }
    s.settings.ExtensionColumns(s.appPrefs.extension_columns_hidden, app::ExtensionColumnWidthPrefs());
}

} // namespace pulse
