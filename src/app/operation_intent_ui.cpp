#include "operation_intent_ui.h"
#include "app_navigation.h"
#include "create_rename_intent.h"
#include "app_details_tree.h"
#include <algorithm>
#include <shlwapi.h>

namespace pulse {
static void ApplyWindowOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed) {
    // 合并到文件夹 arms its rename on the move into the new folder, so the
    // editor opens once the grouped items have left the listing.
    const bool grouped_move = completed.type == ops::OpType::Move && !completed.destinations.empty();
    if (completed.refresh_only || !completed.task_id ||
        (completed.type != ops::OpType::CreateFolder && completed.type != ops::OpType::CreateTextFile &&
         !grouped_move)) return;
    std::vector<std::wstring> created = completed.destinations;
    if (grouped_move) {
        const std::wstring& first = completed.destinations.front();
        const size_t slash = first.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        created = {first.substr(0, slash)};
    }
    std::vector<std::wstring> parents;
    std::vector<std::wstring> tree_parents;
    ForEachPane(state, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        for (const auto& destination : created) {
            if (!app::CompleteCreateRenameIntent(tab->create_rename_intents, completed.task_id,
                    tab->current_path, tab->view_generation, destination)) continue;
            const auto intent = std::find_if(tab->create_rename_intents.begin(), tab->create_rename_intents.end(),
                [&](const auto& value) { return value.task_id == completed.task_id; });
            if (intent != tab->create_rename_intents.end() && !intent->target_parent.empty() && intent->target_parent != intent->parent) {
                if (std::find(tree_parents.begin(), tree_parents.end(), intent->target_parent) == tree_parents.end())
                    tree_parents.push_back(intent->target_parent);
            } else if (std::find(parents.begin(), parents.end(), tab->current_path) == parents.end()) {
                parents.push_back(tab->current_path);
            }
        }
    });
    // A watcher may already have delivered the new item while its owner was
    // unfocused. Request a fresh identity-checked snapshot for that owner too.
    for (const auto& parent : parents) RefreshPath(state, parent, RefreshReason::OperationCompleted);
    for (const auto& parent : tree_parents) NoteDetailsTreeChange(state, fs::NormalizePath(parent));
    if (!tree_parents.empty() && state.hwnd) InvalidateRect(state.hwnd, nullptr, FALSE);
}

static void FinishWindowOperationIntents(AppState& state, const ops::FinishedTask& finished) {
    state.tray.CompleteTask(finished.task_id, finished.moved_sources);
    // 解散文件夹: the chain renames, moves and recycles under one listing; its
    // last step lists the parent afresh rather than trusting the patched snapshot.
    for (auto it = state.dissolveSteps.begin(); it != state.dissolveSteps.end();) {
        if (it->task_id != finished.task_id) {
            ++it;
            continue;
        }
        RefreshPath(state, it->parent, RefreshReason::OperationCompleted);
        it = state.dissolveSteps.erase(it);
    }
    ForEachPane(state, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab())
            app::FinishCreateRenameIntent(tab->create_rename_intents, finished.task_id);
    });
}
void ApplyOperationIntentCompletion(AppState& state, const ops::CompletedOperation& completed) {
    app::ForEachWindowSharingCore(state, [&](AppState& window) {
        ApplyWindowOperationIntentCompletion(window, completed);
    });
}
void FinishOperationIntents(AppState& state, const ops::FinishedTask& finished) {
    app::ForEachWindowSharingCore(state, [&](AppState& window) {
        FinishWindowOperationIntents(window, finished);
    });
}

}
