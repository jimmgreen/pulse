// ai_agent_dups.cpp - Pulse 助手's find_duplicates (E): the scan, the groups and the moves.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

// ------------------------------------------------- find_duplicates (E) --
// Flow E: the local funnel, the groups
// with what each keeps and why, one confirmation, then the copies go to the
// Recycle Bin behind DuplicateCleanupGuard (keepers locked and re-checked).

struct DupRun {
    app::ai::DupScan scan;
    bool newest = false;
    std::vector<std::wstring> labels;     // per root (下载, 图片, …)
    std::wstring where;                   // 图片、下载
    std::vector<size_t> shown;            // groups on the card (at most 4)
    uint32_t funnel_id = 0, card_id = 0, confirm_id = 0, list_id = 0;
    std::string call_id;
    uint64_t gen = 0;
    uint64_t started = 0, run_started = 0;
    wchar_t drive = L'C';                 // where most of the copies are
    app::ai::BinState bin;
    size_t skipped = 0;                   // groups the pre-check left alone
    std::vector<size_t> moved_root;       // per root: copies moved
};

std::shared_ptr<DupRun> g_dup;

std::wstring SizeShort(uint64_t b) {
    wchar_t buf[32];
    if (b >= kGiB) swprintf_s(buf, L"%.1f GB", Gb(b));
    else if (b >= 1024ull * 1024) swprintf_s(buf, L"%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%llu KB", static_cast<unsigned long long>((b + 1023) / 1024));
    return buf;
}

std::wstring Secs1(uint64_t ms) {
    wchar_t buf[24];
    // Under 0.1 s show two decimals (at least 0.01) so a fast pass never reads 0.0.
    if (ms < 100) swprintf_s(buf, L"%.2f", static_cast<double>((std::max<uint64_t>)(ms, 10)) / 1000.0);
    else swprintf_s(buf, L"%.1f", static_cast<double>(ms) / 1000.0);
    return buf;
}

std::wstring DupDate(uint64_t ticks) {
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, lt{};
    SYSTEMTIME st{}, now{};
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return {};
    GetLocalTime(&now);
    const std::wstring t = Text(st.wYear == now.wYear ? I::AiDupDate : I::AiDupDateY);
    return Fill(Fill(Fill(t, L"{y}", Num(st.wYear)), L"{m}", Num(st.wMonth)), L"{d}", Num(st.wDay));
}

FILETIME DupFt(uint64_t ticks) { return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}; }

std::wstring DupRootLabel(const app::ai::DupRoot& r) {
    static const std::pair<const wchar_t*, I> kNames[] = {
        {L"downloads", I::AiDupRootDownloads}, {L"pictures", I::AiDupRootPictures}, {L"desktop", I::AiDupRootDesktop},
        {L"documents", I::AiDupRootDocuments}, {L"videos", I::AiDupRootVideos},     {L"music", I::AiDupRootMusic}};
    for (const auto& [key, id] : kNames)
        if (r.key == key) return Text(id);
    return FolderLabel(r.path);
}

size_t DupKeeper(const app::ai::DupGroup& g) {
    for (size_t i = 0; i < g.files.size(); ++i)
        if (g.files[i].keep) return i;
    return 0;
}

// A stable group title: the clean-named copy (no (1) / 副本), else the first;
// it does not follow the keeper when the user picks another one.
std::wstring DupGroupName(const app::ai::DupGroup& g) {
    for (const app::ai::DupFile& f : g.files)
        if (f.clean) return Leaf(f.path);
    return g.files.empty() ? std::wstring() : Leaf(g.files[0].path);
}

struct DupTotals {
    size_t groups = 0, kept = 0, extra = 0, both = 0, acting = 0;
    uint64_t bytes = 0;
    size_t first_both = SIZE_MAX;
};

DupTotals DupCount(const DupRun& run) {
    DupTotals t;
    t.groups = run.scan.groups.size();
    for (size_t i = 0; i < run.scan.groups.size(); ++i) {
        const app::ai::DupGroup& g = run.scan.groups[i];
        t.kept += g.Kept();
        t.extra += g.Extra();
        t.bytes += g.ExtraBytes();
        if (g.Extra()) ++t.acting;
        if (g.both) {
            ++t.both;
            if (t.first_both == SIZE_MAX) t.first_both = i;
        }
    }
    return t;
}

std::wstring DupWhyText(const app::ai::DupGroup& g) {
    using W = app::ai::DupWhy;
    switch (g.why) {
    case W::Organized: return Text(I::AiDupWhy1);
    case W::CleanName: return Text(I::AiDupWhy2);
    case W::Newest: return Text(I::AiDupWhy3New);
    case W::Both: return Fill(Text(I::AiDupWhyBoth), L"{k}", Num(g.Kept()));
    case W::Picked: return Text(I::AiDupWhyPicked);
    default: return Text(I::AiDupWhy3);
    }
}

void FillDupGroup(const DupRun& run, size_t gi, ui::AiDupGroupView& v) {
    const app::ai::DupGroup& g = run.scan.groups[gi];
    if (g.files.empty()) return;
    v.name = DupGroupName(g);
    AiChatGroupLook({v.name}, v.icon, v.tint);
    if (v.icon == ui::AiIcon::Folder) v.icon = ui::AiIcon::File;   // a group is a file, never a folder
    v.meta = Fill(Fill(Text(I::AiDupGroupMeta), L"{k}", Num(g.files.size())), L"{size}", SizeShort(g.files[0].bytes));
    v.both = g.both;
    v.why = DupWhyText(g);
    std::vector<ui::AiDupFileView> files;
    for (size_t i = 0; i < g.files.size() && i < 5; ++i) {
        const app::ai::DupFile& f = g.files[i];
        ui::AiDupFileView fv;
        fv.dir = app::ai::DupDirLabel(run.scan, f, f.root < run.labels.size() ? run.labels[f.root] : std::wstring());
        fv.name = Leaf(f.path);
        fv.date = DupDate(f.mtime);
        fv.keep = f.keep;
        fv.swap_ms = i < v.files.size() ? v.files[i].swap_ms : 0;
        files.push_back(std::move(fv));
    }
    v.files = std::move(files);
    v.more = g.files.size() > 5 ? Fill(Text(I::AiDupFileMore), L"{n}", Num(g.files.size() - 5)) : std::wstring();
}

// The 重复文件 tab: every group under its header, 保留 / 副本 in 状态, the
// copies selected.
void PutDupList(const DupRun& run) {
    std::vector<app::ai::AiListRow> rows;
    for (size_t gi = 0; gi < run.scan.groups.size() && gi < 254; ++gi) {
        const app::ai::DupGroup& g = run.scan.groups[gi];
        const std::wstring label = DupGroupName(g);
        for (const app::ai::DupFile& f : g.files) {
            app::ai::AiListRow r;
            r.path = f.path;
            r.reason = Text(f.keep ? I::AiDupKeep : I::AiDupExtra);
            r.bytes = f.bytes;
            r.mtime = DupFt(f.mtime);
            r.on = !f.keep;
            r.group = static_cast<uint8_t>(gi + 1);
            r.group_label = label;
            rows.push_back(std::move(r));
        }
    }
    app::ai::PutAiList(run.list_id, Fill(Text(I::AiDupListTab), L"{g}", Int(run.scan.groups.size())), std::move(rows),
                       Text(I::AiDupColumn));
}

std::wstring DupListPath(const DupRun& run) { return L"pulse:ai-list:" + std::to_wstring(run.list_id); }

// Reloads the open tab in place (keeps its scroll).
void RefreshDupList(AppState& s, const DupRun& run) {
    PutDupList(run);
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != DupListPath(run)) return;
    const float sy = tab->scroll_y;
    RefreshPath(s, tab->current_path);
    if (app::Tab* again = ActiveTab(s)) again->scroll_y = sy;
}

void SyncDup(AppState& s, DupRun& run, bool bump) {
    const DupTotals t = DupCount(run);
    AiChat& ch = Chat(s);
    const uint64_t now = Now();
    if (AiBlockView* b = ch.Find(run.card_id)) {
        b->meta = Fill(Fill(Text(I::AiDupMeta), L"{n}", Int(t.extra)), L"{size}", SizeShort(t.bytes));
        ui::AiDupView& dv = b->dup;
        dv.rules = {Text(I::AiDupRule1), Text(I::AiDupRule2), Text(run.newest ? I::AiDupRule3New : I::AiDupRule3)};
        for (size_t i = 0; i < run.shown.size() && i < dv.groups.size(); ++i) FillDupGroup(run, run.shown[i], dv.groups[i]);
        const std::wstring size = SizeShort(t.bytes);
        const size_t sp = size.find(L' ');
        std::vector<std::pair<std::wstring, bool>> sum = {
            {Text(I::AiDupSumPre) + L" ", false},       {Int(t.kept), true},
            {L" " + Text(I::AiDupSumMid) + L" ", false}, {Int(t.extra), true},
            {L" " + Text(I::AiDupSumMid2) + L" ", false}, {size.substr(0, sp), true},
            {sp == std::wstring::npos ? std::wstring() : size.substr(sp), false}};
        if (bump && sum != dv.sum) dv.sum_bump_ms = now;
        dv.sum = std::move(sum);
    }
    if (AiBlockView* cf = ch.Find(run.confirm_id); cf && !cf->done) {
        cf->title = Fill(Text(I::AiDupConfirmTitle), L"{n}", Int(t.extra));
        const std::wstring size = SizeShort(t.bytes);
        if (t.both == 0) {
            cf->sub = Fill(Fill(Text(I::AiDupConfirmSub), L"{size}", size), L"{g}", Int(t.acting));
        } else {
            const app::ai::DupGroup& g = run.scan.groups[t.first_both];
            const std::wstring name = DupGroupName(g);
            cf->sub = t.both == 1
                ? Fill(Fill(Fill(Fill(Text(I::AiDupConfirmSubBoth), L"{size}", size), L"{g}", Int(t.acting)), L"{name}", name),
                       L"{k}", Num(g.Kept()))
                : Fill(Fill(Fill(Fill(Text(I::AiDupConfirmSubBothN), L"{size}", size), L"{g}", Int(t.acting)), L"{name}", name),
                       L"{b}", Num(t.both));
        }
        uint64_t on_drive = 0;
        for (const auto& g : run.scan.groups)
            for (const auto& f : g.files)
                if (!f.keep && !f.path.empty() && towupper(f.path[0]) == run.drive) on_drive += f.bytes;
        const app::ai::BinState& bin = run.bin;
        const bool fits = bin.available && bin.used_bytes + on_drive <= bin.max_bytes;
        cf->hint2_warn = !fits;
        cf->hint2 = !bin.available ? Text(I::AiClBinOff)
                  : Fill(Fill(Text(fits ? I::AiClBinOk : I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}",
                         Gb1(bin.used_bytes));
        cf->accept_off = !fits || t.extra == 0;
        if (cf->accept_off == false) AiChatSetChip(ch, ui::StatusAiState::Waiting, Fill(Text(I::AiDupChipReady), L"{n}", Int(t.extra)));
    }
}

void DupClick(AppState& s, ui::AiPanelPart part, int g, int f) {
    std::shared_ptr<DupRun> run = g_dup;
    if (!run) return;
    AiBlockView* b = Chat(s).Find(run->card_id);
    if (!b) return;
    if (part == ui::AiPanelPart::DupMore) {
        PutDupList(*run);
        NavigateTo(s, DupListPath(*run));
        return;
    }
    if (part == ui::AiPanelPart::DupRule) {
        run->newest = !run->newest;
        for (app::ai::DupGroup& grp : run->scan.groups) app::ai::ApplyKeepRules(grp, run->newest);
        b->dup.rule_ms = Now();
    } else if (part == ui::AiPanelPart::DupFile) {
        if (g < 0 || static_cast<size_t>(g) >= run->shown.size() || static_cast<size_t>(g) >= b->dup.groups.size()) return;
        app::ai::DupGroup& grp = run->scan.groups[run->shown[static_cast<size_t>(g)]];
        if (f < 0 || static_cast<size_t>(f) >= grp.files.size()) return;
        if (grp.files[static_cast<size_t>(f)].keep && grp.Kept() == 1) return;   // already the one
        app::ai::PickKeeper(grp, static_cast<size_t>(f));
        for (ui::AiDupFileView& fv : b->dup.groups[static_cast<size_t>(g)].files) fv.swap_ms = Now();
    }
    SyncDup(s, *run, true);
    RefreshDupList(s, *run);
}

json::Value DupJson(const DupRun& run, size_t limit) {
    json::Value groups = json::Value::Array();
    for (size_t gi = 0; gi < run.scan.groups.size() && gi < limit; ++gi) {
        const app::ai::DupGroup& g = run.scan.groups[gi];
        json::Value e = json::Value::Object();
        e.set("name", Utf8(DupGroupName(g)));
        json::Value kept = json::Value::Array(), extra = json::Value::Array();
        for (const app::ai::DupFile& f : g.files) {
            json::Value fe = json::Value::Object();
            fe.set("path", Utf8(f.path));
            fe.set("date", Utf8(DupDate(f.mtime)));
            (f.keep ? kept : extra).push(std::move(fe));
        }
        e.set("kept", std::move(kept));
        e.set("copies", std::move(extra));
        e.set("why", Utf8(DupWhyText(g)));
        groups.push(std::move(e));
    }
    return groups;
}

void SettleDupRecycle(AppState& s, std::shared_ptr<DupRun> run, std::shared_ptr<SpaceClean> sc) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    size_t left = 0;
    std::vector<size_t> per_root(run->scan.roots.size(), 0);
    for (size_t i = 0; i < sc->sources.size(); ++i) {
        if (sc->moved[i]) {
            ++sc->moved_n;
            sc->moved_bytes += sc->source_bytes[i];
        } else {
            ++left;
        }
    }
    // Per root, for the result line (下载 49 份 · 图片 6 份).
    for (const app::ai::DupGroup& g : run->scan.groups)
        for (const app::ai::DupFile& f : g.files)
            for (size_t i = 0; i < sc->sources.size(); ++i)
                if (sc->moved[i] && sc->sources[i] == f.path && f.root < per_root.size()) ++per_root[f.root];
    RefreshDupList(s, *run);
    const std::wstring size = SizeShort(sc->moved_bytes);
    EndStep(s, a, sc->moved_n > 0,
            Fill(Fill(Fill(Text(I::AiDupRecDone), L"{n}", Int(sc->moved_n)), L"{size}", size), L"{t}",
                 Secs1(Now() - run->run_started)) +
                (left ? Fill(Text(I::AiClRecSkip), L"{k}", Int(left)) : std::wstring()));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->collapsed = true;
        sb->open = false;
    }
    a.steps_id = 0;
    if (AiBlockView* db = c.Find(run->card_id)) {
        db->dup.settled = sc->moved_n > 0;
        if (sc->moved_n) db->collapsed = true;
    }
    const DupTotals t = DupCount(*run);
    // Only groups that lost a copy count: a skipped or 都留 group is not "kept".
    size_t groups_moved = 0, kept_moved = 0;
    for (const app::ai::DupGroup& g : run->scan.groups) {
        bool any = false;
        for (const app::ai::DupFile& f : g.files)
            for (size_t i = 0; i < sc->sources.size() && !any; ++i)
                if (sc->moved[i] && sc->sources[i] == f.path) any = true;
        if (!any) continue;
        ++groups_moved;
        kept_moved += g.Kept();
    }
    if (sc->moved_n) {
        AiBlockView& r = c.Add(AiBlockKind::Result, Now());
        r.title = Fill(Text(I::AiDupResTitle), L"{n}", Int(sc->moved_n));
        r.sub = Fill(Fill(Text(I::AiDupResSub), L"{g}", Int(groups_moved)), L"{size}", size);
        {
            ui::AiResultLine ln;
            bool first = true;
            for (size_t ri = 0; ri < per_root.size(); ++ri) {
                if (!per_root[ri]) continue;
                const std::wstring label = ri < run->labels.size() ? run->labels[ri] : std::wstring();
                if (first) {
                    ln.before = label + L" ";
                    ln.bold = Int(per_root[ri]);
                    ln.after = L" " + Text(I::AiDupResUnit);
                    first = false;
                } else {
                    ln.after += L" \u00B7 " + label + L" " + Int(per_root[ri]) + L" " + Text(I::AiDupResUnit);
                }
            }
            if (!first) r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.before = Fill(Text(I::AiDupResKept), L"{n}", Int(kept_moved));
            r.lines.push_back(std::move(ln));
        }
        if (run->skipped || left) {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = run->skipped ? Fill(Text(I::AiDupResSkipped), L"{g}", Int(run->skipped))
                                     : Fill(Text(I::AiClResSkipped), L"{k}", Int(left));
            r.lines.push_back(std::move(ln));
        }
        {
            ui::AiResultLine ln;
            ln.warn = true;
            ln.before = Fill(Text(I::AiDupResStill), L"{size}", size);
            r.lines.push_back(std::move(ln));
        }
        r.deny = Text(I::AiAsstUndo);
        r.purge = Text(I::AiClPurge);
        sc->result_id = r.id;
        c.result_id = r.id;
        c.on_show = nullptr;
        c.on_undo = MakeCleanUndo(sc);
        c.on_purge = [sc](AppState& st) { AskPurge(st, sc); };
        c.peek_text = Fill(Fill(Text(I::AiDupPeek), L"{n}", Int(sc->moved_n)), L"{size}", size);
        c.peek_undo = true;
    } else {
        AiBlockView& note = c.Add(AiBlockKind::Note, Now());
        note.text = Text(I::AiDupGuardNote);
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", sc->moved_n > 0);
    out.data.set("moved_copies", static_cast<uint64_t>(sc->moved_n));
    out.data.set("moved_size", Utf8(size));
    out.data.set("groups", static_cast<uint64_t>(t.groups));
    out.data.set("groups_where_all_copies_stay", static_cast<uint64_t>(t.both));
    out.data.set("kept_files", static_cast<uint64_t>(kept_moved));
    json::Value where = json::Value::Object();
    for (size_t ri = 0; ri < per_root.size(); ++ri)
        if (per_root[ri] && ri < run->labels.size()) where.set(Utf8(run->labels[ri]), static_cast<uint64_t>(per_root[ri]));
    out.data.set("moved_by_folder", std::move(where));
    out.data.set("examples", DupJson(*run, 6));
    if (sc->moved_n) {
        out.data.set("note", "The copies are in the Recycle Bin, so they still take space until it is emptied. The user can "
                             "undo with the card's Undo button, or free the space with its "
                             "\xE6\xB0\xB8\xE4\xB9\x85\xE5\x88\xA0\xE9\x99\xA4\xE8\xBF\x99\xE4\xBA\x9B button, which deletes "
                             "only this batch. Say so briefly and name a group where all copies stayed, if any."
                             + std::string(run->skipped ? " groups_skipped_because_files_changed: a file in those groups "
                                                          "changed after the compare, so Pulse left the whole group "
                                                          "untouched; say that plainly (not that its copies were kept on "
                                                          "purpose) and offer to compare again."
                                                        : ""));
    } else {
        out.data.set("note", "Pulse moved nothing: files changed after the compare, so the guard stopped. Suggest running "
                             "find_duplicates again.");
    }
    if (run->skipped) out.data.set("groups_skipped_because_files_changed", static_cast<uint64_t>(run->skipped));
    if (left) out.data.set("not_moved", static_cast<uint64_t>(left));
    AddToolResult(a, sc->call_id, out);
    ++a.call_index;
    AiChatPush(s, 350, [](AppState& s2) { NextTool(s2); return -1; });
    Redraw(s);
}

void RunDupRecycle(AppState& s, std::shared_ptr<DupRun> run) {
    AiAgent& a = Agent(s);
    const DupTotals t = DupCount(*run);
    run->run_started = Now();
    a.organized = true;
    AddStep(s, a, ui::AiIcon::Trash, Text(I::AiClRecStep),
            Fill(Fill(Text(I::AiDupRecArg), L"{n}", Int(t.extra)), L"{size}", SizeShort(t.bytes)), Text(I::AiDupRecVerify));
    if (AiBlockView* sb = Block(s, a.steps_id)) {
        sb->label = Fill(Text(I::AiDupRecSteps), L"{n}", Int(t.extra));
        sb->open = true;
    }
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiDupChipVerify), 0.1f);
    // Pre-check each group on a worker: the guard is all or nothing, so a
    // group whose files changed since the compare is dropped here.
    auto ok = std::make_shared<std::vector<char>>(run->scan.groups.size(), 0);
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([run, ok, job] {
        for (size_t gi = 0; gi < run->scan.groups.size(); ++gi) {
            if (job->cancelled.load()) break;
            const app::ai::DupGroup& g = run->scan.groups[gi];
            if (!g.Extra()) continue;
            const app::ai::DupFile& keeper = g.files[DupKeeper(g)];
            bool same = true;
            for (const app::ai::DupFile& f : g.files)
                if (!f.keep && !app::ai::DupStillSame(keeper, f, job->cancelled)) {
                    same = false;
                    break;
                }
            (*ok)[gi] = same ? 1 : 0;
        }
        job->done.store(true);
    }).detach();
    AiChatPush(s, 60, [run, ok, job](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != run->gen) {
            job->cancelled.store(true);
            return -1;
        }
        if (!job->done.load()) return 60;
        ag.job.reset();
        auto sc = std::make_shared<SpaceClean>();
        sc->dup = true;
        sc->drive = run->drive;
        sc->call_id = run->call_id;
        sc->gen = run->gen;
        sc->bin = run->bin;
        sc->scan.cats.resize(1);
        app::ai::CleanCategory& cat = sc->scan.cats[0];
        cat.kind = app::ai::CleanKind::Copy;
        ops::OpRequest r;
        r.type = ops::OpType::RecycleDelete;
        r.duplicate_cleanup = true;
        run->skipped = 0;
        for (size_t gi = 0; gi < run->scan.groups.size(); ++gi) {
            const app::ai::DupGroup& g = run->scan.groups[gi];
            if (!g.Extra()) continue;
            if (!(*ok)[gi]) {
                ++run->skipped;
                continue;
            }
            const app::ai::DupFile& keeper = g.files[DupKeeper(g)];
            ops::DuplicateCleanupGroup cg;
            cg.keeper = {keeper.path, keeper.bytes, keeper.mtime};
            for (const app::ai::DupFile& f : g.files) {
                if (f.keep) continue;
                cg.extras.push_back({f.path, f.bytes, f.mtime});
                r.sources.push_back(f.path);
                app::ai::CleanItem it;
                it.path = f.path;
                it.bytes = f.bytes;
                it.files = 1;
                it.mtime = DupFt(f.mtime);
                cat.items.push_back(it);
                cat.files += 1;
                cat.bytes += f.bytes;
                sc->sources.push_back(f.path);
                sc->source_bytes.push_back(f.bytes);
                sc->source_cat.push_back(0);
            }
            r.duplicate_groups.push_back(std::move(cg));
        }
        sc->moved.assign(sc->sources.size(), false);
        DriveSpace(sc->drive, sc->total, sc->free_before);
        sc->bin_items_before = sc->bin.items;
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        sc->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const uint64_t since = sc->undo_group - 20000000ull;
        sc->since.dwLowDateTime = static_cast<DWORD>(since);
        sc->since.dwHighDateTime = static_cast<DWORD>(since >> 32);
        if (sc->sources.empty()) {
            SettleDupRecycle(st, run, sc);
            return -1;
        }
        r.undo_group = sc->undo_group;
        r.undo_label = Text(I::AiDupUndoLabel);
        st.ops.Submit(std::move(r));
        struct Watch { uint64_t started = 0, last_change = 0; size_t gone = 0; };
        auto w = std::make_shared<Watch>();
        w->started = w->last_change = Now();
        AiChatPush(st, 150, [run, sc, w](AppState& s2) {
            AiAgent& a2 = Agent(s2);
            if (a2.generation != sc->gen) return -1;
            const uint64_t now = Now();
            size_t gone = 0;
            for (size_t i = 0; i < sc->sources.size(); ++i) {
                if (!sc->moved[i] && !Exists(sc->sources[i])) sc->moved[i] = true;
                if (sc->moved[i]) ++gone;
            }
            if (gone != w->gone) {
                w->gone = gone;
                w->last_change = now;
            }
            const float prog = static_cast<float>(gone) / static_cast<float>(sc->sources.size());
            if (ui::AiStepView* step = LastStep(s2, a2)) {
                step->progress = prog;
                step->sub = Fill(Fill(Text(I::AiDupRecSub), L"{k}", Int(gone)), L"{n}", Int(sc->sources.size()));
            }
            AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                          Fill(Text(I::AiClChipRec), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f))), prog);
            const bool idle = !s2.ops.Status().active;
            if (gone >= sc->sources.size() || (idle && now - w->last_change > 2500) || now - w->started > 600000) {
                SettleDupRecycle(s2, run, sc);
                return -1;
            }
            return 150;
        });
        Redraw(st);
        return -1;
    });
}

// The card, the tab and the confirmation once the funnel is done.
void ShowDupGroups(AppState& s, std::shared_ptr<DupRun> run) {
    AiChat& c = Chat(s);
    if (AiBlockView* fb = c.Find(run->funnel_id)) fb->collapsed = true;
    AiBlockView& b = c.Add(AiBlockKind::Dup, Now());
    run->card_id = b.id;
    run->list_id = b.id * 8;
    b.label = Fill(Text(I::AiDupTitle), L"{g}", Int(run->scan.groups.size()));
    ui::AiDupView& dv = b.dup;
    dv.change = Text(I::AiDupChange);
    dv.both_tag = Text(I::AiDupBoth);
    dv.keep_tag = Text(I::AiDupKeep);
    dv.extra_tag = Text(I::AiDupExtra);
    dv.safe = Text(I::AiDupSafe);
    // Three groups that act plus the first 都留 one, in list order.
    std::vector<size_t> pick;
    size_t first_both = SIZE_MAX;
    for (size_t i = 0; i < run->scan.groups.size(); ++i) {
        if (run->scan.groups[i].both) {
            if (first_both == SIZE_MAX) first_both = i;
        } else if (pick.size() < 3) {
            pick.push_back(i);
        }
    }
    if (first_both != SIZE_MAX) pick.push_back(first_both);
    for (size_t i = 0; i < run->scan.groups.size() && pick.size() < 4; ++i)
        if (std::find(pick.begin(), pick.end(), i) == pick.end()) pick.push_back(i);
    std::sort(pick.begin(), pick.end());
    run->shown = pick;
    const uint64_t now = Now();
    for (size_t i = 0; i < pick.size(); ++i) {
        ui::AiDupGroupView v;
        v.in_ms = now + 120 + 170 * i;
        dv.groups.push_back(std::move(v));
    }
    if (run->scan.groups.size() > pick.size()) {
        dv.more = Fill(Text(I::AiDupMore), L"{n}", Int(run->scan.groups.size() - pick.size()));
        dv.more_link = Text(I::AiDupMoreLink);
    }
    SyncDup(s, *run, false);
    c.dup_id = b.id;
    c.on_dup = [](AppState& s2, ui::AiPanelPart part, int g, int f) { DupClick(s2, part, g, f); };
    AiChatSetChip(c, ui::StatusAiState::Done, Fill(Text(I::AiDupChipDone), L"{g}", Int(run->scan.groups.size())));
    // The tab opens once the groups landed; the confirmation a second later.
    AiChatPush(s, static_cast<uint32_t>(120 + 170 * pick.size()), [run](AppState& s2) {
        if (Agent(s2).generation != run->gen) return -1;
        PutDupList(*run);
        NavigateTo(s2, DupListPath(*run));
        return -1;
    });
    AiChatPush(s, 1000, [run](AppState& s4) {
        AiAgent& a4 = Agent(s4);
        if (a4.generation != run->gen) return -1;
        AiChat& ch = Chat(s4);
        if (!s4.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.tag = Text(I::AiClTagDelete);
        cf.hint = Text(I::AiDupLockHint);
        cf.hint_icon = ui::AiIcon::Lock;
        cf.hint1 = Text(I::AiDupUndoHint);
        cf.hint1_icon = ui::AiIcon::Undo;
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiClAccept);
        run->confirm_id = cf.id;
        ch.confirm_id = cf.id;
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        SyncDup(s4, *run, false);
        ch.on_confirm = [run](AppState& s3, bool accept) {
            AiChat& c3 = Chat(s3);
            AiAgent& a3 = Agent(s3);
            const DupTotals t = DupCount(*run);
            if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                const uint64_t now2 = Now();
                if (now2 > card->born_ms) c3.turn_start += now2 - card->born_ms;
                card->done = true;
                card->tag.clear();
                card->hint2.clear();
                card->title = accept ? Fill(Text(I::AiDupConfirmed), L"{n}", Int(t.extra)) : Text(I::AiDupDenied);
            }
            c3.confirm_id = 0;
            c3.on_confirm = nullptr;
            c3.dup_id = 0;
            c3.on_dup = nullptr;
            if (a3.generation != run->gen) return;
            if (!accept) {
                if (AiBlockView* db = c3.Find(run->card_id)) db->collapsed = true;
                ToolOutcome out = ToolOutcome::Fail("user_denied", "The user declined moving the duplicate copies in "
                                                                   "Pulse; nothing was moved. Do not offer it again; ask "
                                                                   "what they would like instead.");
                AddToolResult(a3, run->call_id, out);
                ++a3.call_index;
                AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s3);
                Redraw(s3);
                return;
            }
            RunDupRecycle(s3, run);
            Redraw(s3);
        };
        Redraw(s4);
        return -1;
    });
    Redraw(s);
}

void ToolFindDuplicates(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    std::vector<std::wstring> names;
    if (const json::Value* f = args.find("folders"); f && f->is_array())
        for (size_t i = 0; i < f->size() && i < 8; ++i)
            if (f->at(i).is_string()) names.push_back(json::Wide(f->at(i).as_string()));
    std::wstring current;
    if (app::Tab* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) current = NormDir(tab->current_path);
    auto run = std::make_shared<DupRun>();
    std::string error;
    if (!app::ai::ResolveDupRoots(names, current, run->scan.roots, error)) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", error));
        ++a.call_index;
        NextTool(s);
        return;
    }
    run->newest = args.get_string("prefer") == "newest";
    run->call_id = call.id;
    run->gen = a.generation;
    run->started = Now();
    for (const app::ai::DupRoot& r : run->scan.roots) {
        run->labels.push_back(DupRootLabel(r));
        run->where += (run->where.empty() ? L"" : Text(I::AiTkListSep)) + run->labels.back();
    }
    AddStep(s, a, ui::AiIcon::Duplicate, Text(I::AiDupStep), run->where, Text(I::AiDupStepSub));
    AiChat& c = Chat(s);
    AiChatSetChip(c, ui::StatusAiState::Busy, Fill(Text(I::AiDupChipRun), L"{i}", L"1"), 0.05f);
    // 比对过程.
    {
        AiBlockView& fb = c.Add(AiBlockKind::Funnel, Now());
        run->funnel_id = fb.id;
        fb.label = Text(I::AiDupFunnel);
        fb.funnel.foot = Text(I::AiDupFunnelFoot);
        const I labels[] = {I::AiDupS1, I::AiDupS2, I::AiDupS3};
        const I subs[] = {I::AiDupS1Sub, I::AiDupS2Sub, I::AiDupS3Sub};
        for (int i = 0; i < 3; ++i) {
            ui::AiFunnelStageView st;
            st.label = Text(labels[i]);
            st.sub = i == 0 ? Fill(Text(I::AiDupS1Run), L"{n}", L"0") : Text(subs[i]);
            st.unit = Text(I::AiDupUnit);
            st.count_dur = i == 2 ? 1300 : 650;
            fb.funnel.stages.push_back(std::move(st));
        }
        fb.funnel.stages[0].state = ui::AiFunnelState::Run;
        fb.funnel.stages[0].state_ms = Now();
    }
    auto progress = std::make_shared<app::ai::DupProgress>();
    auto job = std::make_shared<AiAgent::Job>();
    a.job = job;
    std::thread([job, run, progress] {
        app::ai::FindDuplicates(run->scan, 1024, *progress, job->cancelled);
        if (run->newest)
            for (app::ai::DupGroup& g : run->scan.groups) app::ai::ApplyKeepRules(g, true);
        job->done.store(true);
    }).detach();
    // The rows follow the scan; each holds its count-up before the next.
    struct Pace { int ds = 0, phase = 0; uint64_t t = 0; };
    auto pace = std::make_shared<Pace>();
    pace->t = Now();
    AiChatPush(s, 40, [run, job, progress, pace](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != run->gen) {
            job->cancelled.store(true);
            return -1;
        }
        AiChat& ch = Chat(st);
        AiBlockView* fb = ch.Find(run->funnel_id);
        if (!fb || fb->funnel.stages.size() != 3) return -1;
        const uint64_t now = Now();
        const int reached = job->done.load() ? 3 : progress->stage.load();
        ui::AiFunnelStageView& row = fb->funnel.stages[static_cast<size_t>(pace->ds)];
        const app::ai::DupScan& scan = run->scan;
        if (pace->phase == 0) {
            if (pace->ds == 0) row.sub = Fill(Text(I::AiDupS1Run), L"{n}", Int(progress->files.load()));
            else if (reached == pace->ds && progress->todo.load())
                row.sub = Fill(Fill(Text(I::AiDupSRun), L"{k}", Int(progress->done.load())), L"{n}", Int(progress->todo.load()));
            if (reached > pace->ds && now - pace->t >= 300) {
                const uint64_t base = (std::max)(scan.size_matches, uint64_t{1});
                const auto bar = [&](uint64_t v) {
                    return v ? (std::max)(0.04f, static_cast<float>(std::pow(static_cast<double>(v) / static_cast<double>(base), 0.6)))
                             : 0.0f;
                };
                if (pace->ds == 0) {
                    row.value = scan.size_matches;
                    row.sub = Fill(Text(I::AiDupS1Sub), L"{n}", Int(scan.total_files));
                    row.time = Fill(Text(I::AiDupSec), L"{t}", Secs1(scan.ms[0]));
                    row.bar = scan.size_matches ? 1.0f : 0.0f;
                } else if (pace->ds == 1) {
                    row.value = scan.head_matches;
                    row.sub = Text(I::AiDupS2Sub);
                    row.time = Fill(Text(I::AiDupSec), L"{t}", Secs1(scan.ms[1]));
                    row.bar = bar(scan.head_matches);
                } else {
                    row.value = scan.dup_files;
                    row.unit = Fill(Text(I::AiDupUnitGroups), L"{g}", Int(scan.groups.size()));
                    row.sub = Text(I::AiDupS3Sub);
                    row.time = Fill(Fill(Text(I::AiDupSecRead), L"{t}", Secs1(scan.ms[2])), L"{size}", SizeShort(scan.read_bytes));
                    row.bar = bar(scan.dup_files);
                }
                row.count_ms = now;
                pace->phase = 1;
                pace->t = now;
            }
        } else if (pace->phase == 1) {
            if (now - pace->t >= row.count_dur) {
                row.state = ui::AiFunnelState::Ok;
                row.state_ms = now;
                pace->phase = 2;
                pace->t = now;
            }
        } else if (now - pace->t >= 160) {
            ++pace->ds;
            pace->phase = 0;
            pace->t = now;
            if (pace->ds < 3) {
                fb->funnel.stages[static_cast<size_t>(pace->ds)].state = ui::AiFunnelState::Run;
                fb->funnel.stages[static_cast<size_t>(pace->ds)].state_ms = now;
            }
        }
        if (pace->ds < 3) {
            const float prog = (static_cast<float>(pace->ds) + (pace->phase ? 0.9f : 0.3f)) / 3.0f;
            if (ui::AiStepView* step = LastStep(st, ag)) step->progress = prog;
            AiChatSetChip(ch, ui::StatusAiState::Busy, Fill(Text(I::AiDupChipRun), L"{i}", Num(static_cast<size_t>(pace->ds + 1))), prog);
            Redraw(st);
            return 40;
        }
        // Done.
        ag.job.reset();
        const uint64_t total_ms = scan.ms[0] + scan.ms[1] + scan.ms[2];
        fb->meta = Fill(Text(I::AiDupFunnelMeta), L"{t}", Secs1(total_ms));
        const bool none = scan.groups.empty();
        EndStep(st, ag, true,
                none ? Fill(Fill(Text(I::AiDupStepNone), L"{n}", Int(scan.total_files)), L"{t}", Secs1(total_ms))
                     : Fill(Fill(Fill(Text(I::AiDupStepDone), L"{g}", Int(scan.groups.size())), L"{n}", Int(scan.dup_files)),
                            L"{t}", Secs1(total_ms)));
        if (AiBlockView* sb = Block(st, ag.steps_id)) {
            sb->label = none ? Fill(Text(I::AiDupStepsNone), L"{where}", run->where)
                             : Fill(Fill(Text(I::AiDupSteps), L"{where}", run->where), L"{g}", Int(scan.groups.size()));
            sb->collapsed = true;
            sb->open = false;
        }
        ag.steps_id = 0;
        if (none) {
            AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            ToolOutcome out;
            out.data = json::Value::Object();
            out.data.set("groups", static_cast<uint64_t>(0));
            out.data.set("files_compared", scan.total_files);
            out.data.set("note", "No two files have the same content in these folders. Say so in one sentence.");
            AddToolResult(ag, run->call_id, out);
            ++ag.call_index;
            AiChatPush(st, 350, [](AppState& s2) { NextTool(s2); return -1; });
            Redraw(st);
            return -1;
        }
        // The Recycle Bin of the drive with most of the copies.
        std::map<wchar_t, uint64_t> per_drive;
        for (const auto& g : scan.groups)
            for (const auto& f : g.files)
                if (!f.keep && !f.path.empty()) per_drive[static_cast<wchar_t>(towupper(f.path[0]))] += f.bytes;
        uint64_t most = 0;
        for (const auto& [d, n] : per_drive)
            if (n > most) {
                most = n;
                run->drive = d;
            }
        run->bin = app::ai::ReadBin(run->drive);
        g_dup = run;
        AiChatSetChip(ch, ui::StatusAiState::Done, Fill(Text(I::AiDupChipDone), L"{g}", Int(scan.groups.size())));
        Redraw(st);
        AiChatPush(st, 800, [run](AppState& s2) {
            if (Agent(s2).generation != run->gen) return -1;
            ShowDupGroups(s2, run);
            return -1;
        });
        return -1;
    });
}
} // namespace ai_agent

} // namespace pulse
