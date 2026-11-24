// seven_zip_store_probe.h - TEST-ONLY probe used by the in-process store flow tests
// (extension_store_flow_test.cpp, extension_store_operations_test.cpp). It reads and
// commits the real 7-Zip plugin's own settings-page states, exactly as the plugin's
// SDK settings tab does. Production code has no 7-Zip store/settings surface: the
// store shows 7-Zip through the generic quick-settings card and detail page.
#pragma once
#include "app_state.h"
#include "extensions/runtime.h"
#include "extensions/native/session.h"
#include "extensions/native/settings_pages_adapter.h"
#include "extensions/native/commands_adapter.h"
#include "../common/localization.h"
#include "../common/json_value.h"
#include "../ui/extension_store_view.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::store_probe {
inline constexpr char kSevenZipId[] = "pulse.7zip";

// The running, enabled 7-Zip package, or -1.
inline int SevenZipPackage() {
    for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p)
        if (const auto* manifest = extensions::PackageManifest(p);
            manifest && manifest->id == kSevenZipId && manifest->native && extensions::PackageEnabled(p))
            return static_cast<int>(p);
    return -1;
}
inline extensions::native::SettingsPagesAdapter* SevenZipPage(AppState& s) {
    const int package = SevenZipPackage();
    if (package < 0) return nullptr;
    auto* pages = extensions::native::PackageSettings(s.hwnd, static_cast<size_t>(package));
    return pages && pages->Page() ? pages : nullptr;
}
// One user edit on the plugin's own settings page (the plugin validates and saves it).
inline bool SetSevenZipState(AppState& s, const std::string& key, const std::string& value) {
    auto* page = SevenZipPage(s);
    return page && page->CommitState(key, value);
}

struct SevenZipState {
    bool available = false, menu_available = false, formats_available = false;
    int item = -1, installed_index = -1;
    int mode = 0, location = 0, format = 0, level = 3;
    bool remember = true, submenu = false;
    uint32_t commands = 0x3f;
    std::wstring folder, status;
    std::vector<std::wstring> formats_rw, formats_ro;
    std::vector<ui::SevenZipMenuRow> archive_rows, file_rows; // live menus for sample selections
};

inline std::vector<std::wstring> SplitCsv(const std::string& list) {
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
inline std::vector<ui::SevenZipMenuRow> PreviewRows(
    HWND owner, const std::vector<extensions::native::CommandsAdapter::PreviewEntry>& entries) {
    std::vector<ui::SevenZipMenuRow> rows;
    const auto directory = json::Utf8(l10n::Get(l10n::StringId::SzSampleParent));
    for (const auto& info : extensions::native::CommandsAdapter::Preview(owner, kSevenZipId, entries, directory))
        rows.push_back({json::Wide(info.title), json::Wide(info.detail), info.icon, ui::StoreCommandIconFor(info.icon, info.identity)});
    return rows;
}

inline SevenZipState ReadSevenZip(AppState& s, const ui::ExtensionStoreView& view) {
    SevenZipState z;
    for (size_t i = 0; i < view.items.size(); ++i)
        if (view.items[i].id == kSevenZipId) z.item = static_cast<int>(i);
    for (size_t i = 0; i < view.installed.size(); ++i)
        if (view.installed[i].id == json::Wide(kSevenZipId)) z.installed_index = static_cast<int>(i);
    const int package = SevenZipPackage();
    if (package < 0) return z;
    extensions::native::EnsureCommandPackageStarted(s, static_cast<size_t>(package));
    auto* page = SevenZipPage(s);
    if (!page) return z;
    const auto get = [&](const std::string& key) { const auto value = page->State(key); return value ? *value : std::string{}; };
    const auto digit = [](const std::string& value, int max, int fallback) {
        return value.size() == 1 && value[0] >= '0' && value[0] <= '0' + max ? value[0] - '0' : fallback;
    };
    if (get("ready") != "true") return z;
    z.available = true;
    z.menu_available = page->State("placement0").has_value() && page->State("placement1").has_value();
    for (int i = 0; i < 6; ++i) z.menu_available = z.menu_available && page->State("cmd" + std::to_string(i)).has_value();
    z.mode = digit(get("mode"), 2, 0);
    z.location = digit(get("location"), 1, 0);
    z.format = digit(get("format"), 5, 0);
    z.level = digit(get("level"), 5, 3);
    z.remember = get("remember") != "false";
    z.submenu = get("placement1") == "true";
    z.commands = 0;
    for (int i = 0; i < 6; ++i)
        if (get("cmd" + std::to_string(i)) == "true") z.commands |= 1u << i;
    z.folder = json::Wide(get("folder"));
    z.status = json::Wide(get("status"));
    z.formats_rw = SplitCsv(get("formats_rw"));
    z.formats_ro = SplitCsv(get("formats_ro"));
    z.formats_available = !z.formats_rw.empty() || !z.formats_ro.empty();
    using Id = l10n::StringId;
    z.archive_rows = PreviewRows(s.hwnd, {{json::Utf8(l10n::Get(Id::SzSampleArchive)), false}});
    z.file_rows = PreviewRows(s.hwnd, {{json::Utf8(l10n::Get(Id::SzSampleFolder)), true}, {json::Utf8(l10n::Get(Id::SzSampleFile)), false}});
    return z;
}
} // namespace pulse::store_probe
