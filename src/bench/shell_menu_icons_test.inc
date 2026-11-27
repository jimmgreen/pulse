#include "../ipc/protocol.h"

namespace {
#ifndef PULSE_BASELINE_MENU_CONTROLLER
int BaseFlyoutReplacementTests() {
    using namespace pulse;
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    auto old_child = MenuRow(901, L"Existing type");
    auto new_child = MenuRow(902, L"Loaded type");
    ui::FluentMenuItem flyout; flyout.text = L"New fixture"; flyout.children = {old_child};
    auto sibling = MenuRow(903, L"Sibling");
    std::vector<ui::FluentMenuItem> base = {flyout, sibling};
    controller.OpenMenu(base);
    bool changed = false;
    const auto initial = controller.BuildOpenDisplay(prefs, base, changed);
    int attempts = 0;
    auto reject = [&](std::vector<ui::FluentMenuItem> candidate) {
        ++attempts;
        Check(candidate[0].children.size() == 2 && candidate[0].children[1].command == 902,
              "replacement prepares new children before asking renderer");
        return false;
    };
    Check(!controller.TryReplaceBaseFlyout(flyout.text, {old_child, new_child}, prefs, reject) &&
          controller.base_items()[0].children.size() == 1,
          "renderer rejection preserves base command mapping");
    const auto unchanged = controller.ReconcileDisplay(prefs, changed, 1000);
    Check(unchanged[0].children.size() == 1 && unchanged[0].children[0].command == 901 &&
          unchanged[1].command == 903 && unchanged.size() == initial.size(),
          "renderer rejection preserves committed shape and sibling position");
    Check(controller.TryReplaceBaseFlyout(flyout.text, {old_child, new_child}, prefs,
              [&](auto candidate) { ++attempts; return candidate[0].children.size() == 2; }) &&
          controller.base_items()[0].children.size() == 2 && attempts == 2,
          "retry acceptance commits replacement children atomically");
    const auto committed = controller.ReconcileDisplay(prefs, changed, 1000);
    Check(committed[0].children.size() == 2 && committed[0].children[1].command == 902 &&
          committed[1].command == 903,
          "accepted replacement persists without shifting sibling rows");
    Check(!controller.TryReplaceBaseFlyout(L"Missing", {new_child}, prefs, reject) && attempts == 2,
          "absent target never calls renderer or changes base menu");
    controller.CloseMenu();
    Check(!controller.TryReplaceBaseFlyout(flyout.text, {new_child}, prefs, reject) && attempts == 2,
          "closed menu never accepts a late replacement");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
#endif

int ShellMenuIconsTests() {
    using namespace pulse;
    auto icon = std::make_shared<common::MenuIconPixels>();
    icon->width = icon->height = 1;
    icon->bgra = {16, 32, 64, 255};
    app::ShellMenuEntry leaf;
    leaf.command = 7101; leaf.text = L"Vendor action";
    app::ShellMenuEntry second;
    second.text = L"Vendor group"; second.children = {leaf};
    app::ShellMenuEntry root;
    root.text = L"Vendor"; root.children = {second};
    std::vector<ui::FluentMenuItem> display;
    app::AppendShellSection(display, {root});
    Check(display.size() == 1 && !display[0].icon && display[0].vector_icon == ui::StoreIcon::None,
          "missing provider image stays missing without invented category icon");
    root.icon = icon; root.children[0].icon = icon; root.children[0].children[0].icon = icon;
    display.clear(); app::AppendShellSection(display, {root});
    Check(display[0].icon == icon && display[0].vector_icon == ui::StoreIcon::None &&
          display[0].children[0].icon == icon &&
          display[0].children[0].children[0].icon == icon,
          "real provider bitmap wins at every submenu depth");
    std::vector<ops::ShellMenuItem> wire = {Item(1, L"Root"), Item(2, L"Child"), Item(3, L"Leaf")};
    for (auto& row : wire) row.icon = icon;
    ipc::PayloadWriter writer; ipc::PutCtxItemIcons(writer, wire);
    auto received = wire;
    for (auto& row : received) row.icon.reset();
    ipc::PayloadReader reader(writer.data().data(), writer.data().size());
    bool decoded = ipc::TryCtxItemIcons(reader, received);
    for (const auto& row : received) decoded = decoded && row.icon && row.icon->bgra == icon->bgra;
    Check(decoded && reader.remaining() == 0, "IPC preserves distinct row slots at every depth");
    std::vector<ops::ShellMenuItem> old_host = {Item(1, L"Legacy")};
    ipc::PayloadReader absent(nullptr, 0);
    Check(ipc::TryCtxItemIcons(absent, old_host) && !old_host[0].icon,
          "older host without optional icon field remains accepted");

    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t session = 0;
    app::ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { return ++session; };
    controller.SetShellOperations(std::move(operations));
    controller.Close();
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\icons.xyz"}, false, L".xyz", false, {}, {});
    auto cached = Item(1, L"Vendor action"); cached.clsid = L"{ICONS}";
    controller.CompleteComQuery(session, {cached}, 1, false);
    controller.TakeComLayoutDirty();
    controller.Close();
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\icons.xyz"}, false, L".xyz", false, {}, {});
    cached.icon = icon;
    controller.CompleteComQuery(session, {cached}, 2, false);
    Check(!controller.TakeComLayoutDirty(), "icon-only change does not rewrite iconless disk layout");
    controller.Close();
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\icons.xyz"}, false, L".xyz", false, {}, {});
    Check(controller.com_items().size() == 1 && controller.com_items()[0].icon == icon,
          "next menu retains live icon even when cached layout labels did not change");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
}
