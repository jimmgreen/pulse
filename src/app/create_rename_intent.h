#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
struct CreateRenameIntent {
    uint64_t task_id = 0;
    std::wstring parent;
    uint64_t view_generation = 0;
    std::wstring actual_path;
    bool ready = false;
    std::wstring target_parent; // may be an expanded child of the bound view parent
};
struct CreateRenameEntry {
    std::wstring_view name;
    std::wstring_view full_path;
};
struct CreateRenameSelection {
    uint64_t task_id = 0;
    size_t entry_index = 0;
};
void QueueCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id,
    std::wstring_view parent, uint64_t view_generation, std::wstring_view target_parent = {});
bool CompleteCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id,
    std::wstring_view current_parent, uint64_t view_generation, std::wstring_view actual_created_path);
// Success is delivered before terminal IDs; retain only successfully armed intents.
void FinishCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id);
std::optional<CreateRenameIntent> PendingCreateRenameIntent(std::vector<CreateRenameIntent>& intents,
    std::wstring_view current_parent, uint64_t view_generation);
bool CreateRenameEntryMatches(const CreateRenameIntent& intent, std::wstring_view entry_name,
    std::wstring_view entry_full_path);
std::optional<CreateRenameSelection> SelectCreateRenameIntent(std::vector<CreateRenameIntent>& intents,
    std::wstring_view current_parent, uint64_t view_generation, std::span<const CreateRenameEntry> entries,
    bool focused, bool editor_active);
void ConsumeCreateRenameIntent(std::vector<CreateRenameIntent>& intents, uint64_t task_id);
} // namespace pulse::app
