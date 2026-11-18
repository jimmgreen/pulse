int ShellActionRoutingTests() {
    namespace app = pulse::app;
    for (const wchar_t* name : {L"install", L"InStAlL", L"print", L"PRINT", L"edit"}) {
        app::ContextMenuController controller;
        app::ContextMenuPrefs prefs; prefs.persist = false;
        std::wstring by_verb, by_command;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 17u; };
        operations.execute_verb = [&](const auto&, const auto& verb) { by_verb = verb; };
        operations.execute_command = [&](const auto& command, const auto&) { by_command = command; };
        controller.SetShellOperations(std::move(operations));
        app::StaticVerb row; row.verb = name; row.display = name;
        row.command = L"rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 %1";
        controller.CompleteStaticVerbs(L".inf", {row}, controller.cache_generation());
        controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\setup.inf"}, false, L".inf", false, {}, {});
        controller.OpenMenu({}); controller.CloseMenu();
        controller.ExecuteShellCommand(app::CmdShellStaticBase, {}, {});
        const bool shell = row.verb != L"edit";
        Check(shell ? by_verb == row.verb && by_command.empty() : by_verb.empty() && by_command == row.command,
            shell ? "install/print preserve Windows verb dispatch instead of raw command" : "unrelated command dispatch remains unchanged");
    }
    return failures ? 1 : 0;
}
