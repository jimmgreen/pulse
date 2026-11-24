#pragma once
#include "../ops/ops_manager.h"
#include <string>
#include <functional>
#include "../ui/confirm_dialog.h"
#include <vector>

namespace pulse { struct AppState; namespace app { struct Tab; struct Pane; } }
namespace pulse::app {
struct TreeDestinationRow {
    std::wstring path;
    bool folder = false;
    bool placeholder = false;
};
struct TreeTransferDestinations {
    std::vector<std::wstring> folders;
    bool invalid = false;
};
TreeTransferDestinations ResolveTreeTransferDestinations(bool tree, const std::wstring& current,
                                                        const std::vector<TreeDestinationRow>& selected);
TreeTransferDestinations ResolveClipboardTransferDestinations(const std::wstring& current,
    const std::vector<std::wstring>& sources, const TreeTransferDestinations& selected);
struct TreeTransferPlan {
    std::vector<ops::OpRequest> requests;
    std::wstring invalid_target;
};
// Pure planning: multiple destinations always mean Copy, never repeated Move.
TreeTransferPlan PlanTreeTransfer(const std::vector<std::wstring>& sources,
                                  const TreeTransferDestinations& targets, bool move,
                                  bool confirmed, uint64_t undo_group);
size_t SubmitTreeTransferPlan(TreeTransferPlan plan, const std::function<bool()>& confirm,
                             const std::function<bool(ops::OpRequest)>& submit);
}
namespace pulse {
ui::ConfirmDialogSpec BuildTreeTransferConfirmation(const app::TreeTransferDestinations& targets);
app::TreeTransferDestinations TreeTransferDestinationsForTab(const app::Tab& tab);
bool SubmitTreeTransfer(AppState& state, const std::vector<std::wstring>& sources,
                        const app::TreeTransferDestinations& targets, bool move,
                        app::Pane* pane = nullptr, bool* moved = nullptr);
}
