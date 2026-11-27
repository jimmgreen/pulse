#include "../app/settings_context_tree.h"
#include "../common/localization.h"
#include <cstdio>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
int main() {
    using namespace pulse;
    using namespace pulse::app;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    ContextMenuPrefs prefs; prefs.persist = false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    auto add = [&](const wchar_t* key, const wchar_t* parent, const wchar_t* text, bool configurable = true) {
        SeenMenuItem node; node.tree_node = true; node.key = key; node.parent_key = parent;
        node.root_key = L"h:{fixture}"; node.text = text; node.source = L"fixture";
        node.configurable = configurable; node.flyout = *parent == 0;
        prefs.RecordSeenNode(std::move(node));
    };
    add(L"root", L"", L"Archive");
    add(L"a", L"root", L"Extract here");
    add(L"b", L"root", L"Extract to folder");
    add(L"nested", L"root", L"Advanced");
    add(L"deep", L"nested", L"Check archive");
    add(L"other", L"", L"Other app");
    add(L"ambiguous", L"other", L"Duplicate", false);
    add(L"stable", L"other", L"Stable child");
    std::wstring parent;
    const auto roots = BuildSettingsContextTreePage(prefs);
    check(!roots.items.empty() && !roots.actions.empty(), "observed root entry and action map are discoverable");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::Open, L"root"}, parent) ==
          ContextTreeActionResult::Navigated && parent == L"root", "open submenu uses stable node key");
    auto page = BuildSettingsContextTreePage(prefs, parent);
    check(page.items[0].text == L"Archive", "submenu heading exposes current breadcrumb");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"a", false}, parent) ==
          ContextTreeActionResult::Changed && !prefs.NodeEnabled(L"a") && prefs.NodeEnabled(L"b"),
          "hide affects only the selected sibling");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::MoveUp, L"b"}, parent) ==
          ContextTreeActionResult::Changed && prefs.NodeChildren(L"root")[0].key == L"b", "move uses current sibling order");
    check(prefs.NodeChildren(L"nested")[0].key == L"deep", "sibling reorder preserves descendant ownership");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::Open, L"nested"}, parent) ==
          ContextTreeActionResult::Navigated, "deeper submenu navigation is available");
    check(BuildSettingsContextTreePage(prefs, parent).items[0].text == L"Archive › Advanced",
          "nested heading contains parent and child names");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::Back, L"root"}, parent) ==
          ContextTreeActionResult::Navigated && parent == L"root", "back returns to actual parent");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::ResetGroup, L"root"}, parent) ==
          ContextTreeActionResult::Changed && prefs.NodeEnabled(L"a") && prefs.NodeChildren(L"root")[0].key == L"a",
          "reset restores direct group visibility and original order");
    const auto snapshot = prefs.ToJson();
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"ambiguous", false}, parent) ==
          ContextTreeActionResult::Rejected && prefs.ToJson() == snapshot, "ambiguous item cannot be changed");

    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"gone", false}, parent) ==
          ContextTreeActionResult::Rejected && prefs.ToJson() == snapshot, "stale popup action cannot invent a preference");
    prefs.SetNodeEnabled(L"stable", false);
    prefs.item_enabled[L"ambiguous"] = false; // stale preference from an older unambiguous observation
    const auto mixed_page = BuildSettingsContextTreePage(prefs, L"other");
    bool ambiguous_shown = false;
    for (const auto& row : mixed_page.items) if (row.text == L"Duplicate") ambiguous_shown = row.checked;
    check(ambiguous_shown, "ambiguous stale hide is described as read-only rather than effective hidden state");
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::ResetGroup, L"other"}, parent) ==
          ContextTreeActionResult::Changed && prefs.NodeEnabled(L"stable") && !prefs.item_enabled.count(L"ambiguous"),
          "stable parent resets mixed group and clears stale ambiguous overrides without executing commands");
    auto filtered = BuildSettingsContextTreePage(prefs, L"", L"h:{fixture}");
    bool no_root_reset = true;
    for (const auto& [id, action] : filtered.actions) {
        (void)id;
        if (action.kind == ContextTreeActionKind::ResetGroup) no_root_reset = false;
    }
    check(no_root_reset, "filtered root view does not reset unrelated root groups");
    auto empty = BuildSettingsContextTreePage(prefs, L"", L"unseen-root");
    check(empty.actions.empty() && empty.items.size() >= 3, "unseen root has an explicit open-menu instruction instead of editable guesses");
    prefs.SetItemEnabled(L"h:{fixture}", false);
    auto legacy_hidden_roots = BuildSettingsContextTreePage(prefs);
    bool legacy_hidden = false, restore_available = false;
    for (const auto& row : legacy_hidden_roots.items) {
        if (row.text != L"Archive") continue;
        legacy_hidden = !row.checked && row.detail == l10n::Get(l10n::StringId::ContextTreeHidden);
        for (const auto& command : row.children) {
            const auto found = legacy_hidden_roots.actions.find(command.command);
            if (found != legacy_hidden_roots.actions.end() && found->second.kind == ContextTreeActionKind::SetVisible)
                restore_available = command.enabled && found->second.visible;
        }
    }
    check(legacy_hidden && restore_available, "legacy-hidden root is marked hidden and offers restore");
    prefs.SetNodeEnabled(L"root", false);
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"root", true}, parent) ==
          ContextTreeActionResult::Changed && prefs.NodeEnabled(L"root") &&
          prefs.RowEnabled(L"h:{fixture}", ipc::CtxMenuCategory::Software, false),
          "root restore clears both node and legacy hiding");
    prefs.item_enabled.erase(L"h:{fixture}");
    prefs.SetGroupEnabled(ipc::CtxMenuGroup::Software, false);
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"root", true}, parent) ==
          ContextTreeActionResult::Changed && prefs.software &&
          prefs.RowEnabled(L"h:{fixture}", ipc::CtxMenuCategory::Software, false),
          "category-hidden root restore follows the existing legacy toggle behavior");
    prefs.SetItemEnabled(L"h:{fixture}", false);
    prefs.SetNodeEnabled(L"deep", false);
    check(ApplySettingsContextTreeAction(prefs, {ContextTreeActionKind::SetVisible, L"deep", true}, parent) ==
          ContextTreeActionResult::Changed && prefs.NodeEnabled(L"deep") &&
          !prefs.RowEnabled(L"h:{fixture}", ipc::CtxMenuCategory::Software, false),
          "child restore keeps the legacy parent hidden");
    const auto ancestor_hidden_page = BuildSettingsContextTreePage(prefs, L"nested");
    check(ancestor_hidden_page.items[0].detail.find(L"Archive · " + l10n::Get(l10n::StringId::HiddenInPulse)) !=
          std::wstring::npos, "nested page names the hidden ancestor separately from child own visibility");
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
