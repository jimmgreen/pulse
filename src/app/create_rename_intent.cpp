#include "create_rename_intent.h"
#include "../common/path_utils.h"
#include <algorithm>

namespace pulse::app {
namespace {
std::wstring Canonical(std::wstring_view value) {
    auto result = path::StripExtendedPathPrefix(value);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
bool Same(std::wstring_view a, std::wstring_view b) {
    auto ordinary = [](std::wstring_view value) {
        if (value.starts_with(L"\\\\?\\") && !value.starts_with(L"\\\\?\\UNC\\")) value.remove_prefix(4);
        while (value.size() > 3 && value.back() == L'\\') value.remove_suffix(1);
        return value;
    };
    if (path::EqualInsensitive(ordinary(a), ordinary(b))) return true;
    return path::EqualInsensitive(Canonical(a), Canonical(b));
}
void Prune(std::vector<CreateRenameIntent>& intents, std::wstring_view parent, uint64_t generation) {
    std::erase_if(intents, [&](const auto& intent) {
        return intent.view_generation != generation || !Same(intent.parent, parent);
    });
}
}
void QueueCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id,
    std::wstring_view parent, uint64_t view_generation, std::wstring_view target_parent) {
    Prune(intents, parent, view_generation);
    if (!task_id || parent.empty()) return;
    intents.push_back({task_id, Canonical(parent), view_generation, {}, false,
        Canonical(target_parent.empty() ? parent : target_parent)});
}
bool CompleteCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id,
    std::wstring_view current_parent, uint64_t view_generation, std::wstring_view actual_created_path) {
    Prune(intents, current_parent, view_generation);
    const auto found = std::find_if(intents.begin(), intents.end(), [&](const auto& value) { return value.task_id == task_id; });
    if (found == intents.end()) return false;
    auto actual = Canonical(actual_created_path);
    const auto separator = actual.find_last_of(L'\\');
    const auto parent = separator == 2 ? actual.substr(0, 3) : actual.substr(0, separator);
    if (separator == std::wstring::npos || separator + 1 == actual.size() ||
        !Same(parent, found->target_parent.empty() ? found->parent : found->target_parent)) {
        intents.erase(found);
        return false;
    }
    found->actual_path = std::move(actual);
    found->ready = true;
    return true;
}
void FinishCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id) {
    std::erase_if(intents, [&](const auto& value) { return value.task_id == task_id && !value.ready; });
}
std::optional<CreateRenameIntent> PendingCreateRenameIntent(std::vector<CreateRenameIntent>& intents,
    std::wstring_view current_parent, uint64_t view_generation) {
    Prune(intents, current_parent, view_generation);
    const auto found = std::find_if(intents.begin(), intents.end(), [](const auto& value) { return value.ready; });
    if (found == intents.end()) return std::nullopt;
    return *found;
}
bool CreateRenameEntryMatches(const CreateRenameIntent& intent, std::wstring_view entry_name,
    std::wstring_view entry_full_path) {
    if (!intent.ready || entry_name.empty() || entry_name == L"." || entry_name == L".." ||
        entry_name.find_first_of(L"\\/") != std::wstring_view::npos) return false;
    const auto leaf = std::wstring_view(intent.actual_path).substr(intent.actual_path.find_last_of(L'\\') + 1);
    if (!path::EqualInsensitive(leaf, entry_name)) return false;
    const auto& target_parent = intent.target_parent.empty() ? intent.parent : intent.target_parent;
    if (!Same(target_parent, intent.parent)) {
        // A basename alone could be a different row at the tree root.
        return !entry_full_path.empty() && Canonical(entry_full_path) == Canonical(intent.actual_path);
    }
    auto composed = target_parent;
    if (!composed.ends_with(L'\\')) composed += L'\\';
    composed += entry_name;
    return Same(composed, intent.actual_path) &&
        (entry_full_path.empty() || Same(entry_full_path, intent.actual_path));
}
void ConsumeCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id) {
    std::erase_if(intents, [&](const auto& value) { return value.task_id == task_id; });
}
std::optional<CreateRenameSelection> SelectCreateRenameIntent(std::vector<CreateRenameIntent>& intents,
    std::wstring_view current_parent, uint64_t view_generation, std::span<const CreateRenameEntry> entries,
    bool focused, bool editor_active) {
    Prune(intents, current_parent, view_generation);
    if (!focused || editor_active) return std::nullopt;
    for (const auto& intent : intents) {
        if (!intent.ready) continue;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (CreateRenameEntryMatches(intent, entries[i].name, entries[i].full_path))
                return CreateRenameSelection{intent.task_id, i};
        }
    }
    return std::nullopt;
}
} // namespace pulse::app
