namespace {
template<class T> void SetTreeDepth(T& row, uint8_t depth) {
    if constexpr (requires { row.depth; }) row.depth = depth;
}
int NestedMenuControllerTests() {
    using namespace pulse;
    app::ShellMenuEntry leaf;
    leaf.command = 8123; leaf.text = L"Run"; leaf.mnemonic = L'R';
    app::ShellMenuEntry third; third.text = L"Third"; third.children = {leaf};
    app::ShellMenuEntry second; second.text = L"Second"; second.children = {third};
    app::ShellMenuEntry root; root.text = L"Vendor"; root.children = {second};
    auto is_tree = [](const std::vector<ui::FluentMenuItem>& rows, int command) {
        const std::vector<ui::FluentMenuItem>* level = &rows;
        for (int depth = 0; depth < 3; ++depth) {
            if (level->empty() || (depth > 0 && level->size() != 1) || (*level)[0].children.empty()) return false;
            level = &(*level)[0].children;
        }
        return level->size() == 1 && (*level)[0].command == command;
    };
    std::vector<ui::FluentMenuItem> display;
    app::AppendShellSection(display, {root});
    Check(is_tree(display, 8123), "nested conversion retains four levels and leaf command");
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t invoked = 0;
    app::ContextMenuController::ShellOperations operations;
    operations.query = [](auto, HWND, bool, bool, auto) { return 7u; };
    operations.invoke = [&](uint32_t, uint32_t id, auto, auto) { invoked = id; };
    controller.SetShellOperations(std::move(operations));
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\tree.xyz"}, false, L".xyz", false, {}, {});
    std::vector<ops::ShellMenuItem> rows = {
        Row(0, L"{TREE}", L"", L"Vendor", true),
        Row(0, L"{TREE}", L"", L"Second", true, true),
        Row(0, L"{TREE}", L"", L"Third", true, true),
        Row(123, L"{TREE}", L"run", L"Run", false, true)};
    for (size_t i = 0; i < rows.size(); ++i) SetTreeDepth(rows[i], static_cast<uint8_t>(i));
    controller.CompleteComQuery(7, rows, 1, false);
    bool changed = false;
    display = controller.BuildDisplay(prefs, {}, changed);
    Check(is_tree(display, app::CmdShellComBase + 123), "COM preorder reconstructs nested headers and live leaf");
    controller.ExecuteShellCommand(app::CmdShellComBase + 123, {}, {});
    Check(invoked == 123, "deep COM command invokes exact host ID");
    app::CtxLayoutRecords records;
    records[L".xyz"].rows = rows;
    const auto bytes = app::SerializeCtxLayout(records, L"nested-test");
    app::CtxLayoutRecords parsed;
    Check(app::ParseCtxLayout(bytes.data(), bytes.size(), L"nested-test", parsed), "nested layout cache roundtrip parses");
    bool depths = parsed[L".xyz"].rows.size() == 4;
    for (size_t i = 0; i < parsed[L".xyz"].rows.size(); ++i) {
        auto expected = parsed[L".xyz"].rows[i];
        SetTreeDepth(expected, static_cast<uint8_t>(i));
        auto check_depth = [](const auto& x, const auto& y) {
            if constexpr (requires { x.depth; }) return x.depth == y.depth;
            else return false;
        };
        depths = depths && check_depth(parsed[L".xyz"].rows[i], expected);
    }
    Check(depths, "nested depth survives persistent cache roundtrip");
    // Static descendants use preorder IDs while preserving existing one-level IDs.
    app::StaticVerb run; run.verb = L"run"; run.display = L"Run"; run.command = L"fixture.exe %1";
    app::StaticVerb c; c.verb = L"third"; c.display = L"Third"; c.children = {run};
    app::StaticVerb b; b.verb = L"second"; b.display = L"Second"; b.children = {c};
    app::StaticVerb a; a.verb = L"vendor"; a.display = L"Vendor"; a.children = {b};
    app::ContextMenuController statics;
    std::wstring executed;
    app::ContextMenuController::ShellOperations static_ops;
    static_ops.execute_command = [&](const std::wstring& cmd, const std::wstring&) { executed = cmd; };
    statics.SetShellOperations(std::move(static_ops));
    statics.StartQuery(prefs, nullptr, {L"C:\\fixture\\tree.xyz"}, false, L".xyz", false, {}, {});
    statics.CompleteStaticVerbs(L".xyz", {a}, statics.cache_generation());
    display = statics.BuildDisplay(prefs, {}, changed);
    Check(is_tree(display, app::CmdShellStaticBase + 3), "static nested rows receive stable preorder IDs");
    statics.ExecuteShellCommand(app::CmdShellStaticBase + 3, {}, {});
    Check(executed == run.command, "static deep leaf executes its own command template");
    return failures ? 1 : 0;
}
}
