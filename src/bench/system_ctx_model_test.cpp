#include "../app/system_ctx_model.h"
#include "../ipc/ctx_menu_util.h"
#include "../common/localization.h"
#include <cstdio>
#include <chrono>

namespace pulse::app {
struct SystemCtxModelTestPeer {
    static void Seed(SystemCtxModel& model) {
        model.loaded_ = true;
        model.state_ = ui::SysCtxView::State::Ready;
        model.overview_ = false;
        model.nav_index_ = 0;
        model.entries_.resize(model.scene_ids_.size());
        model.scene_keys_.resize(model.scene_ids_.size());
        for (const auto* name : {L"NanaZip", L"Bandizip", L"Notepad"}) {
            SystemCtxModel::Entry entry;
            entry.item.kind = sysctx::Kind::ShellEx;
            entry.item.source = sysctx::Source::Packaged;
            entry.item.display = name;
            entry.item.key_name = name;
            entry.item.key_path = name;
            entry.item.clsid = std::wstring(L"{") + name + L"}";
            model.entries_[0].push_back(std::move(entry));
        }
        model.dirty_ = true;
    }
    static void SeedRegistryTree(SystemCtxModel& model) {
        auto& list = model.entries_[0];
        list.clear();
        SystemCtxModel::Entry root;
        root.item.kind = sysctx::Kind::Verb;
        root.item.display = L"Archive"; root.item.key_path = L"*\\shell\\archive";
        root.item.tree_id = L"root"; root.item.has_children = true;
        root.item.registry_path = L"Software\\Classes\\*\\shell\\archive";
        list.push_back(root);
        for (const auto* id : {L"child-a", L"child-b"}) {
            auto child = root;
            child.item.display = L"Extract";
            child.item.key_path = L"commandstore:shared";
            child.item.registry_path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\shared";
            child.item.tree_id = id; child.item.parent_id = L"root";
            child.item.depth = 1; child.item.registry_read_only = true;
            list.push_back(child);
        }
        model.dirty_ = true;
    }
    static void RootName(SystemCtxModel& model, std::wstring name) {
        model.entries_[0][0].item.display = std::move(name); model.dirty_ = true;
    }
    static size_t SceneCount(SystemCtxModel& model) { return model.scene_ids_.size(); }
    static int CustomScene(SystemCtxModel& model) { return model.custom_extension_scene_; }
    static void Loading(SystemCtxModel& model) {
        model.state_ = ui::SysCtxView::State::Loading;
        model.dirty_ = true;
    }
};
}
int main() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    app::SystemCtxModel model;
    app::SystemCtxModelTestPeer::Seed(model);
    auto view = model.View();
    check(view->rows.size() == 3 && view->rows[0].read_only && view->rows[0].pulse == 1,
          "packaged Windows read-only entry retains Pulse visibility");
    model.ToggleRow(0);
    check(model.View()->pending == 0, "Windows toggle does not queue a packaged registry write");
    model.TogglePulseRow(0);
    check(model.View()->pending_pulse == 1 && model.RowPulseHidden(0), "Pulse hide is queued without registry edits");
    model.TogglePulseRow(0);
    check(model.View()->pending == 0 && !model.RowPulseHidden(0), "second click cancels queued hide");
    model.TogglePulseRow(0);
    model.DiscardPending();
    check(model.View()->pending == 0 && !model.RowPulseHidden(0), "discard restores original Pulse visibility");
    model.TogglePulseRow(0);
    const auto changes = model.TakePulseChanges();
    check(changes.hidden.size() == 1 && changes.hidden[0].second && changes.skipped.empty(),
          "apply returns exactly one Pulse preference and no disabled COM handler");
    const auto first = model.RowPulseKey(0), second = model.RowPulseKey(1), third = model.RowPulseKey(2);
    model.SetPulseHidden({third, first, second, first});
    check(model.RowPulseHidden(0) && model.RowPulseHidden(1) && model.RowPulseHidden(2),
          "unsorted duplicate hidden keys normalize before binary search");
    model.TogglePulseRow(0);
    check(!model.RowPulseHidden(0) && model.View()->pending_pulse == 1, "hidden entry queues restore in Pulse");
    const auto restore = model.TakePulseChanges();
    check(restore.hidden.size() == 1 && !restore.hidden[0].second, "restore persists enabled state");
    model.SelectScene(1);
    check(model.View()->rows.empty(), "scene selection isolates unrelated menu entries");
    model.SelectScene(0);
    check(model.View()->rows.size() == 3, "returning to the scene keeps its entries");
    app::SystemCtxModelTestPeer::Loading(model);
    model.TogglePulseRow(0);
    check(model.View()->pending == 0, "loading state rejects stale Pulse row actions");
    app::SystemCtxModel tree;
    app::SystemCtxModelTestPeer::Seed(tree);
    app::SystemCtxModelTestPeer::SeedRegistryTree(tree);
    auto tree_view = tree.View();
    check(tree_view->rows.size() == 3 && tree_view->preview.size() == 1,
          "registry descendants are listed but not flattened into top-level preview");
    tree.SelectRow(1); tree_view = tree.View();
    check(tree_view->rows[1].selected && !tree_view->rows[2].selected && tree_view->rows[1].depth == 1,
          "same CommandStore key in two occurrences has distinct selection identity");
    check(tree.RowPulseTreeRoot(0) == ipc::CatalogKey(L"Archive", true),
          "registry cascade root uses the actual flyout catalog identity");
    check(tree.RowPulseKey(1).empty() && tree.RowPulseTreeRoot(1) == tree.RowPulseKey(0),
          "registry child cannot hide by guessed label and resolves observed-tree ancestor root");
    check(tree.RowRegistryPath(1) == LR"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\CommandStore\shell\shared)",
          "CommandStore details use its real registry location rather than Classes prefix");
    app::SystemCtxModelTestPeer::RootName(tree, L"7-Zip");
    tree.View();
    check(tree.RowPulseTreeRoot(0) == ipc::CompressCatalogKey() && tree.RowPulseTreeRoot(1) == ipc::CompressCatalogKey(),
          "compression vendor root and descendants resolve the production merged root key");
    check(app::SystemCtxModel::NormalizeExtension(L" DWG ") == L".dwg" &&
          app::SystemCtxModel::NormalizeExtension(L".psd") == L".psd", "custom extension normalizes case and optional dot");
    check(app::SystemCtxModel::NormalizeExtension(L"..\\dwg").empty() &&
          app::SystemCtxModel::NormalizeExtension(L"*.dwg").empty() &&
          app::SystemCtxModel::NormalizeExtension(L".").empty(), "custom extension rejects paths wildcards and empty suffix");
    auto ready = [](app::SystemCtxModel& model) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < until) {
            if (model.View()->state == ui::SysCtxView::State::Ready) return true;
            Sleep(10);
        }
        return false;
    };
    check(!model.SelectExtension(L".psd"), "loading state rejects a competing scene mutation");
    const auto scene_count = app::SystemCtxModelTestPeer::SceneCount(tree);
    check(tree.SelectExtension(L".dwg"), "custom extension schedules worker scan");
    tree.SelectScene(0);
    check(ready(tree) && tree.View()->scene == 0, "worker completion preserves navigation to a different scene");
    const auto custom_scene = app::SystemCtxModelTestPeer::CustomScene(tree);
    tree.SelectScene(0);
    check(tree.View()->rows.size() == 3, "targeted scan preserves unrelated scene entries");
    check(tree.SelectExtension(L".psd") && ready(tree), "second custom extension rescans its bounded scene slot");
    check(app::SystemCtxModelTestPeer::SceneCount(tree) == scene_count + 1 &&
          app::SystemCtxModelTestPeer::CustomScene(tree) == custom_scene,
          "custom types reuse one scene rather than growing thousands of chips");
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
