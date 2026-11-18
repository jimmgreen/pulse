// ai_agent_search.cpp - Pulse 助手's search_files: the query card, its chips and follow-ups.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

// ------------------------------------------------------- search_files --
// Flow A: the model only turns the
// sentence into conditions; Pulse's own search runs them in the active tab
// (sortable, previewable, Back returns to the folder) and the 搜索条件 card
// follows the answer. A chip's × and + 条件 search again locally: no model.

struct SearchStats {
    size_t total = 0;
    uint64_t bytes = 0;
    bool dirs = false;
    std::vector<std::pair<std::wstring, size_t>> folders;   // parent, count; most first
    std::vector<const fs::DirEntry*> items;                  // the page Pulse loaded
};

SearchStats StatsOf(const app::Tab& tab) {
    SearchStats st;
    std::unordered_map<std::wstring, size_t> at;   // lower-case parent -> index in folders
    if (tab.search_entries) {
        for (const fs::DirEntry& e : *tab.search_entries) {
            st.items.push_back(&e);
            if (e.is_dir) st.dirs = true;
            else st.bytes += e.size;
            const std::wstring parent = app::ai::ParentFolder(NormDir(e.full_path));
            const std::wstring key = ui::AiListLower(parent);
            auto it = at.find(key);
            if (it == at.end()) {
                at.emplace(key, st.folders.size());
                st.folders.push_back({parent, 1});
            } else {
                ++st.folders[it->second].second;
            }
        }
    }
    st.total = (std::max)(static_cast<size_t>(tab.search_total), st.items.size());
    std::stable_sort(st.folders.begin(), st.folders.end(),
                     [](const auto& x, const auto& y) { return x.second > y.second; });
    return st;
}

// A results folder as the card names it: 下载, or 文档 › 财务 inside a known
// folder.
std::wstring ResultFolderLabel(const std::wstring& folder) {
    const std::wstring label = FolderLabel(folder);
    if (label != Leaf(folder)) return label;
    const std::wstring parent = app::ai::ParentFolder(folder);
    if (parent.empty()) return label;
    const std::wstring up = FolderLabel(parent);
    return up != Leaf(parent) ? up + L" \u203A " + label : label;
}

std::wstring Secs(uint64_t ms) {
    wchar_t buf[16];
    swprintf_s(buf, L"%.2f", static_cast<double>((std::max<uint64_t>)(ms, 10)) / 1000.0);
    return buf;
}

std::wstring SearchRest(const std::wstring& path) {
    std::wstring kind, rest;
    return app::ParsePulsePath(path, &kind, &rest) && kind == L"search" ? rest : std::wstring();
}

// Calls `done` once Pulse's search of `path` in the active tab has its first
// page (nullptr: the tab went elsewhere or the index did not answer).
void AwaitSearch(AppState& s, const std::wstring& path, std::function<void(AppState&, const app::Tab*, uint64_t)> done) {
    const uint64_t started = Now();
    AiChatPush(s, 0, [path, started, done = std::move(done)](AppState& st) {
        app::Tab* tab = ActiveTab(st);
        const bool here = tab && tab->current_path == path;
        if (here && !tab->loading && tab->pending_generation == 0) {
            done(st, tab, Now() - started);
            return -1;
        }
        if (!here || Now() - started > 15000) {
            done(st, nullptr, Now() - started);
            return -1;
        }
        return 16;
    });
}

AiAgent::QueryCard* CardOf(AiAgent& a, uint32_t block_id, bool follow) {
    for (auto& q : a.queries)
        if ((follow ? q->follow_id : q->block_id) == block_id) return q.get();
    return nullptr;
}

// The chips of `q.spec` (new ones pop in one after another).
void FillChips(AppState& s, AiAgent::QueryCard& q, bool pop) {
    AiBlockView* b = Block(s, q.block_id);
    if (!b) return;
    const std::vector<app::ai::SearchChip> chips = app::ai::SearchChips(q.spec, &ResultFolderLabel);
    size_t removable = 0;
    for (const auto& c : chips)
        if (!c.fixed && c.field != app::ai::SearchField::Folder) ++removable;
    std::vector<ui::AiQueryChipView> views;
    q.fields.clear();
    uint64_t at = Now() + 120;
    for (const auto& c : chips) {
        ui::AiQueryChipView v;
        v.key = c.key;
        v.value = c.value;
        // The last condition stays: without it the search would list everything.
        v.fixed = c.fixed || (removable <= 1 && c.field != app::ai::SearchField::Folder);
        bool known = false;
        for (const ui::AiQueryChipView& old : b->chips)
            if (!old.out_ms && old.key == v.key && old.value == v.value) known = true;
        if (pop && !known) {
            v.in_ms = at;
            at += 90;
        }
        views.push_back(std::move(v));
        q.fields.push_back(c.field);
    }
    b->chips = std::move(views);
}

void FillCounts(AppState& s, AiAgent::QueryCard& q, const SearchStats& st, uint64_t ms, bool rerun) {
    q.count = st.total;
    q.paths.clear();
    for (const fs::DirEntry* e : st.items) q.paths.push_back(NormDir(e->full_path));
    if (AiBlockView* b = Block(s, q.block_id)) {
        b->title = Text(I::AiQTitle);
        b->meta = Fill(Text(I::AiQMeta), L"{t}", Secs(ms));
        b->add = Text(I::AiQAdd);
        const std::wstring count = Num(st.total);
        if (rerun && b->count != count) b->bump_ms = Now();
        b->count = count;
        b->label = Fill(Text(st.dirs ? I::AiQItems : I::AiQFiles), L"{size}", AiChatFormatSize(st.bytes));
        if (rerun) {
            if (!b->re_ms) b->re_ms = Now();
            b->hint = Text(I::AiQRerun);
        }
        b->tags.clear();
        size_t shown = 0, rest = 0;
        for (const auto& [folder, n] : st.folders) {
            if (shown < 4) {
                ui::AiTagView t;
                t.text = ResultFolderLabel(folder);
                t.bold = Num(n);
                b->tags.push_back(std::move(t));
                ++shown;
            } else {
                rest += n;
            }
        }
        if (rest) {
            ui::AiTagView t;
            t.text = Text(I::AiQOther);
            t.bold = Num(rest);
            b->tags.push_back(std::move(t));
        }
    }
    if (AiBlockView* f = Block(s, q.follow_id); f && !f->follow.empty())
        f->follow[0].text = Fill(Text(I::AiFollowZip), L"{n}", Num(st.total));
}

json::Value SearchResultForModel(const AiAgent::QueryCard& q, const SearchStats& st) {
    json::Value out = json::Value::Object();
    out.set("query", Utf8(SearchRest(q.path)));
    out.set("shown_in", "the active Pulse tab, as a search the user can sort, preview and edit");
    out.set("total", static_cast<uint64_t>(st.total));
    out.set("total_size", st.bytes);
    json::Value folders = json::Value::Array();
    for (size_t i = 0; i < st.folders.size() && i < 8; ++i) {
        json::Value f = json::Value::Object();
        f.set("folder", Utf8(st.folders[i].first));
        f.set("count", static_cast<uint64_t>(st.folders[i].second));
        folders.push(std::move(f));
    }
    out.set("folders", std::move(folders));
    json::Value items = json::Value::Array();
    for (size_t i = 0; i < st.items.size() && i < 40; ++i) {
        const fs::DirEntry& e = *st.items[i];
        json::Value it = json::Value::Object();
        it.set("path", Utf8(NormDir(e.full_path)));
        it.set("type", e.is_dir ? "folder" : "file");
        if (!e.is_dir) it.set("size", e.size);
        const uint64_t ft = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        if (const std::string t = app::ai::IsoTime(ft); !t.empty()) it.set("modified", t);
        items.push(std::move(it));
    }
    out.set("items", std::move(items));
    out.set("note", "Pulse already shows these results and the conditions as chips the user can edit. Do not list "
                    "the files, folders, paths or size; say in one short sentence how many were found.");
    return out;
}

void ToolSearch(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    auto q = std::make_shared<AiAgent::QueryCard>();
    ToolOutcome error;
    std::wstring arg = json::Wide(args.get_string("label"));
    if (arg.size() > 40) arg = arg.substr(0, 40);
    AddStep(s, a, ui::AiIcon::Search, Text(I::AiQStep), arg, Text(I::AiQStepRun));
    if (!app::ai::SearchSpecFromArgs(args, q->spec, error) || !s.appPrefs.search_enabled) {
        if (!s.appPrefs.search_enabled && error.ok)
            error = ToolOutcome::Fail("unsupported", "Search is turned off in Pulse's settings, so nothing can be found by name.");
        EndStep(s, a, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(error.error.message)));
        AddToolResult(a, call.id, error);
        ++a.call_index;
        AiChatPush(s, 250, [](AppState& s2) { NextTool(s2); return -1; });
        return;
    }
    q->label = arg;
    q->path = app::MakeSearchPath(app::CompileSearchQuery(q->spec));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.4f);
    NavigateTo(s, q->path);
    const std::string id = call.id;
    const uint64_t gen = a.generation;
    const uint64_t shown_at = Now();
    AwaitSearch(s, q->path, [q, id, gen, shown_at](AppState& st, const app::Tab* tab, uint64_t ms) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return;
        if (!tab) {
            EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiQNoAnswer)));
            AddToolResult(ag, id, ToolOutcome::Fail("timeout", "Pulse's search did not finish (the tab went elsewhere or the index did not answer).", true));
        } else {
            const SearchStats stats = StatsOf(*tab);
            q->count = stats.total;
            q->ms = ms;
            ToolOutcome out;
            out.data = SearchResultForModel(*q, stats);
            AddToolResult(ag, id, out);
            EndStep(st, ag, true, Fill(Fill(Text(I::AiQStepDone), L"{n}", Num(stats.total)), L"{t}", Secs(ms)));
            AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 1.0f);
            ag.pending_query = q;
            ag.searched = stats.total;
            ag.did_search = true;
        }
        ++ag.call_index;
        // Keep the running step on screen long enough to read.
        const uint64_t held = Now() - shown_at;
        AiChatPush(st, held < 450 ? static_cast<uint32_t>(450 - held) : 120u, [](AppState& s2) {
            AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(s2);
            return -1;
        });
    });
}

// The turn's search card and 接着做 go under the answer (EndTurn).
void AddQueryCard(AppState& s, AiAgent& a) {
    std::shared_ptr<AiAgent::QueryCard> q = std::move(a.pending_query);
    a.pending_query.reset();
    if (!q) return;
    AiChat& c = Chat(s);
    AiBlockView& b = c.Add(AiBlockKind::Query, Now());
    q->block_id = b.id;
    AiBlockView& f = c.Add(AiBlockKind::Follow, Now() + 500);
    q->follow_id = f.id;
    f.label = Text(I::AiFollowLabel);
    f.follow = {{ui::AiIcon::Package, Fill(Text(I::AiFollowZip), L"{n}", Num(q->count))},
                {ui::AiIcon::Move, Text(I::AiFollowMonth)},
                {ui::AiIcon::Rename, Text(I::AiFollowRename)},
                {ui::AiIcon::Copy, Text(I::AiFollowCopy)}};
    FillChips(s, *q, true);
    // The card shows what the tab shows now (the user may have moved on).
    app::Tab* tab = ActiveTab(s);
    SearchStats stats;
    if (tab && tab->current_path == q->path) stats = StatsOf(*tab);
    else stats.total = q->count;
    FillCounts(s, *q, stats, q->ms, false);
    c.peek_text = Fill(Text(I::AiQPeek), L"{n}", Num(q->count));
    a.queries.push_back(std::move(q));
    if (a.queries.size() > 8) a.queries.erase(a.queries.begin());
}

// The card's search changed in Pulse: show it in the tab, count again, and
// tell the model with the next prompt.
void Research(AppState& s, AiAgent::QueryCard& q) {
    const uint32_t block = q.block_id;
    q.path = app::MakeSearchPath(app::CompileSearchQuery(q.spec));
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path != q.path) NavigateTo(s, q.path);
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.5f);
    const std::wstring path = q.path;
    AwaitSearch(s, path, [block, path](AppState& st, const app::Tab* t, uint64_t ms) {
        AiAgent& ag = Agent(st);
        AiAgent::QueryCard* card = CardOf(ag, block, false);
        AiChat& c = Chat(st);
        if (!card || card->path != path) return;
        if (!t) {
            AiChatSetChip(c, ui::StatusAiState::Idle, L"");
            return;
        }
        const SearchStats stats = StatsOf(*t);
        FillCounts(st, *card, stats, ms, true);
        if (!c.busy) AiChatSetChip(c, ui::StatusAiState::Done, Fill(Text(I::AiQChipFound), L"{n}", Num(stats.total)));
        ag.pending_note = L"(In Pulse the user changed the search conditions locally; the search is now \u201C" +
                          SearchRest(path) + L"\u201D with " + Num(stats.total) +
                          L" results. Follow-up requests about \u201Cthese files\u201D mean these results.)";
        Redraw(st);
    });
}
} // namespace ai_agent


void AiAgentQueryChip(AppState& s, int block, int chip) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& b = c.blocks[static_cast<size_t>(block)];
    AiAgent::QueryCard* q = CardOf(*c.agent, b.id, false);
    if (!q || chip < 0 || chip >= static_cast<int>(b.chips.size()) || chip >= static_cast<int>(q->fields.size())) return;
    ui::AiQueryChipView& v = b.chips[static_cast<size_t>(chip)];
    if (v.fixed || v.out_ms) return;
    v.out_ms = Now();
    app::ai::ClearSearchField(q->spec, q->fields[static_cast<size_t>(chip)]);
    // Search first (the count bumps while the chip shrinks), then the chip
    // leaves; a cleared folder becomes 所有位置.
    Research(s, *q);
    const uint32_t id = b.id;
    AiChatPush(s, 0, [id](AppState& st) {
        AiBlockView* card = Chat(st).Find(id);
        if (!card) return -1;
        bool waiting = false;
        for (const auto& v2 : card->chips)
            if (v2.out_ms && Now() - v2.out_ms < 460) waiting = true;
        if (waiting) return 30;
        if (AiAgent::QueryCard* q2 = Chat(st).agent ? CardOf(*Chat(st).agent, id, false) : nullptr)
            FillChips(st, *q2, true);
        Redraw(st);
        return -1;
    });
    Redraw(s);
}

void AiAgentQueryAdd(AppState& s, int block) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiAgent::QueryCard* q = CardOf(*c.agent, c.blocks[static_cast<size_t>(block)].id, false);
    if (!q) return;
    const uint32_t id = q->block_id;
    // Pulse's own advanced search, filled with the card's search.
    if (app::Tab* tab = ActiveTab(s); !tab || tab->current_path != q->path) NavigateTo(s, q->path);
    ShowAdvancedSearch(s);
    AiAgent::QueryCard* card = Chat(s).agent ? CardOf(*Chat(s).agent, id, false) : nullptr;
    app::Tab* tab = ActiveTab(s);
    if (!card || !tab || tab->current_path == card->path) return;
    const std::wstring rest = SearchRest(tab->current_path);
    if (rest.empty()) return;
    card->spec = app::ai::SearchSpecFromQuery(rest);
    FillChips(s, *card, true);
    Research(s, *card);
    Redraw(s);
}

void AiAgentFollow(AppState& s, int block, int item) {
    AiChat& c = Chat(s);
    if (!c.agent || block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& f = c.blocks[static_cast<size_t>(block)];
    AiAgent::QueryCard* q = CardOf(*c.agent, f.id, true);
    if (!q || item < 0 || item >= static_cast<int>(f.follow.size())) return;
    switch (item) {
        case 0: AiAssistantFillComposer(s, Fill(Text(I::AiFollowZipPrompt), L"{n}", Num(q->count))); break;
        case 1: AiAssistantFillComposer(s, Text(I::AiFollowMonthPrompt)); break;
        case 2: AiAssistantFillComposer(s, Text(I::AiFollowRenamePrompt)); break;
        default: {
            // 复制路径 needs no model: the results' paths, one per line.
            std::wstring text;
            for (const std::wstring& p : q->paths) text += (text.empty() ? L"" : L"\r\n") + p;
            if (text.empty()) break;
            AiChatCopyText(s.hwnd, text);
            const std::wstring back = f.follow[3].text;
            f.follow[3].icon = ui::AiIcon::Check;
            f.follow[3].text = Fill(Text(I::AiFollowCopied), L"{n}", Num(q->paths.size()));
            const uint32_t id = f.id;
            AiChatPush(s, 1600, [id, back](AppState& st) {
                if (AiBlockView* fb = Chat(st).Find(id); fb && fb->follow.size() > 3) {
                    fb->follow[3].icon = ui::AiIcon::Copy;
                    fb->follow[3].text = back;
                }
                Redraw(st);
                return -1;
            });
            break;
        }
    }
    Redraw(s);
}

} // namespace pulse
