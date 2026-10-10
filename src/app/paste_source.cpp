// paste_source.cpp — see paste_source.h.
#include "paste_source.h"

#include "../common/path_utils.h"

#include <windows.h>
#include <algorithm>

namespace pulse::app {
namespace {

std::wstring PathKey(const std::wstring& raw) {
    std::wstring key = path::StripExtendedPathPrefix(raw);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 1 && key.back() == L'\\' &&
           !(key.size() == 3 && key[1] == L':'))
        key.pop_back();
    return key;
}

int CompareKeys(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()), TRUE);
}

std::vector<std::wstring> SortedKeys(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> keys;
    keys.reserve(paths.size());
    for (const auto& path : paths) keys.push_back(PathKey(path));
    std::sort(keys.begin(), keys.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareKeys(a, b) == CSTR_LESS_THAN;
    });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareKeys(a, b) == CSTR_EQUAL;
    }), keys.end());
    return keys;
}

} // namespace

bool SamePathSet(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    const std::vector<std::wstring> left = SortedKeys(a);
    const std::vector<std::wstring> right = SortedKeys(b);
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (CompareKeys(left[i], right[i]) != CSTR_EQUAL) return false;
    return true;
}

PasteSource ChoosePasteSource(const PasteChoiceInput& input) {
    if (!input.clipboard_has_files || input.clipboard_written_for_batch) return PasteSource::TrayBatch;
    if (!input.batch_paths || !input.clipboard_paths || input.batch_paths->empty())
        return PasteSource::Clipboard;
    if (input.clipboard_cut == input.batch_move && SamePathSet(*input.batch_paths, *input.clipboard_paths))
        return PasteSource::TrayBatch;
    return PasteSource::Clipboard;
}

} // namespace pulse::app
