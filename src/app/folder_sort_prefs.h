// folder_sort_prefs.h — Per-folder sort memory (#26) with a global default
// that "apply to all folders" replaces (#31).
#pragma once

#include "../ui/ui_view_model.h"
#include <map>
#include <optional>
#include <string>

namespace pulse::app {

struct FolderSort {
    ui::SortColumn column = ui::SortColumn::Name;
    ui::SortDirection direction = ui::SortDirection::Asc;
    bool operator==(const FolderSort&) const = default;
};

// Real folders and This PC (empty navigation path); virtual views such as
// search results keep the tab's order and are never stored.
class FolderSortPrefs {
public:
    std::optional<FolderSort> Find(const std::wstring& path) const;
    bool Set(const std::wstring& path, FolderSort sort);
    // Order for folders without a saved choice.
    FolderSort Default() const { return default_; }
    // Where folders sit next to files (app::FolderSortMode as 0..2) when
    // chosen in this folder; others use Settings > Folders first.
    std::optional<int> FindFolderMode(const std::wstring& path) const;
    bool SetFolderMode(const std::wstring& path, int mode);
    // `sort` becomes the default and per-folder choices (folder modes
    // included) are forgotten.
    void ApplyToAll(FolderSort sort);
    void Clear();
    void AppendJson(std::wstring& out) const;
    void ReadJson(const std::wstring& json);

private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    std::map<std::wstring, FolderSort, PathLess> sorts_;
    std::map<std::wstring, int, PathLess> folder_modes_;
    FolderSort default_;
};

} // namespace pulse::app
