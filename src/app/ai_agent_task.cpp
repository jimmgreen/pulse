// ai_agent_task.cpp - Pulse 助手's run_task: the plan, its steps and the result card.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

// ------------------------------------------------------------ run_task --
// Flow C: find files (or take the
// selection), file them into a folder (by month, year or type) and zip them.
// The plan is built from the real files, the user confirms it once, the whole
// task is one undo group, and a file that cannot move pauses the task and asks.

std::unordered_set<uint64_t> g_task_ops;   // operations the running task submitted

constexpr size_t kMaxTask = 2000;

struct TaskPlan {
    std::wstring label;              // 今年的发票 (the model's words)
    std::wstring noun;               // 发票: what the files are
    bool from_selection = false;
    std::wstring dest;               // absolute
    std::wstring dest_label;         // 文档 › 发票
    bool dest_exists = false;
    enum class By : uint8_t { None, Month, Year, Type } by = By::None;
    bool move = true;
    struct Item {
        std::wstring src, name, group;   // group: subfolder name (empty: dest itself)
        std::wstring target;             // the folder it goes to
        uint64_t size = 0;
        bool clash = false;              // the name is taken there (KeepBoth numbers it)
        bool already = false;            // already where it goes
        enum class St : uint8_t { Todo, Moved, Skipped } st = St::Todo;
        std::wstring now;                // its path once moved
    };
    std::vector<Item> items;
    struct Folder {
        std::wstring name, path;
        bool exists = false;
    };
    std::vector<Folder> folders;     // subfolders, in order
    size_t new_folders = 0;          // dest itself counts when missing
    size_t moves = 0;                // items that move
    size_t clashes = 0;
    std::wstring zip_name, zip_path;
    std::wstring where;              // 下载 8 · 桌面 3 · 文档 3
    size_t ignored = 0;              // folders, hidden and system files left out
    uint64_t bytes = 0;
    uint64_t find_ms = 0;
    // The card.
    uint32_t card_id = 0;
    int step_folders = -1, step_move = -1, step_zip = -1;
    size_t steps = 1;
    // Running.
    uint64_t undo_group = 0;
    uint64_t started = 0;
    size_t moved = 0, skipped = 0;
    bool skipped_locked = false;
    bool zip_ok = false;
    uint64_t zip_size = 0;
    std::wstring zip_error;
};

std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

bool UnderPath(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}

// dest: absolute, or a known folder's name first (Documents\发票, 文档\发票).
std::wstring ResolveTaskDest(std::wstring d) {
    for (wchar_t& ch : d)
        if (ch == L'/') ch = L'\\';
    while (!d.empty() && std::iswspace(d.front())) d.erase(0, 1);
    while (!d.empty() && (std::iswspace(d.back()) || d.back() == L'\\')) d.pop_back();
    if (d.empty()) return {};
    std::wstring base, rest;
    if ((d.size() >= 2 && d[1] == L':') || d.rfind(L"\\\\", 0) == 0) {
        d = NormDir(d);
        if (d.size() == 2) d += L'\\';
        return d.size() >= 3 ? d : std::wstring();
    }
    const size_t cut = d.find(L'\\');
    const std::wstring head = d.substr(0, cut);
    rest = cut == std::wstring::npos ? std::wstring() : d.substr(cut + 1);
    struct Known {
        const KNOWNFOLDERID* id;
        const wchar_t* names;   // '|' separated
    };
    static const Known kKnown[] = {
        {&FOLDERID_Documents, L"documents|document|docs|my documents|文档|我的文档"},
        {&FOLDERID_Downloads, L"downloads|download|下载"},
        {&FOLDERID_Desktop, L"desktop|桌面"},
        {&FOLDERID_Pictures, L"pictures|picture|photos|图片|照片"},
        {&FOLDERID_Videos, L"videos|video|视频"},
        {&FOLDERID_Music, L"music|音乐"},
        {&FOLDERID_Profile, L"home|~|用户|个人文件夹"},
    };
    for (const Known& k : kKnown) {
        bool hit = false;
        std::wstring_view names = k.names;
        while (!names.empty() && !hit) {
            const size_t bar = names.find(L'|');
            const std::wstring_view one = names.substr(0, bar);
            hit = one.size() == head.size() && _wcsnicmp(one.data(), head.c_str(), head.size()) == 0;
            names = bar == std::wstring_view::npos ? std::wstring_view() : names.substr(bar + 1);
        }
        const std::wstring path = KnownFolder(*k.id);
        if (path.empty()) continue;
        if (!hit) hit = _wcsicmp(head.c_str(), Leaf(path).c_str()) == 0 || head == app::TabTitle(path);
        if (hit) {
            base = path;
            break;
        }
    }
    if (base.empty()) return {};
    return rest.empty() ? base : JoinPath(base, rest);
}

// Each part under a known root is a valid name.
bool ValidDestParts(const std::wstring& dest) {
    size_t start = dest.rfind(L"\\\\", 0) == 0 ? 2 : 3;
    while (start < dest.size()) {
        size_t end = dest.find(L'\\', start);
        if (end == std::wstring::npos) end = dest.size();
        if (!ValidName(dest.substr(start, end - start))) return false;
        start = end + 1;
    }
    return true;
}

I TypeFolder(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : ui::AiListLower(name.substr(dot + 1));
    struct Kind {
        I id;
        const wchar_t* exts;
    };
    static const Kind kKinds[] = {
        {I::AiTkTypeDocs, L"|pdf|doc|docx|xls|xlsx|ppt|pptx|txt|rtf|odt|ods|odp|csv|md|epub|wps|et|dps|ofd|xps|pages|numbers|key|"},
        {I::AiTkTypeImages, L"|jpg|jpeg|png|gif|bmp|webp|heic|heif|tif|tiff|svg|raw|cr2|cr3|nef|arw|dng|avif|ico|psd|jxl|"},
        {I::AiTkTypeVideos, L"|mp4|mkv|mov|avi|wmv|flv|webm|m4v|ts|mts|m2ts|3gp|mpg|mpeg|"},
        {I::AiTkTypeAudio, L"|mp3|wav|flac|aac|m4a|ogg|wma|opus|ape|aiff|amr|mid|"},
        {I::AiTkTypeArchives, L"|zip|rar|7z|tar|gz|bz2|xz|tgz|zst|iso|cab|lzh|"},
        {I::AiTkTypeCode, L"|c|cc|cpp|h|hpp|cs|js|jsx|tsx|py|java|go|rs|json|xml|html|css|sql|sh|ps1|bat|yaml|yml|toml|kt|swift|php|rb|lua|"},
    };
    if (!ext.empty()) {
        const std::wstring key = L"|" + ext + L"|";
        for (const Kind& k : kKinds)
            if (wcsstr(k.exts, key.c_str())) return k.id;
    }
    return I::AiTkTypeOther;
}

int TypeRank(const std::wstring& name) {
    static const I kOrder[] = {I::AiTkTypeDocs, I::AiTkTypeImages, I::AiTkTypeVideos, I::AiTkTypeAudio,
                               I::AiTkTypeArchives, I::AiTkTypeCode, I::AiTkTypeOther};
    for (int i = 0; i < 7; ++i)
        if (name == Text(kOrder[i])) return i;
    return 7;
}

// The plan from the real files (worker thread: attributes and folder checks).
ToolOutcome BuildTaskPlan(TaskPlan& p, const std::vector<std::wstring>& paths,
                          const std::vector<std::wstring>& guarded, const std::vector<std::wstring>& type_names) {
    p.dest_exists = IsDirectory(p.dest);
    if (!p.dest_exists && Exists(p.dest)) return ToolOutcome::Fail("invalid_args", "dest is a file, not a folder");
    std::unordered_set<std::wstring> seen;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> taken;   // target -> names claimed
    std::unordered_map<std::wstring, size_t> folder_at;
    for (const std::wstring& raw : paths) {
        const std::wstring path = NormDir(raw);
        if (!seen.insert(ui::AiListLower(path)).second) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(app::ai::Win32Path(path).c_str(), GetFileExInfoStandard, &fa)) {
            ++p.ignored;
            continue;
        }
        bool skip = (fa.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        for (const std::wstring& g : guarded) skip = skip || UnderPath(path, g);
        if (skip) {
            ++p.ignored;
            continue;
        }
        if (p.items.size() >= kMaxTask)
            return ToolOutcome::Fail("too_many", "more than 2000 files; narrow the search");
        TaskPlan::Item it;
        it.src = path;
        it.name = Leaf(path);
        it.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        FILETIME local{};
        SYSTEMTIME t{};
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        wchar_t buf[16];
        switch (p.by) {
        case TaskPlan::By::Month:
            swprintf_s(buf, L"%04u-%02u", t.wYear, t.wMonth);
            it.group = buf;
            break;
        case TaskPlan::By::Year:
            swprintf_s(buf, L"%04u", t.wYear);
            it.group = buf;
            break;
        case TaskPlan::By::Type:
            it.group = type_names[static_cast<size_t>(TypeRank(Text(TypeFolder(it.name))))];
            break;
        case TaskPlan::By::None:
            break;
        }
        it.target = it.group.empty() ? p.dest : JoinPath(p.dest, it.group);
        if (p.move) {
            it.already = SameDir(app::ai::ParentFolder(path), it.target);
            const std::wstring tkey = ui::AiListLower(it.target);
            const std::wstring nkey = ui::AiListLower(it.name);
            if (!it.already) {
                it.clash = !taken[tkey].insert(nkey).second || Exists(JoinPath(it.target, it.name));
                if (it.clash) ++p.clashes;
                ++p.moves;
            } else {
                taken[tkey].insert(nkey);
            }
            if (!it.group.empty() && !folder_at.count(ui::AiListLower(it.group))) {
                folder_at.emplace(ui::AiListLower(it.group), p.folders.size());
                TaskPlan::Folder f;
                f.name = it.group;
                f.path = it.target;
                f.exists = IsDirectory(f.path);
                if (!f.exists && Exists(f.path))
                    return ToolOutcome::Fail("invalid_args", "a file named " + Utf8(f.name) + " is in the way in dest");
                p.folders.push_back(std::move(f));
            }
        }
        p.bytes += it.size;
        p.items.push_back(std::move(it));
    }
    if (p.items.empty())
        return ToolOutcome::Fail("no_files", p.ignored ? "only folders, hidden or system files matched; nothing to do"
                                                       : "no files matched");
    if (p.by == TaskPlan::By::Type)
        std::stable_sort(p.folders.begin(), p.folders.end(),
                         [](const auto& x, const auto& y) { return TypeRank(x.name) < TypeRank(y.name); });
    else
        std::sort(p.folders.begin(), p.folders.end(), [](const auto& x, const auto& y) { return x.name < y.name; });
    p.new_folders = p.dest_exists ? 0 : 1;
    for (const auto& f : p.folders)
        if (!f.exists) ++p.new_folders;
    if (!p.zip_name.empty()) {
        std::wstring stem = p.zip_name;
        if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".zip") == 0) stem.resize(stem.size() - 4);
        p.zip_name = stem + L".zip";
        p.zip_path = JoinPath(p.dest, p.zip_name);
        for (int k = 2; Exists(p.zip_path) && k < 100; ++k) {
            p.zip_name = stem + L" (" + std::to_wstring(k) + L").zip";
            p.zip_path = JoinPath(p.dest, p.zip_name);
        }
    }
    if (p.moves == 0 && p.zip_name.empty())
        return ToolOutcome::Fail("nothing_to_do", "every file is already where it should go");
    return {};
}

// "2026-01、02、03…" for month folders, else the names; at most `max`.
std::wstring FolderList(const TaskPlan& p, bool new_only, size_t max) {
    std::vector<std::wstring> names;
    if (new_only && !p.dest_exists) names.push_back(Leaf(p.dest));
    for (const auto& f : p.folders)
        if (!new_only || !f.exists) names.push_back(f.name);
    std::wstring out, year;
    const std::wstring sep = Text(I::AiTkListSep);
    for (size_t i = 0; i < names.size() && i < max; ++i) {
        std::wstring n = names[i];
        if (p.by == TaskPlan::By::Month && n.size() == 7 && n[4] == L'-') {
            if (n.substr(0, 4) == year) n = n.substr(5);
            else year = n.substr(0, 4);
        }
        if (!out.empty()) out += sep;
        out += n;
    }
    if (names.size() > max) out += L"\u2026";
    return out;
}

// Splits "新建 {n} 个文件夹" around {n}.
ui::AiTaskSumView Sum(ui::AiIcon icon, I text, size_t n) {
    ui::AiTaskSumView v;
    v.icon = icon;
    const std::wstring t = Text(text);
    const size_t at = t.find(L"{n}");
    v.before = at == std::wstring::npos ? t : t.substr(0, at);
    v.bold = Num(n);
    v.after = at == std::wstring::npos ? std::wstring() : t.substr(at + 3);
    return v;
}

ui::AiResultLine Line(bool warn, std::wstring text, const std::wstring& key, const std::wstring& bold) {
    ui::AiResultLine l;
    l.warn = warn;
    const size_t at = text.find(key);
    if (at == std::wstring::npos) {
        l.before = std::move(text);
        return l;
    }
    l.before = text.substr(0, at);
    l.bold = bold;
    l.after = text.substr(at + key.size());
    return l;
}

struct TaskRun {
    std::shared_ptr<TaskPlan> p;
    std::string call_id;
    uint64_t gen = 0;
    enum class Phase : uint8_t { Folders, Moves, Zip, Done } phase = Phase::Folders;
    // Folders.
    bool dest_sent = false;
    size_t folder_next = 0;
    uint64_t next_at = 0;
    uint64_t last_change = 0;
    bool navigated = false;
    // Moves.
    size_t next = 0;                 // the next item to send
    size_t cur = SIZE_MAX;           // the item in flight
    uint64_t cur_id = 0, cur_at = 0;
    std::unordered_set<std::wstring> before;   // a clashing item's target names before it moved
    uint32_t stagger = 0;
    // Zip.
    std::shared_ptr<AiAgent::Job> zip_job;
    std::shared_ptr<std::atomic<uint64_t>> zip_read, zip_total;
    std::shared_ptr<app::ZipCompressResult> zip_res;
    std::vector<std::wstring> zip_sources, zip_names;
    uint64_t zip_started = 0;
    // Paused.
    bool paused = false;
    uint64_t paused_at = 0;
    bool pause_lock = false;
    bool pause_folder = false;
    std::wstring pause_name;
};

void PumpTask(AppState& s, std::shared_ptr<TaskRun> run, uint32_t delay);

AiBlockView* TaskCard(AppState& s, const TaskPlan& p) { return Chat(s).Find(p.card_id); }

void SetTaskStep(AppState& s, const TaskPlan& p, int idx, ui::AiTaskState state, float progress,
                 const std::wstring* desc = nullptr) {
    AiBlockView* b = TaskCard(s, p);
    if (!b || idx < 0 || static_cast<size_t>(idx) >= b->task.size()) return;
    ui::AiTaskStepView& st = b->task[static_cast<size_t>(idx)];
    if (st.state != state) {
        st.state = state;
        st.state_ms = Now();
    }
    st.progress = progress;
    if (desc) st.desc = *desc;
    if (state == ui::AiTaskState::Running || state == ui::AiTaskState::Paused)
        b->meta = Fill(Fill(Text(I::AiTkPlanMetaRun), L"{n}", Num(p.steps)), L"{k}", Num(static_cast<size_t>(idx) + 1));
}

void TaskChip(AppState& s, const TaskPlan& p, int idx, const std::wstring& extra, float progress) {
    std::wstring t = Fill(Fill(Text(I::AiTkChipStep), L"{k}", Num(static_cast<size_t>(idx) + 1)), L"{n}", Num(p.steps));
    if (!extra.empty()) t += L" \u00B7 " + extra;
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, t, progress);
}

std::wstring MovedOf(const TaskPlan& p) {
    return Fill(Fill(Text(I::AiTkMoving), L"{k}", Num(p.moved)), L"{n}", Num(p.moves));
}

ops::OpRequest TaskRequest(const TaskPlan& p) {
    ops::OpRequest r;
    r.undo_group = p.undo_group;
    r.undo_label = Fill(Text(I::AiTkUndoLabel), L"{label}", p.label);
    return r;
}

void SubmitTaskFolder(AppState& s, TaskRun& run, const std::wstring& path) {
    ops::OpRequest r = TaskRequest(*run.p);
    r.type = ops::OpType::CreateFolder;
    r.sources.push_back(path);
    g_task_ops.insert(s.ops.Submit(std::move(r)));
}

std::unordered_set<std::wstring> NamesLike(const std::wstring& dir, const std::wstring& name) {
    std::unordered_set<std::wstring> out;
    const size_t dot = name.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    const std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(app::ai::Win32Path(JoinPath(dir, stem + L"*" + ext)).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        out.insert(ui::AiListLower(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void SubmitTaskMove(AppState& s, TaskRun& run, size_t i) {
    TaskPlan& p = *run.p;
    TaskPlan::Item& it = p.items[i];
    if (it.clash) run.before = NamesLike(it.target, it.name);
    ops::OpRequest r = TaskRequest(p);
    r.type = ops::OpType::Move;
    r.dest_dir = it.target;
    r.collision_policy = ops::CollisionPolicy::KeepBoth;
    r.sources.push_back(it.src);
    run.cur = i;
    run.cur_at = Now();
    run.cur_id = s.ops.Submit(std::move(r));
    g_task_ops.insert(run.cur_id);
    if (ui::AiListOverlay* o = OverlayOf(p.dest); o && !it.group.empty())
        o->bumps[ui::AiListLower(it.group)].push_back(Now() + 420);
}

// Where a moved item ended up.
std::wstring LandedPath(const TaskRun& run, const TaskPlan::Item& it) {
    if (!it.clash) return JoinPath(it.target, it.name);
    for (const std::wstring& n : NamesLike(it.target, it.name)) {
        if (run.before.count(n)) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(app::ai::Win32Path(JoinPath(it.target, n)).c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        return JoinPath(it.target, fd.cFileName);
    }
    return JoinPath(it.target, it.name);
}

void FinishTask(AppState& s, std::shared_ptr<TaskRun> run);
void TaskUndo(AppState& s, std::shared_ptr<TaskPlan> p, std::shared_ptr<TaskRun> abort_run);

// A file (or folder) would not go: the card turns amber and asks.
void PauseTask(AppState& s, std::shared_ptr<TaskRun> run, const ops::OpStatus& st) {
    TaskPlan& p = *run->p;
    AiChat& c = Chat(s);
    run->paused = true;
    run->paused_at = Now();
    run->pause_lock = !st.lock_owners.empty();
    const int idx = run->pause_folder ? p.step_folders : p.step_move;
    std::wstring desc;
    if (!run->pause_folder) {
        desc = Fill(Fill(Fill(Text(I::AiTkMovePaused), L"{k}", Num(p.moved)), L"{n}", Num(p.moves)), L"{name}",
                    run->pause_name);
    }
    SetTaskStep(s, p, idx, ui::AiTaskState::Paused,
                run->pause_folder ? 0.0f : static_cast<float>(p.moved) / static_cast<float>((std::max<size_t>)(p.moves, 1)),
                run->pause_folder ? nullptr : &desc);
    AiBlockView& e = c.Add(AiBlockKind::Error, Now());
    e.tone = ui::AiErrTone::Warn;
    e.icon = run->pause_lock ? ui::AiIcon::Lock : ui::AiIcon::Warn;
    e.title = Text(run->pause_lock ? I::AiTkPausedLock : I::AiTkPausedFail);
    if (run->pause_folder)
        e.sub = Text(I::AiTkPausedFolderSub);
    else
        e.sub = Fill(Fill(Fill(Fill(Text(I::AiTkPausedSub), L"{d}", Num(static_cast<size_t>(idx))), L"{k}",
                               Num(static_cast<size_t>(idx) + 1)),
                          L"{m}", Num(p.moved)),
                     L"{n}", Num(p.moves));
    e.file = run->pause_name;
    if (run->pause_lock) {
        const ops::LockOwner& o = st.lock_owners.front();
        std::wstring app_name = o.app_name;
        if (app_name.empty() && !o.image_name.empty()) {
            app_name = o.image_name;
            if (app_name.size() > 4 && _wcsicmp(app_name.c_str() + app_name.size() - 4, L".exe") == 0)
                app_name.resize(app_name.size() - 4);
        }
        e.file_note = app_name.empty() ? Text(I::AiTkInUseAny) : Fill(Text(I::AiTkInUse), L"{app}", app_name);
    } else {
        std::wstring why = !st.failures.empty() ? st.failures.front().reason : st.last_error;
        if (why.size() > 28) why = why.substr(0, 27) + L"\u2026";
        e.file_note = why;
    }
    auto btn = [](ui::AiErrAct act, ui::AiIcon icon, I text, bool primary) {
        ui::AiErrButton b;
        b.act = act;
        b.icon = icon;
        b.text = Text(text);
        b.primary = primary;
        return b;
    };
    e.actions.push_back(btn(ui::AiErrAct::TaskRetry, ui::AiIcon::Retry, I::AiAsstRetry, run->pause_folder));
    if (!run->pause_folder) e.actions.push_back(btn(ui::AiErrAct::TaskSkip, ui::AiIcon::Skip, I::AiTkBtnSkip, true));
    e.actions.push_back(btn(ui::AiErrAct::TaskUndo, ui::AiIcon::Undo, I::AiTkBtnUndo, false));
    c.pause_id = e.id;
    AiChatSetChip(c, ui::StatusAiState::Waiting, Text(I::AiTkChipPaused));
    if (!s.aiAssistant.open) c.unread = true;
    c.on_task_act = [run](AppState& s2, ui::AiErrAct act) {
        AiChat& c2 = Chat(s2);
        TaskPlan& p2 = *run->p;
        AiBlockView* card = c2.Find(c2.pause_id);
        const std::wstring name = run->pause_name;
        if (card) {
            card->done = true;
            card->icon = act == ui::AiErrAct::TaskSkip ? ui::AiIcon::Skip
                         : act == ui::AiErrAct::TaskRetry ? ui::AiIcon::Retry : ui::AiIcon::Undo;
            card->title = act == ui::AiErrAct::TaskUndo ? Text(I::AiTkChoseUndo)
                                                        : Fill(Text(act == ui::AiErrAct::TaskSkip ? I::AiTkChoseSkip
                                                                                                    : I::AiTkChoseRetry),
                                                               L"{name}", name);
        }
        c2.pause_id = 0;
        c2.on_task_act = nullptr;
        const uint64_t now = Now();
        if (now > run->paused_at) {
            c2.turn_start += now - run->paused_at;   // waiting is not model time
            p2.started += now - run->paused_at;      // nor the task's own time
        }
        run->paused = false;
        if (Agent(s2).generation != run->gen) return;
        if (act == ui::AiErrAct::TaskUndo) {
            TaskUndo(s2, run->p, run);
            return;
        }
        if (run->pause_folder) {
            run->pause_folder = false;
            run->dest_sent = false;
            run->folder_next = 0;
            run->last_change = now;
            SetTaskStep(s2, p2, p2.step_folders, ui::AiTaskState::Running, 0.0f);
        } else if (act == ui::AiErrAct::TaskSkip) {
            p2.items[run->cur].st = TaskPlan::Item::St::Skipped;
            ++p2.skipped;
            if (run->pause_lock) p2.skipped_locked = true;
            run->cur = SIZE_MAX;
            if (AiBlockView* b = TaskCard(s2, p2); b && p2.step_move >= 0) {
                ui::AiTaskStepView& sv = b->task[static_cast<size_t>(p2.step_move)];
                sv.tag = Fill(Text(I::AiTkTagSkipped), L"{n}", Num(p2.skipped));
                sv.tag_warn = true;
            }
            const std::wstring d = MovedOf(p2);
            SetTaskStep(s2, p2, p2.step_move, ui::AiTaskState::Running,
                        static_cast<float>(p2.moved + p2.skipped) / static_cast<float>((std::max<size_t>)(p2.moves, 1)), &d);
        } else {
            const size_t i = run->cur;
            run->cur = SIZE_MAX;
            SubmitTaskMove(s2, *run, i);
            const std::wstring d = MovedOf(p2);
            SetTaskStep(s2, p2, p2.step_move, ui::AiTaskState::Running,
                        static_cast<float>(p2.moved) / static_cast<float>((std::max<size_t>)(p2.moves, 1)), &d);
        }
        PumpTask(s2, run, 120);
        Redraw(s2);
    };
}

void StartTaskZip(AppState& s, std::shared_ptr<TaskRun> run) {
    TaskPlan& p = *run->p;
    run->phase = TaskRun::Phase::Zip;
    run->zip_sources.clear();
    run->zip_names.clear();
    std::unordered_set<std::wstring> used;
    for (const auto& it : p.items) {
        if (it.st == TaskPlan::Item::St::Skipped) continue;
        const std::wstring path = !p.move || it.already ? it.src : it.now;
        if (path.empty() || !Exists(path)) continue;
        std::wstring leaf = Leaf(path);
        std::wstring entry = it.group.empty() ? leaf : it.group + L"/" + leaf;
        // Two files of one name from different folders (zip only).
        for (int k = 2; !used.insert(ui::AiListLower(entry)).second && k < 1000; ++k) {
            const size_t dot = leaf.rfind(L'.');
            const std::wstring n = dot == std::wstring::npos
                                       ? leaf + L" (" + std::to_wstring(k) + L")"
                                       : leaf.substr(0, dot) + L" (" + std::to_wstring(k) + L")" + leaf.substr(dot);
            entry = it.group.empty() ? n : it.group + L"/" + n;
        }
        run->zip_sources.push_back(path);
        run->zip_names.push_back(entry);
    }
    if (run->zip_sources.empty()) {
        p.zip_error = Text(I::AiTkZipNothing);
        run->phase = TaskRun::Phase::Done;
        return;
    }
    const std::wstring d = Fill(Text(I::AiTkZipping), L"{p}", L"0");
    SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Running, 0.0f, &d);
    TaskChip(s, p, p.step_zip, Text(I::AiTkChipZip), 0.0f);
    auto job = std::make_shared<AiAgent::Job>();
    Agent(s).job = job;
    run->zip_job = job;
    run->zip_read = std::make_shared<std::atomic<uint64_t>>(0);
    run->zip_total = std::make_shared<std::atomic<uint64_t>>(0);
    run->zip_res = std::make_shared<app::ZipCompressResult>();
    run->zip_started = Now();
    std::thread([job, read = run->zip_read, total = run->zip_total, res = run->zip_res, sources = run->zip_sources,
                 names = run->zip_names, zip = p.zip_path] {
        *res = app::CompressToZip(sources, zip, &job->cancelled,
                                  [read, total](uint64_t r, uint64_t t) {
                                      read->store(r);
                                      total->store(t);
                                  },
                                  &names);
        job->done.store(true);
    }).detach();
}

// One tick of the running task; returns the next delay or -1.
int TickTask(AppState& s, std::shared_ptr<TaskRun> run) {
    if (Agent(s).generation != run->gen || run->paused) return -1;
    TaskPlan& p = *run->p;
    const uint64_t now = Now();
    if (run->phase == TaskRun::Phase::Folders) {
        if (!p.dest_exists && !run->dest_sent) {
            SubmitTaskFolder(s, *run, p.dest);
            run->dest_sent = true;
            run->last_change = now;
            return 60;
        }
        const bool dest_ready = IsDirectory(p.dest);
        if (dest_ready && !run->navigated) {
            run->navigated = true;
            NavigateTo(s, p.dest);
            ui::AiListOverlay& o = ListOverlay(p.dest);
            o.new_badge = Text(I::AiAsstNewBadge);
            o.new_folders.clear();
            o.leaving.clear();
            o.target.clear();
            o.targets.clear();
            o.bumps.clear();
            o.dests.clear();
            for (const auto& f : p.folders) o.targets.insert(ui::AiListLower(f.name));
            run->next_at = now + 300;
        }
        if (dest_ready && p.move) {
            while (run->folder_next < p.folders.size() && p.folders[run->folder_next].exists) ++run->folder_next;
            if (run->folder_next < p.folders.size() && now >= run->next_at) {
                const TaskPlan::Folder& f = p.folders[run->folder_next++];
                SubmitTaskFolder(s, *run, f.path);
                if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->new_folders[ui::AiListLower(f.name)] = now;
                run->next_at = now + 120;
                run->last_change = now;
            }
        }
        size_t ready = dest_ready && !p.dest_exists ? 1 : 0;
        size_t want = p.dest_exists ? 0 : 1;
        if (p.move)
            for (const auto& f : p.folders)
                if (!f.exists) {
                    ++want;
                    if (IsDirectory(f.path)) ++ready;
                }
        if (p.step_folders >= 0) {
            const std::wstring d = Fill(Fill(Text(I::AiTkCreating), L"{k}", Num(ready)), L"{n}", Num(want));
            SetTaskStep(s, p, p.step_folders, ui::AiTaskState::Running,
                        want ? static_cast<float>(ready) / static_cast<float>(want) : 1.0f, &d);
            TaskChip(s, p, p.step_folders, {}, want ? static_cast<float>(ready) / static_cast<float>(want) : 1.0f);
        }
        const bool fed = dest_ready && (!p.move || run->folder_next >= p.folders.size());
        if (fed && ready >= want) {
            if (p.step_folders >= 0) {
                const std::wstring d = Fill(Text(I::AiTkFoldersDone), L"{n}", Num(want));
                SetTaskStep(s, p, p.step_folders, ui::AiTaskState::Done, 1.0f, &d);
            }
            run->phase = p.moves ? TaskRun::Phase::Moves : TaskRun::Phase::Zip;
            run->next_at = now + 380;
            if (run->phase == TaskRun::Phase::Zip) {
                StartTaskZip(s, run);
                return 60;
            }
            const std::wstring d = MovedOf(p);
            SetTaskStep(s, p, p.step_move, ui::AiTaskState::Running, 0.0f, &d);
            TaskChip(s, p, p.step_move, Fill(Fill(Text(I::AiTkChipOf), L"{k}", L"0"), L"{n}", Num(p.moves)), 0.0f);
            return 380;
        }
        // Nothing for four seconds with the queue idle: a folder failed.
        if (!s.ops.Status().active && now - run->last_change > 4000) {
            run->pause_folder = true;
            run->pause_name = !dest_ready ? Leaf(p.dest) : std::wstring();
            if (run->pause_name.empty())
                for (const auto& f : p.folders)
                    if (!f.exists && !IsDirectory(f.path)) {
                        run->pause_name = f.name;
                        break;
                    }
            PauseTask(s, run, s.ops.Status());
            Redraw(s);
            return -1;
        }
        return 60;
    }
    if (run->phase == TaskRun::Phase::Moves) {
        if (run->cur != SIZE_MAX) {
            TaskPlan::Item& it = p.items[run->cur];
            if (!Exists(it.src)) {
                it.st = TaskPlan::Item::St::Moved;
                it.now = LandedPath(*run, it);
                ++p.moved;
                run->cur = SIZE_MAX;
                const std::wstring d = MovedOf(p);
                const float prog = static_cast<float>(p.moved + p.skipped) / static_cast<float>(p.moves);
                SetTaskStep(s, p, p.step_move, ui::AiTaskState::Running, prog, &d);
                TaskChip(s, p, p.step_move,
                         Fill(Fill(Text(I::AiTkChipOf), L"{k}", Num(p.moved)), L"{n}", Num(p.moves)), prog);
                run->next_at = now + run->stagger;
            } else {
                const ops::OpStatus st = s.ops.Status();
                const bool failed = !st.active && st.task_id == run->cur_id && st.phase == ops::OpPhase::Failed;
                const bool lost = !st.active && st.task_id != run->cur_id && now - run->cur_at > 3000;
                const bool quiet = !st.active && st.task_id == run->cur_id && now - run->cur_at > 1500;
                if (failed || lost || quiet) {
                    run->pause_name = it.name;
                    PauseTask(s, run, st.task_id == run->cur_id ? st : ops::OpStatus{});
                    Redraw(s);
                    return -1;
                }
                return 40;
            }
        }
        while (run->next < p.items.size() && (p.items[run->next].already)) ++run->next;
        if (run->next >= p.items.size()) {
            const std::wstring d = Fill(Text(I::AiTkMoveDone), L"{n}", Num(p.moved));
            SetTaskStep(s, p, p.step_move, ui::AiTaskState::Done, 1.0f, &d);
            if (p.step_zip >= 0) {
                StartTaskZip(s, run);
                return run->phase == TaskRun::Phase::Zip ? 300 : 0;
            }
            run->phase = TaskRun::Phase::Done;
            return 0;
        }
        if (now < run->next_at) return static_cast<int>(run->next_at - now);
        SubmitTaskMove(s, *run, run->next++);
        return 30;
    }
    if (run->phase == TaskRun::Phase::Zip) {
        const uint64_t total = run->zip_total->load(), read = run->zip_read->load();
        const float prog = total ? static_cast<float>(read) / static_cast<float>(total) : 0.0f;
        const std::wstring d = Fill(Text(I::AiTkZipping), L"{p}", Num(static_cast<size_t>(prog * 100.0f + 0.5f)));
        SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Running, prog, &d);
        TaskChip(s, p, p.step_zip, Text(I::AiTkChipZip), prog);
        if (!run->zip_job->done.load() || now - run->zip_started < 500) return 60;
        Agent(s).job.reset();
        const app::ZipCompressResult& r = *run->zip_res;
        p.zip_ok = r.ok;
        if (r.ok) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(app::ai::Win32Path(p.zip_path).c_str(), GetFileExInfoStandard, &fa))
                p.zip_size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            s.ops.RecordExternalCreate(p.zip_path, p.undo_group, Fill(Text(I::AiTkUndoLabel), L"{label}", p.label));
            if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->new_folders[ui::AiListLower(p.zip_name)] = now;
            const std::wstring dd = p.zip_name + L" \u00B7 " + AiChatFormatSize(p.zip_size);
            SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Done, 1.0f, &dd);
        } else {
            p.zip_error = r.error.empty() ? Text(I::AiTkZipNothing) : r.error;
            const std::wstring dd = Fill(Text(I::AiTkZipFailed), L"{reason}", p.zip_error);
            SetTaskStep(s, p, p.step_zip, ui::AiTaskState::Paused, 0.0f, &dd);
        }
        run->phase = TaskRun::Phase::Done;
        return 450;   // let the zip row land
    }
    FinishTask(s, run);
    return -1;
}

void PumpTask(AppState& s, std::shared_ptr<TaskRun> run, uint32_t delay) {
    AiChatPush(s, delay, [run](AppState& st) {
        const int next = TickTask(st, run);
        if (next >= 0) Redraw(st);
        return next;
    });
}

json::Value TaskResultForModel(const TaskPlan& p) {
    json::Value out = json::Value::Object();
    out.set("ok", p.moved > 0 || p.zip_ok);
    out.set("found", static_cast<uint64_t>(p.items.size()));
    out.set("dest", Utf8(p.dest));
    out.set("folders_created", static_cast<uint64_t>(p.new_folders));
    out.set("moved", static_cast<uint64_t>(p.moved));
    out.set("already_in_place", static_cast<uint64_t>(p.items.size() - p.moves));
    if (p.skipped) {
        json::Value names = json::Value::Array();
        for (const auto& it : p.items)
            if (it.st == TaskPlan::Item::St::Skipped) names.push(Utf8(it.name));
        out.set("skipped", std::move(names));
        out.set("skipped_reason", p.skipped_locked ? "in use by another program; the user chose to skip"
                                                   : "could not be moved; the user chose to skip");
    }
    if (p.clashes) out.set("renamed_for_clash", static_cast<uint64_t>(p.clashes));
    if (!p.zip_name.empty()) {
        json::Value z = json::Value::Object();
        z.set("ok", p.zip_ok);
        z.set("path", Utf8(p.zip_path));
        if (p.zip_ok) z.set("size", Utf8(AiChatFormatSize(p.zip_size)));
        else z.set("error", Utf8(p.zip_error));
        out.set("zip", std::move(z));
    }
    if (p.ignored) out.set("ignored", static_cast<uint64_t>(p.ignored));
    out.set("undo", "The user can undo the whole task with 撤销全部 on the result card in Pulse.");
    return out;
}

void FinishTask(AppState& s, std::shared_ptr<TaskRun> run) {
    std::shared_ptr<TaskPlan> plan = run->p;
    TaskPlan& p = *plan;
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    g_task_ops.clear();
    if (AiBlockView* b = TaskCard(s, p)) {
        b->collapsed = true;
        b->meta = p.skipped ? Fill(Fill(Text(I::AiTkPlanMetaSkip), L"{n}", Num(p.steps)), L"{k}", Num(p.skipped))
                            : Fill(Text(I::AiTkPlanMetaDone), L"{n}", Num(p.steps));
    }
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Text(I::AiTkDoneTitle);
    wchar_t secs[16];
    swprintf_s(secs, L"%.1f", static_cast<double>(Now() - p.started) / 1000.0);
    r.sub = Fill(Fill(Text(I::AiTkDoneSub), L"{folder}", p.dest_label), L"{t}", secs);
    r.lines.push_back(Line(false, Fill(Text(p.from_selection ? I::AiTkLnSel : I::AiTkLnFound), L"{noun}", p.noun),
                           L"{n}", Num(p.items.size())));
    if (p.new_folders) {
        const bool subs = p.dest_exists && p.by != TaskPlan::By::None;
        r.lines.push_back(Line(false,
                               Text(!subs ? I::AiTkLnFolders
                                    : p.by == TaskPlan::By::Month ? I::AiTkLnFoldersMonth
                                    : p.by == TaskPlan::By::Year ? I::AiTkLnFoldersYear : I::AiTkLnFoldersType),
                               L"{n}", Num(p.new_folders)));
    }
    if (p.moves) {
        if (p.skipped)
            r.lines.push_back(Line(true,
                                   Fill(Fill(Text(I::AiTkLnSkipped), L"{k}", Num(p.skipped)), L"{reason}",
                                        Text(p.skipped_locked ? I::AiTkReasonLock : I::AiTkReasonFail)),
                                   L"{n}", Num(p.moved)));
        else
            r.lines.push_back(Line(false, Text(I::AiTkLnMoved), L"{n}", Num(p.moved)));
    }
    if (!p.zip_name.empty()) {
        if (p.zip_ok)
            r.lines.push_back(Line(false, Fill(Text(I::AiTkLnZip), L"{size}", AiChatFormatSize(p.zip_size)), L"{name}",
                                   p.zip_name));
        else
            r.lines.push_back(Line(true, Fill(Text(I::AiTkLnZipFail), L"{reason}", p.zip_error), L"{name}", p.zip_name));
    }
    r.deny = Text(I::AiTkUndoAll);
    r.accept = Text(I::AiAsstShowInPulse);
    c.result_id = r.id;
    // 在 Pulse 中查看: the folder, with the zip selected and flashing.
    c.on_show = [plan](AppState& st) {
        NavigateTo(st, plan->dest);
        if (!plan->zip_ok) return;
        if (app::Tab* tab = ActiveTab(st); tab && SameDir(tab->current_path, plan->dest)) {
            tab->pending_selected_names = {plan->zip_name};
            tab->pending_selected_name = plan->zip_name;
            tab->pending_selection_revision = tab->selection_revision;
        }
        ui::AiListOverlay& o = ListOverlay(plan->dest);
        o.flash[ui::AiListLower(plan->zip_name)] = Now() + 200;
        o.end_ms = Now() + 2500;
    };
    c.on_undo = [plan](AppState& st) { TaskUndo(st, plan, nullptr); };
    c.peek_text = Fill(Fill(Text(I::AiTkPeek), L"{n}", Num(p.moved)), L"{folder}", p.dest_label);
    c.peek_undo = p.moved > 0 || p.zip_ok;
    if (ui::AiListOverlay* o = OverlayOf(p.dest)) o->end_ms = Now() + 6000;
    ToolOutcome out;
    out.data = TaskResultForModel(p);
    AddToolResult(a, run->call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

// 撤销全部 on the result card, or 撤销已做的 on the pause card (abort_run).
void TaskUndo(AppState& s, std::shared_ptr<TaskPlan> plan, std::shared_ptr<TaskRun> abort_run) {
    AiChat& c = Chat(s);
    g_task_ops.clear();
    const bool nothing = plan->moved == 0 && !plan->zip_ok && plan->new_folders == 0;
    const auto outcome = nothing ? ops::OpsManager::GroupUndo::Started : s.ops.UndoGroup(plan->undo_group);
    if (outcome != ops::OpsManager::GroupUndo::Started) {
        AiBlockView& note = c.Add(AiBlockKind::Note, Now());
        const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
        note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
        if (!abort_run && pending) c.on_undo = [plan](AppState& st) { TaskUndo(st, plan, nullptr); };
        if (abort_run) {
            AiAgent& a = Agent(s);
            AddToolResult(a, abort_run->call_id,
                          ToolOutcome::Fail("stopped", "The user stopped the task in Pulse after a file could not be "
                                                       "moved, and the undo could not run; some files were moved."));
            ++a.call_index;
            NextTool(s);
        }
        Redraw(s);
        return;
    }
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
    c.peek_undo = false;
    AiChatHidePeek(s);
    if (ui::AiListOverlay* o = OverlayOf(plan->dest)) {
        const uint64_t now = Now();
        o->new_folders.clear();
        o->bumps.clear();
        o->leaving.clear();
        for (const auto& f : plan->folders)
            if (!f.exists) o->leaving[ui::AiListLower(f.name)] = now;
        if (plan->zip_ok) o->leaving[ui::AiListLower(plan->zip_name)] = now;
        o->end_ms = now + 4000;
    }
    auto started = std::make_shared<uint64_t>(Now());
    AiChatPush(s, 300, [plan, started, abort_run](AppState& s3) {
        size_t back = 0;
        for (const auto& it : plan->items)
            if (it.st == TaskPlan::Item::St::Moved && Exists(it.src)) ++back;
        if (back < plan->moved && !(Now() - *started > 3000 && !s3.ops.Status().active) && Now() - *started < 600000)
            return 150;
        AiChat& c3 = Chat(s3);
        if (AiBlockView* b = TaskCard(s3, *plan)) {
            b->collapsed = true;
            b->meta = Fill(Text(I::AiTkPlanMetaUndone), L"{n}", Num(plan->steps));
        }
        if (!abort_run) {
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(I::AiTkUndoneSub), L"{n}", Num(back));
            }
        }
        AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
        note.text = Text(abort_run ? I::AiTkAborted : I::AiTkUndoNote);
        AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
        if (abort_run) {
            AiAgent& a = Agent(s3);
            if (a.generation == abort_run->gen) {
                AddToolResult(a, abort_run->call_id,
                              ToolOutcome::Fail("user_undid", "A file could not be moved, the task paused and the user "
                                                              "chose to undo it in Pulse: everything done so far was "
                                                              "reversed and nothing changed. Do not run it again unless "
                                                              "asked."));
                ++a.call_index;
                AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s3);
            }
        } else if (c3.agent) {
            c3.agent->pending_note = L"(The user clicked 撤销全部 in Pulse: the last run_task was reversed; every file "
                                     L"is back where it was and the new folders and zip are gone.)";
        }
        Redraw(s3);
        return -1;
    });
}

void RunTask(AppState& s, std::shared_ptr<TaskPlan> plan, const std::string& call_id) {
    AiAgent& a = Agent(s);
    a.organized = true;
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    plan->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    plan->started = Now();
    g_task_ops.clear();
    auto run = std::make_shared<TaskRun>();
    run->p = plan;
    run->call_id = call_id;
    run->gen = a.generation;
    run->last_change = Now();
    run->stagger = plan->moves <= 60 ? (std::max)(20u, (std::min)(190u, static_cast<uint32_t>(3000 / (std::max<size_t>)(plan->moves, 1)))) : 0u;
    if (ui::AiListOverlay* o = g_list.get()) {
        o->dest_end = Now();
        o->end_ms = Now() + 500;
    }
    if (plan->step_folders >= 0) {
        SetTaskStep(s, *plan, plan->step_folders, ui::AiTaskState::Running, 0.0f);
    }
    PumpTask(s, run, 200);
}

void ToolTask(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const auto fail = [&](const char* code, std::string message) {
        AddToolResult(a, call.id, ToolOutcome::Fail(code, std::move(message)));
        ++a.call_index;
        NextTool(s);
    };
    auto plan = std::make_shared<TaskPlan>();
    plan->label = json::Wide(args.get_string("label"));
    if (plan->label.size() > 30) plan->label = plan->label.substr(0, 30);
    plan->noun = json::Wide(args.get_string("noun"));
    if (plan->noun.size() > 12 || plan->noun.empty()) plan->noun = Text(I::AiTkFiles);
    plan->dest = ResolveTaskDest(json::Wide(args.get_string("dest")));
    if (plan->dest.empty() || fs::IsVirtualPath(plan->dest) || !ValidDestParts(plan->dest))
        return fail("invalid_args", "dest must be an absolute folder path or start with Documents, Downloads, Desktop, "
                                    "Pictures, Videos or Music, e.g. Documents\\\\Invoices");
    const std::string by = args.get_string("group_by");
    plan->by = by == "month" ? TaskPlan::By::Month : by == "year" ? TaskPlan::By::Year
             : by == "type" ? TaskPlan::By::Type : TaskPlan::By::None;
    if (const json::Value* m = args.find("move"); m && m->is_bool()) plan->move = m->as_bool();
    plan->zip_name = json::Wide(args.get_string("zip"));
    while (!plan->zip_name.empty() && std::iswspace(plan->zip_name.back())) plan->zip_name.pop_back();
    if (!plan->zip_name.empty() && !ValidName(plan->zip_name))
        return fail("invalid_args", "zip must be a plain file name such as Invoices-2026.zip");
    if (!plan->move && plan->zip_name.empty()) return fail("invalid_args", "with move false, give zip");
    if (!plan->zip_name.empty() && !app::ZipCompressAvailable())
        return fail("unsupported", "Zip files cannot be made on this PC (the archive component is missing).");
    // Places the task never takes files from.
    std::vector<std::wstring> guarded;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
                                    &FOLDERID_ProgramData, &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData})
        if (std::wstring g = KnownFolder(*id); !g.empty()) guarded.push_back(std::move(g));
    for (const std::wstring& g : guarded)
        if (UnderPath(plan->dest, g)) return fail("invalid_args", "dest is inside a system or program folder");
    std::vector<std::wstring> type_names;
    for (I id : {I::AiTkTypeDocs, I::AiTkTypeImages, I::AiTkTypeVideos, I::AiTkTypeAudio, I::AiTkTypeArchives,
                 I::AiTkTypeCode, I::AiTkTypeOther, I::AiTkTypeOther})
        type_names.push_back(Text(id));
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;

    // After the files are known: the plan on a worker, then the card.
    auto build = [plan, guarded, type_names, call_id, gen](AppState& st, std::vector<std::wstring> paths, uint64_t ms) {
        plan->find_ms = ms;
        auto list = std::make_shared<std::vector<std::wstring>>(std::move(paths));
        RunJob(st, [plan, list, guarded, type_names](const std::atomic<bool>&) {
                   return BuildTaskPlan(*plan, *list, guarded, type_names);
               },
               [plan, call_id, gen](AppState& s2, const ToolOutcome& out) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return;
                   AiChat& c2 = Chat(s2);
                   if (!out.ok) {
                       EndStep(s2, a2, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message)));
                       AddToolResult(a2, call_id, out);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   TaskPlan& p = *plan;
                   p.dest_label = ResultFolderLabel(p.dest);
                   // Where the files are: 下载 8 · 桌面 3 · 文档 3.
                   std::vector<std::pair<std::wstring, size_t>> where;
                   for (const auto& it : p.items) {
                       const std::wstring l = ResultFolderLabel(app::ai::ParentFolder(it.src));
                       auto f = std::find_if(where.begin(), where.end(), [&](const auto& w) { return w.first == l; });
                       if (f == where.end()) where.push_back({l, 1});
                       else ++f->second;
                   }
                   std::stable_sort(where.begin(), where.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
                   for (size_t i = 0; i < where.size() && i < 3; ++i)
                       p.where += (i ? L" \u00B7 " : L"") + where[i].first + L" " + Num(where[i].second);
                   if (where.size() > 3) p.where += L" \u2026";
                   EndStep(s2, a2, true,
                           Fill(Fill(Fill(Text(I::AiTkFindDone), L"{n}", Num(p.items.size())), L"{where}", p.where),
                                L"{t}", Secs(p.find_ms)));
                   if (AiBlockView* sb = Block(s2, a2.steps_id)) {
                       sb->label = Fill(Fill(Text(p.from_selection ? I::AiTkStepsSel : I::AiTkStepsFound), L"{label}",
                                             p.label),
                                        L"{n}", Num(p.items.size()));
                       sb->collapsed = true;
                       sb->open = false;
                   }
                   a2.steps_id = 0;
                   // 任务计划.
                   AiBlockView& b = c2.Add(AiBlockKind::Task, Now());
                   p.card_id = b.id;
                   b.label = Text(I::AiTkPlanLabel);
                   {
                       ui::AiTaskStepView v;
                       v.title = p.from_selection ? Text(I::AiTkStepSel) : Fill(Text(I::AiTkStepFind), L"{label}", p.label);
                       v.tag = Text(I::AiTkTagReadDone);
                       v.desc = Fill(Fill(Text(I::AiTkFindDesc), L"{n}", Num(p.items.size())), L"{where}", p.where);
                       v.state = ui::AiTaskState::Done;
                       b.task.push_back(std::move(v));
                   }
                   if (p.new_folders) {
                       ui::AiTaskStepView v;
                       const bool subs = p.by != TaskPlan::By::None && p.new_folders > (p.dest_exists ? 0u : 1u);
                       v.title = Text(!subs ? I::AiTkStepFolder
                                      : p.by == TaskPlan::By::Month ? I::AiTkStepFoldersMonth
                                      : p.by == TaskPlan::By::Year ? I::AiTkStepFoldersYear : I::AiTkStepFoldersType);
                       v.arg = p.dest_label;
                       v.desc = Fill(Fill(Text(I::AiTkFoldersDesc), L"{n}", Num(p.new_folders)), L"{names}",
                                     FolderList(p, true, 6));
                       p.step_folders = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   if (p.moves) {
                       ui::AiTaskStepView v;
                       v.title = Fill(Text(I::AiTkStepMove), L"{n}", Num(p.moves));
                       if (p.step_folders < 0) v.arg = p.dest_label;
                       v.desc = p.by == TaskPlan::By::Month ? Text(I::AiTkMoveDescMonth)
                              : p.by == TaskPlan::By::Year ? Text(I::AiTkMoveDescYear)
                              : p.by == TaskPlan::By::Type ? Text(I::AiTkMoveDescType)
                                                           : Fill(Text(I::AiTkMoveDescNone), L"{folder}", p.dest_label);
                       if (p.clashes) v.desc += Fill(Text(I::AiTkMoveClash), L"{n}", Num(p.clashes));
                       p.step_move = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   if (!p.zip_name.empty()) {
                       ui::AiTaskStepView v;
                       v.title = Text(I::AiTkStepZip);
                       v.arg = p.zip_name;
                       v.desc = Fill(Text(p.move ? I::AiTkZipDesc : I::AiTkZipDescStay), L"{folder}", p.dest_label);
                       p.step_zip = static_cast<int>(b.task.size());
                       b.task.push_back(std::move(v));
                   }
                   p.steps = b.task.size();
                   b.meta = Fill(Text(I::AiTkPlanMeta), L"{n}", Num(p.steps));
                   if (p.new_folders) b.sums.push_back(Sum(ui::AiIcon::FolderPlus, I::AiTkSumFolders, p.new_folders));
                   if (p.moves) b.sums.push_back(Sum(ui::AiIcon::Move, I::AiTkSumMove, p.moves));
                   if (!p.zip_name.empty()) b.sums.push_back(Sum(ui::AiIcon::Package, I::AiTkSumZip, 1));
                   b.safe = Text(I::AiTkSafe);
                   // The rows of the search show where each file goes.
                   if (app::Tab* tab = ActiveTab(s2); tab && Shown(s2, tab->current_path) && p.move) {
                       ui::AiListOverlay& o = ListOverlay(tab->current_path);
                       o.dests.clear();
                       for (const auto& it : p.items) {
                           if (it.already) continue;
                           bool fresh = it.group.empty() ? !p.dest_exists : false;
                           for (const auto& f : p.folders)
                               if (f.name == it.group) fresh = !f.exists;
                           o.dests[ui::AiListLower(it.name)] = {it.group.empty() ? Leaf(p.dest) : it.group,
                                                                fresh ? ui::AiListOverlay::kDestAccent
                                                                      : static_cast<uint8_t>(ui::AiTint::Neutral)};
                       }
                       o.dest_start = Now() + 500;
                       o.dest_end = 0;
                   }
                   // The confirmation; the turn waits here for the user.
                   AiChatPush(s2, 650, [plan, call_id, gen](AppState& s4) {
                       AiAgent& ag = Agent(s4);
                       if (ag.generation != gen) return -1;
                       AiChat& ch = Chat(s4);
                       TaskPlan& tp = *plan;
                       AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
                       if (!s4.aiAssistant.open) ch.unread = true;
                       AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
                       cf.title = Text(I::AiTkConfirmTitle);
                       std::wstring what;
                       const std::wstring sep = Text(I::AiTkListSep);
                       if (tp.new_folders) what += Fill(Text(I::AiTkSumFolders), L"{n}", Num(tp.new_folders));
                       if (tp.moves) what += (what.empty() ? L"" : sep) + Fill(Text(I::AiTkSumMove), L"{n}", Num(tp.moves));
                       if (!tp.zip_name.empty()) what += (what.empty() ? L"" : sep) + Fill(Text(I::AiTkSumZip), L"{n}", L"1");
                       cf.sub = Fill(Text(I::AiTkConfirmSub), L"{what}", what);
                       cf.hint = Text(I::AiTkConfirmHint);
                       cf.hint_icon = ui::AiIcon::Undo;
                       cf.deny = Text(I::AiAsstDeny);
                       cf.accept = Text(I::AiTkRun);
                       ch.confirm_id = cf.id;
                       ch.on_confirm = [plan, call_id, gen](AppState& s3, bool accept) {
                           AiChat& c3 = Chat(s3);
                           AiAgent& a3 = Agent(s3);
                           if (AiBlockView* card = c3.Find(c3.confirm_id)) {
                               const uint64_t now = Now();
                               if (now > card->born_ms) c3.turn_start += now - card->born_ms;
                               card->done = true;
                               card->title = Text(accept ? I::AiTkConfirmed : I::AiTkDenied);
                           }
                           c3.confirm_id = 0;
                           c3.on_confirm = nullptr;
                           if (a3.generation != gen) return;
                           if (!accept) {
                               if (AiBlockView* pb = TaskCard(s3, *plan)) pb->collapsed = true;
                               if (g_list) {
                                   g_list->dest_end = Now();
                                   g_list->end_ms = Now() + 500;
                               }
                               AddToolResult(a3, call_id,
                                             ToolOutcome::Fail("user_denied",
                                                               "The user declined this task in Pulse; nothing changed. "
                                                               "Do not propose the same task again; ask what they would "
                                                               "like instead."));
                               ++a3.call_index;
                               AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                               NextTool(s3);
                               Redraw(s3);
                               return;
                           }
                           RunTask(s3, plan, call_id);
                           Redraw(s3);
                       };
                       Redraw(s4);
                       return -1;
                   });
               });
    };

    const json::Value* find = args.find("find");
    if (find && find->is_object()) {
        app::AdvancedSearchSpec spec;
        ToolOutcome error;
        AddStep(s, a, ui::AiIcon::Search, Text(I::AiQStep), plan->label, Text(I::AiQStepRun));
        if (!app::ai::SearchSpecFromArgs(*find, spec, error) || !s.appPrefs.search_enabled) {
            if (!s.appPrefs.search_enabled && error.ok)
                error = ToolOutcome::Fail("unsupported", "Search is turned off in Pulse's settings, so nothing can be found by name.");
            EndStep(s, a, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(error.error.message)));
            AddToolResult(a, call.id, error);
            ++a.call_index;
            AiChatPush(s, 250, [](AppState& s2) { NextTool(s2); return -1; });
            return;
        }
        const std::wstring path = app::MakeSearchPath(app::CompileSearchQuery(spec));
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiQChipSearching), 0.4f);
        NavigateTo(s, path);
        AwaitSearch(s, path, [build, call_id, gen](AppState& st, const app::Tab* tab, uint64_t ms) {
            AiAgent& ag = Agent(st);
            if (ag.generation != gen) return;
            if (!tab) {
                EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiQNoAnswer)));
                AddToolResult(ag, call_id, ToolOutcome::Fail("timeout", "Pulse's search did not finish.", true));
                ++ag.call_index;
                AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                return;
            }
            std::vector<std::wstring> paths;
            if (tab->search_entries)
                for (const fs::DirEntry& e : *tab->search_entries)
                    if (!e.is_dir) paths.push_back(e.full_path);
            if (static_cast<size_t>(tab->search_total) > (tab->search_entries ? tab->search_entries->size() : 0)) {
                EndStep(st, ag, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", Text(I::AiTkTooMany)));
                AddToolResult(ag, call_id, ToolOutcome::Fail("too_many", "The search found more files than Pulse "
                                                                         "loaded; narrow the conditions."));
                ++ag.call_index;
                AiChatPush(st, 250, [](AppState& s2) { NextTool(s2); return -1; });
                return;
            }
            build(st, std::move(paths), ms);
        });
        return;
    }
    // The selection of the active tab.
    app::Tab* tab = ActiveTab(s);
    std::vector<std::wstring> paths;
    if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) paths = SelectedFullPaths(*tab);
    if (paths.empty())
        return fail("no_files", "Nothing is selected in Pulse. Pass find with search conditions, or ask the user to "
                                "select the files first.");
    plan->from_selection = true;
    AddStep(s, a, ui::AiIcon::Rows, Text(I::AiTkStepSel), {}, Text(I::AiQStepRun));
    build(s, std::move(paths), 0);
}
} // namespace ai_agent

} // namespace pulse
