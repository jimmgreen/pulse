#include "entry_sort.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <cwctype>

namespace pulse::app {

namespace {

std::wstring Extension(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = std::towlower(c);
    return ext;
}

int NameCompare(const std::wstring& a, const std::wstring& b) {
    int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
    if (cmp == 0) cmp = wcscmp(a.c_str(), b.c_str());
    return cmp;
}

} // namespace

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    if (a.is_dir != b.is_dir) return a.is_dir;
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
        std::wstring ea = Extension(a.name);
        std::wstring eb = Extension(b.name);
        cmp = _wcsicmp(ea.c_str(), eb.c_str());
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
