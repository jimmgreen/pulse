#include "tree_transfer_destination.h"
#include "../common/localization.h"

namespace pulse {
ui::ConfirmDialogSpec BuildTreeTransferConfirmation(const app::TreeTransferDestinations& targets) {
    using I = l10n::StringId;
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(I::TreeTransferCopyAllTitle);
    confirm.message = l10n::Get(I::TreeTransferCopyAllMessage);
    const auto at = confirm.message.find(L"{n}");
    if (at != std::wstring::npos) confirm.message.replace(at, 3, std::to_wstring(targets.folders.size()));
    confirm.items = targets.folders;
    confirm.confirm_text = l10n::Get(I::TreeTransferCopyAllAction);
    confirm.note = l10n::Get(I::TreeTransferCopyAllRetain);
    confirm.default_choice = ui::ConfirmChoice::Cancel;
    return confirm;
}
}
