#include "picker_sidebar_model.h"
#include "folder_picker_model.h"

#include <windows.h>
#include <algorithm>

namespace pulse::ui {
namespace {
bool Contains(const std::vector<std::wstring>& paths, const std::wstring& path) {
    return std::any_of(paths.begin(), paths.end(), [&](const auto& p) { return SamePickerPath(p, path); });
}
bool HasRow(const std::vector<PickerCommonRow>& rows, const std::wstring& path) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return SamePickerPath(r.path, path); });
}
std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}
bool IsSeparator(wchar_t ch) { return ch == L'\\' || ch == L'/'; }
} // namespace

std::wstring PickerParentPath(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && IsSeparator(p.back())) p.pop_back();
    if (p.size() <= 3 && p.size() >= 2 && p[1] == L':') return {};   // a drive root
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (p.starts_with(L"\\\\")) {
        // \\server\share is the root of a UNC path: nothing above it.
        const size_t server_end = p.find_first_of(L"\\/", 2);
        if (server_end == std::wstring::npos || slash <= server_end) return {};
    }
    if (slash == 2 && p[1] == L':') return p.substr(0, 3);            // "C:\a" -> "C:\"
    return p.substr(0, slash);
}

void AddPickerSameNameHints(std::vector<PickerCommonRow>& rows, const PickerPathTextFn& parent_label) {
    for (auto& row : rows) row.hint.clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        const std::wstring name = Lower(rows[i].label);
        std::vector<size_t> same;
        for (size_t j = 0; j < rows.size(); ++j) if (Lower(rows[j].label) == name) same.push_back(j);
        if (same.size() < 2 || same.front() != i) continue;
        for (size_t k : same) {
            const std::wstring parent = PickerParentPath(rows[k].path);
            rows[k].hint = parent.empty() ? rows[k].path : parent_label ? parent_label(parent) : parent;
        }
        // Parents with the same name too (two "打印文件夹" under two "资料"): the full parent path.
        std::vector<size_t> clash;
        for (size_t a : same)
            for (size_t b : same)
                if (a != b && Lower(rows[a].hint) == Lower(rows[b].hint)) { clash.push_back(a); break; }
        for (size_t a : clash) {
            const std::wstring parent = PickerParentPath(rows[a].path);
            rows[a].hint = parent.empty() ? rows[a].path : parent;
        }
    }
}

std::vector<PickerCommonRow> BuildPickerCommonRows(const PickerCommonInput& input, const PickerPathTextFn& leaf,
                                                   const PickerPathTextFn& parent_label) {
    std::vector<PickerCommonRow> rows;
    const size_t limit = (std::max)(size_t{1}, input.limit);
    const auto usable = [&](const std::wstring& path) {
        return !path.empty() && !Contains(input.forgotten, path) && !HasRow(rows, path);
    };
    const auto name = [&](const PickerCommonFolder& folder) {
        return folder.label.empty() ? (leaf ? leaf(folder.path) : folder.path) : folder.label;
    };
    // Pinned folders are the user's own choice: all of them, ahead of the rest.
    for (const auto& path : input.pinned) {
        if (!usable(path)) continue;
        PickerCommonRow row;
        row.path = path;
        row.label = leaf ? leaf(path) : path;
        row.pinned = true;
        rows.push_back(std::move(row));
    }
    const size_t pinned_count = rows.size();
    for (const auto& folder : input.history) {
        if (rows.size() >= (std::max)(limit, pinned_count)) break;
        if (!usable(folder.path)) continue;
        PickerCommonRow row;
        row.path = folder.path;
        row.label = name(folder);
        rows.push_back(std::move(row));
    }
    // The last place always shows: it takes the slot of the least used folder.
    if (!input.last_location.empty() && usable(input.last_location)) {
        const auto found = std::find_if(input.history.begin(), input.history.end(),
            [&](const auto& f) { return SamePickerPath(f.path, input.last_location); });
        PickerCommonRow row;
        row.path = input.last_location;
        row.label = found != input.history.end() ? name(*found) : (leaf ? leaf(input.last_location) : input.last_location);
        if (rows.size() >= (std::max)(limit, pinned_count + 1) && rows.size() > pinned_count) rows.pop_back();
        if (rows.size() < (std::max)(limit, pinned_count + 1)) rows.push_back(std::move(row));
    }
    if (input.pinned.empty() && input.history.empty()) {
        for (const auto& folder : input.pulse_recent) {
            if (rows.size() >= limit) break;
            if (!usable(folder.path)) continue;
            PickerCommonRow row;
            row.path = folder.path;
            row.label = name(folder);
            row.from_pulse = true;
            rows.push_back(std::move(row));
        }
    }
    for (auto& row : rows) {
        row.last = !input.last_location.empty() && SamePickerPath(row.path, input.last_location);
        row.favorite = Contains(input.favorites, row.path);
    }
    AddPickerSameNameHints(rows, parent_label ? parent_label : leaf);
    return rows;
}

} // namespace pulse::ui
