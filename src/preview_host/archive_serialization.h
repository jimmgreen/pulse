// Shared bounded archive tree serialization; no file readers or UI dependencies.
#pragma once
#include "archive_listing.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pulse::preview::archive_serialization {
using Entry = ArchiveListingEntry;

struct Listing {
    std::wstring format;
    std::vector<Entry> entries;
    uint64_t packed_total = 0;
    bool has_packed = false;
    bool incomplete = false;  // time or entry budget reached
    uint64_t skipped = 0;     // entries counted but not kept
};

constexpr size_t kMaxRows = 20000;
constexpr size_t kMaxDepth = 64;  // also bounds recursive totals, emission and destruction
constexpr size_t kPayloadBudget = 500u * 1024u;  // under ipc::kPreviewMaxArchiveChars

struct Node {
    std::wstring name;
    std::vector<std::unique_ptr<Node>> children;
    std::unordered_map<std::wstring, Node*> index;
    uint64_t size = 0;
    uint64_t packed = 0;
    SYSTEMTIME time{};
    bool has_time = false;
    bool time_inferred = false;
    bool dir = false;
    bool encrypted = false;
};

inline Node* Child(Node& parent, const std::wstring& name) {
    if (const auto it = parent.index.find(name); it != parent.index.end()) return it->second;
    auto node = std::make_unique<Node>();
    node->name = name;
    Node* raw = node.get();
    parent.children.push_back(std::move(node));
    parent.index.emplace(name, raw);
    return raw;
}

// Folder totals, newest inner time for folders without their own record
// (common in ZIP), then folders-first natural order.
inline void Finish(Node& node) {
    if (!node.children.empty()) node.dir = true;
    if (!node.dir) return;
    uint64_t total = 0, packed = 0;
    for (auto& child : node.children) {
        Finish(*child);
        total = child->size > UINT64_MAX - total ? UINT64_MAX : total + child->size;
        packed = child->packed > UINT64_MAX - packed ? UINT64_MAX : packed + child->packed;
        if (!node.has_time && child->has_time) {
            node.time = child->time;
            node.has_time = true;
            node.time_inferred = true;
        } else if (node.time_inferred && child->has_time) {
            FILETIME a{}, b{};
            if (SystemTimeToFileTime(&child->time, &a) && SystemTimeToFileTime(&node.time, &b) &&
                CompareFileTime(&a, &b) > 0)
                node.time = child->time;
        }
        node.encrypted = node.encrypted || child->encrypted;
    }
    node.size = total;
    node.packed = packed;
    node.index.clear();
    std::sort(node.children.begin(), node.children.end(),
        [](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
            if (a->dir != b->dir) return a->dir;
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT,
                NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a->name.c_str(),
                static_cast<int>(a->name.size()), b->name.c_str(),
                static_cast<int>(b->name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
}

// Payload rows, pre-order:
//   depth \t flags(d=folder e=encrypted) \t size \t packed|- \t YYYY-MM-DD HH:MM|- \t name
inline bool EmitRows(const Node& node, int depth, bool has_packed, std::wstring& out, size_t& rows) {
    for (const auto& child : node.children) {
        if (rows >= kMaxRows || out.size() > kPayloadBudget) return false;
        ++rows;
        wchar_t head[96]{};
        wchar_t date[24] = L"-";
        if (child->has_time)
            swprintf_s(date, L"%04u-%02u-%02u %02u:%02u", child->time.wYear, child->time.wMonth,
                       child->time.wDay, child->time.wHour, child->time.wMinute);
        std::wstring flags;
        if (child->dir) flags += L'd';
        if (child->encrypted) flags += L'e';
        if (flags.empty()) flags = L"-";
        swprintf_s(head, L"%d\t%s\t%llu\t", depth, flags.c_str(),
                   static_cast<unsigned long long>(child->size));
        out += head;
        if (has_packed) out += std::to_wstring(child->packed);
        else out += L'-';
        out += L'\t';
        out += date;
        out += L'\t';
        std::wstring name = child->name.substr(0, 260);
        for (wchar_t& c : name) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        out += name;
        out += L'\n';
        if (child->dir && !EmitRows(*child, depth + 1, has_packed, out, rows)) return false;
    }
    return true;
}

inline std::wstring Serialize(Listing& listing) {
    Node root;
    root.dir = true;
    for (Entry& entry : listing.entries) {
        std::replace(entry.path.begin(), entry.path.end(), L'\\', L'/');
        size_t depth = 0;
        size_t cursor = 0;
        while (cursor < entry.path.size()) {
            size_t end = entry.path.find(L'/', cursor);
            if (end == std::wstring::npos) end = entry.path.size();
            const std::wstring_view part(entry.path.data() + cursor, end - cursor);
            if (!part.empty() && part != L"." && part != L"..") ++depth;
            if (depth > kMaxDepth) break;
            cursor = end + 1;
        }
        if (depth > kMaxDepth) {
            listing.incomplete = true;
            ++listing.skipped;
            continue;
        }
        Node* node = &root;
        size_t start = 0;
        while (start < entry.path.size()) {
            size_t end = entry.path.find(L'/', start);
            if (end == std::wstring::npos) end = entry.path.size();
            const std::wstring part = entry.path.substr(start, end - start);
            if (!part.empty() && part != L"." && part != L"..") {
                node = Child(*node, part);
                if (end < entry.path.size()) node->dir = true;
            }
            start = end + 1;
        }
        if (node == &root) continue;
        if (entry.dir) {
            node->dir = true;
        } else {
            node->size = entry.size;
            node->packed = entry.packed;
        }
        if (entry.has_time) { node->time = entry.time; node->has_time = true; }
        node->encrypted = node->encrypted || entry.encrypted;
    }
    Finish(root);
    std::wstring rows_text;
    size_t rows = 0;
    const bool complete = EmitRows(root, 0, listing.has_packed, rows_text, rows);
    if (!complete) listing.incomplete = true;
    std::wstring head = L"PULSEARC\t1\t";
    head += listing.format;
    head += L'\t';
    head += listing.has_packed ? std::to_wstring(listing.packed_total) : L"-";
    head += listing.incomplete ? L"\t1\n" : L"\t0\n";
    return head + rows_text;
}

} // namespace pulse::preview::archive_serialization

namespace pulse::preview {
// A missing packed total preserves unknown packed sizes in both header and rows.
inline std::wstring SerializeArchiveTree(std::vector<ArchiveListingEntry> entries,
    const std::wstring& format, bool incomplete,
    std::optional<uint64_t> packed_total = std::nullopt) {
    archive_serialization::Listing listing;
    listing.entries = std::move(entries);
    listing.format = format;
    listing.incomplete = incomplete;
    listing.has_packed = packed_total.has_value();
    listing.packed_total = packed_total.value_or(0);
    return archive_serialization::Serialize(listing);
}
} // namespace pulse::preview
