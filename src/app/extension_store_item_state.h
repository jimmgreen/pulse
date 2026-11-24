#pragma once
#include "../ui/extension_store_view.h"
#include "../common/localization.h"

namespace pulse::app {
// A local unlisted revision must not be offered a catalog downgrade.
inline void ApplyInstalledStoreItem(ui::ExtensionStoreItem& item, bool known_revision,
                                    bool latest_revision, const std::wstring& installed_version) {
    using Id = l10n::StringId;
    // An unlisted local build is not evidence of an older version.
    // Do not offer the public catalog's older package as its "update".
    if (!known_revision || latest_revision) { item.button_label = l10n::Get(Id::Installed); item.action_enabled = false; }
    else item.button_label = l10n::Get(Id::StoreUpdateItem);
    // What is installed is what the card names: a local build shows itself, not the catalog's release.
    if (!known_revision) { item.version = installed_version; item.size.clear(); }
}
}
