#include "../app/context_menu_controller.h"
#include "../app/settings_context_tree.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
int main() {
    using namespace pulse;
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        failures += !ok;
    };
    ContextMenuPrefs prefs; prefs.persist = false;
    ContextMenuController controller;
    StaticVerb word; word.verb = L"__openwith"; word.display = L"用 Word 打开";
    word.app_path = L"C:\\Fixture Apps\\Office\\WINWORD.EXE";
    StaticVerb notepad; notepad.verb = L"__openwith"; notepad.display = L"用 Notepad3 打开";
    notepad.app_path = L"C:\\Fixture Apps\\Notepad3.exe";
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\sample.xlsx"}, false, L".xlsx", false, {}, {});
    controller.CompleteStaticVerbs(L".xlsx", {word, notepad}, controller.cache_generation());
    bool changed = false;
    auto rows = controller.BuildDisplay(prefs, {}, changed);
    auto find = [&](const wchar_t* text) -> SeenMenuItem {
        for (const auto& item : prefs.NodeChildren(L"")) if (item.text == text) return item;
        return {};
    };
    const auto word_node = find(word.display.c_str()), notepad_node = find(notepad.display.c_str());
    check(prefs.NodeChildren(L"").size() == 2 && word_node.configurable && notepad_node.configurable &&
          !word_node.key.empty() && !notepad_node.key.empty() && word_node.key != notepad_node.key,
          "actual controller discovers distinct configurable Word and Notepad MRU rows");
    if (word_node.key.empty() || notepad_node.key.empty() || !word_node.configurable || !notepad_node.configurable)
        return 1;
    const auto page = BuildSettingsContextTreePage(prefs);
    auto hide = std::find_if(page.actions.begin(), page.actions.end(), [&](const auto& pair) {
        return pair.second.kind == ContextTreeActionKind::SetVisible && pair.second.key == word_node.key && !pair.second.visible;
    });
    check(hide != page.actions.end(), "settings exposes a hide action for the exact Word row");
    std::wstring parent;
    check(hide != page.actions.end() && ApplySettingsContextTreeAction(prefs, hide->second, parent) == ContextTreeActionResult::Changed,
          "production settings action hides Word without changing a file association");
    auto has = [](const auto& menu, const auto& title) {
        return std::any_of(menu.begin(), menu.end(), [&](const auto& item) { return item.text == title; });
    };
    rows = controller.BuildDisplay(prefs, {}, changed);
    check(!has(rows, word.display) && has(rows, notepad.display), "next actual context menu hides only Word and retains Notepad");
    check(prefs.NodeChildren(L"").size() == 2, "hidden Word remains discoverable for restoration");
    std::filesystem::create_directories(L"bench_data/open-with-settings-20261126/test");
    const auto file = L"bench_data/open-with-settings-20261126/test/preferences.json";
    std::wstring saved; ContextMenuPrefs reloaded; reloaded.persist = false;
    check(WriteUtf8FileAtomic(file, prefs.ToJson()) && ReadUtf8File(file, saved) && reloaded.FromJson(saved),
          "isolated preferences persist and reload through production JSON");
    rows = controller.BuildDisplay(reloaded, {}, changed);
    check(!has(rows, word.display) && has(rows, notepad.display), "hidden MRU choice survives settings reload");
    word.display = L"Open with Word"; word.app_path = L"c:/fixture apps/office/winword.exe";
    controller.CompleteStaticVerbs(L".xlsx", {notepad, word}, controller.cache_generation());
    rows = controller.BuildDisplay(reloaded, {}, changed);
    check(!has(rows, word.display) && has(rows, notepad.display), "MRU order language path casing and separators do not lose the hide choice");
    controller.StartQuery(reloaded, nullptr, {L"C:\\fixture\\sample.docx"}, false, L".docx", false, {}, {});
    controller.CompleteStaticVerbs(L".docx", {word}, controller.cache_generation());
    rows = controller.BuildDisplay(reloaded, {}, changed);
    check(has(rows, word.display), "hiding Word for Excel does not hide Word for another file type");
    controller.StartQuery(reloaded, nullptr, {L"C:\\fixture\\sample.xlsx"}, false, L".xlsx", false, {}, {});
    controller.BuildDisplay(reloaded, {}, changed);
    auto current = BuildSettingsContextTreePage(reloaded);
    auto restore = std::find_if(current.actions.begin(), current.actions.end(), [&](const auto& pair) {
        return pair.second.kind == ContextTreeActionKind::SetVisible && pair.second.key == word_node.key && pair.second.visible;
    });
    check(restore != current.actions.end() && ApplySettingsContextTreeAction(reloaded, restore->second, parent) == ContextTreeActionResult::Changed,
          "settings can restore the hidden Excel Word row after reopening");
    rows = controller.BuildDisplay(reloaded, {}, changed);
    check(has(rows, word.display), "restored choice reaches actual menu output");
    const auto root_page = BuildSettingsContextTreePage(reloaded);
    check(std::any_of(root_page.items.begin(), root_page.items.end(), [](const auto& item) {
        return item.detail.find(L".xlsx") != std::wstring::npos;
    }), "settings identifies the file type beside dynamic open-with rows");
    // Old builds collapsed all MRU entries into one unknown-source tree node.
    auto part = [](const std::wstring& value) { return std::to_wstring(value.size()) + L":" + value; };
    SeenMenuItem obsolete;
    obsolete.tree_node = true; obsolete.configurable = false; obsolete.source = L"registry:0:";
    obsolete.key = L"n:" + part(L"") + part(obsolete.source) + part(L"v:__openwith") + L":v";
    obsolete.text = L"用 Word 打开";
    SeenMenuItem unrelated = obsolete;
    unrelated.key = L"n:" + part(L"") + part(obsolete.source) + part(L"v:unknown") + L":v";
    unrelated.text = L"Unrelated unknown source";
    reloaded.seen.push_back(obsolete); reloaded.seen.push_back(unrelated);
    check(reloaded.FromJson(reloaded.ToJson()), "old dynamic catalog loads through the migration path");
    const auto contains = [&](const auto& key) { return std::any_of(reloaded.seen.begin(), reloaded.seen.end(), [&](const auto& item) { return item.key == key; }); };
    check(!contains(obsolete.key) && contains(unrelated.key) && reloaded.migrated,
          "migration removes only the obsolete ambiguous MRU tree row, not other unknown sources");
    controller.BuildDisplay(reloaded, {}, changed);
    check(!changed, "stable MRU rebuild does not dirty settings repeatedly");
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
