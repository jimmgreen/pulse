#pragma once

#include "details_tree_rows.h"
#include "folder_picker_model.h"
#include <cwctype>
#include <unordered_map>

namespace pulse::ui {

// UI-thread state only. The caller performs individual folder reads and owns
// monotonically increasing request tokens, including across Reset/Clear.
class PickerDetailsTree {
public:
    struct View {
        std::vector<PickerEntry> entries;
        fs::SnapshotPtr snapshot;
        std::shared_ptr<DetailsTreeRows> rows;
    };
    void Clear() {
        root_ = {};
        CollapseAll();
    }
    void CollapseAll() {
        nodes_.clear();
        ++revision_;
    }
    void Reset(const PickerListing& root, bool allow_expansion = true) {
        Clear();
        root_ = root;
        allow_expansion_ = allow_expansion;
    }
    bool Expanded(std::wstring_view path) const { return nodes_.contains(Key(path)); }
    bool Toggle(std::wstring_view path) {
        const auto key = Key(path);
        if (nodes_.contains(key)) {
            for (auto it = nodes_.begin(); it != nodes_.end();) {
                if (it->first == key || (it->first.starts_with(key) &&
                    (key.ends_with(L'\\') || it->first[key.size()] == L'\\')))
                    it = nodes_.erase(it);
                else ++it;
            }
            ++revision_;
            return true;
        }
        if (nodes_.size() >= kExtraRowBudget || !FindExpandable(root_, key, 0)) return false;
        nodes_.try_emplace(key);
        ++revision_;
        return true;
    }
    bool NeedsLoad(std::wstring_view path) const {
        const auto it = nodes_.find(Key(path));
        return it != nodes_.end() && !it->second.loading && !it->second.listing;
    }
    void BeginLoad(std::wstring_view path, uint64_t token, uint64_t since_ms = 0) {
        const auto it = nodes_.find(Key(path));
        if (it == nodes_.end() || token <= last_token_) return;
        last_token_ = token;
        auto& node = it->second;
        node.token = token;
        node.since_ms = since_ms;
        node.loading = true;
        node.listing.reset();
        ++revision_;
    }
    bool Complete(std::unique_ptr<PickerListing> listing) {
        if (!listing) return false;
        const auto it = nodes_.find(Key(listing->path));
        if (it == nodes_.end() || !it->second.loading ||
            listing->generation != it->second.token) return false;
        it->second.loading = false;
        it->second.listing = std::move(listing);
        ++revision_;
        return true;
    }
    void ShowMore(std::wstring_view path, bool all = false) {
        const auto it = nodes_.find(Key(path));
        if (it == nodes_.end() || !it->second.listing) return;
        auto& node = it->second;
        node.limit = all ? node.listing->entries.size()
                        : (std::min)(node.limit + kTreeMoreStep, node.listing->entries.size());
        ++revision_;
    }
    View Build() const {
        View view;
        auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
        view.rows = std::make_shared<DetailsTreeRows>();
        view.rows->revision = revision_;
        size_t budget = kExtraRowBudget;
        AppendListing(root_, 0, root_.entries.size(), budget, view, *snapshot);
        view.snapshot = std::move(snapshot);
        return view;
    }

private:
    static constexpr size_t kExtraRowBudget = 10000;
    struct Node {
        uint64_t token = 0;
        uint64_t since_ms = 0;
        size_t limit = kTreeMoreStep;
        bool loading = false;
        std::unique_ptr<PickerListing> listing;
    };
    PickerListing root_;
    std::unordered_map<std::wstring, Node> nodes_;
    uint64_t revision_ = 0;
    uint64_t last_token_ = 0;
    bool allow_expansion_ = true;

    static std::wstring Key(std::wstring_view path) {
        std::wstring key(path);
        for (auto& ch : key) ch = ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch));
        if (key.starts_with(L"\\\\?\\unc\\")) key = L"\\\\" + key.substr(8);
        else if (key.starts_with(L"\\\\?\\")) key.erase(0, 4);
        while (key.size() > 3 && key.ends_with(L'\\')) key.pop_back();
        return key;
    }
    bool Expandable(const PickerEntry& entry, uint16_t depth) const {
        return allow_expansion_ && depth < kTreeGuideMaxLevels && !entry.archive &&
            (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive) &&
            IsPickerDiskPath(entry.path);
    }
    bool FindExpandable(const PickerListing& listing, const std::wstring& key, uint16_t depth) const {
        for (const auto& entry : listing.entries) {
            if (!Expandable(entry, depth)) continue;
            const auto entry_key = Key(entry.path);
            if (entry_key == key) return true;
            const auto it = nodes_.find(entry_key);
            if (it != nodes_.end() && it->second.listing &&
                FindExpandable(*it->second.listing, key, static_cast<uint16_t>(depth + 1))) return true;
        }
        return false;
    }
    static void Append(PickerEntry entry, fs::DirEntry disk, DetailsTreeRow facts,
                       View& view, std::vector<fs::DirEntry>& snapshot) {
        entry.source = view.entries.size();
        if (disk.full_path.empty()) disk.full_path = entry.path;
        view.rows->view_of_source.push_back(static_cast<int>(view.entries.size()));
        view.entries.push_back(std::move(entry));
        snapshot.push_back(std::move(disk));
        view.rows->rows.push_back(facts);
    }
    static void Placeholder(const PickerEntry& parent, uint16_t depth, TreeRowKind kind,
                            const Node& node, size_t shown, View& view,
                            std::vector<fs::DirEntry>& snapshot) {
        PickerEntry entry;
        entry.path = parent.path;
        entry.kind = PickerEntryKind::File;
        fs::DirEntry disk;
        DetailsTreeRow facts;
        facts.depth = depth;
        facts.kind = kind;
        facts.since_ms = node.since_ms;
        facts.shown = static_cast<uint32_t>(shown);
        facts.total = node.listing ? static_cast<uint32_t>(node.listing->entries.size()) : 0;
        Append(std::move(entry), std::move(disk), facts, view, snapshot);
    }
    size_t AppendListing(const PickerListing& listing, uint16_t depth, size_t limit,
                        size_t& budget, View& view, std::vector<fs::DirEntry>& snapshot) const {
        size_t shown = 0;
        const size_t count = (std::min)(limit, listing.entries.size());
        for (; shown < count && (depth == 0 || budget > 0); ++shown) {
            if (depth > 0) --budget;
            const auto& entry = listing.entries[shown];
            fs::DirEntry disk;
            if (listing.snapshot && shown < listing.snapshot->size()) disk = (*listing.snapshot)[shown];
            else {
                disk.name = entry.name;
                disk.is_dir = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive;
                disk.size = entry.size;
                disk.mtime = entry.modified;
                disk.attrs = disk.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            }
            DetailsTreeRow facts;
            facts.depth = depth;
            facts.expandable = Expandable(entry, depth);
            const auto it = nodes_.find(Key(entry.path));
            facts.expanded = facts.expandable && it != nodes_.end();
            if (facts.expanded && it->second.listing)
                facts.total = static_cast<uint32_t>(it->second.listing->entries.size());
            Append(entry, std::move(disk), facts, view, snapshot);
            if (!facts.expanded) continue;
            const auto& node = it->second;
            const auto child_depth = static_cast<uint16_t>(depth + 1);
            if (node.loading || !node.listing)
                Placeholder(entry, child_depth, TreeRowKind::Loading, node, 0, view, snapshot);
            else if (node.listing->error != ERROR_SUCCESS)
                Placeholder(entry, child_depth, TreeRowKind::Failed, node, 0, view, snapshot);
            else if (node.listing->entries.empty())
                Placeholder(entry, child_depth, TreeRowKind::Empty, node, 0, view, snapshot);
            else {
                const size_t child_shown = AppendListing(*node.listing, child_depth, node.limit, budget, view, snapshot);
                if (child_shown < node.listing->entries.size())
                    Placeholder(entry, child_depth, TreeRowKind::More, node, child_shown, view, snapshot);
            }
        }
        return shown;
    }
};

} // namespace pulse::ui
