#include "../common/windows_compat.h"
#include "folder_picker_model.h"
#include "../common/picker_shell_namespace.h"

#include <algorithm>
#include <cwctype>

namespace pulse::ui {
namespace {

bool IsSeparator(wchar_t ch) { return ch == L'\\' || ch == L'/'; }

bool IsDriveRoot(std::wstring_view path) {
    return path.size() == 3 && std::iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\';
}

// "\\server\share" (with or without a trailing slash) has no parent folder.
bool IsShareRoot(std::wstring_view path) {
    if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\') return false;
    const size_t server_end = path.find(L'\\', 2);
    if (server_end == std::wstring_view::npos || server_end == 2) return false;
    const size_t share_end = path.find(L'\\', server_end + 1);
    return share_end == std::wstring_view::npos || share_end + 1 == path.size();
}

int CompareNames(const std::wstring& a, const std::wstring& b) {
    const int result = CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                                       NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                                       a.c_str(), static_cast<int>(a.size()),
                                       b.c_str(), static_cast<int>(b.size()),
                                       nullptr, nullptr, 0);
    if (result == 0) return a.compare(b);
    return result - CSTR_EQUAL;
}

} // namespace

bool IsPickerImageName(std::wstring_view name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring_view::npos) return false;
    std::wstring ext(name.substr(dot + 1));
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    // Same list as the system picker this replaces.
    for (const wchar_t* known : {L"jpg", L"jpeg", L"png", L"bmp", L"webp", L"jfif"}) {
        if (ext == known) return true;
    }
    return false;
}

bool PickerMatchesFilter(std::wstring_view name, PickerMode mode, const PickerOptions& options) {
    if (mode == PickerMode::Folder) return false;
    if (mode == PickerMode::Image && !IsPickerImageName(name)) return false;
    if (options.filters.empty()) return true;
    const auto& patterns = options.filters[std::min(options.filter_index, options.filters.size() - 1)].pattern;
    auto match = [](std::wstring_view text, std::wstring_view pattern) {
        if (pattern == L"*.*") return true;
        size_t t = 0, p = 0, star = std::wstring_view::npos, retry = 0;
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == L'?' || std::towlower(pattern[p]) == std::towlower(text[t]))) { ++p; ++t; }
            else if (p < pattern.size() && pattern[p] == L'*') { star = p++; retry = t; }
            else if (star != std::wstring_view::npos) { p = star + 1; t = ++retry; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == L'*') ++p;
        return p == pattern.size();
    };
    size_t start = 0;
    while (start <= patterns.size()) {
        const size_t end = patterns.find(L';', start);
        auto pattern = std::wstring_view(patterns).substr(start, end == std::wstring::npos ? end : end - start);
        while (!pattern.empty() && std::iswspace(pattern.front())) pattern.remove_prefix(1);
        while (!pattern.empty() && std::iswspace(pattern.back())) pattern.remove_suffix(1);
        if (!pattern.empty() && match(name, pattern)) return true;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

DWORD FollowPickerShortcutChain(std::wstring& path,
    const std::function<DWORD(std::wstring&, bool&)>& read_next,
    const std::function<bool()>& cancelled) {
    std::vector<std::wstring> visited;
    for (;;) {
        if (cancelled && cancelled()) return ERROR_CANCELLED;
        for (const auto& previous : visited)
            if (SamePickerPath(previous, path)) return ERROR_CANT_RESOLVE_FILENAME;
        visited.push_back(path);
        bool followed = false;
        const DWORD error = read_next(path, followed);
        if (error != ERROR_SUCCESS) return error;
        if (!followed) return ERROR_SUCCESS;
        if (visited.size() > 16) return ERROR_CANT_RESOLVE_FILENAME;
    }
}

bool ParsePickerNames(std::wstring_view text, std::vector<std::wstring>& names) {
    names.clear();
    while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && std::iswspace(text.back())) text.remove_suffix(1);
    if (text.empty()) return true;
    if (text.find(L'"') == std::wstring_view::npos) { names.emplace_back(text); return true; }
    while (!text.empty()) {
        if (text.front() != L'"') { names.clear(); return false; }
        text.remove_prefix(1);
        const size_t end = text.find(L'"');
        if (end == std::wstring_view::npos || end == 0) { names.clear(); return false; }
        names.emplace_back(text.substr(0, end));
        text.remove_prefix(end + 1);
        if (!text.empty() && !std::iswspace(text.front())) { names.clear(); return false; }
        while (!text.empty() && std::iswspace(text.front())) text.remove_prefix(1);
    }
    return true;
}

bool PickerShowsEntry(DWORD attributes, std::wstring_view name, PickerMode mode, const PickerOptions& options) {
    if (name.empty() || name == L"." || name == L"..") return false;
    if (!options.show_hidden && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) return false;
    if (!options.search.empty()) {
        std::wstring folded(name), needle(options.search);
        for (auto& c : folded) c = static_cast<wchar_t>(std::towlower(c));
        for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
        if (folded.find(needle) == std::wstring::npos) return false;
    }
    const bool shortcut = name.size() >= 4 && CompareStringOrdinal(name.data() + name.size() - 4, 4, L".lnk", 4, TRUE) == CSTR_EQUAL;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) || (shortcut && !options.no_dereference_links) || PickerMatchesFilter(name, mode, options);
}

void SortPickerEntries(std::vector<PickerEntry>& entries, const PickerOptions& options) {
    std::stable_sort(entries.begin(), entries.end(), [&](const PickerEntry& a, const PickerEntry& b) {
        const bool a_folder = a.kind == PickerEntryKind::Folder || a.kind == PickerEntryKind::Drive;
        const bool b_folder = b.kind == PickerEntryKind::Folder || b.kind == PickerEntryKind::Drive;
        if (a_folder != b_folder) return a_folder;
        int order = 0;
        if (options.sort == PickerSort::Modified) order = CompareFileTime(&a.modified, &b.modified);
        else if (options.sort == PickerSort::Size) order = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
        else if (options.sort == PickerSort::Type) {
            auto extension = [](const std::wstring& name) { const auto dot = name.rfind(L'.'); return dot == std::wstring::npos ? std::wstring() : name.substr(dot); };
            order = CompareNames(extension(a.name), extension(b.name));
        }
        if (!order) order = CompareNames(a.kind == PickerEntryKind::Drive ? a.path : a.name,
                                        b.kind == PickerEntryKind::Drive ? b.path : b.name);
        return options.descending ? order > 0 : order < 0;
    });
}

std::wstring PickerParent(std::wstring_view path) {
    if (picker_shell::IsParsingName(path)) return picker_shell::Parent(path);
    if (path.empty() || IsDriveRoot(path) || IsShareRoot(path)) return L"";
    std::wstring_view trimmed = path;
    while (trimmed.size() > 1 && IsSeparator(trimmed.back())) trimmed.remove_suffix(1);
    const size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return L"";
    std::wstring parent(trimmed.substr(0, slash));
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
    return parent;
}

std::wstring NormalizePickerInput(std::wstring_view text) {
    std::wstring value(text);
    auto trim = [&value]() {
        while (!value.empty() && std::iswspace(value.front())) value.erase(0, 1);
        while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    };
    trim();
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
        trim();
    }
    if (picker_shell::IsParsingName(value)) return value;
    // Pulse keeps long-path prefixes internally; the picker shows plain paths.
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) value = L"\\\\" + value.substr(8);
    else if (value.rfind(L"\\\\?\\", 0) == 0) value = value.substr(4);
    if (value.find(L'%') != std::wstring::npos) {
        const DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring expanded(needed, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
            if (written > 0 && written <= needed) {
                expanded.resize(written - 1);
                value = std::move(expanded);
            }
        }
    }
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.size() == 2 && std::iswalpha(value[0]) && value[1] == L':') value += L'\\';
    if (value.size() >= 2 && value[1] == L':') value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}

bool IsPickerFolderName(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' ||
        std::iswspace(name.back()) || name.find_first_of(L"\\/:*?\"<>|") != name.npos) return false;
    for (wchar_t ch : name) if (ch < 32) return false;
    std::wstring stem(name.substr(0, name.find(L'.')));
    for (auto& ch : stem) ch = static_cast<wchar_t>(std::towupper(ch));
    return stem != L"CON" && stem != L"PRN" && stem != L"AUX" && stem != L"NUL" &&
        !(stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") && stem[3] >= L'0' && stem[3] <= L'9');
}

bool IsPickerDiskPath(std::wstring_view path) {
    if (path.size() > 32767 || path.find(L'\0') != path.npos || path.find_first_of(L"\r\n") != path.npos) return false;
    if (path.starts_with(L"\\\\?\\UNC\\")) return IsPickerDiskPath(std::wstring(L"\\\\") + std::wstring(path.substr(8)));
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && IsSeparator(path[2])) return true;
    if (!path.starts_with(L"\\\\") || path.size() < 5 || path[2] == L'.' || path[2] == L'?') return false;
    const auto split = path.find(L'\\', 2);
    return split != path.npos && split > 2 && split + 1 < path.size() && path[split + 1] != L'\\';
}

bool SamePickerPath(std::wstring_view a, std::wstring_view b) {
    auto trimmed = [](std::wstring_view s) {
        while (s.size() > 3 && IsSeparator(s.back())) s.remove_suffix(1);
        return s;
    };
    a = trimmed(a);
    b = trimmed(b);
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring PickerChosenPath(PickerMode mode, std::wstring_view current,
                              const PickerEntry* selected) {
    const bool shortcut = selected && selected->name.size() >= 4 &&
        CompareStringOrdinal(selected->name.data() + selected->name.size() - 4, 4, L".lnk", 4, TRUE) == CSTR_EQUAL;
    if (shortcut) return selected->path;
    if (mode != PickerMode::Folder) {
        return selected && (selected->kind == PickerEntryKind::Image || ((mode == PickerMode::File || mode == PickerMode::Save) && selected->kind == PickerEntryKind::File)) ? selected->path
                                                                     : std::wstring();
    }
    if (selected && (selected->kind == PickerEntryKind::Folder || selected->kind == PickerEntryKind::Drive)) return selected->path;
    return std::wstring(current);
}

int PickerTypeAhead(const std::vector<PickerEntry>& entries, int from, wchar_t ch) {
    const int count = static_cast<int>(entries.size());
    if (count == 0) return -1;
    const wchar_t wanted = static_cast<wchar_t>(std::towlower(ch));
    for (int step = 1; step <= count; ++step) {
        const int index = ((from < 0 ? -1 : from) + step + count) % count;
        const std::wstring& name = entries[static_cast<size_t>(index)].name;
        if (!name.empty() && std::towlower(name.front()) == wanted) return index;
    }
    return -1;
}

void PickerHistory::Navigate(std::wstring from) {
    forward_.clear();
    if (!back_.empty() && SamePickerPath(back_.back(), from)) return;
    back_.push_back(std::move(from));
    constexpr size_t kMaxDepth = 64;
    if (back_.size() > kMaxDepth) back_.erase(back_.begin());
}

std::wstring PickerHistory::Back() {
    if (back_.empty()) return L"";
    std::wstring previous = std::move(back_.back());
    back_.pop_back();
    return previous;
}

std::wstring PickerHistory::Back(std::wstring current) {
    if (back_.empty()) return current;
    forward_.push_back(std::move(current));
    return Back();
}

std::wstring PickerHistory::Forward(std::wstring current) {
    if (forward_.empty()) return current;
    back_.push_back(std::move(current));
    auto next = std::move(forward_.back());
    forward_.pop_back();
    return next;
}

} // namespace pulse::ui
