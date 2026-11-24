#include "tree_transfer_destination.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <filesystem>

namespace pulse::app {
namespace {
std::wstring Key(const std::wstring& value) {
    auto key = std::filesystem::path(path::StripExtendedPathPrefix(value)).lexically_normal().wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return path::FoldedKey(key);
}
bool Within(const std::wstring& target, const std::wstring& source) {
    const auto t = Key(target), s = Key(source);
    return t == s || (!s.empty() && t.starts_with(s) &&
        (s.back() == L'\\' || (t.size() > s.size() && t[s.size()] == L'\\')));
}
}
TreeTransferDestinations ResolveTreeTransferDestinations(bool tree, const std::wstring& current,
                                                        const std::vector<TreeDestinationRow>& selected) {
    TreeTransferDestinations result;
    if (tree) for (const auto& row : selected) {
        if (!row.folder || row.placeholder) continue;
        if (row.path.empty()) { result.invalid = true; continue; }
        if (std::none_of(result.folders.begin(), result.folders.end(), [&](const auto& p) { return Key(p) == Key(row.path); }))
            result.folders.push_back(row.path);
    }
    if (result.folders.empty() && !result.invalid) result.folders.push_back(current);
    return result;
}
TreeTransferDestinations ResolveClipboardTransferDestinations(const std::wstring& current,
    const std::vector<std::wstring>& sources, const TreeTransferDestinations& selected) {
    // Clipboard sources can remain selected after Ctrl+C/X. They are not an
    // explicit destination; resolve against the visible directory instead.
    // Mixed source/other destinations still go through the atomic safety check.
    if (!selected.invalid && !selected.folders.empty() &&
        std::all_of(selected.folders.begin(), selected.folders.end(), [&](const auto& target) {
            return !target.empty() && std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
                return !source.empty() && Key(target) == Key(source);
            });
        })) return {{current}, false};
    return selected;
}
TreeTransferPlan PlanTreeTransfer(const std::vector<std::wstring>& sources,
                                  const TreeTransferDestinations& targets, bool move,
                                  bool confirmed, uint64_t undo_group) {
    TreeTransferPlan plan;
    if (targets.invalid) { plan.invalid_target = L"?"; return plan; }
    if (sources.empty() || targets.folders.empty() || (targets.folders.size() > 1 && !confirmed)) return plan;
    // Validate every pair before creating any request. The worker still validates
    // filesystem identity/access and handles conflicts; the UI never enumerates.
    for (const auto& dest : targets.folders) {
        if (dest.empty()) { plan.invalid_target = L"?"; return plan; }
        for (const auto& source : sources)
            if (!source.empty() && Within(dest, source)) { plan.invalid_target = dest; return plan; }
    }
    for (const auto& dest : targets.folders) {
        ops::OpRequest request;
        request.type = move && targets.folders.size() == 1 ? ops::OpType::Move : ops::OpType::Copy;
        request.sources = sources;
        request.dest_dir = dest;
        request.undo_group = targets.folders.size() > 1 ? undo_group : 0;
        plan.requests.push_back(std::move(request));
    }
    return plan;
}
size_t SubmitTreeTransferPlan(TreeTransferPlan plan, const std::function<bool()>& confirm,
                             const std::function<bool(ops::OpRequest)>& submit) {
    if (!plan.invalid_target.empty() || plan.requests.empty()) return 0;
    if (plan.requests.size() > 1 && !confirm()) return 0;
    size_t submitted = 0;
    for (auto& request : plan.requests) if (submit(std::move(request))) ++submitted;
    return submitted;
}

}
