// ai_agent_files.cpp - Pulse 助手's run_actions' file steps (the plan card), the step loop and the tools.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

uint64_t FoGroup() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring FoWhat(const fo::FilePlan& p) {
    if (p.count == 1 && !p.rows.empty()) return Fill(Text(I::AiFoOneName), L"{name}", p.rows[0].name);
    return Fill(Text(p.files_only ? I::AiFoNFiles : I::AiFoNItems), L"{n}", Num(p.count));
}

std::wstring FoLeaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos || p.size() <= 3 ? p : p.substr(slash + 1);
}

// A destination short enough for a card row: …\财务\2026 past ~22 characters.
std::wstring FoShort(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() <= 22) return p;
    const size_t last = p.find_last_of(L'\\');
    if (last == std::wstring::npos || last == 0) return p;
    const size_t prev = p.find_last_of(L'\\', last - 1);
    if (prev == std::wstring::npos || prev < 3) return p;
    return L"\u2026" + p.substr(prev);
}

// The step's verb: 移动 / 复制 / 重命名 / 合并到文件夹 / … (its row and card).
std::wstring FoVerb(fo::FileOp op) {
    switch (op) {
    case fo::FileOp::Move: return Text(I::AiFoAcceptMove);
    case fo::FileOp::Copy: return Text(I::Copy);
    case fo::FileOp::Rename: return Text(I::Rename);
    case fo::FileOp::Group: return Text(I::ActGroupIntoFolder);
    case fo::FileOp::Dissolve: return Text(I::ActDissolveFolder);
    case fo::FileOp::Recycle: return Text(I::AiClAccept);
    case fo::FileOp::Purge: return Text(I::PermanentDelete);
    case fo::FileOp::Restore: return Text(I::Restore);
    case fo::FileOp::EmptyBin: return Text(I::EmptyRecycleBin);
    case fo::FileOp::Organize: return Text(I::AiFoAcceptOrganize);
    case fo::FileOp::None: break;
    }
    return {};
}

// 移动 6 项 (the 整理计划 row's argument; the card's done line).
std::wstring FoArg(const fo::FilePlan& p) {
    switch (p.op) {
    case fo::FileOp::Move: return Fill(Text(I::AiFoArgMove), L"{n}", Num(p.count));
    case fo::FileOp::Copy: return Fill(Text(I::AiFoArgCopy), L"{n}", Num(p.count));
    case fo::FileOp::Rename: return Fill(Text(I::AiFoArgRename), L"{name}", p.new_name);
    case fo::FileOp::Group: return Fill(Text(I::AiFoArgGroup), L"{n}", Num(p.count));
    case fo::FileOp::Dissolve: return Fill(Text(I::AiFoArgDissolve), L"{n}", Num(p.count));
    case fo::FileOp::EmptyBin: return Fill(Text(I::AiFoNItems), L"{n}", Num(p.count));
    case fo::FileOp::Organize:
        return Fill(Fill(Text(I::AiFoArgOrganize), L"{n}", Num(p.count)), L"{k}", Num(p.dests.size()));
    default: return FoWhat(p);
    }
}

// 右栏打开 D:\财务\2026: a later step of the call, for 确认后会和另外 1 步一起做.
std::wstring FoLater(AppState& s, const json::Value& step) {
    const std::string id = StepAction(step);
    const json::Value& args = StepArgs(step);
    if (id == "nav.go") {
        std::wstring pane;
        std::vector<app::Pane*> panes;
        std::string error;
        if (args.find("pane") && app::actions::PanesForArg(s, args.find("pane"), panes, error) && panes.size() == 1)
            pane = PanePos(s, PaneIndex(s, panes[0]));
        const std::wstring path = json::Wide(args.get_string("path"));
        const std::wstring shown = ResolveTaskDest(path).empty() ? path : ResolveTaskDest(path);
        return Fill(Fill(Text(I::AiFoLaterOpen), L"{pane}", pane), L"{path}", FoShort(shown));
    }
    // window.layout with panes: 右栏打开 …\财务\2026 for each folder that
    // changes, named by the layout it asks for.
    if (const json::Value* v = args.find("panes"); id == "window.layout" && v && v->is_array()) {
        const std::string layout = args.get_string("layout");
        const app::Tab* tab = ActiveTab(s);
        const std::wstring here = tab ? pulse::path::StripExtendedPathPrefix(tab->current_path) : std::wstring();
        std::wstring out;
        for (size_t i = 0; i < v->size() && i < 4; ++i) {
            if (!v->at(i).is_string() || v->at(i).as_string().empty()) continue;
            const std::wstring path = json::Wide(v->at(i).as_string());
            const std::wstring shown = ResolveTaskDest(path).empty() ? path : ResolveTaskDest(path);
            if (!here.empty() && _wcsicmp(shown.c_str(), here.c_str()) == 0) continue;
            std::wstring pane;
            if (layout == "side_by_side" && i < 2) pane = Text(i == 0 ? I::AiCtlPaneLeft : I::AiCtlPaneRight);
            else if (layout == "stacked" && i < 2) pane = Text(i == 0 ? I::AiCtlPaneTop : I::AiCtlPaneBottom);
            else pane = Fill(Text(I::AiCtlPaneN), L"{n}", Num(i + 1));
            out += (out.empty() ? L"" : Text(I::AiTkListSep)) +
                   Fill(Fill(Text(I::AiFoLaterOpen), L"{pane}", pane), L"{path}", FoShort(shown));
        }
        if (!out.empty()) return out;
    }
    return StepTitle(s, id);
}

void FoCard(AppState& s, CtlRun& run, const fo::FilePlan& p, AiBlockView& cf) {
    const std::wstring what = FoWhat(p);
    const std::wstring folder = FoLeaf(p.dest);
    std::wstring later;
    size_t n_later = 0;
    for (size_t k = run.index + 1; k < run.queue.size(); ++k, ++n_later)
        later += (later.empty() ? L"" : Text(I::AiTkListSep)) + FoLater(s, run.queue[k]);
    const std::wstring more = n_later ? Fill(Fill(Text(I::AiFoMoreSteps), L"{n}", Num(n_later)), L"{steps}", later) : std::wstring();
    cf.deny = Text(I::Cancel);
    cf.accept = FoVerb(p.op);
    cf.hint_icon = ui::AiIcon::Undo;
    size_t show = 3;
    switch (p.op) {
    case fo::FileOp::Move:
    case fo::FileOp::Copy:
        cf.cf_icon = p.op == fo::FileOp::Move ? ui::AiIcon::Move : ui::AiIcon::Copy;
        cf.title = Fill(Fill(Text(p.op == fo::FileOp::Move ? I::AiFoTitleMove : I::AiFoTitleCopy), L"{what}", what), L"{folder}", folder);
        cf.sub = !more.empty() ? more : Fill(Text(p.dest_exists ? I::AiFoSubTo : I::AiFoSubNewDest), L"{dest}", p.dest);
        cf.hint = Text(p.op == fo::FileOp::Move ? I::AiFoHintMove : I::AiFoHintCopy);
        if (p.clashes) {
            cf.hint2 = Fill(Text(I::AiFoClash), L"{n}", Num(p.clashes));
            cf.hint2_warn = true;
        }
        break;
    case fo::FileOp::Rename:
        cf.cf_icon = ui::AiIcon::Rename;
        cf.title = Fill(Fill(Text(I::AiFoTitleRename), L"{old}", p.rows.empty() ? std::wstring() : p.rows[0].name), L"{name}", p.new_name);
        cf.sub = more;
        cf.hint = Text(I::AiFoHintRename);
        break;
    case fo::FileOp::Group:
        cf.cf_icon = ui::AiIcon::FolderPlus;
        cf.title = Fill(Fill(Text(I::AiFoTitleGroup), L"{what}", what), L"{name}", p.new_name);
        cf.sub = !more.empty() ? more : Fill(Text(I::AiFoSubIn), L"{folder}", folder);
        cf.hint = Text(I::AiFoHintUndo);
        break;
    case fo::FileOp::Dissolve:
        cf.cf_icon = ui::AiIcon::Folder;
        cf.title = Fill(Text(I::AiFoTitleDissolve), L"{n}", Num(p.count));
        cf.sub = !more.empty() ? more : Fill(Fill(Text(I::AiFoSubDissolve), L"{n}", Num(p.inner)), L"{folder}", folder);
        cf.hint = Text(I::AiFoHintUndo);
        break;
    case fo::FileOp::Recycle:
        // 把 5 个文件移到回收站？ / rows with sizes / 回收站放得下 (P4 scene 3).
        cf.cf_icon = ui::AiIcon::Trash;
        cf.title = Fill(Text(I::AiFoTitleRecycle), L"{what}", what);
        cf.sub = !more.empty() ? more : Text(I::AiFoSubRecycle);
        cf.cf_bin = Fill(Fill(Text(I::AiClBinOk), L"{max}", Gb1(p.bin_max)), L"{used}", Gb1(p.bin_used));
        cf.hint = Text(I::AiFoHintDelete);
        cf.hint_icon = ui::AiIcon::Shield;
        show = 2;
        break;
    case fo::FileOp::Purge:
    case fo::FileOp::EmptyBin:
        // The red card; the first click only arms it (再点一次，永久删除).
        cf.danger = true;
        cf.tag = Text(I::PermanentDelete);
        cf.title = p.op == fo::FileOp::Purge ? Fill(Text(I::AiFoTitlePurge), L"{what}", what) : Text(I::AiFoTitleEmpty);
        cf.sub = p.op == fo::FileOp::Purge
                     ? Fill(Text(I::AiFoSubPurge), L"{n}", Num(p.count))
                     : Fill(Fill(Text(I::AiFoSubEmpty), L"{n}", Num(p.count)), L"{size}", AiChatFormatSize(p.bytes));
        if (!more.empty()) {
            cf.hint1 = more;
            cf.hint1_icon = ui::AiIcon::Tasks;
        }
        cf.hint = Text(I::AiFoHintArm);
        cf.hint_icon = ui::AiIcon::Shield;
        cf.arm = Text(I::AiFoArm);
        show = 2;
        break;
    case fo::FileOp::Restore:
        cf.cf_icon = ui::AiIcon::Undo;
        cf.title = Fill(Text(I::AiFoTitleRestore), L"{what}", what);
        cf.sub = !more.empty() ? more : Text(I::AiFoSubRestore);
        cf.hint = Text(I::AiFoHintRestore);
        cf.hint_icon = ui::AiIcon::Shield;
        if (p.clashes) {
            cf.hint2 = Fill(Text(I::AiFoRestoreClash), L"{n}", Num(p.clashes));
            cf.hint2_warn = true;
        }
        break;
    case fo::FileOp::Organize: {
        // 「Claude」想把 23 个项目整理到 4 个文件夹 / 都在「下载」里。不会覆盖… (task 46 ⑧).
        cf.cf_icon = ui::AiIcon::Move;
        const std::wstring n = Num(p.count), k = Num(p.dests.size());
        cf.title = run.mcp ? Fill(Fill(Fill(Text(I::AiFoTitleOrganizeMcp), L"{client}", run.mcp->client), L"{n}", n), L"{k}", k)
                           : Fill(Fill(Text(I::AiFoTitleOrganize), L"{n}", n), L"{k}", k);
        cf.sub = !more.empty() ? more
                 : p.one_source_folder ? Fill(Text(I::AiFoSubOrganize), L"{folder}", folder)
                                       : Text(I::AiFoSubOrganizeMixed);
        size_t made = 0;
        for (const fo::OrganizeDest& d : p.dests) made += d.make ? 1 : 0;
        if (made) cf.hint1 = Fill(Text(I::AiFoOrgNewN), L"{n}", Num(made));
        cf.hint2 = Text(I::AiFoOrgNoClash);
        cf.hint = Text(I::AiFoHintUndo);
        break;
    }
    case fo::FileOp::None: break;
    }
    // 把「a.txt」移到 / 永久删除「a.txt」: no space before a quoted name.
    for (size_t at = cf.title.find(L" \u300C"); at != std::wstring::npos; at = cf.title.find(L" \u300C", at)) cf.title.erase(at, 1);
    if (p.op == fo::FileOp::EmptyBin) return;   // no rows: the bin as a whole
    if (p.rows.size() <= show + 1) show = p.rows.size();   // never 还有 1 个
    for (size_t k = 0; k < show; ++k) {
        const fo::FileRow& r = p.rows[k];
        ui::AiConfirmRow row;
        row.name = r.name;
        row.folder = r.folder;
        switch (p.op) {
        case fo::FileOp::Recycle:
        case fo::FileOp::Purge: row.to = r.folder ? std::wstring() : AiChatFormatSize(r.bytes); break;
        case fo::FileOp::Dissolve: row.to = Fill(Text(I::AiFoNItems), L"{n}", Num(static_cast<size_t>(r.bytes))); break;
        case fo::FileOp::Restore: row.to = r.skip ? Text(I::AiFoRowTaken) : L"\u2192 " + FoShort(r.to); break;
        case fo::FileOp::Rename: row.to = L"\u2192 " + r.to; break;
        default: row.to = L"\u2192 " + FoShort(r.to); break;
        }
        cf.cf_rows.push_back(std::move(row));
    }
    if (p.rows.size() > show) {
        const size_t left = p.rows.size() - show;
        cf.cf_more = p.files_only && p.bytes
                         ? Fill(Fill(Text(I::AiFoMoreSize), L"{n}", Num(left)), L"{size}", AiChatFormatSize(p.bytes))
                         : Fill(Text(I::AiFoMore), L"{n}", Num(left));
    }
}

// The finished tasks' outcome: 6 项已移到「2026」, or why not.
void FoDone(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan, uint64_t group, bool failed,
            std::wstring error) {
    AiAgent& a = Agent(s);
    auto st = std::make_shared<CtlStep>();
    st->icon = ui::AiIcon::Move;
    st->title = FoVerb(plan->op);
    st->arg = FoArg(*plan);
    st->quiet = true;   // its row is up already
    st->data = json::Value::Object();
    if (failed) {
        st->error = "Pulse could not finish it: " + Utf8(error);
        st->why = error.empty() ? Text(I::AiActWhyFailed) : error;
        if (!run->mcp) EndStep(s, a, false, st->why);
    } else {
        st->ok = true;
        const std::wstring folder = FoLeaf(plan->dest);
        std::wstring line;
        switch (plan->op) {
        case fo::FileOp::Move: line = Fill(Fill(Text(I::AiFoLineMove), L"{n}", Num(plan->count)), L"{folder}", folder); break;
        case fo::FileOp::Copy: line = Fill(Fill(Text(I::AiFoLineCopy), L"{n}", Num(plan->count)), L"{folder}", folder); break;
        case fo::FileOp::Rename:
            line = Fill(Fill(Text(I::AiFoLineRename), L"{old}", plan->rows.empty() ? std::wstring() : plan->rows[0].name), L"{name}",
                        plan->new_name);
            break;
        case fo::FileOp::Group: line = Fill(Fill(Text(I::AiFoLineGroup), L"{n}", Num(plan->count)), L"{name}", plan->new_name); break;
        case fo::FileOp::Dissolve:
            line = Fill(Fill(Fill(Text(I::AiFoLineDissolve), L"{n}", Num(plan->count)), L"{k}", Num(plan->inner)), L"{folder}", folder);
            break;
        case fo::FileOp::Recycle: line = Fill(Text(I::AiFoLineRecycle), L"{n}", Num(plan->count)); break;
        case fo::FileOp::Purge: line = Fill(Text(I::AiFoLinePurge), L"{n}", Num(plan->count)); break;
        case fo::FileOp::Restore: line = Fill(Text(I::AiFoLineRestore), L"{n}", Num(plan->sources.size())); break;
        case fo::FileOp::EmptyBin: line = Fill(Text(I::AiFoLineEmpty), L"{n}", Num(plan->count)); break;
        case fo::FileOp::Organize:
            line = Fill(Fill(Text(I::AiFoLineOrganize), L"{n}", Num(plan->count)), L"{k}", Num(plan->dests.size()));
            break;
        case fo::FileOp::None: break;
        }
        st->sub = line;
        if (!run->mcp) EndStep(s, a, true, line);
        run->touched = true;
        if (run->mcp) {
            // The notification after the call (ai_settings_ui.cpp NoteAiChange) and undo_last.
            run->mcp_line = line;
            run->mcp_steps.push_back(line);
            run->mcp_files = true;
            if (plan->op == fo::FileOp::Move || plan->op == fo::FileOp::Copy || plan->op == fo::FileOp::Organize)
                run->mcp_open = plan->dest;
            if (fo::FileOpUndoes(plan->op) && group) {
                std::lock_guard lock(run->mcp->mu);
                run->mcp->file_group = group;
            }
        }
        PlainDone(*run, SplitLine(line, {}, {}));
        if (fo::FileOpUndoes(plan->op) && group) {
            run->undo.push_back([group](AppState& st2) { st2.ops.UndoGroup(group); });
            ++run->undo_steps;
        }
        if (plan->op == fo::FileOp::Purge || plan->op == fo::FileOp::EmptyBin) recycle_empty::NotifyShell();
        st->data.set("done", static_cast<uint64_t>(plan->op == fo::FileOp::Restore ? plan->sources.size() : plan->count));
        if (!plan->dest.empty() && (plan->op == fo::FileOp::Move || plan->op == fo::FileOp::Copy)) st->data.set("dest", Utf8(plan->dest));
        if (plan->clashes && plan->op == fo::FileOp::Restore) st->data.set("left_in_bin", static_cast<uint64_t>(plan->clashes));
        if (plan->clashes && plan->op != fo::FileOp::Restore) st->data.set("renamed_to_keep_both", static_cast<uint64_t>(plan->clashes));
        if (plan->op == fo::FileOp::Organize) {
            st->data.set("moved", static_cast<uint64_t>(plan->count));
            st->data.set("created_folders", static_cast<uint64_t>(plan->make_folders.size()));
            json::Value folders = json::Value::Array();
            for (const fo::OrganizeDest& d : plan->dests) folders.push(json::Value(Utf8(d.path)));
            st->data.set("folders", std::move(folders));
        }
        st->data.set("undoable", fo::FileOpUndoes(plan->op));
    }
    run->file_step = st;
    run->file_state = 2;
    Redraw(s);
    CtlNext(s, run);
}

// Waits for the queued tasks (the progress dialog shows them as for the menus).
void FoWait(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan, std::vector<uint64_t> tasks,
            uint64_t group) {
    const uint64_t started = Now();
    Later(s, *run, 150, [run, plan, tasks, group, started](AppState& s2) {
        if (RunGone(s2, *run)) return -1;
        bool all = true, failed = false;
        std::wstring error;
        for (uint64_t id : tasks) {
            const std::optional<ops::OpStatus> fs2 = s2.ops.FinalStatus(id);
            if (!fs2) {
                all = false;
                continue;
            }
            if (fs2->phase == ops::OpPhase::Failed || fs2->failed_items > 0) {
                failed = true;
                if (error.empty()) error = !fs2->failures.empty() ? fs2->failures[0].reason : fs2->last_error;
            }
        }
        // A task the manager no longer lists (more than 16 since): over.
        if (!all && !(Now() - started > 3000 && !s2.ops.Status().active)) return 150;
        FoDone(s2, run, plan, group, failed, error);
        return -1;
    });
}

// After 确认: queues the menus' own requests under one undo group and waits.
void FoSubmit(AppState& s, std::shared_ptr<CtlRun> run, std::shared_ptr<fo::FilePlan> plan) {
    const uint64_t group = fo::FileOpUndoes(plan->op) ? FoGroup() : 0;
    std::wstring label = Text(I::AiAsstUndoLabel);
    if (run->mcp) {
        label = Fill(Text(I::AiMcpUndoLabel), L"{client}", run->mcp->client);
    } else {
        AiAgent& a = Agent(s);
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, FoVerb(plan->op), 0.5f);
        AddStep(s, a, plan->op == fo::FileOp::Recycle || plan->op == fo::FileOp::Purge || plan->op == fo::FileOp::EmptyBin
                          ? ui::AiIcon::Trash
                          : ui::AiIcon::Move,
                FoVerb(plan->op), FoArg(*plan), {});
    }
    const std::vector<uint64_t> tasks = fo::SubmitFilePlan(s, *plan, group, label);
    Redraw(s);
    if (tasks.empty()) {
        FoDone(s, run, plan, group, true, Text(I::AiActWhyFailed));
        return;
    }
    FoWait(s, run, plan, tasks, group);
}

// The card of the step at run->index. False: no card (the plan failed or
// there is nothing to do); run->file_step then says so.
bool FoAsk(AppState& s, std::shared_ptr<CtlRun> run, const std::string& id, const json::Value& args) {
    AiAgent& a = Agent(s);
    if (!run->mcp && fo::FileOpOf(id) == fo::FileOp::Organize) {
        // The assistant sorts with organize_files (its own plan card and rules).
        auto st = std::make_shared<CtlStep>();
        st->icon = ui::AiIcon::Move;
        st->title = StepTitle(s, id);
        st->error = "file.organize is for AI apps; in Pulse use organize_files (it shows the user a plan)";
        st->why = Text(I::AiActWhyLater);
        run->file_step = st;
        run->file_state = 2;
        return false;
    }
    auto plan = std::make_shared<fo::FilePlan>(fo::PlanFileOp(s, id, args, &ResolveTaskDest));
    if (!plan->ok || plan->skip) {
        auto st = std::make_shared<CtlStep>();
        st->icon = ui::AiIcon::Move;
        st->title = StepTitle(s, id);
        st->ok = plan->ok;
        st->skip = plan->skip;
        st->error = plan->error;
        st->why = plan->why;
        st->sub = plan->why;
        if (plan->skip && !plan->why.empty()) {
            st->data = json::Value::Object();
            st->data.set("note", Utf8(plan->why));
        }
        run->file_step = st;
        run->file_state = 2;
        return false;
    }
    run->file_state = 1;
    if (run->mcp) {
        McpFoAsk(s, run, plan);
        return true;
    }
    // 整理计划 · 移动 6 项 / 到 D:\财务\2026 · 文件夹已存在 (P4 scene 2).
    const bool plan_row = plan->op == fo::FileOp::Move || plan->op == fo::FileOp::Copy || plan->op == fo::FileOp::Group ||
                          plan->op == fo::FileOp::Dissolve || plan->op == fo::FileOp::Rename;
    if (plan_row) {
        std::wstring sub;
        if (plan->op == fo::FileOp::Move || plan->op == fo::FileOp::Copy)
            sub = Fill(Text(plan->dest_exists ? I::AiFoStepDest : I::AiFoStepNewDest), L"{dest}", plan->dest);
        else
            sub = Fill(Text(I::AiFoSubIn), L"{folder}", FoLeaf(plan->dest));
        AddStep(s, a, ui::AiIcon::Tasks, Text(I::AiFoPlanStep), FoArg(*plan), {});
        EndStep(s, a, true, sub);
    }
    AiChat& ch = Chat(s);
    AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
    if (!s.aiAssistant.open) ch.unread = true;
    AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
    FoCard(s, *run, *plan, cf);
    const std::wstring done_title = FoVerb(plan->op) + L" \u00B7 " + FoArg(*plan);
    ch.confirm_id = cf.id;
    ch.on_confirm = [run, plan, done_title](AppState& s3, bool accept) {
        AiChat& c3 = Chat(s3);
        if (AiBlockView* card = c3.Find(c3.confirm_id)) {
            const uint64_t now2 = Now();
            if (now2 > card->born_ms) c3.turn_start += now2 - card->born_ms;
            card->done = true;
            card->tag.clear();
            card->title = Fill(Text(accept ? I::AiActConfirmedSys : I::AiActDenied), L"{title}", done_title);
        }
        c3.confirm_id = 0;
        c3.on_confirm = nullptr;
        AiAgent& a3 = Agent(s3);
        if (a3.generation != run->gen) return;
        if (!accept) {
            if (run->ok == 0) {
                // Nothing of the call is done yet: none of it is (as FirstAsk).
                AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiActWhyDeclined));
                ToolOutcome out;
                out.data = json::Value::Object();
                out.data.set("ok", false);
                out.data.set("declined", true);
                out.data.set("message", "The user declined the file step in Pulse; no step of this call was done. "
                                        "Do not ask again the same way; ask what they want instead.");
                AddToolResult(a3, run->call_id, out);
                ++a3.call_index;
                Redraw(s3);
                NextTool(s3);
                return;
            }
            auto st = std::make_shared<CtlStep>();
            st->icon = ui::AiIcon::Move;
            st->title = FoVerb(plan->op);
            st->arg = FoArg(*plan);
            st->error = "the user declined this step in Pulse; the steps before it stay done";
            st->why = Text(I::AiActWhyDeclined);
            run->file_step = st;
            run->file_state = 2;
            CtlNext(s3, run);
            return;
        }
        FoSubmit(s3, run, plan);
    };
    Redraw(s);
    return true;
}

CtlStep CtlRunStep(AppState& s, CtlRun& run, const std::string& id, const json::Value& act) {
    if (!app::actions::FindAction(id)) {
        CtlStep st;
        st.title = Display(id);
        st.error = "unknown action (find_actions lists them)";
        st.why = Text(I::AiActWhyUnknown);
        return st;
    }
    const app::actions::Policy policy = app::actions::AiPolicy(id);
    // Plan without a file step of its own (tray.send_to): the flow tools.
    if (policy.gate == app::actions::Gate::Plan || policy.gate == app::actions::Gate::Locked) {
        CtlStep st;
        st.title = StepTitle(s, id);
        st.icon = policy.gate == app::actions::Gate::Locked ? ui::AiIcon::Lock : ui::AiIcon::Move;
        st.error = policy.gate == app::actions::Gate::Plan
                       ? "this move is not a run_actions step: use organize_files, rename_files, run_task, find_cleanup "
                         "or find_duplicates (they show the user a plan)"
                       : "locked: only the user can do this";
        st.why = Text(policy.gate == app::actions::Gate::Plan ? I::AiActWhyLater : I::AiActWhyLocked);
        if (policy.gate == app::actions::Gate::Locked && app::actions::HasLockText(id)) {
            // 解锁 E 盘, 退出 Pulse: the lock card says why and where; as the
            // call's first step it is the whole answer, so no step row.
            const app::actions::ArgLock lock = app::actions::LockFor(s, id, act);
            run.locked = true;
            run.lock_title = lock.title;
            run.lock_sub = lock.sub;
            run.lock_where = lock.where;
            st.quiet = run.ok == 0;
        }
        return st;
    }
    if (IsChooser(id)) return CtlChooser(s, id);
    if (id == "window.layout") return CtlLayout(s, run, act);
    if (id == "nav.go") return CtlOpen(s, run, act);
    if (id == "window.new") return CtlNewWindow(s, run, act);
    if (id == "tab.to_window") return CtlToWindow(s, run, act, false);
    if (id == "pane.to_window") return CtlToWindow(s, run, act, true);
    if (id == "window.merge") return CtlMerge(s, run);
    if (id == "window.overview") return CtlOverview(s, run);
    if (id == "view.mode") return CtlView(s, run, act);
    if (id == "view.sort") return CtlSort(s, run, act);
    if (id == "view.group") return CtlGroup(s, run, act);
    if (id == "sidebar.toggle") return CtlBar(s, run, act, false);
    if (id == "favbar.toggle") return CtlBar(s, run, act, true);
    if (id == "tray.add") return CtlTray(s, run, act);
    if (id == "tag.toggle") return CtlTag(s, run, act);
    if (id == "fav.add") return CtlFavorite(s, run, act);
    if (id == "select.items") return CtlSelect(s, run, act);
    if (id == "select.clear") return CtlSelectNone(s, run);
    if (id == "nav.open") return CtlOpenFile(s, run, act);
    if (id == "settings.set") return CtlSetting(s, run, act);
    if (id == "file.new_folder" && act.find("name")) return CtlNewFolder(s, run, act);
    if (id == "tab.new" && !act.get_string("path").empty()) {
        CtlStep st;
        st.icon = ui::AiIcon::Plus;
        st.title = Text(I::TooltipNewTab);
        const std::wstring path = CtlFolder(act.get_string("path"));
        st.arg = path.empty() ? json::Wide(act.get_string("path")) : PlaceLabel(path);
        if (path.empty()) {
            st.error = "no such folder";
            st.why = Text(I::AiActWhyNoFolder);
            return st;
        }
        OpenTabAt(s, path);
        st.ok = true;
        st.sub = Text(I::AiCtlApplied);
        run.other = true;
        AddLine(run, SplitLine(Fill(Text(I::AiActLineOpened), L"{name}", st.arg), st.arg, st.arg));
        AddKeys(run, Text(I::TooltipNewTab), KeysFor(app::KeyAction::NewTab));
        return st;
    }
    if (app::actions::HasArgRunner(id)) return CtlArgs(s, run, id, act);
    return CtlGeneric(s, run, id, act);
}

// A wait the step asks for ("wait"), or the listing a selection needs.
bool StepWait(AppState& s, const json::Value& step, const std::string& id, app::actions::WaitSpec& spec) {
    const json::Value& args = StepArgs(step);
    if (const json::Value* w = step.find("wait")) {
        json::Value obj = json::Value::Object();
        if (w->is_object()) obj = *w;
        else if (w->is_string()) obj.set("wait", w->as_string());
        if (!obj.find("pane"))
            if (const json::Value* pane = args.find("pane"); pane && !(pane->is_string() && pane->as_string() == "all"))
                obj.set("pane", *pane);
        std::string error;
        return app::actions::ParseWait(obj, spec, error);
    }
    if (id != "select.items") return false;
    spec = {};
    spec.kind = app::actions::WaitKind::ListingLoaded;
    spec.timeout_ms = 5000;
    int64_t n = 0;
    if (const json::Value* pane = args.find("pane")) {
        if (pane->is_string() && pane->as_string() == "target") spec.pane = -1;
        else if (pane->get_int(n, 1, 4)) spec.pane = static_cast<int>(n);
    }
    // A search page: all its results first, 10 s at most (P4 decision 4).
    if (app::actions::PaneShowsSearch(s, spec.pane)) {
        spec.kind = app::actions::WaitKind::SearchDone;
        spec.timeout_ms = 10000;
    }
    return true;
}

// 等待 搜索完成 · 186 项 · 用了 1.2 秒 (or 已有 120 项 · 10 秒内没搜完).
void SearchWaited(AppState& s, CtlRun& run, const json::Value& out) {
    if (RunGone(s, run)) return;
    AiAgent& a = Agent(s);
    int64_t results = 0, waited = 0;
    if (const json::Value* v = out.find("results")) v->get_int(results, 0);
    if (const json::Value* v = out.find("waited_ms")) v->get_int(waited, 0);
    run.search_partial = out.get_bool("timed_out");
    if (run.mcp) return;   // no step row
    const std::wstring items = Fill(Text(I::AiCtlItems), L"{n}", Num(static_cast<size_t>(results)));
    wchar_t secs[16]{};
    swprintf_s(secs, L"%.1f", static_cast<double>(waited) / 1000.0);
    EndStep(s, a, true,
            run.search_partial ? Fill(Text(I::AiActWaitedPartial), L"{items}", items)
                               : Fill(Fill(Text(I::AiActWaitedSub), L"{items}", items), L"{s}", secs));
    Redraw(s);
}

void CtlNext(AppState& s, std::shared_ptr<CtlRun> run) {
    if (RunGone(s, *run)) return;
    AiAgent& a = Agent(s);
    while (run->index < run->queue.size()) {
        const json::Value step = run->queue[run->index];
        const std::string what = StepAction(step);
        if (run->waited != run->index) {
            run->waited = run->index;
            app::actions::WaitSpec spec;
            json::Value ready;
            if (StepWait(s, step, what, spec) && !app::actions::WaitReady(s, spec, ready)) {
                AppState* self = &s;
                const bool search = spec.kind == app::actions::WaitKind::SearchDone;
                if (search && !run->mcp) {
                    AddStep(s, a, ui::AiIcon::Clock, Text(I::AiActStepWait), Text(I::AiActArgSearchDone), {});
                    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiActStepWait),
                                  static_cast<float>(run->index) / static_cast<float>(run->queue.size() + 1));
                    Redraw(s);
                }
                app::actions::WaitFor(s, spec, [self, run] { return RunGone(*self, *run); },
                                      [run, search](AppState& s2, json::Value out) {
                                          if (search) SearchWaited(s2, *run, out);
                                          CtlNext(s2, run);
                                      });
                return;
            }
        }
        // A file step: its card first; the call goes on from FoDone (P4c).
        if (run->file_state == 0 && app::actions::IsFileOp(what) && FoAsk(s, run, what, StepArgs(step))) return;
        if (run->file_state == 0 && what == "select.items" && SelectAsk(s, run, StepArgs(step))) return;
        if (run->file_state == 0 && what == "file.find_duplicates" && DupStepAsk(s, run, StepArgs(step))) return;
        if (run->file_state == 1) return;   // its card is up or its tasks run
        const size_t undo_before = run->undo.size();
        CtlStep st;
        if (run->file_state == 2 && run->file_step) {
            st = *run->file_step;
            run->file_step.reset();
            run->file_state = 0;
        } else {
            st = CtlRunStep(s, *run, what, StepArgs(step));
        }
        // A layout's folders are part of the layout line, not things of their own.
        if (run->undo.size() > undo_before && !(what == "nav.go" && run->layout)) ++run->undo_steps;
        if (!st.ok && st.why.empty()) st.why = Text(I::AiActWhyFailed);
        json::Value rep = json::Value::Object();
        rep.set("action", what);
        rep.set("ok", st.ok);
        if (!st.ok) rep.set("error", st.error);
        else if (st.wait) rep.set("result", Utf8(L"opened " + st.arg));
        else if (!st.sub.empty()) rep.set("result", Utf8(st.sub));
        if (st.skip) rep.set("already", true);   // nothing to do: it was so already
        if (!st.data.is_null())
            for (size_t k = 0; k < st.data.keys().size(); ++k) rep.set(st.data.keys()[k], st.data.items()[k]);
        run->report.push(std::move(rep));
        ++run->index;
        if (st.ok) ++run->ok;
        if (run->mcp && st.ok && st.skip) ++run->mcp_already;
        if (run->mcp && st.ok && !st.skip && !st.quiet) {
            const std::wstring title = st.title.empty() ? Display(what) : st.title;
            run->mcp_steps.push_back(st.arg.empty() ? title : title + L" " + st.arg);
        }
        if (!st.ok) {
            // Stop here: later steps often build on this one.
            run->fail_title = st.title;
            run->fail_arg = st.arg;
            run->fail_why = st.why;
            json::Value skipped = json::Value::Array();
            for (size_t k = run->index; k < run->queue.size(); ++k) {
                const std::string id = StepAction(run->queue[k]);
                run->not_run.push_back(StepTitle(s, id));
                skipped.push(id);
            }
            if (skipped.size()) {
                json::Value rep2 = json::Value::Object();
                rep2.set("not_run", std::move(skipped));
                run->report.push(std::move(rep2));
            }
            run->index = run->queue.size();
        }
        if (st.skip || st.quiet) continue;
        if (run->mcp) {
            // No step rows; a folder it opened still loads before the next step reads it (1.5 s at most).
            if (!st.wait) continue;
            const uint64_t opened = Now();
            Later(s, *run, 30, [run, st, opened](AppState& s2) {
                const app::Tab* t = PaneAlive(s2, st.wait) ? st.wait->ActiveTab() : nullptr;
                if (t && t->loading && Now() - opened < 1500) return 30;
                CtlNext(s2, run);
                return -1;
            });
            return;
        }
        AddStep(s, a, st.icon, st.title.empty() ? Display(what) : st.title, st.arg, {});
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, st.title,
                      static_cast<float>(run->index) / static_cast<float>(run->queue.size() + 1));
        Redraw(s);
        const uint64_t started = Now();
        AiChatPush(s, 450, [run, st, started](AppState& s2) {
            AiAgent& ag = Agent(s2);
            if (RunGone(s2, *run)) return -1;
            std::wstring sub = st.ok ? st.sub : st.why;
            if (st.wait) {
                // The folder's item count once it has loaded (1.5 s at most).
                const app::Tab* t = PaneAlive(s2, st.wait) ? st.wait->ActiveTab() : nullptr;
                if (t && t->loading && Now() - started < 1500) return 60;
                sub = t && !t->loading ? Fill(Text(I::AiCtlItemsIn), L"{n}", Num(t->EntryCount())) : Text(I::AiCtlApplied);
            }
            EndStep(s2, ag, st.ok, sub);
            CtlNext(s2, run);
            return -1;
        });
        return;
    }
    CtlFinish(s, run);
}

// A step that asks first: a program or script (nav.open), or a System action.
struct AskStep {
    size_t index = SIZE_MAX;
    std::string id;
    std::wstring target;   // nav.open: the file
    size_t count = 0;      // how many steps of the call ask
    bool has_text = false; // P4b: the action's own card words (推送到远端？)
    app::actions::ArgAsk text;
};

AskStep FirstAsk(AppState& s, const std::vector<json::Value>& queue) {
    AskStep ask;
    for (size_t i = 0; i < queue.size(); ++i) {
        const std::string id = StepAction(queue[i]);
        const std::wstring target = id == "nav.open" ? OpenTarget(s, StepArgs(queue[i])) : std::wstring();
        if (app::actions::AiPolicy(id, target).gate != app::actions::Gate::Confirm) continue;
        app::actions::ArgAsk text;
        const bool has_text = app::actions::HasAskText(id);
        if (has_text) {
            // Nothing to push, no repository: no card; the step says why.
            text = app::actions::AskFor(s, id, StepArgs(queue[i]));
            if (text.state != app::actions::ArgAsk::State::Ask) continue;
        }
        ++ask.count;
        if (ask.index != SIZE_MAX) continue;
        ask.index = i;
        ask.id = id;
        ask.target = target;
        ask.has_text = has_text;
        ask.text = std::move(text);
    }
    return ask;
}

ui::AiIcon AskIconOf(app::actions::AskIcon icon) {
    using K = app::actions::AskIcon;
    switch (icon) {
    case K::Push: return ui::AiIcon::Push;
    case K::Pull: return ui::AiIcon::Pull;
    case K::Verb: return ui::AiIcon::Package;
    case K::OpenWith: return ui::AiIcon::Open;
    case K::Cloud: return ui::AiIcon::Cloud;
    case K::Eject: return ui::AiIcon::Eject;
    case K::Disconnect: return ui::AiIcon::Plug;
    case K::System: break;
    }
    return ui::AiIcon::WinSettings;
}

void ToolRunActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    auto run = std::make_shared<CtlRun>();
    run->call_id = call.id;
    run->gen = a.generation;
    if (const json::Value* v = args.find("steps"); v && v->is_array())
        for (size_t i = 0; i < v->size() && i < 12; ++i)
            if (v->at(i).is_object()) run->queue.push_back(v->at(i));
    if (run->queue.empty()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "steps is empty"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    // Only settings: the settings card of the third batch (old -> new, 显示).
    bool only_settings = true;
    for (const auto& step : run->queue) only_settings = only_settings && StepAction(step) == "settings.set";
    if (only_settings) {
        json::Value changes = json::Value::Array();
        for (const auto& step : run->queue) {
            const json::Value& sa = StepArgs(step);
            json::Value ch = json::Value::Object();
            ch.set("id", sa.get_string("key", sa.get_string("id")));
            if (const json::Value* val = sa.find("value")) ch.set("value", *val);
            changes.push(std::move(ch));
        }
        json::Value wrapped = json::Value::Object();
        wrapped.set("changes", std::move(changes));
        ToolChangeSettings(s, call, wrapped);
        return;
    }
    size_t file_steps = 0;
    for (const auto& step : run->queue) file_steps += app::actions::IsFileOp(StepAction(step)) ? 1 : 0;
    if (file_steps > 1) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "one file step (move, copy, rename, delete, restore) per "
                                                                    "run_actions call: call again for the next, or use "
                                                                    "organize_files / run_task for many"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    HideRenameOverlay(s, true);   // panes and windows are about to move: end a rename like a click elsewhere
    FoldSteps(s, a);
    const AskStep ask = FirstAsk(s, run->queue);
    if (ask.index == SIZE_MAX) {
        CtlNext(s, run);
        return;
    }
    // One confirm card for the call, before any step runs.
    AiChat& ch = Chat(s);
    AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
    AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
    const bool program = ask.id == "nav.open";
    const std::wstring name = program ? LeafName(ask.target) : StepTitle(s, ask.id);
    cf.tag_amber = true;
    if (program) {
        cf.title = Text(I::AiActRunTitle);
        cf.tag = Text(I::AiActTagExternal);
        cf.sub = Text(I::AiActRunSub);
        cf.cf_icon = ui::AiIcon::Package;
        cf.file = name;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const size_t slash = ask.target.find_last_of(L"\\/");
        const std::wstring folder = slash == std::wstring::npos ? std::wstring() : FolderLabel(ask.target.substr(0, slash));
        if (GetFileAttributesExW(ask.target.c_str(), GetFileExInfoStandard, &fa))
            cf.file_note = AiChatFormatSize((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) +
                           (folder.empty() ? L"" : L" \u00B7 " + folder);
        cf.deny = Text(I::AiActRunDeny);
        cf.accept = Text(I::AiActRunAccept);
    } else if (ask.has_text) {
        // 推送到远端？ 外部效果 / 会把 feat/report 上的 3 个提交推到 origin。… (P4 scene 4)
        const app::actions::ArgAsk& k = ask.text;
        cf.title = k.title;
        cf.tag = Text(k.external ? I::AiActTagExternal : I::AiSetSys);
        cf.sub = k.sub;
        cf.cf_icon = AskIconOf(k.icon);
        cf.file = k.file;
        cf.file_note = k.file_note;
        cf.deny = k.deny.empty() ? Text(I::AiAsstDeny) : k.deny;
        cf.accept = k.accept.empty() ? Text(I::AiActSysAccept) : k.accept;
    } else {
        cf.title = Fill(Text(I::AiActSysTitle), L"{title}", name);
        cf.tag = Text(I::AiSetSys);
        cf.sub = Text(I::AiActSysSub);
        cf.cf_icon = ui::AiIcon::WinSettings;
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiActSysAccept);
    }
    cf.hint = Text(I::AiActConfirmHint);
    cf.hint_icon = ui::AiIcon::Shield;
    if (run->queue.size() > 1) {
        cf.hint1 = Fill(Text(I::AiActMoreSteps), L"{n}", Num(run->queue.size() - 1));
        cf.hint1_icon = ui::AiIcon::Tasks;
    }
    ch.confirm_id = cf.id;
    ch.on_confirm = [run, program, name](AppState& s3, bool accept) {
        AiChat& c3 = Chat(s3);
        if (AiBlockView* card = c3.Find(c3.confirm_id)) {
            card->done = true;
            card->title = accept ? Fill(Text(program ? I::AiActConfirmedRun : I::AiActConfirmedSys),
                                        program ? L"{name}" : L"{title}", name)
                                 : Fill(Text(program ? I::AiActDeniedRun : I::AiActDenied),
                                        program ? L"{name}" : L"{title}", name);
        }
        c3.confirm_id = 0;
        c3.on_confirm = nullptr;
        AiAgent& a3 = Agent(s3);
        if (a3.generation != run->gen) return;
        if (!accept) {
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiActWhyDeclined));
            ToolOutcome out;
            out.data = json::Value::Object();
            out.data.set("ok", false);
            out.data.set("declined", true);
            out.data.set("message", "The user declined in Pulse; no step of this call was done.");
            AddToolResult(a3, run->call_id, out);
            ++a3.call_index;
            Redraw(s3);
            NextTool(s3);
            return;
        }
        AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiCtlApplied), 0.2f);
        CtlNext(s3, run);
    };
    Redraw(s);
}

void ToolFindActions(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const std::string domain = args.get_string("domain");
    const std::wstring query = json::Wide(args.get_string("query"));
    const auto views = app::actions::ListActions(s);
    json::Value rows = json::Value::Array();
    std::wstring names;
    size_t found = 0;
    const auto add = [&](const app::actions::ActionView& v) {
        json::Value row = app::actions::ActionJson(v);
        if (!v.key.empty())
            if (const app::actions::SetDef* d = app::actions::FindSetting(v.key)) row.set("value", SettingJson(*d, d->get(s)));
        rows.push(std::move(row));
        if (found < 3) names += (names.empty() ? L"" : Text(I::AiTkListSep)) + v.title;
        ++found;
    };
    if (domain == "settings") {
        for (const auto& v : views)
            if (!v.key.empty()) add(v);
    } else {
        for (const app::actions::ActionView* v : app::actions::FindActions(views, query, 8)) add(*v);
    }
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("actions", std::move(rows));
    if (!found) out.data.set("note", "nothing matched; try other words (Chinese or English) or domain settings");
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiActStepFind), domain == "settings" ? Text(I::AiSetStep) : query, {});
    AddToolResult(a, call.id, out);
    const std::wstring sub = !found       ? Text(I::AiActFoundNone)
                             : found > 3 ? Fill(Fill(Text(I::AiActFound), L"{n}", Num(found)), L"{names}",
                                                names + Text(I::AiTkListSep) + L"\u2026")
                                         : Fill(Fill(Text(I::AiActFound), L"{n}", Num(found)), L"{names}", names);
    const uint64_t gen = a.generation;
    AiChatPush(s, 350, [gen, sub, found](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        EndStep(st, ag, found > 0, sub);
        ++ag.call_index;
        NextTool(st);
        return -1;
    });
    Redraw(s);
}
} // namespace ai_agent

} // namespace pulse
