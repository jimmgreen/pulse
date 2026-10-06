#include "folder_sort_prefs.h"
#include "folder_view_prefs.h"
#include "../common/json_utils.h"
#include <windows.h>

namespace pulse::app {
namespace {

// Indexed by ui::SortColumn; stable JSON names.
constexpr const wchar_t* kColumnNames[] = { L"name", L"modified", L"type", L"size", L"path",
                                            L"created", L"accessed", L"title", L"artist",
                                            L"album" };
constexpr int kColumnCount = static_cast<int>(sizeof(kColumnNames) / sizeof(kColumnNames[0]));

bool Valid(FolderSort sort) {
    const int column = static_cast<int>(sort.column);
    const int direction = static_cast<int>(sort.direction);
    return column >= 0 && column < kColumnCount && (direction == 0 || direction == 1);
}

std::wstring JsonKey(int column, int direction) {
    return std::wstring(L"sort_by_folder_") + kColumnNames[column] +
           (direction == 0 ? L"_asc" : L"_desc");
}

FolderSort FromInts(int column, int direction) {
    FolderSort sort{ static_cast<ui::SortColumn>(column), static_cast<ui::SortDirection>(direction) };
    return Valid(sort) ? sort : FolderSort{};
}

} // namespace

bool FolderSortPrefs::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

std::optional<FolderSort> FolderSortPrefs::Find(const std::wstring& path) const {
    const auto it = sorts_.find(FolderPrefKey(path));
    return it == sorts_.end() ? std::nullopt : std::optional(it->second);
}

bool FolderSortPrefs::Set(const std::wstring& path, FolderSort sort) {
    const auto key = FolderPrefKey(path);
    if (key.empty() || !Valid(sort)) return false;
    const auto [it, inserted] = sorts_.try_emplace(key, sort);
    if (!inserted && it->second == sort) return false;
    it->second = sort;
    return true;
}

void FolderSortPrefs::ApplyToAll(FolderSort sort) {
    sorts_.clear();
    default_ = Valid(sort) ? sort : FolderSort{};
}

void FolderSortPrefs::Clear() {
    sorts_.clear();
    default_ = {};
}

void FolderSortPrefs::AppendJson(std::wstring& out) const {
    out += L",\n  \"sort_default_column\":" + std::to_wstring(static_cast<int>(default_.column));
    out += L",\n  \"sort_default_direction\":" + std::to_wstring(static_cast<int>(default_.direction));
    // One path array per (column, direction); empty arrays are omitted.
    for (int column = 0; column < kColumnCount; ++column) {
        for (int direction = 0; direction <= 1; ++direction) {
            const FolderSort wanted = FromInts(column, direction);
            bool first = true;
            for (const auto& [path, saved] : sorts_) {
                if (saved != wanted) continue;
                if (first) out += L",\n  \"" + JsonKey(column, direction) + L"\":[";
                else out += L",";
                out += L"\"";
                first = false;
                json::Escape(path, out);
                out += L"\"";
            }
            if (!first) out += L"]";
        }
    }
}

void FolderSortPrefs::ReadJson(const std::wstring& input) {
    Clear();
    default_ = FromInts(json::ExtractInt(input, L"sort_default_column", 0),
                        json::ExtractInt(input, L"sort_default_direction", 0));
    for (int column = 0; column < kColumnCount; ++column)
        for (int direction = 0; direction <= 1; ++direction)
            for (const auto& path : json::ExtractStringArray(input, JsonKey(column, direction)))
                Set(path, FromInts(column, direction));
}

} // namespace pulse::app
