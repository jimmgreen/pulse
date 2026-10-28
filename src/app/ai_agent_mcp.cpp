// ai_agent_mcp.cpp - Pulse 助手's run_actions for AI apps (ai_mcp_run.h): timers, questions and the run.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"
#include "ai_confirm.h"

namespace pulse {
namespace ai_agent {

// ---- P5: the same runs for AI apps (ai_mcp_run.h) ----------------------------
// An MCP run has no chat: no step rows or cards; its waits run on thread
// timers of its own (the chat's queue belongs to the assistant's turn and a
// new chat clears it), and its questions are Pulse's confirm dialog.

bool RunGone(AppState& s, const CtlRun& run) {
    if (run.mcp) return run.mcp->cancelled.load();
    return Agent(s).generation != run.gen;
}

struct McpTimerJob {
    HWND hwnd = nullptr;
    AppState* state = nullptr;
    std::shared_ptr<app::ai::McpActionsRun> mcp;
    std::function<int(AppState&)> fn;
};

std::map<UINT_PTR, McpTimerJob>& McpTimers() {
    static std::map<UINT_PTR, McpTimerJob> jobs;   // UI thread only
    return jobs;
}

void CALLBACK McpTick(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    auto& jobs = McpTimers();
    const auto it = jobs.find(id);
    if (it == jobs.end()) return;
    McpTimerJob job = std::move(it->second);
    jobs.erase(it);
    if (job.hwnd && !IsWindow(job.hwnd)) {
        if (job.mcp) app::ai::FinishMcpRun(*job.mcp, ToolOutcome::Fail("unsupported", "The Pulse window closed during the call."));
        return;
    }
    if (job.mcp && job.mcp->cancelled) return;
    const int again = job.fn(*job.state);
    if (again >= 0) McpLater(*job.state, std::move(job.mcp), static_cast<uint32_t>(again), std::move(job.fn));
}

void McpLater(AppState& s, std::shared_ptr<app::ai::McpActionsRun> mcp, uint32_t delay, std::function<int(AppState&)> fn) {
    const UINT_PTR id = SetTimer(nullptr, 0, delay ? delay : 1, McpTick);
    if (!id) {
        if (mcp) app::ai::FinishMcpRun(*mcp, ToolOutcome::Fail("internal", "Pulse could not schedule the call.", true));
        return;
    }
    McpTimers()[id] = {s.hwnd, &s, std::move(mcp), std::move(fn)};
}

// ---- P5: an MCP run's questions and end -------------------------------------

// The small print every question for an AI app ends with.
std::wstring McpFrom(const CtlRun& run) { return Fill(Text(I::AiMcpAskFrom), L"{client}", run.mcp->client); }

// "step 2 (file.move_to)": how the AI app hears which step it was.
std::string McpWhat(const CtlRun& run, size_t index) {
    const std::string id = index < run.queue.size() ? StepAction(run.queue[index]) : std::string();
    return "step " + std::to_string(index + 1) + " (" + id + ")";
}

// Asks for an MCP run with Pulse's confirm dialog, from a timer so it never
// nests inside the step that asks. A question the call's thread closes (2
// minutes, a cancel) answers Cancel; a cancelled call hears nothing more.
void McpAsk(AppState& s, std::shared_ptr<CtlRun> run, ui::ConfirmDialogSpec spec,
            std::function<void(AppState&, ui::ConfirmChoice)> done) {
    std::shared_ptr<app::ai::McpActionsRun> mcp = run->mcp;
    McpLater(s, mcp, 1, [mcp, spec = std::move(spec), done = std::move(done)](AppState& s2) mutable {
        std::weak_ptr<app::ai::McpActionsRun> weak = mcp;
        spec.on_open = [weak](HWND dialog) {
            app::ai::NoteMcpQuestion(dialog);   // the AI chip brings it forward
            const auto m = weak.lock();
            if (!m) return;
            std::lock_guard lock(m->mu);
            m->dialog = dialog;
            m->dialog_since = GetTickCount64();
            if (dialog && (m->cancelled || m->timed_out)) PostMessageW(dialog, WM_CLOSE, 0, 0);
            m->cv.notify_all();
        };
        // A question needs a visible Pulse to sit on (it may be in the tray).
        if (!IsWindowVisible(s2.hwnd) || IsIconic(s2.hwnd)) RevealWindow(s2);
        if (GetForegroundWindow() != s2.hwnd) {
            FLASHWINFO flash{static_cast<UINT>(sizeof(FLASHWINFO)), s2.hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
            FlashWindowEx(&flash);
        }
        const ui::ConfirmChoice choice = ui::ShowConfirmDialogEx(s2.hwnd, spec, s2.darkMode, s2.accentColor);
        {
            std::lock_guard lock(mcp->mu);
            app::ai::ForgetMcpQuestion(mcp->dialog);
        }
        if (mcp->cancelled) return -1;
        done(s2, choice);
        return -1;
    });
}

// What the run did stays undoable by undo_last (ai_tools.cpp keeps it).
void McpKeepUndo(AppState& s, CtlRun& run) {
    std::lock_guard lock(run.mcp->mu);
    run.mcp->window = s.hwnd;
    for (auto& fn : run.undo) run.mcp->undo.push_back(std::move(fn));
    run.undo.clear();
}

// 本次运行's lines for the call: 「2 步」 after the tool's name, then
// 选中 6 项 → 6 项已移到「2026」 · 你点了「移动」.
std::wstring McpSummary(const CtlRun& run, const std::wstring& last = {}) {
    std::wstring line;
    auto add = [&](const std::wstring& piece) {
        if (!piece.empty()) line += (line.empty() ? L"" : L" \u2192 ") + piece;
    };
    for (const std::wstring& piece : run.mcp_steps) add(piece);
    if (!run.fail_title.empty()) add(Fill(Text(I::AiMcpSumFailed), L"{title}", run.fail_title));
    add(last);
    // Every step found it so already: say so instead of an empty line.
    if (line.empty() && run.mcp_already > 0) line = Text(I::AiMcpSumAlready);
    if (!run.mcp_clicked.empty()) line += L" \u00B7 " + Fill(Text(I::AiMcpSumClicked), L"{button}", run.mcp_clicked);
    else if (run.mcp_by_rule) line += L" \u00B7 " + Text(I::AiMcpSumByRule);
    if (line.size() > 160) line = line.substr(0, 159) + L"\u2026";
    return Fill(Text(I::AiMcpSumSteps), L"{n}", Num(run.queue.size())) + L"\n" + line;
}

// The user said no (or nobody answered in 2 minutes): the call ends there.
// step: the declined step's title for 本次运行; button: what the user clicked.
void McpDenied(AppState& s, std::shared_ptr<CtlRun> run, const std::string& what, const std::wstring& step,
               const std::wstring& button) {
    bool late = false;
    {
        std::lock_guard lock(run->mcp->mu);
        late = run->mcp->timed_out;
    }
    std::string message = late ? "Nobody answered the question in Pulse within 2 minutes (" + what + ")"
                               : "The user declined " + what + " in Pulse";
    message += run->ok ? "; the " + std::to_string(run->ok) + " step(s) before it stay done (undo_last undoes them), the "
                         "rest were not run."
                       : "; nothing of this call was done.";
    if (!late) message += " Do not ask again the same way; ask the user what they want instead.";
    McpKeepUndo(s, *run);
    Redraw(s);
    ToolOutcome out = ToolOutcome::Fail(late ? "user_timeout" : "user_denied", message, late);
    if (!late) run->mcp_clicked = button;
    out.summary = McpSummary(*run, step);
    app::ai::FinishMcpRun(*run->mcp, std::move(out));
}

// CtlFinish for an MCP run: the steps' report, as the assistant's model hears it.
void McpFinish(AppState& s, std::shared_ptr<CtlRun> run) {
    const bool failed = !run->fail_title.empty();
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", run->ok > 0 && !failed);
    out.data.set("done", static_cast<uint64_t>(run->ok));
    if (failed) out.data.set("stopped", "at the step that failed; later steps were not run, earlier ones stay done");
    out.data.set("steps", std::move(run->report));
    if (run->locked && !run->lock_title.empty()) {
        json::Value lock = json::Value::Object();
        lock.set("title", Utf8(run->lock_title));
        if (!run->lock_sub.empty()) lock.set("why", Utf8(run->lock_sub));
        if (!run->lock_where.empty()) lock.set("where", Utf8(run->lock_where));
        out.data.set("only_the_user", std::move(lock));
    }
    if (!run->undo.empty()) out.data.set("undo", "undo_last undoes this whole call");
    if (!run->mcp_line.empty()) out.data.set("summary", Utf8(run->mcp_line));
    if (!run->mcp_open.empty()) out.data.set("open", Utf8(run->mcp_open));
    out.changed_files = run->mcp_files;
    out.summary = McpSummary(*run);
    McpKeepUndo(s, *run);
    Redraw(s);
    app::ai::FinishMcpRun(*run->mcp, std::move(out));
}

// A file row of the dialog: 发票.pdf  → D:\财务\2026, 旧备份.zip  4.1 GB.
std::wstring McpFoRow(const fo::FilePlan& p, const fo::FileRow& r) {
    std::wstring to;
    switch (p.op) {
    case fo::FileOp::Recycle:
    case fo::FileOp::Purge: to = r.folder ? std::wstring() : AiChatFormatSize(r.bytes); break;
    case fo::FileOp::Dissolve: to = Fill(Text(I::AiFoNItems), L"{n}", Num(static_cast<size_t>(r.bytes))); break;
    case fo::FileOp::Restore: to = r.skip ? Text(I::AiFoRowTaken) : L"\u2192 " + FoShort(r.to); break;
    case fo::FileOp::Rename: to = L"\u2192 " + r.to; break;
    default: to = L"\u2192 " + FoShort(r.to); break;
    }
    return to.empty() ? r.name : r.name + L"    " + to;
}

// The file step's plan in a dialog: the assistant's card words (FoCard).
void McpFoAsk(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan) {
    AiBlockView cf;
    FoCard(s, *run, *plan, cf);
    ui::ConfirmDialogSpec spec;
    spec.title = cf.title;
    spec.message = cf.sub;
    if (plan->op == fo::FileOp::Organize) {
        // One row per folder: 文档  ·  新建  ·  季度报告.docx、合同扫描.pdf … 9 项.
        for (const fo::OrganizeDest& d : plan->dests) {
            if (spec.items.size() >= 5) break;
            std::wstring names;
            for (size_t i = 0; i < d.sources.size() && i < 3; ++i)
                names += (i ? Text(I::AiTkListSep) : std::wstring()) + Leaf(d.sources[i]);
            if (d.sources.size() > 3) names += L" \u2026";
            spec.items.push_back(Leaf(d.path) + L"    " + Text(d.make ? I::AiFoOrgRowNew : I::AiFoOrgRowHave) + L"    " +
                                 names + L"    " + Fill(Text(I::AiFoNItems), L"{n}", Num(d.sources.size())));
        }
        spec.item_glyph = L"\xE8B7";
        spec.items_total = plan->dests.size();
    } else if (plan->op != fo::FileOp::EmptyBin) {
        for (const fo::FileRow& r : plan->rows) {
            if (spec.items.size() >= 5) break;
            spec.items.push_back(McpFoRow(*plan, r));
        }
        spec.items_total = plan->rows.size();
    }
    std::wstring note;
    for (const std::wstring* part : {&cf.cf_bin, &cf.hint1, &cf.hint2})
        if (!part->empty()) note += *part + L"\n";
    if (!cf.danger && !cf.hint.empty()) note += cf.hint + L"\n";
    spec.note = note + McpFrom(*run);
    spec.confirm_text = cf.accept;
    spec.cancel_text = Text(I::AiDeny);
    if (cf.danger) {
        // Two presses, like the red card: the first only arms it.
        spec.danger = true;
        spec.confirm_text = Text(I::PermanentDelete);
        spec.confirm_again_text = cf.arm;
        spec.confirm_again_note = Text(I::AiMcpArmNote) + L"\n" + McpFrom(*run);
        spec.default_choice = ui::ConfirmChoice::Cancel;
    }
    const std::string what = McpWhat(*run, run->index);
    const std::wstring clicked = spec.confirm_text;
    const std::wstring step = run->index < run->queue.size() ? StepTitle(s, StepAction(run->queue[run->index])) : std::wstring();
    const std::wstring deny = spec.cancel_text;
    McpAsk(s, run, std::move(spec), [run, plan, what, clicked, step, deny](AppState& s2, ui::ConfirmChoice choice) {
        if (choice != ui::ConfirmChoice::Confirm) {
            McpDenied(s2, run, what, step, deny);
            return;
        }
        run->mcp_clicked = clicked;
        FoSubmit(s2, run, plan);
    });
}

// ---- P5: run_actions for AI apps (ai_mcp_run.h) ------------------------------

// A question asked before any step of an MCP run runs.
struct McpQuestion {
    ui::ConfirmDialogSpec spec;
    std::string what;          // step 2 (file.new_folder)
    std::wstring step;         // 本次运行: 新建文件夹「报销」
    std::string rule_tool;     // 始终允许这个文件夹: pulse.<id>; empty without one
    std::wstring rule_folder;
};

// The question for a step outside Pulse: the assistant's card words (ToolRunActions).
ui::ConfirmDialogSpec McpConfirmSpec(AppState& s, const CtlRun& run, const std::string& id, const std::wstring& target,
                                     const app::actions::ArgAsk* text) {
    ui::ConfirmDialogSpec spec;
    if (id == "nav.open") {
        spec.title = Text(I::AiActRunTitle);
        spec.message = Text(I::AiActRunSub);
        spec.items.push_back(target);
        spec.confirm_text = Text(I::AiActRunAccept);
        spec.cancel_text = Text(I::AiActRunDeny);
    } else if (text) {
        spec.title = text->title;
        spec.message = text->sub;
        if (!text->file.empty()) spec.items.push_back(text->file_note.empty() ? text->file : text->file + L"    " + text->file_note);
        spec.confirm_text = text->accept.empty() ? Text(I::AiActSysAccept) : text->accept;
        spec.cancel_text = text->deny.empty() ? Text(I::AiAsstDeny) : text->deny;
    } else {
        spec.title = Fill(Text(I::AiActSysTitle), L"{title}", StepTitle(s, id));
        spec.message = Text(I::AiActSysSub);
        spec.confirm_text = Text(I::AiActSysAccept);
        spec.cancel_text = Text(I::AiAsstDeny);
    }
    spec.note = McpFrom(run);
    spec.tone = ui::ConfirmTone::Warning;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    return spec;
}

void McpAskNext(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<std::vector<McpQuestion>> questions, size_t i) {
    if (RunGone(s, *run)) return;
    if (i >= questions->size()) {
        HideRenameOverlay(s, true);   // panes and windows are about to move
        CtlNext(s, run);
        return;
    }
    McpAsk(s, run, (*questions)[i].spec, [run, questions, i](AppState& s2, ui::ConfirmChoice choice) {
        const McpQuestion& q = (*questions)[i];
        if (choice == ui::ConfirmChoice::Cancel) {
            McpDenied(s2, run, q.what, q.step, q.spec.cancel_text);
            return;
        }
        run->mcp_clicked = choice == ui::ConfirmChoice::Secondary ? q.spec.secondary_text : q.spec.confirm_text;
        if (choice == ui::ConfirmChoice::Secondary && !q.rule_tool.empty() && run->mcp->allow_always)
            run->mcp->allow_always(q.rule_tool, q.rule_folder);
        McpAskNext(s2, run, questions, i + 1);
    });
}

// Steps that may change the folder the active tab shows (navigation, tabs,
// panes, windows, searches); the ones that only restyle a tab do not.
bool McpMovesFolder(const std::string& id) {
    static constexpr std::string_view kStill[] = {"nav.refresh", "tab.color", "tab.pin", "tab.rename",
                                                  "tab.group", "window.maximize", "pane.mark_target"};
    for (std::string_view still : kStill)
        if (id == still) return false;
    static constexpr std::string_view kMoves[] = {"nav.", "tab.", "pane.", "window.", "search."};
    for (std::string_view prefix : kMoves)
        if (id.rfind(prefix, 0) == 0) return true;
    return false;
}

void McpStart(AppState& s, std::shared_ptr<app::ai::McpActionsRun> mcp) {
    using app::actions::Gate;
    using app::actions::McpGroup;
    {
        std::lock_guard lock(mcp->mu);
        mcp->window = s.hwnd;
    }
    auto run = std::make_shared<CtlRun>();
    run->mcp = mcp;
    for (const json::Value& step : mcp->steps)
        if (step.is_object() && run->queue.size() < 12) run->queue.push_back(step);
    if (run->queue.empty()) {
        app::ai::FinishMcpRun(*mcp, ToolOutcome::Fail("invalid_args", "steps is empty"));
        return;
    }
    // Every step must be allowed before any runs: a call is all or nothing up front.
    size_t file_steps = 0;
    std::string moved_by;   // the first step that may change the folder Pulse shows
    // Refused before anything ran: the AI page's line says which step and why
    // (本次运行: 2 步 / 新建文件夹 · 要先打开文件夹，再新建).
    auto refuse = [&](const char* code, const std::string& message, const std::wstring& step, I why) {
        ToolOutcome out = ToolOutcome::Fail(code, message);
        out.summary = Fill(Text(I::AiMcpSumSteps), L"{n}", Num(run->queue.size())) + L"\n" +
                      (step.empty() ? Text(why) : step + L" \u00B7 " + Text(why));
        return out;
    };
    for (size_t i = 0; i < run->queue.size(); ++i) {
        const std::string id = StepAction(run->queue[i]);
        const std::string what = McpWhat(*run, i);
        if (id.empty() || !app::actions::FindAction(id)) {
            ToolOutcome out = refuse("invalid_args", what + ": no such action (find_actions lists them); nothing of "
                                                           "this call was done", {}, I::AiMcpWhyUnknown);
            out.summary = Fill(out.summary, L"{id}", json::Wide(id));
            app::ai::FinishMcpRun(*mcp, std::move(out));
            return;
        }
        const std::wstring target = id == "nav.open" ? OpenTarget(s, StepArgs(run->queue[i])) : std::wstring();
        const McpGroup group = app::actions::McpGroupOf(id, target);
        const Gate gate = app::actions::AiPolicy(id, target).gate;
        // 本次运行: 给 8 项打标签「报销」 · 整理 Pulse 未打开 (被设置拦下).
        auto blocked = [&](McpGroup g) {
            ToolOutcome out = app::ai::McpNotAllowed(g, what);
            out.summary = Fill(Text(I::AiMcpSumSteps), L"{n}", Num(run->queue.size())) + L"\n" + StepTitle(s, id) +
                          L" \u00B7 " +
                          Fill(Text(g == McpGroup::Locked ? I::AiMcpSumLocked : I::AiMcpSumOff), L"{group}",
                               json::Wide(app::ai::McpGroupSwitchName(g)));
            app::ai::FinishMcpRun(*mcp, std::move(out));
        };
        if (group == McpGroup::Locked || gate == Gate::Locked) {
            blocked(McpGroup::Locked);
            return;
        }
        if (gate == Gate::Plan && !app::actions::IsFileOp(id)) {
            app::ai::FinishMcpRun(*mcp, refuse("unsupported", what + " is not available to AI apps: the user does it "
                                                                   "in Pulse. Nothing of this call was done.",
                                               StepTitle(s, id), I::AiMcpWhyUserOnly));
            return;
        }
        if (!mcp->allowed[static_cast<size_t>(group)]) {
            blocked(group);
            return;
        }
        file_steps += app::actions::IsFileOp(id) ? 1 : 0;
        // A new folder or file is made in the folder Pulse shows. The question
        // comes before any step runs, so it must be the folder shown now: no
        // earlier step may move it, and it must be a real folder (not Settings).
        if (id == "file.new_folder" || id == "file.new_file") {
            if (!moved_by.empty()) {
                app::ai::FinishMcpRun(*mcp, refuse("invalid_args", what + " makes it in the folder Pulse shows, and " +
                                                                       moved_by + " changes that folder first: open the "
                                                                       "folder in one call, then make it in the next. "
                                                                       "Nothing of this call was done.",
                                                   StepTitle(s, id), I::AiMcpWhyOpenFirst));
                return;
            }
            const app::Tab* shown = ActiveTab(s);
            if (!shown || shown->current_path.empty() || fs::IsVirtualPath(NormDir(shown->current_path))) {
                app::ai::FinishMcpRun(*mcp, refuse("invalid_args", what + ": the active tab shows no folder (a Pulse "
                                                                       "page such as Settings), so there is nowhere to "
                                                                       "make it. Open a folder first (nav.go in a call "
                                                                       "of its own). Nothing of this call was done.",
                                                   StepTitle(s, id), I::AiMcpWhyNoFolder));
                return;
            }
        }
        if (moved_by.empty() && McpMovesFolder(id)) moved_by = what;
    }
    if (file_steps > 1) {
        app::ai::FinishMcpRun(*mcp, refuse("invalid_args", "one file step (move, copy, rename, delete, restore) per "
                                                           "run_actions call: call again for the next",
                                           {}, I::AiMcpWhyOneFile));
        return;
    }
    // The questions, before anything runs: Create steps (unless a rule covers
    // the folder) and effects outside Pulse. File steps ask with their plan
    // when they come (FoAsk).
    auto questions = std::make_shared<std::vector<McpQuestion>>();
    app::Tab* tab = ActiveTab(s);
    std::wstring folder = tab ? NormDir(tab->current_path) : std::wstring();
    if (fs::IsVirtualPath(folder)) folder.clear();
    for (size_t i = 0; i < run->queue.size(); ++i) {
        const std::string id = StepAction(run->queue[i]);
        const json::Value& args = StepArgs(run->queue[i]);
        const std::wstring target = id == "nav.open" ? OpenTarget(s, args) : std::wstring();
        McpQuestion q;
        q.what = McpWhat(*run, i);
        const std::wstring named = json::Wide(args.get_string("name"));
        q.step = StepTitle(s, id) + (named.empty() ? std::wstring() : L"\u300C" + named + L"\u300D");
        if (app::actions::McpGroupOf(id, target) == McpGroup::Create) {
            const std::string tool = "pulse." + id;
            if (!folder.empty() && mcp->always_allowed && mcp->always_allowed(tool, folder)) {
                run->mcp_by_rule = true;   // 本次运行 says why nobody was asked
                continue;
            }
            q.spec.title = Fill(Fill(Text(I::AiMcpCreateTitle), L"{client}", mcp->client), L"{title}", StepTitle(s, id));
            if (!folder.empty()) q.spec.message = Fill(Text(I::AiCreateGenericMessage), L"{folder}", folder);
            const std::wstring name = json::Wide(args.get_string("name"));
            if (!name.empty()) q.spec.items.push_back(name);
            if (id == "file.new_folder") q.spec.item_glyph = L"\xE8B7";   // a folder, not the document mark
            q.spec.confirm_text = Text(I::AiCreateAllow);
            if (!folder.empty()) {
                q.spec.secondary_text = Text(I::AiCreateAlways);
                q.rule_tool = tool;
                q.rule_folder = folder;
            }
            q.spec.cancel_text = Text(I::AiDeny);
            q.spec.note = McpFrom(*run);
            q.spec.tone = ui::ConfirmTone::Question;
        } else if (app::actions::AiPolicy(id, target).gate == Gate::Confirm) {
            app::actions::ArgAsk text;
            const bool has_text = app::actions::HasAskText(id);
            if (has_text) {
                text = app::actions::AskFor(s, id, args);
                if (text.state != app::actions::ArgAsk::State::Ask) continue;   // the step says why
            }
            q.spec = McpConfirmSpec(s, *run, id, target, has_text ? &text : nullptr);
        } else {
            continue;
        }
        questions->push_back(std::move(q));
    }
    McpAskNext(s, run, questions, 0);
}
} // namespace ai_agent

namespace app::ai {
void StartMcpRunActions(AppState& s, std::shared_ptr<McpActionsRun> run) { McpStart(s, std::move(run)); }
} // namespace app::ai

} // namespace pulse
