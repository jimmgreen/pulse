#include "global_search_arrange.h"
#include "entry_group.h"
#include "../common/string_ids.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <unordered_map>

namespace pulse::app {
namespace {

using l10n::StringId;

int CompareText(std::wstring_view a, std::wstring_view b) {
    const int result = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
        a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (result == 0) { const int c = a.compare(b); return c < 0 ? -1 : c > 0 ? 1 : 0; }
    return result - CSTR_EQUAL;
}

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

std::wstring_view StripPrefix(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return path.substr(6);  // keeps "\\server"
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

std::wstring_view ParentOf(std::wstring_view path) {
    path = StripPrefix(path);
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return {};
    if (slash == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);  // "C:\"
    return path.substr(0, slash);
}

std::wstring_view LeafOf(std::wstring_view path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos || slash + 1 >= path.size()) return path;
    return path.substr(slash + 1);
}

uint64_t SortKeyTicks(const GlobalSearchArrangeItem& item) { return item.mtime; }

int Compare(const GlobalSearchArrangeItem& a, const GlobalSearchArrangeItem& b, GlobalSearchSort sort) {
    switch (sort) {
    case GlobalSearchSort::Name: return CompareText(a.name, b.name);
    case GlobalSearchSort::Modified: {
        const auto x = SortKeyTicks(a), y = SortKeyTicks(b);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    case GlobalSearchSort::Size:
        return a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
    case GlobalSearchSort::Type: {
        if (a.directory != b.directory) return a.directory ? -1 : 1;
        const auto x = GlobalSearchExtensionOf(a.name), y = GlobalSearchExtensionOf(b.name);
        if (const int c = x.compare(y)) return c < 0 ? -1 : 1;
        return CompareText(a.name, b.name);
    }
    case GlobalSearchSort::Path: {
        if (const int c = CompareText(ParentOf(a.path), ParentOf(b.path))) return c;
        return CompareText(a.name, b.name);
    }
    default: return 0;
    }
}

fs::DirEntry EntryOf(const GlobalSearchArrangeItem& item) {
    fs::DirEntry entry;
    entry.name.assign(item.name);
    entry.size = item.size;
    entry.mtime.dwLowDateTime = static_cast<DWORD>(item.mtime);
    entry.mtime.dwHighDateTime = static_cast<DWORD>(item.mtime >> 32);
    entry.attrs = item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return entry;
}

StringId MatchTitle(GlobalSearchRowClass cls) {
    switch (cls) {
    case GlobalSearchRowClass::Jump: return StringId::GlobalSearchJump;
    case GlobalSearchRowClass::Best: return StringId::GlobalSearchBestMatch;
    case GlobalSearchRowClass::App: return StringId::GlobalSearchGroupApps;
    case GlobalSearchRowClass::Folder: return StringId::Folder;
    case GlobalSearchRowClass::Content: return StringId::GlobalSearchContentMatches;
    default: return StringId::FilterFiles;
    }
}

StringId KindTitle(index::SearchKind kind) {
    switch (kind) {
    case index::SearchKind::Folder: return StringId::KindFolder;
    case index::SearchKind::Document: return StringId::KindDocument;
    case index::SearchKind::Image: return StringId::KindImage;
    case index::SearchKind::Video: return StringId::KindVideo;
    case index::SearchKind::Audio: return StringId::KindAudio;
    case index::SearchKind::Archive: return StringId::KindArchive;
    case index::SearchKind::Code: return StringId::KindCode;
    default: return StringId::GroupNameOther;
    }
}

constexpr index::SearchKind kKindOrder[] = {index::SearchKind::Folder, index::SearchKind::Document, index::SearchKind::Image,
    index::SearchKind::Video, index::SearchKind::Audio, index::SearchKind::Archive, index::SearchKind::Code, index::SearchKind::Any};

} // namespace

std::wstring GlobalSearchExtensionOf(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= name.size()) return {};
    return Lower(name.substr(dot + 1));
}

index::SearchKind GlobalSearchKindOf(std::wstring_view name, bool directory) {
    if (directory) return index::SearchKind::Folder;
    static const std::unordered_map<std::wstring, index::SearchKind> kinds = [] {
        std::unordered_map<std::wstring, index::SearchKind> out;
        for (const auto kind : kKindOrder) {
            if (kind == index::SearchKind::Folder || kind == index::SearchKind::Any) continue;
            const std::wstring list = index::KindExtensions(kind);
            for (size_t start = 0; start < list.size();) {
                size_t end = list.find(L';', start);
                if (end == std::wstring::npos) end = list.size();
                if (end > start) out.emplace(list.substr(start, end - start), kind);
                start = end + 1;
            }
        }
        return out;
    }();
    const auto ext = GlobalSearchExtensionOf(name);
    const auto it = ext.empty() ? kinds.end() : kinds.find(ext);
    return it == kinds.end() ? index::SearchKind::Any : it->second;
}

std::vector<GlobalSearchSpan> ArrangeGlobalSearch(const std::vector<GlobalSearchArrangeItem>& items,
                                                  const GlobalSearchArrangeOptions& options, const GroupClock* clock) {
    std::vector<GlobalSearchSpan> spans;
    if (items.empty()) return spans;
    const bool relevance = options.sort == GlobalSearchSort::Relevance;
    if (options.group == GlobalSearchGroup::Match && relevance) {
        // The merged order already runs class by class; only 应用 may move up.
        constexpr GlobalSearchRowClass order[] = {GlobalSearchRowClass::Jump, GlobalSearchRowClass::Best,
            GlobalSearchRowClass::App, GlobalSearchRowClass::Folder, GlobalSearchRowClass::File, GlobalSearchRowClass::Content};
        std::vector<GlobalSearchRowClass> classes(std::begin(order), std::end(order));
        if (options.apps_first) {
            classes.erase(std::find(classes.begin(), classes.end(), GlobalSearchRowClass::App));
            classes.insert(classes.begin(), GlobalSearchRowClass::App);
        }
        for (const auto cls : classes) {
            GlobalSearchSpan span;
            for (size_t i = 0; i < items.size(); ++i) if (items[i].cls == cls) span.items.push_back(i);
            if (span.items.empty()) continue;
            span.key = L"m:" + std::to_wstring(static_cast<int>(cls));
            span.title = MatchTitle(cls);
            span.app = cls == GlobalSearchRowClass::App;
            spans.push_back(std::move(span));
        }
        return spans;
    }

    std::vector<size_t> apps, others;
    for (size_t i = 0; i < items.size(); ++i) (items[i].cls == GlobalSearchRowClass::App ? apps : others).push_back(i);
    if (!relevance) {
        std::stable_sort(others.begin(), others.end(), [&](size_t x, size_t y) {
            const int c = Compare(items[x], items[y], options.sort);
            return options.descending ? c > 0 : c < 0;
        });
        if (options.folders != GlobalSearchFolders::Mixed) {
            const bool top = options.folders == GlobalSearchFolders::Top;
            std::stable_partition(others.begin(), others.end(), [&](size_t i) { return items[i].directory == top; });
        }
    }

    if (options.group == GlobalSearchGroup::Match || options.group == GlobalSearchGroup::None) {
        GlobalSearchSpan flat;
        flat.key = L"flat"; flat.header = false;
        if (options.apps_first) flat.items = apps;
        flat.items.insert(flat.items.end(), others.begin(), others.end());
        if (!options.apps_first) flat.items.insert(flat.items.end(), apps.begin(), apps.end());
        spans.push_back(std::move(flat));
        return spans;
    }

    GroupClock local_clock;
    if (!clock && (options.group == GlobalSearchGroup::Date || options.group == GlobalSearchGroup::Size)) {
        local_clock = MakeGroupClock();
        clock = &local_clock;
    }
    // Buckets keyed by their place in the shown order.
    std::map<std::pair<int, std::wstring>, GlobalSearchSpan> buckets;
    for (const size_t i : others) {
        const auto& item = items[i];
        int rank = 0;
        std::wstring key;
        GlobalSearchSpan proto;
        switch (options.group) {
        case GlobalSearchGroup::Type: {
            const auto kind = GlobalSearchKindOf(item.name, item.directory);
            rank = static_cast<int>(std::find(std::begin(kKindOrder), std::end(kKindOrder), kind) - std::begin(kKindOrder));
            key = std::wstring(L"t:") + (kind == index::SearchKind::Any ? L"other" : index::KindId(kind));
            proto.title = KindTitle(kind); proto.kind = kind;
            break;
        }
        case GlobalSearchGroup::Date: {
            const int date = std::clamp(GroupRank(EntryOf(item), GroupBy::Date, *clock), 0, 8);
            const bool oldest_first = options.sort == GlobalSearchSort::Modified && !options.descending;
            rank = oldest_first ? 8 - date : date;
            key = L"d:" + std::to_wstring(date);
            proto.title = static_cast<StringId>(IDS_GROUP_D_FUTURE + date);
            break;
        }
        case GlobalSearchGroup::Size: {
            const int size = std::clamp(GroupRank(EntryOf(item), GroupBy::Size, *clock), 0, 7);
            const bool smallest_first = options.sort == GlobalSearchSort::Size && !options.descending;
            if (size == 0) rank = options.folders == GlobalSearchFolders::Bottom ? 100 : -1;
            else rank = smallest_first ? size : 8 - size;
            key = L"s:" + std::to_wstring(size);
            proto.title = static_cast<StringId>(IDS_GROUP_FOLDERS + size);
            break;
        }
        default: {  // Folder: ordered by size later
            const auto parent = ParentOf(item.path);
            key = L"f:" + Lower(parent);
            proto.label = std::wstring(LeafOf(parent));
            proto.detail = std::wstring(ParentOf(parent));
            break;
        }
        }
        auto& span = buckets[{rank, key}];
        if (span.key.empty()) {
            span = std::move(proto);
            span.key = key; span.foldable = true;
        }
        span.items.push_back(i);
    }
    std::vector<GlobalSearchSpan> grouped;
    for (auto& [slot, span] : buckets) grouped.push_back(std::move(span));
    if (options.group == GlobalSearchGroup::Folder) {
        // Biggest folders first; folders holding a single result share 其他.
        std::stable_sort(grouped.begin(), grouped.end(), [](const GlobalSearchSpan& a, const GlobalSearchSpan& b) {
            if (a.items.size() != b.items.size()) return a.items.size() > b.items.size();
            return CompareText(a.key, b.key) < 0;
        });
        const auto singles = std::count_if(grouped.begin(), grouped.end(), [](const auto& s) { return s.items.size() == 1; });
        if (singles > 1) {
            GlobalSearchSpan other;
            other.key = L"f:*"; other.title = StringId::GroupNameOther; other.foldable = true;
            for (const auto& span : grouped) if (span.items.size() == 1) other.items.push_back(span.items.front());
            // Keep the chosen order inside 其他.
            std::sort(other.items.begin(), other.items.end(), [&](size_t x, size_t y) {
                return std::find(others.begin(), others.end(), x) < std::find(others.begin(), others.end(), y);
            });
            grouped.erase(std::remove_if(grouped.begin(), grouped.end(), [](const auto& s) { return s.items.size() == 1; }), grouped.end());
            grouped.push_back(std::move(other));
        }
    }
    GlobalSearchSpan app_span;
    if (!apps.empty()) {
        app_span.key = L"apps"; app_span.title = StringId::GlobalSearchGroupApps;
        app_span.app = true; app_span.foldable = true; app_span.items = apps;
    }
    if (!apps.empty() && options.apps_first) spans.push_back(app_span);
    for (auto& span : grouped) spans.push_back(std::move(span));
    if (!apps.empty() && !options.apps_first) spans.push_back(std::move(app_span));
    return spans;
}

} // namespace pulse::app
