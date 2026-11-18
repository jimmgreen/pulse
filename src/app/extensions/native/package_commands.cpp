#include "package_commands.h"
#include "../runtime.h"
#include "session.h"
#include "../../actions/action_registry.h"
#include "../../../common/localization.h"
#include "crash_guard.h"
#include <windows.h>
#include <limits>

namespace pulse::extensions::native {
namespace {
constexpr int kDevelopmentCommandBase = 0x40000000;
int CurrentCommandBase(size_t package) {
    if (!DevelopmentMode() && !PackageStoreMode()) return package == 0 ? kCommandBase : 0;
    const uint64_t generation = PackageGeneration(package);
    if (generation > static_cast<uint64_t>((std::numeric_limits<int>::max() - kDevelopmentCommandBase - 63) / 64)) return 0;
    return kDevelopmentCommandBase + static_cast<int>(generation * 64);
}
std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size);
    return result;
}
bool Matches(std::wstring_view query, const std::wstring& title, std::string_view id) {
    if (query.empty()) return true;
    app::actions::ActionView view;
    view.title = title;
    view.id = id;
    return app::actions::ActionMatchTier(view, query) > 0;
}
}

std::vector<ui::FluentMenuItem> PackageEntries(std::wstring_view query) {
    std::vector<ui::FluentMenuItem> rows;
    for (size_t package = 0; package < PackageCount(); ++package) {
        auto contributed = EntriesForPackage(package, query);
        rows.insert(rows.end(), std::make_move_iterator(contributed.begin()), std::make_move_iterator(contributed.end()));
    }
    return rows;
}
std::vector<ui::FluentMenuItem> EntriesForPackage(size_t package, std::wstring_view query) {
    std::vector<ui::FluentMenuItem> rows;
    const Manifest* manifest = PackageManifest(package);
    if (!manifest) return rows;
    const int command_base = CurrentCommandBase(package);
    if (!command_base) return rows;
    if (!manifest->native) return rows;
    if (PackageEnabled(package)) {
        ui::FluentMenuItem open;
        open.text = Wide(manifest->id) + L" · " + Wide(manifest->name);
        open.shortcut = l10n::Get(l10n::StringId::Open);
        open.tooltip = open.text;
        open.command = command_base + (kPanelCommand - kCommandBase);
        if (Matches(query, open.text, manifest->id)) rows.push_back(std::move(open));
    }
    ui::FluentMenuItem toggle;
    toggle.text = Wide(manifest->id) + L" · " + Wide(manifest->name);
    toggle.shortcut = l10n::Get(PackageEnabled(package) ? l10n::StringId::ExtensionPackageDisable
        : l10n::StringId::ExtensionPackageEnable);
    toggle.tooltip = toggle.text;
    toggle.command = command_base + (kToggleCommand - kCommandBase);
    if (Matches(query, toggle.text, manifest->id)) rows.push_back(std::move(toggle));
    if (!rows.empty()) rows.back().separator_after = true;
    return rows;
}

bool HandlePackageCommand(AppState& state, int command) {
    size_t package = 0;
    if (command >= kDevelopmentCommandBase) {
        if (!DevelopmentMode() && !PackageStoreMode()) return true;
        bool found = false;
        for (; package < PackageCount(); ++package) {
            const int base = CurrentCommandBase(package);
            if (!base || command < base || command > base + (kPanelCommand - kCommandBase)) continue;
            command = kCommandBase + (command - base);
            found = true;
            break;
        }
        if (!found) return true;
    } else if ((DevelopmentMode() || PackageStoreMode()) && command >= kCommandBase && command <= kPanelCommand) {
        return true;
    }
    if (command < kCommandBase || command > kPanelCommand) return false;
    const Manifest* manifest = PackageManifest(package);
    if (!manifest || !manifest->native) return true;
    if (command == kPanelCommand) {
        if (PackageEnabled(package)) StartPackage(state, package);
        return true;
    }
    if (command == kToggleCommand) {
        SetPackageEnabled(package, !PackageEnabled(package));
        if (!PackageEnabled(package)) StopPackage(package);
        // Turned back on by the user: this build is trusted again.
        else crash_guard::ForgetPackageCrash(package);
        return true;
    }

    return true;
}
}
