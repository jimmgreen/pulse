// shell_window_plan.cpp — see shell_window_plan.h.
#include "shell_window_plan.h"

#include <windows.h>

namespace pulse::app {
namespace {

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                             static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

const ShellWindowEntry* Find(const std::vector<ShellWindowEntry>& entries, uint64_t key) {
    for (const auto& entry : entries)
        if (entry.key == key) return &entry;
    return nullptr;
}

bool IsDriveLetter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

} // namespace

bool IsShellWindowPath(const std::wstring& path) {
    if (path.empty()) return true;   // This PC
    if (path.size() >= 2 && IsDriveLetter(path[0]) && path[1] == L':') return true;
    return path.size() > 2 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?';
}

std::vector<ShellWindowAction> PlanShellWindowChanges(
    const std::vector<ShellWindowEntry>& current, const std::vector<ShellWindowEntry>& wanted) {
    std::vector<ShellWindowAction> actions;
    for (const auto& entry : current)
        if (!Find(wanted, entry.key))
            actions.push_back({ShellWindowActionKind::Revoke, entry.key, entry.path});
    for (const auto& entry : wanted) {
        const ShellWindowEntry* had = Find(current, entry.key);
        if (!had)
            actions.push_back({ShellWindowActionKind::Register, entry.key, entry.path});
        else if (!SamePath(had->path, entry.path))
            actions.push_back({ShellWindowActionKind::Navigate, entry.key, entry.path});
    }
    return actions;
}

bool SplitShellItemPath(const std::wstring& path, std::wstring& folder, std::wstring& leaf) {
    folder.clear();
    leaf.clear();
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= trimmed.size()) return false;
    leaf = trimmed.substr(slash + 1);
    folder = trimmed.substr(0, slash);
    // "C:\a" keeps the root's backslash; a UNC share root has no folder above.
    if (folder.size() == 2 && folder[1] == L':') folder += L'\\';
    if (folder.empty() || folder == L"\\" || (folder.size() >= 2 && folder[0] == L'\\' &&
        folder[1] == L'\\' && folder.find(L'\\', 2) == std::wstring::npos)) {
        folder.clear();
        leaf.clear();
        return false;
    }
    return true;
}

ExplorerTakeoverStep DecideExplorerTakeover(const ExplorerWindowProbe& probe) {
    if (!probe.view_ready)
        return probe.age_ms >= kExplorerViewTimeoutMs ? ExplorerTakeoverStep::Leave : ExplorerTakeoverStep::Wait;
    if (!probe.supported) return ExplorerTakeoverStep::Leave;
    if (probe.selected > 0 || probe.view_age_ms >= kExplorerSelectionGraceMs) return ExplorerTakeoverStep::Take;
    return ExplorerTakeoverStep::Wait;
}

SourceCloseStep DecideSourceClose(const SourceCloseProbe& probe) {
    if (!probe.identity_kept || !probe.single_tab) return SourceCloseStep::Abort;
    if (probe.selection_changed) {
        // Visible: the user may have picked something else there, unless the
        // change came right after the view appeared (a late /select).
        return probe.hidden || probe.view_age_ms < kExplorerProgrammaticSelectionMs
            ? SourceCloseStep::CloseAndSelect : SourceCloseStep::Abort;
    }
    if (probe.hidden && !probe.sent_selection && probe.view_age_ms < kExplorerLateSelectionMs)
        return SourceCloseStep::Wait;
    return SourceCloseStep::Close;
}

} // namespace pulse::app
