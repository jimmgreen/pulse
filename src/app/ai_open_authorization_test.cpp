#ifdef PULSE_WITH_SELFTEST
#include "ai_agent_internal.h"

namespace pulse::app {

void RunAiOpenAuthorizationTest(void (*check)(bool, const wchar_t*), const std::wstring& fixture_root) {
    auto owner = std::make_unique<AppState>();
    AppState& s = *owner;
    s.places.persist = false;
    s.appPrefs.persist = false;
    s.isolatedTest = true;
    LayoutTab& layout = LiveLayout(s);
    layout.panes.clear();
    layout.panes.push_back(std::make_unique<Pane>());
    s.pane = layout.panes.front().get();
    layout.root = SplitContainer::CreateLeaf(s.pane);
    layout.focused_index = 0;
    Tab& tab = s.pane->view;
    tab.current_path = fixture_root + std::to_wstring(GetCurrentProcessId());
    const std::wstring first = tab.current_path + L"\\first.exe";
    const std::wstring second = tab.current_path + L"\\second.cmd";
    const std::wstring document = tab.current_path + L"\\document.txt";
    // OpenPath returns before calling the OS for missing files. Never create
    // these paths: even an unfixed runner cannot launch an external process.
    const bool missing = !ai_agent::Exists(first) && !ai_agent::Exists(second) && !ai_agent::Exists(document);
    check(missing, L"ai open: external-action fixtures do not exist and cannot be launched");
    if (!missing) return;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(3);
    (*rows)[0].name = L"first.exe";
    (*rows)[1].name = L"second.cmd";
    (*rows)[2].name = L"document.txt";
    tab.SetSnapshot(rows);
    tab.SelectOnly(0);
    ai_agent::CtlRun unbound;
    const ai_agent::CtlStep refused = ai_agent::CtlOpenFile(s, unbound, json::Value::Object());
    check(!refused.ok, L"ai open: shared execution refuses an open that has no preflight authorization");

    auto open_step = [](const std::wstring& target) {
        json::Value args = json::Value::Object();
        if (!target.empty()) args.set("path", json::Utf8(target));
        json::Value step = json::Value::Object();
        step.set("action", "nav.open");
        step.set("args", std::move(args));
        return step;
    };
    ai_agent::CtlRun view_only;
    view_only.queue.push_back(open_step({}));
    view_only.mcp = std::make_shared<ai::McpActionsRun>();
    view_only.mcp->allowed[static_cast<size_t>(actions::McpGroup::View)] = true;
    tab.SelectOnly(2);
    check(ai_agent::BindOpenTargets(s, view_only).empty(),
          L"ai open: an ordinary selected document can be bound with only the view permission");
    tab.SelectOnly(0);
    const auto changed_to_program = ai_agent::CtlOpenFile(s, view_only, json::Value::Object());
    check(changed_to_program.ok && changed_to_program.arg == L"document.txt" &&
              ai_agent::BoundOpenTarget(view_only, 0) == document,
          L"ai open: changing a document selection to a program cannot gain external authority");

    ai_agent::CtlRun selected;
    selected.queue.push_back(open_step({}));
    const bool selected_bound = ai_agent::BindOpenTargets(s, selected).empty() &&
                                ai_agent::BoundOpenTarget(selected, 0) == first;
    check(selected_bound,
          L"ai open: preflight binds the selected item to its full path");
    if (!selected_bound) return;
    tab.SelectOnly(1);
    tab.current_path += L"\\elsewhere";
    const auto unconfirmed = ai_agent::CtlOpenFile(s, selected, json::Value::Object());
    check(!unconfirmed.ok && unconfirmed.error.find("confirmation") != std::string::npos &&
              ai_agent::BoundOpenTarget(selected, 0) == first,
          L"ai open: selection and navigation changes cannot replace the target or supply confirmation");
    selected.open_targets[0].confirmed = true;
    selected.mcp = std::make_shared<ai::McpActionsRun>();
    selected.mcp->allowed[static_cast<size_t>(actions::McpGroup::View)] = true;
    const auto external_off = ai_agent::CtlOpenFile(s, selected, json::Value::Object());
    check(!external_off.ok && external_off.error.find("nav.open") != std::string::npos,
          L"ai open: even a confirmed target is refused at execution when MCP external is off");
    selected.mcp->allowed[static_cast<size_t>(actions::McpGroup::External)] = true;
    const auto bound_mcp = ai_agent::CtlOpenFile(s, selected, json::Value::Object());
    check(bound_mcp.ok && bound_mcp.arg == L"first.exe",
          L"ai open: MCP execution uses the authorized target after the selection and folder changed");
    selected.mcp.reset();
    const auto bound_assistant = ai_agent::CtlOpenFile(s, selected, json::Value::Object());
    check(bound_assistant.ok && bound_assistant.arg == L"first.exe",
          L"ai open: the assistant shares the same bound execution target");

    ai_agent::CtlRun layout_run;
    json::Value layout_step = json::Value::Object();
    layout_step.set("action", "window.layout");
    layout_run.queue.push_back(layout_step);
    layout_run.queue.push_back(open_step(first));
    const bool layout_bound = ai_agent::BindOpenTargets(s, layout_run).empty();
    check(layout_bound,
          L"ai open: a full-path open remains valid after another batch step");
    if (!layout_bound) return;
    layout_run.open_targets[1].confirmed = true;
    json::Value layout_args = json::Value::Object();
    layout_args.set("layout", "single");
    json::Value panes = json::Value::Array();
    panes.push(json::Utf8(tab.current_path));
    layout_args.set("panes", std::move(panes));
    const auto expanded = ai_agent::CtlLayout(s, layout_run, layout_args);
    check(expanded.ok && layout_run.queue.size() == 3 && ai_agent::BoundOpenTarget(layout_run, 1).empty() &&
              ai_agent::BoundOpenTarget(layout_run, 2) == first && layout_run.open_targets[2].confirmed,
          L"ai open: inserted layout navigation cannot shift an authorization onto a different step");

    json::Value steps = json::Value::Array();
    steps.push(open_step(first));
    steps.push(open_step(second));
    json::Value request = json::Value::Object();
    request.set("steps", std::move(steps));
    ai::ModelToolCall call;
    call.id = "ai-open-authorization-test";
    ai_agent::ToolRunActions(s, call, request);
    AiChat& chat = Chat(s);
    const ui::AiBlockView* first_card = chat.Find(chat.confirm_id);
    check(first_card && (first_card->file == first || first_card->file_note.find(first) != std::wstring::npos),
          L"ai open: the assistant confirmation shows the exact full target path");
    tab.ClearSelection(); // The user may change selection while the card waits.
    if (chat.on_confirm) {
        const auto accept_first = chat.on_confirm;
        accept_first(s, true);
    }
    const ui::AiBlockView* second_card = chat.Find(chat.confirm_id);
    check(second_card && (second_card->file == second || second_card->file_note.find(second) != std::wstring::npos),
          L"ai open: approving the first program asks for the second target before either runs");
    ++ai_agent::Agent(s).generation;
    chat.on_confirm = nullptr;
    chat.queue.clear();
}

} // namespace pulse::app
#endif
