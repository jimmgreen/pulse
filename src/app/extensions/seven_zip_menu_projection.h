#pragma once

#include "manifest.h"
#include "../../../extensions/7zip/command_preferences.h"
#include "../../../extensions/7zip/text.h"
#include <algorithm>

namespace pulse::extensions::seven_zip_menu {

inline bool ApplyPreferences(ManifestCommandContribution& command, const seven_zip::CommandPreferences& prefs) {
    const auto it = std::find(seven_zip::kMenuCommands.begin(), seven_zip::kMenuCommands.end(), command.id);
    if (it == seven_zip::kMenuCommands.end()) return true;
    // Which commands show is the host's 右键菜单 tab now (ContextMenuPrefs); the plugin's old
    // per-command bits are carried over once (MigrateSevenZipHiddenCommands) and otherwise unused.
    command.submenu = prefs.submenu;

    using T = ::text::Id;
    using L = ::text::MenuLanguage;
    const auto localize = [](const auto& label) {
        return ManifestLocalizedText{label(L::ZhCN), label(L::EnUS), label(L::ZhTW)};
    };
    const auto replace = [](std::string& value, std::string_view key, std::string_view replacement) {
        if (const auto at = value.find(key); at != value.npos) value.replace(at, key.size(), replacement);
    };
    if (command.id == "extract") {
        const bool fixed = prefs.location == 1 && !prefs.folder.empty();
        const auto title = fixed ? T::CmdFixed : prefs.mode == 1 ? T::CmdFolder : prefs.mode == 2 ? T::CmdHere : T::CmdSmart;
        const auto hint = fixed ? (prefs.mode == 2 ? T::HintFixedHere : T::HintFixed)
            : prefs.mode == 1 ? T::HintFolder : prefs.mode == 2 ? T::HintHere : T::HintSmart;
        command.title = localize([&](L language) { return ::text::Get(title, language); });
        command.detail = localize([&](L language) {
            std::string line = ::text::Get(hint, language);
            if (fixed) replace(line, "{name}", prefs.folder);
            return line.size() <= 160 ? line : std::string{};
        });
    } else if (command.id == "quick-compress") {
        constexpr std::string_view formats[]{"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
        constexpr T levels[]{T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9};
        const auto format = formats[prefs.format];
        command.title = localize([&](L language) {
            std::string line = ::text::Get(T::CmdQuick, language);
            replace(line, "{name}", format);
            return line;
        });
        command.detail = localize([&](L language) {
            std::string line = ::text::Get(prefs.format == 2 ? T::HintTar : T::HintQuick, language);
            replace(line, "{name}", "." + std::string(format));
            replace(line, "{count}", ::text::Get(levels[prefs.level], language));
            return line;
        });
    }
    return true;
}

} // namespace pulse::extensions::seven_zip_menu