#pragma once

#include <string>
#include <vector>

namespace pulse::app {

enum class SavedSearchMode { Name, Content, Duplicates };

struct SavedSearch {
    std::wstring name;
    SavedSearchMode mode = SavedSearchMode::Name;
    std::wstring root;
    std::wstring query;
    bool recursive = true;
};

class SavedSearchStore {
public:
    const std::vector<SavedSearch>& items() const noexcept { return items_; }
    bool Load();
    bool Save() const;
    bool LoadFrom(const std::wstring& path);
    bool SaveTo(const std::wstring& path) const;
    bool Add(SavedSearch search);
    bool Remove(size_t index);
    void Clear() { items_.clear(); }

    static std::wstring DefaultPath();

private:
    std::vector<SavedSearch> items_;
};

} // namespace pulse::app
