#include "extension_store_seven_zip.h"
#include "app_state.h"
#include "extensions/runtime.h"
#include "extensions/native/session.h"
#include "extensions/native/settings_pages_adapter.h"
#include "extensions/native/commands_adapter.h"
#include "../common/localization.h"
#include "../common/json_value.h"
#include "../ui/extension_store_view.h"
#include <cstdlib>

namespace pulse {
namespace {
using Id = l10n::StringId;
// The running, enabled 7-Zip package, or -1.
int Package() {
    for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p)
        if (const auto* manifest = extensions::PackageManifest(p);
            manifest && manifest->id == kSevenZipExtensionId && manifest->native && extensions::PackageEnabled(p))
            return static_cast<int>(p);
    return -1;
}
extensions::native::SettingsPagesAdapter* Page(AppState& s) {
    const int package = Package();
    if (package < 0) return nullptr;
    auto* pages = extensions::native::PackageSettings(s.hwnd, static_cast<size_t>(package));
    return pages && pages->Page() ? pages : nullptr;
}
int Digit(const std::string& value, int max, int fallback) {
    return value.size() == 1 && value[0] >= '0' && value[0] <= '0' + max ? value[0] - '0' : fallback;
}
std::vector<std::wstring> Split(const std::string& list) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < list.size()) {
        const auto comma = list.find(',', start);
        const auto part = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) out.push_back(json::Wide(part));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}
std::vector<ui::SevenZipMenuRow> Rows(HWND owner, const std::vector<extensions::native::CommandsAdapter::PreviewEntry>& entries) {
    std::vector<ui::SevenZipMenuRow> rows;
    const auto directory = json::Utf8(l10n::Get(Id::SzSampleParent));
    for (const auto& info : extensions::native::CommandsAdapter::Preview(owner, kSevenZipExtensionId, entries, directory))
        rows.push_back({json::Wide(info.title), json::Wide(info.detail), info.icon, ui::StoreCommandIconFor(info.icon, info.identity)});
    return rows;
}
}

void FillSevenZipView(AppState& s, ui::ExtensionStoreView& view) {
    auto& z = view.seven_zip;
    z = {};
    for (size_t i = 0; i < view.items.size(); ++i)
        if (view.items[i].id == kSevenZipExtensionId) { z.item = static_cast<int>(i); z.installed = view.items[i].settings_package >= 0; }
    for (size_t i = 0; i < view.installed.size(); ++i)
        if (view.installed[i].id == json::Wide(kSevenZipExtensionId)) { z.installed_index = static_cast<int>(i); z.installed = true; }
    const bool enabled_on_disk = z.installed_index >= 0 && view.installed[z.installed_index].enabled;
    const int package = Package();
    if (package < 0) {
        if (enabled_on_disk || (z.installed && view.pending_restart)) z.needs_restart = true;
        return;
    }
    if (!enabled_on_disk) z.needs_restart = true;
    // Starts the package lazily when its store/settings surface is opened; no-op once running.
    extensions::native::EnsureCommandPackageStarted(s, static_cast<size_t>(package));
    auto* page = Page(s);
    if (!page) {
        if (extensions::native::PackageSessionLoaded(s.hwnd, static_cast<size_t>(package))) z.needs_update = true;
        return;
    }
    const auto get = [&](const char* key) { const auto value = page->State(key); return value ? *value : std::string{}; };
    if (get("ready") != "true") return;  // its settings are still loading
    z.available = true;
    z.menu_available = page->State("placement0").has_value() && page->State("placement1").has_value();
    for (int i = 0; i < 6; ++i)
        z.menu_available = z.menu_available && page->State("cmd" + std::to_string(i)).has_value();
    z.mode = Digit(get("mode"), 2, 0);
    z.location = Digit(get("location"), 1, 0);
    z.format = Digit(get("format"), 5, 0);
    z.level = Digit(get("level"), 5, 3);
    z.remember = get("remember") != "false";
    // Read the same radio/switch states the plugin refreshes after every edit.
    // Its serialized aggregate fields are only published when the page loads.
    z.submenu = get("placement1") == "true";
    z.commands = 0;
    for (int i = 0; i < 6; ++i)
        if (get(("cmd" + std::to_string(i)).c_str()) == "true") z.commands |= 1u << i;
    z.folder = json::Wide(get("folder"));
    z.status = json::Wide(get("status"));
    z.formats_rw = Split(get("formats_rw"));
    z.formats_ro = Split(get("formats_ro"));
    z.formats_available = !z.formats_rw.empty() || !z.formats_ro.empty();
    // 接管压缩包: the extension resolves who opens each format; this only shows it.
    // Its page packs the section into takecfg / takeview / takefmt (extensions/7zip/settings.cpp).
    const auto config = get("takecfg"), view_state = get("takeview"), formats = get("takefmt");
    z.take_available = config.size() == 7 && !formats.empty();
    if (z.take_available) {
        z.take = config[0] == '1';
        z.double_click = config[2] == '1' ? 1 : 0;
        const unsigned groups = static_cast<unsigned>(std::strtoul(config.substr(4, 3).c_str(), nullptr, 16));
        const auto lines = [](const std::string& value) {
            std::vector<std::string> out;
            size_t start = 0;
            for (;;) {
                const auto end = value.find('\n', start);
                out.push_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos) return out;
                start = end + 1;
            }
        };
        const auto format_lines = lines(formats), view_lines = lines(view_state);
        z.icon_library = json::Wide(format_lines.front());
        z.take_busy = !view_lines.empty() && view_lines.front() == "1";
        const auto fill = [](std::wstring text, std::wstring_view key, const std::wstring& value) {
            if (const auto at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
            return text;
        };
        int ours = 0, asks = 0;
        using Owner = ui::SevenZipTakeRow::Owner;
        for (size_t i = 1; i < format_lines.size() && i <= 16; ++i) {
            const auto& line = format_lines[i];
            const auto first = line.find('|'), second = line.find('|', first == std::string::npos ? first : first + 1);
            if (first == std::string::npos || second == std::string::npos) break;
            ui::SevenZipTakeRow row;
            row.label = json::Wide(line.substr(0, first));
            row.extensions = json::Wide(line.substr(first + 1, second - first - 1));
            row.icon = std::atoi(line.substr(second + 1).c_str());
            row.included = (groups >> (i - 1)) & 1;
            std::wstring who;
            if (i < view_lines.size() && view_lines[i].size() >= 2) {
                const auto& state = view_lines[i];
                row.owner = state[0] == 'p' ? Owner::Pulse : state[0] == 'o' ? Owner::Other : state[0] == 'f' ? Owner::Free : Owner::Off;
                row.ask_windows = state[1] == 'w';
                who = json::Wide(state.substr(2));
            }
            switch (row.owner) {
            case Owner::Pulse: row.status = l10n::Get(z.double_click ? Id::SzTakeStatusOpen : Id::SzTakeStatusExtract); ++ours; break;
            case Owner::Other: row.status = fill(l10n::Get(Id::SzTakeStatusOther), L"{name}", who); ++asks; break;
            case Owner::Free: row.status = l10n::Get(Id::SzTakeStatusFree); break;
            default:
                row.status = !z.take ? (who.empty() ? l10n::Get(Id::SzTakeStatusOff) : fill(l10n::Get(Id::SzTakeStatusOffWho), L"{name}", who))
                                     : (who.empty() ? l10n::Get(Id::SzTakeStatusExcluded) : fill(l10n::Get(Id::SzTakeStatusExcludedWho), L"{name}", who));
                break;
            }
            row.ask = z.take && !z.take_busy && row.owner == Owner::Other;
            z.take_rows.push_back(std::move(row));
        }
        z.ask_any = z.take && !z.take_busy && asks > 0;
        z.ask_many = z.ask_any && asks > 2;
        z.take_head = !z.take ? l10n::Get(Id::SzTakeOffHint)
            : asks ? fill(fill(l10n::Get(Id::SzTakeSummaryAsk), L"{count}", std::to_wstring(ours)), L"{name}", std::to_wstring(asks))
                   : fill(l10n::Get(Id::SzTakeSummaryDone), L"{count}", std::to_wstring(ours));
        z.take_config = config;
    }
    z.archive_rows = Rows(s.hwnd, {{json::Utf8(l10n::Get(Id::SzSampleArchive)), false}});
    z.file_rows = Rows(s.hwnd, {{json::Utf8(l10n::Get(Id::SzSampleFolder)), true}, {json::Utf8(l10n::Get(Id::SzSampleFile)), false}});
}

bool SetSevenZipState(AppState& s, const std::string& key, const std::string& value) {
    auto* page = Page(s);
    return page && page->CommitState(key, value);
}

bool BrowseSevenZipFolder(AppState& s) {
    auto* page = Page(s);
    return page && page->Click("browse");
}

bool ClickSevenZip(AppState& s, const std::string& command) {
    auto* page = Page(s);
    return page && page->Click(command);
}
}
