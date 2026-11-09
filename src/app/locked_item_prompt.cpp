// locked_item_prompt.cpp — see locked_item_prompt.h.
#include "locked_item_prompt.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"
#include <algorithm>

namespace pulse {

bool LockedItemOwnersClosable(const ops::OpStatus& status) {
    return !status.lock_owners.empty() &&
        std::all_of(status.lock_owners.begin(), status.lock_owners.end(),
                    [](const ops::LockOwner& owner) { return owner.closable; });
}

ui::ConfirmDialogSpec LockedItemPromptSpec(const ops::OpStatus& status, bool can_end) {
    std::wstring name = status.locked_path;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < name.size()) name.erase(0, slash + 1);
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::LockedItemTitle);
    spec.message = l10n::Get(l10n::StringId::LockedItemMessage);
    const size_t marker = spec.message.find(L"{name}");
    if (marker != std::wstring::npos) spec.message.replace(marker, 6, name);
    // One card row per owner; the dialog folds more than five into "N more".
    for (const ops::LockOwner& owner : status.lock_owners)
        spec.items.push_back(ops::DescribeLockOwner(owner));
    spec.item_glyph = L"\xECAA";   // AppIconDefault
    spec.note = l10n::Get(can_end ? l10n::StringId::LockedItemEndHint
                                  : l10n::StringId::LockedItemCloseHint);
    spec.confirm_text = l10n::Get(can_end ? l10n::StringId::LockedItemEndRetry
                                          : l10n::StringId::LockedItemRetry);
    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    spec.danger = can_end;
    spec.tone = can_end ? ui::ConfirmTone::Danger : ui::ConfirmTone::Warning;
    return spec;
}

// The dialog watches the owners and the item and comes back with Retry on its
// own once the item is free.
void PromptLockedItem(AppState& s, const ops::OpStatus& status) {
    ui::LockedItemInfo info;
    info.type = status.type;
    info.path = status.locked_path;
    info.owners = status.lock_owners;
    info.can_end = LockedItemOwnersClosable(status);
    info.can_skip = s.ops.CanSkipLockedItem(status.task_id);
    switch (ui::ShowLockedItemDialog(s.hwnd, info, s.darkMode, s.accentColor)) {
    case ui::LockedItemChoice::Retry: s.ops.RetryLockedOperation(status.task_id, false); break;
    case ui::LockedItemChoice::EndAndRetry: s.ops.RetryLockedOperation(status.task_id, true); break;
    case ui::LockedItemChoice::Skip: s.ops.RetryLockedOperation(status.task_id, false, true); break;
    case ui::LockedItemChoice::Cancel: break;
    }
}

} // namespace pulse
