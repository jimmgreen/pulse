#include "settings_context_tree.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse::app {
namespace {
using I = l10n::StringId;
const SeenMenuItem* FindNode(const ContextMenuPrefs& prefs, const std::wstring& key) {
    const auto found = std::find_if(prefs.seen.begin(), prefs.seen.end(), [&](const SeenMenuItem& item) {
        return item.tree_node && item.key == key;
    });
    return found == prefs.seen.end() ? nullptr : &*found;
}
bool NodeShown(const ContextMenuPrefs& prefs, const SeenMenuItem& node) {
    return (!node.configurable || prefs.NodeEnabled(node.key)) && (!node.parent_key.empty() ||
        prefs.RowEnabled(node.root_key, node.category, node.from_com));
}
const SeenMenuItem* HiddenAncestor(const ContextMenuPrefs& prefs, const SeenMenuItem* node) {
    for (size_t remaining = prefs.seen.size(); node && remaining > 0; --remaining) {
        if (!NodeShown(prefs, *node)) return node;
        node = FindNode(prefs, node->parent_key);
    }
    return nullptr;
}
std::wstring ShortName(std::wstring text) {
    if (text.size() > 44) {
        text.resize(43);
        if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) text.pop_back();
        text += L"…";
    }
    return text;
}
ui::FluentMenuItem Label(I text) {
    ui::FluentMenuItem item;
    item.text = l10n::Get(text);
    item.enabled = false;
    return item;
}
}
ContextTreePage BuildSettingsContextTreePage(const ContextMenuPrefs& prefs,
    const std::wstring& parent_key, const std::wstring& root_key) {
    ContextTreePage page;
    int next = 1;
    auto action = [&](std::vector<ui::FluentMenuItem>& rows, I text, ContextTreeActionKind kind,
                      const std::wstring& key, bool enabled = true, bool visible = true) {
        ui::FluentMenuItem row;
        row.command = next++;
        row.text = l10n::Get(text);
        row.enabled = enabled;
        page.actions.emplace(row.command, ContextTreeAction{kind, key, visible});
        rows.push_back(std::move(row));
    };
    const auto* parent = FindNode(prefs, parent_key);
    auto heading = Label(I::ContextTreeTitle);
    if (parent) {
        const auto* ancestor = FindNode(prefs, parent->parent_key);
        heading.text = (ancestor ? ShortName(ancestor->text) + L" › " : L"") + ShortName(parent->text);
    }
    heading.detail = l10n::Get(I::ContextTreeScope);
    if (const auto* hidden = HiddenAncestor(prefs, parent)) {
        heading.detail = ShortName(hidden->text) + L" · " + l10n::Get(I::HiddenInPulse) + L" · " + heading.detail;
    }
    heading.separator_after = true;
    page.items.push_back(std::move(heading));
    if (!parent_key.empty()) {
        action(page.items, I::ContextTreeBack, ContextTreeActionKind::Back,
               parent ? parent->parent_key : std::wstring());
    }
    const auto children = prefs.NodeChildren(parent_key, root_key);
    if (children.empty()) page.items.push_back(Label(I::ContextTreeEmpty));
    for (size_t index = 0; index < children.size(); ++index) {
        const auto& child = children[index];
        ui::FluentMenuItem row;
        row.text = ShortName(child.text);
        row.checked = NodeShown(prefs, child);
        row.detail = l10n::Get(!child.configurable ? I::ContextTreeAmbiguous
                              : row.checked ? I::ContextTreeShown : I::ContextTreeHidden);
        constexpr std::wstring_view open_with_prefix = L"openwith:";
        const std::wstring_view source = child.source;
        if (source.starts_with(open_with_prefix)) {
            const auto separator = source.find(L'|', open_with_prefix.size());
            if (separator != source.npos && separator > open_with_prefix.size())
                row.detail = std::wstring(source.substr(open_with_prefix.size(), separator - open_with_prefix.size())) +
                    L" · " + row.detail;
        }
        const bool has_children = !prefs.NodeChildren(child.key).empty();
        if (has_children) action(row.children, I::ContextTreeOpen, ContextTreeActionKind::Open, child.key);
        if (!child.configurable) {
            row.children.push_back(Label(I::ContextTreeAmbiguous));
        } else {
            action(row.children, row.checked ? I::SysCtxHideInPulse : I::SysCtxRestoreInPulse,
                   ContextTreeActionKind::SetVisible, child.key, true, !row.checked);
            action(row.children, I::ContextTreeMoveUp, ContextTreeActionKind::MoveUp, child.key,
                   (parent_key.size() > 0 || root_key.empty()) && index > 0 && children[index - 1].configurable);
            action(row.children, I::ContextTreeMoveDown, ContextTreeActionKind::MoveDown, child.key,
                   (parent_key.size() > 0 || root_key.empty()) && index + 1 < children.size() && children[index + 1].configurable);
        }
        page.items.push_back(std::move(row));
    }
    // A filtered root view may only contain one handler. ResetNodeGroup("")
    // would affect every handler's root, so offer reset only inside a group.
    if (parent && !children.empty()) {
        page.items.back().separator_after = true;
        action(page.items, I::ContextTreeResetGroup, ContextTreeActionKind::ResetGroup, parent_key, parent->configurable);
    }
    auto note = Label(I::ContextTreeObservedOnly);
    page.items.push_back(std::move(note));
    return page;
}
ContextTreeActionResult ApplySettingsContextTreeAction(ContextMenuPrefs& prefs,
    const ContextTreeAction& action, std::wstring& parent_key) {
    const auto* node = FindNode(prefs, action.key);
    if (action.kind == ContextTreeActionKind::Back) {
        if (!action.key.empty() && !node) return ContextTreeActionResult::Rejected;
        parent_key = action.key;
        return ContextTreeActionResult::Navigated;
    }
    if (!node) return ContextTreeActionResult::Rejected;
    if (action.kind == ContextTreeActionKind::Open) {
        if (prefs.NodeChildren(action.key).empty()) return ContextTreeActionResult::Rejected;
        parent_key = action.key;
        return ContextTreeActionResult::Navigated;
    }
    if (!node->configurable) return ContextTreeActionResult::Rejected;
    bool changed = false;
    switch (action.kind) {
    case ContextTreeActionKind::SetVisible:
        changed = prefs.SetNodeEnabled(action.key, action.visible);
        // Only a root restore also restores the legacy handler/category control,
        // matching ToggleUi. Restoring a child never changes its ancestors.
        if (action.visible && node->parent_key.empty() &&
            !prefs.RowEnabled(node->root_key, node->category, node->from_com)) {
            prefs.SetItemEnabled(node->root_key, true);
            prefs.SetGroupEnabled(ipc::GroupOf(node->category), true);
            changed = true;
        }
        break;
    case ContextTreeActionKind::MoveUp:
        changed = prefs.MoveNode(action.key, -1); break;
    case ContextTreeActionKind::MoveDown:
        changed = prefs.MoveNode(action.key, 1); break;
    case ContextTreeActionKind::ResetGroup:
        // Reset only removes Pulse overrides. A stable parent may safely
        // restore a mixed group, including stale overrides on ambiguous rows.
        changed = prefs.ResetNodeGroup(action.key); break;
    default: return ContextTreeActionResult::Rejected;
    }
    return changed ? ContextTreeActionResult::Changed : ContextTreeActionResult::None;
}
} // namespace pulse::app
