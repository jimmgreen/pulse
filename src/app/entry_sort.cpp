#include "entry_sort.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <cwctype>
#include <string_view>

namespace pulse::app {

namespace {

std::wstring_view ExtensionView(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return std::wstring_view(name).substr(dot + 1);
}

int ExtensionCompare(const std::wstring& a, const std::wstring& b) {
    const std::wstring_view ea = ExtensionView(a);
    const std::wstring_view eb = ExtensionView(b);
    const size_t n = std::min(ea.size(), eb.size());
    for (size_t i = 0; i < n; ++i) {
        const wint_t ca = std::towlower(ea[i]);
        const wint_t cb = std::towlower(eb[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (ea.size() == eb.size()) return 0;
    return ea.size() < eb.size() ? -1 : 1;
}

int NameCompare(const std::wstring& a, const std::wstring& b) {
    int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
    if (cmp == 0) cmp = wcscmp(a.c_str(), b.c_str());
    return cmp;
}

} // namespace

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    const bool a_folder = a.is_dir || (!a.link_target.empty() && a.link_target_is_dir);
    const bool b_folder = b.is_dir || (!b.link_target.empty() && b.link_target_is_dir);
    if (a_folder != b_folder) return a_folder;
    int cmp = 0;
    switch (col) {
    case ui::SortColumn::Name:
        cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Size:
        if (a.size < b.size) cmp = -1;
        else if (a.size > b.size) cmp = 1;
        else cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Mtime:
        cmp = CompareFileTime(&a.mtime, &b.mtime);
        if (cmp == 0) cmp = NameCompare(a.name, b.name);
        break;
    case ui::SortColumn::Type: {
        cmp = ExtensionCompare(a.name, b.name);
        if (cmp == 0) cmp = NameCompare(a.name, b.name);
        break;
    }
    case ui::SortColumn::Path:
        cmp = _wcsicmp(a.full_path.c_str(), b.full_path.c_str());
        if (cmp == 0) cmp = NameCompare(a.name, b.name);
        break;
    }
    if (dir == ui::SortDirection::Desc) cmp = -cmp;
    return cmp < 0;
}

} // namespace pulse::app
