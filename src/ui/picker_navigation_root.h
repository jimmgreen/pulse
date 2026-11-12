#pragma once
#include "folder_picker_loader.h"
#include "ui_view_model.h"
#include <algorithm>
#include <deque>

namespace pulse::ui {
// Reuses the picker's bounded worker queue and shared sidebar tree rows.
// Paths identify UI nodes, not an authorization or filesystem boundary.
class PickerNavigationRoot {
    struct Node {
        std::wstring path, label;
        bool expanded = false, loaded = false;
        std::vector<Node> children;
    };
public:
    void Set(HWND window, UINT message, std::wstring path, const PickerOptions& options) {
        if (!loader_) loader_ = std::make_unique<PickerLoader>(window, message);
        pending_.clear(); queued_.clear(); ++generation_;
        root_ = {}; root_.path = std::move(path); root_.label = root_.path;
        while (root_.label.size() > 3 && root_.label.back() == L'\\') root_.label.pop_back();
        const auto slash = root_.label.find_last_of(L'\\');
        if (slash != root_.label.npos && slash + 1 < root_.label.size()) root_.label.erase(0, slash + 1);
        if (root_.label.empty()) root_.label = root_.path;
        root_.expanded = true; options_ = options; options_.filters.clear(); options_.search.clear();
        Request(root_);
    }
    bool Active() const noexcept { return !root_.path.empty(); }
    bool RootLoaded() const noexcept { return root_.loaded; }
    const std::wstring& Path() const noexcept { return root_.path; }
    bool Toggle(const std::wstring& path) {
        auto* node = Find(root_, path); if (!node) return false;
        node->expanded = !node->expanded;
        if (node->expanded && !node->loaded) {
            if (pending_.empty()) Request(*node);
            else if (!SamePickerPath(pending_, path) && std::none_of(queued_.begin(), queued_.end(),
                [&](const auto& value) { return SamePickerPath(value, path); })) queued_.push_back(path);
        } else if (!node->expanded) {
            std::erase_if(queued_, [&](const auto& value) { return SamePickerPath(value, path); });
            if (SamePickerPath(pending_, path)) { pending_.clear(); ++generation_; Next(); }
        }
        return true;
    }
    DWORD Complete(LPARAM value) {
        const auto listing = PickerLoader::Take(value);
        if (!listing || listing->generation != generation_) return ERROR_SUCCESS;
        auto* node = Find(root_, pending_);
        if (!node) { pending_.clear(); Next(); return ERROR_SUCCESS; }
        node->children.clear();
        if (!listing->error) for (const auto& entry : listing->entries) {
            if (entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive) continue;
            node->children.push_back({entry.path, entry.name});
        }
        node->loaded = !listing->error; pending_.clear(); Next(); return listing->error;
    }
    void Append(std::vector<SidebarGroup>& sidebar) const {
        SidebarGroup group; group.id = 6;
        Rows(root_, 0, group.items); sidebar.push_back(std::move(group));
    }
    void Close() { loader_.reset(); root_ = {}; pending_.clear(); queued_.clear(); ++generation_; }
private:
    static Node* Find(Node& node, const std::wstring& path) {
        if (SamePickerPath(node.path, path)) return &node;
        for (auto& child : node.children) if (auto* found = Find(child, path)) return found;
        return nullptr;
    }
    static void Rows(const Node& node, int depth, std::vector<SidebarItem>& rows) {
        SidebarItem item; item.path = node.path; item.label = node.label; item.indent = depth;
        item.icon_glyph = L"\xE8B7"; item.expandable = !node.loaded || !node.children.empty(); item.expanded = node.expanded;
        rows.push_back(std::move(item));
        if (node.expanded) for (const auto& child : node.children) Rows(child, depth + 1, rows);
    }
    void Request(const Node& node) { pending_ = node.path; loader_->Load(++generation_, node.path, PickerMode::Folder, options_); }
    void Next() {
        while (!queued_.empty()) {
            auto path = std::move(queued_.front()); queued_.pop_front();
            if (auto* node = Find(root_, path); node && node->expanded && !node->loaded) { Request(*node); break; }
        }
    }
    Node root_;
    PickerOptions options_;
    std::wstring pending_;
    std::deque<std::wstring> queued_;
    uint64_t generation_ = 0;
    std::unique_ptr<PickerLoader> loader_;
};
}
