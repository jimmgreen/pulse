// locked_item_prompt.h — "File in use" prompt after a delete/move/copy failed
// because other processes hold the item (B站 #12). The owners were resolved
// on the ops worker; this only shows them and queues the chosen retry.
#pragma once
#include <string>
#include "../ops/ops_manager.h"
#include "../ui/confirm_dialog.h"

namespace pulse {
struct AppState;

// The prompt: item name, one row per owning process, and the note that
// matches what the confirm button will do.
ui::ConfirmDialogSpec LockedItemPromptSpec(const ops::OpStatus& status, bool can_end);
// True when every owner may be ended (otherwise the prompt only offers retry).
bool LockedItemCanEndAny(const ops::OpStatus& status);
// Modal; call from the UI thread outside paint. Never blocks on the owners:
// ending them and retrying run on the ops worker.
void PromptLockedItem(AppState& s, const ops::OpStatus& status);
}
