#include "../common/windows_compat.h"
#include "folder_picker_model.h"
#include "type_ahead.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

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

bool PickerShowsEntry(DWORD attributes, std::wstring_view name, PickerMode mode) {
    if (name.empty() || name == L"." || name == L"..") return false;
    if (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return false;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return true;
    return mode == PickerMode::Image && IsPickerImageName(name);
}

void SortPickerEntries(std::vector<PickerEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PickerEntry& a, const PickerEntry& b) {
        const bool a_folder = a.kind != PickerEntryKind::Image;
        const bool b_folder = b.kind != PickerEntryKind::Image;
        if (a_folder != b_folder) return a_folder;
        // Drives keep their letter order.
        if (a.kind == PickerEntryKind::Drive && b.kind == PickerEntryKind::Drive)
            return CompareNames(a.path, b.path) < 0;
        return CompareNames(a.name, b.name) < 0;
    });
}

std::wstring PickerParent(std::wstring_view path) {
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
    if (mode == PickerMode::Image) {
        return selected && selected->kind == PickerEntryKind::Image ? selected->path
                                                                     : std::wstring();
    }
    if (selected && selected->kind != PickerEntryKind::Image) return selected->path;
    return std::wstring(current);
}

int PickerTypeAhead(const std::vector<PickerEntry>& entries, int from, wchar_t ch) {
    const int count = static_cast<int>(entries.size());
    return NextPrefixMatch(count, from,
                           [&entries](int index) -> std::wstring_view {
                               return entries[static_cast<size_t>(index)].name;
                           },
                           std::wstring_view(&ch, 1));
}

void PickerHistory::Navigate(std::wstring from) {
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

} // namespace pulse::ui
