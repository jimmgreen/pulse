// ai_agent_rename.cpp - Pulse 助手's rename_files: the rule, the preview list and the renames.
// Split from ai_assistant_agent.cpp; the shared parts are in ai_agent_internal.h.
#include "ai_agent_internal.h"

namespace pulse {
namespace ai_agent {

// ------------------------------------------------------- rename_files --
// Flow B: the model gives one rule in
// the batch rename engine's steps; Pulse reads the dates taken, makes every
// name on this PC, shows old → new and asks once. 在批量重命名中调整… opens
// the dialog with the rule filled in.

namespace rn = app::rename;

constexpr size_t kMaxRename = 2000;
constexpr size_t kRenameRows = 200;   // rows the card lists when open

struct RenameJob {
    std::vector<rn::Source> sources;
    std::vector<std::wstring> cameras;   // per source (empty: unknown)
    // lower-case parent -> lower-case names in it that are not renamed
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> existing;
    std::vector<rn::Step> steps;
    rn::Options opts;
    rn::Plan plan;
    bool needs_taken = false;
    size_t media = 0, with_taken = 0, cams = 0;
    std::wstring folder;                 // the first file's folder
    std::wstring label;                  // its name, or 3 个文件夹
    bool one_folder = true;
    uint64_t undo_group = 0;
    std::vector<size_t> rows;            // plan rows that rename, in display order
    std::vector<std::wstring> old_paths, new_paths;   // per entry of rows
};

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring ExtPart(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? std::wstring() : name.substr(dot);
}

bool ExtIn(const std::wstring& lower_ext, std::initializer_list<const wchar_t*> list) {
    for (const wchar_t* e : list)
        if (lower_ext == e) return true;
    return false;
}
bool IsVideoExt(const std::wstring& e) {
    return ExtIn(e, {L".mp4", L".mov", L".m4v", L".avi", L".mkv", L".3gp", L".wmv", L".mts", L".m2ts"});
}
bool IsPhotoExt(const std::wstring& e) {
    return ExtIn(e, {L".jpg", L".jpeg", L".png", L".heic", L".heif", L".tif", L".tiff", L".webp", L".gif", L".bmp",
                     L".dng", L".cr2", L".cr3", L".nef", L".arw", L".orf", L".rw2", L".raf", L".srw", L".jxl", L".avif"});
}

uint64_t LocalTicksOf(const FILETIME& utc) {
    FILETIME local{};
    const FILETIME& ft = FileTimeToLocalFileTime(&utc, &local) ? local : utc;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// The worker: file times, the other names in each folder, dates taken and
// cameras (properties only, never the pictures), then the plan.
ToolOutcome ReadRenameSources(RenameJob& job, const std::atomic<bool>& cancelled) {
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> selected;
    std::unordered_map<std::wstring, std::wstring> dirs;   // lower -> as written
    for (rn::Source& src : job.sources) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(src.path.c_str(), GetFileExInfoStandard, &fad)) src.modified = LocalTicksOf(fad.ftLastWriteTime);
        const std::wstring dir = ParentDir(src.path);
        selected[rn::Lower(dir)].insert(rn::Lower(src.name));
        dirs.emplace(rn::Lower(dir), dir);
    }
    for (const auto& kv : selected) {
        if (cancelled.load()) break;
        auto& names = job.existing[kv.first];
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW((dirs[kv.first] + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
            std::wstring lower = rn::Lower(fd.cFileName);
            if (!kv.second.count(lower)) names.insert(std::move(lower));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    job.cameras.assign(job.sources.size(), std::wstring());
    if (job.needs_taken) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unordered_set<std::wstring> cams;
        for (size_t i = 0; i < job.sources.size() && !cancelled.load(); ++i) {
            rn::Source& src = job.sources[i];
            const std::wstring ext = rn::Lower(ExtPart(src.name));
            const bool video = IsVideoExt(ext);
            if (!video && !IsPhotoExt(ext)) continue;
            ++job.media;
            // The shell parses no \\?\ paths (tabs keep the prefix).
            std::wstring shell_path = src.path;
            if (shell_path.rfind(L"\\\\?\\UNC\\", 0) == 0) shell_path = L"\\\\" + shell_path.substr(8);
            else if (shell_path.rfind(L"\\\\?\\", 0) == 0) shell_path.erase(0, 4);
            IPropertyStore* store = nullptr;
            if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))))
                continue;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Photo_DateTaken, &v)) && v.vt == VT_FILETIME) src.taken = LocalTicksOf(v.filetime);
            PropVariantClear(&v);
            if (!src.taken && video) {
                PropVariantInit(&v);
                if (SUCCEEDED(store->GetValue(PKEY_Media_DateEncoded, &v)) && v.vt == VT_FILETIME)
                    src.taken = LocalTicksOf(v.filetime);
                PropVariantClear(&v);
            }
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Photo_CameraModel, &v)) && v.vt == VT_LPWSTR && v.pwszVal && *v.pwszVal)
                job.cameras[i] = v.pwszVal;
            PropVariantClear(&v);
            store->Release();
            if (src.taken) ++job.with_taken;
            if (!job.cameras[i].empty()) cams.insert(job.cameras[i]);
        }
        job.cams = cams.size();
        if (SUCCEEDED(init)) CoUninitialize();
    }
    if (cancelled.load()) return ToolOutcome::Fail("cancelled", "stopped");
    const auto* existing = &job.existing;
    job.plan = rn::Run(job.sources, job.steps, job.opts,
                       [existing](const std::wstring& source_path, const std::wstring& lower_name) {
                           auto it = existing->find(rn::Lower(ParentDir(source_path)));
                           return it != existing->end() && it->second.count(lower_name) != 0;
                       });
    ToolOutcome out;
    out.data = json::Value::Object();
    return out;
}

// A template or insert text split into literal text and {tokens}.
struct PatternPiece {
    bool token = false;
    std::wstring text;   // literal text, or the token's key
    std::wstring fmt;    // {taken:yyyyMMdd} -> yyyyMMdd
};

bool KeyIn(const std::wstring& k, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        if (k == n) return true;
    return false;
}

bool KnownKey(const std::wstring& k) {
    return KeyIn(k, {L"原名", L"name", L"扩展名", L"ext", L"序号", L"序號", L"n", L"number", L"拍摄日期", L"拍攝日期",
                     L"taken", L"修改日期", L"modified", L"date", L"文件夹", L"資料夾", L"folder", L"尺寸", L"size"});
}

std::vector<PatternPiece> SplitPattern(const std::wstring& p) {
    std::vector<PatternPiece> out;
    std::wstring lit;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == L'{') {
            const size_t close = p.find(L'}', i + 1);
            if (close != std::wstring::npos) {
                std::wstring key = p.substr(i + 1, close - i - 1), fmt;
                const size_t colon = key.find(L':');
                if (colon != std::wstring::npos) {
                    fmt = key.substr(colon + 1);
                    key.resize(colon);
                }
                if (KnownKey(key)) {
                    if (!lit.empty()) out.push_back({false, std::move(lit), {}});
                    lit.clear();
                    out.push_back({true, std::move(key), std::move(fmt)});
                    i = close + 1;
                    continue;
                }
            }
        }
        lit += p[i++];
    }
    if (!lit.empty()) out.push_back({false, std::move(lit), {}});
    return out;
}

std::wstring PadNum(long long v, int digits) {
    std::wstring s = std::to_wstring(v);
    while (static_cast<int>(s.size()) < digits) s.insert(s.begin(), L'0');
    return s;
}

std::wstring NumberLabel(int start, int digits, bool daily) {
    return Fill(Text(daily ? I::AiRnTokNumDaily : I::AiRnTokNum), L"{s}", PadNum(start, (std::max)(1, digits)));
}

std::wstring TokenLabel(const PatternPiece& t, const rn::Step& st, bool daily) {
    const std::wstring& k = t.text;
    if (KeyIn(k, {L"拍摄日期", L"拍攝日期", L"taken"})) return Text(I::BrTokTaken);
    if (KeyIn(k, {L"序号", L"序號", L"n", L"number"})) {
        int digits = st.digits_n;
        if (!t.fmt.empty()) digits = static_cast<int>(std::wcstol(t.fmt.c_str(), nullptr, 10));
        else if (k == L"n") digits = 1;
        return NumberLabel(st.start, digits, daily);
    }
    if (KeyIn(k, {L"原名", L"name"})) return Text(I::BrTokName);
    if (KeyIn(k, {L"修改日期", L"modified", L"date"})) return Text(I::BrTokModified);
    if (KeyIn(k, {L"文件夹", L"資料夾", L"folder"})) return Text(I::BrTokFolder);
    if (KeyIn(k, {L"尺寸", L"size"})) return Text(I::BrTokSize);
    return L".ext";
}

// The rule as the card shows it; `hint` is the extension line (扩展名不变).
std::vector<ui::AiRuleToken> RuleOf(const RenameJob& job, std::wstring& hint) {
    std::vector<ui::AiRuleToken> out;
    hint = Text(I::AiRnExtKeep);
    const auto word = [&](std::wstring w) { out.push_back({std::move(w), false, true}); };
    const auto lit = [&](std::wstring w) { out.push_back({std::move(w), false, false}); };
    const auto chip = [&](std::wstring w) { out.push_back({std::move(w), true, false}); };
    const auto text = [&](const std::wstring& t, const rn::Step& st) {
        for (const PatternPiece& p : SplitPattern(t)) {
            if (p.token) chip(TokenLabel(p, st, job.opts.daily));
            else lit(p.text);
        }
    };
    bool first = true;
    for (const rn::Step& st : job.steps) {
        if (!st.on) continue;
        if (st.type == rn::StepType::Ext) {
            if (st.mode == rn::CaseMode::Change) hint = Fill(Text(I::AiRnExtTo), L"{ext}", L"." + st.ext_to);
            else hint = Text(st.mode == rn::CaseMode::Upper ? I::AiRnExtUpper : I::AiRnExtLower);
            continue;
        }
        if (!first) word(L"\u00B7");
        first = false;
        switch (st.type) {
        case rn::StepType::Template: text(st.pattern, st); break;
        case rn::StepType::Replace:
            if (st.with.empty()) {
                word(Text(I::AiRnStepRemove));
                lit(st.find);
            } else {
                lit(st.find);
                word(Text(I::AiRnStepReplace));
                lit(st.with);
            }
            break;
        case rn::StepType::Remove:
            if (st.brackets) word(Text(I::AiRnStepBrackets));
            if (st.spaces) word(Text(I::AiRnStepSpaces));
            if (st.digits) word(Text(I::AiRnStepDigits));
            if (!st.custom.empty()) {
                word(Text(I::AiRnStepRemove));
                lit(st.custom);
            }
            break;
        case rn::StepType::Insert:
            if (st.where == rn::Where::Start) word(Text(I::AiRnStepStart));
            else if (st.where == rn::Where::End) word(Text(I::AiRnStepEnd));
            else word(Fill(Text(I::AiRnStepAt), L"{n}", Num(static_cast<size_t>((std::max)(0, st.at)))));
            text(st.text, st);
            break;
        case rn::StepType::Case:
            word(Text(st.mode == rn::CaseMode::Upper   ? I::AiRnStepUpper
                      : st.mode == rn::CaseMode::Title ? I::AiRnStepTitleCase
                                                        : I::AiRnStepLower));
            break;
        case rn::StepType::Number:
            word(Text(st.where == rn::Where::Start ? I::AiRnStepStart : I::AiRnStepEnd));
            chip(NumberLabel(st.start, st.digits_n, job.opts.daily));
            break;
        default: break;
        }
    }
    return out;
}

// A new name in parts: with one template rule what the tokens made is accent
// and its literal text plain; other rules mark what changed (Diff).
std::vector<ui::AiRuleToken> NewNameParts(const RenameJob& job, const rn::Row& row, std::wstring& ext) {
    const rn::Source& src = job.sources[row.src];
    ext = ExtPart(row.new_name);
    std::wstring stem = row.new_name.substr(0, row.new_name.size() - ext.size());
    std::wstring suffix;
    if (row.added_n > 0) {
        const std::wstring sfx = L" (" + std::to_wstring(row.added_n) + L")";
        if (stem.size() > sfx.size() && stem.compare(stem.size() - sfx.size(), sfx.size(), sfx) == 0) {
            suffix = sfx;
            stem.resize(stem.size() - sfx.size());
        }
    }
    std::vector<ui::AiRuleToken> parts;
    const rn::Step* only = nullptr;
    int active = 0;
    for (const rn::Step& st : job.steps)
        if (st.on && st.type != rn::StepType::Ext) {
            ++active;
            only = &st;
        }
    bool done = false;
    if (active == 1 && only->type == rn::StepType::Template && only->pattern.find(L"{ext}") == std::wstring::npos &&
        only->pattern.find(L"{扩展名}") == std::wstring::npos) {
        const std::vector<PatternPiece> pieces = SplitPattern(only->pattern);
        size_t pos = 0;
        bool ok = true;
        for (size_t k = 0; k < pieces.size() && ok;) {
            if (!pieces[k].token) {
                if (stem.compare(pos, pieces[k].text.size(), pieces[k].text) == 0) {
                    parts.push_back({pieces[k].text, false, false});
                    pos += pieces[k].text.size();
                } else {
                    ok = false;
                }
                ++k;
                continue;
            }
            // Tokens up to the next literal make one accent run.
            size_t j = k;
            while (j < pieces.size() && pieces[j].token) ++j;
            size_t end = stem.size();
            if (j < pieces.size()) {
                end = stem.find(pieces[j].text, pos);
                if (end == std::wstring::npos) ok = false;
            }
            if (ok && end > pos) parts.push_back({stem.substr(pos, end - pos), true, false});
            if (ok) pos = end;
            k = j;
        }
        done = ok && pos == stem.size();
        if (!done) parts.clear();
    }
    if (!done) {
        const std::wstring old_ext = ExtPart(src.name);
        const std::wstring old_stem = src.name.substr(0, src.name.size() - old_ext.size());
        std::vector<rn::Seg> old_side, new_side;
        rn::Diff(old_stem, stem, old_side, new_side);
        for (const rn::Seg& sg : new_side) {
            if (sg.kind == L'-' || sg.text.empty()) continue;
            const bool accent = sg.kind == L'+';
            if (!parts.empty() && parts.back().token == accent) parts.back().text += sg.text;
            else parts.push_back({sg.text, accent, false});
        }
        if (parts.empty()) parts.push_back({stem, false, false});
    }
    if (!suffix.empty()) parts.push_back({suffix, false, false});
    return parts;
}

void SettleRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id, size_t renamed);

// 撤销 on the result card: the group back through the queue; the names slide
// back in the list (60 ms apart, renameRows(false, 60)).
std::function<void(AppState&)> MakeRenameUndo(std::shared_ptr<RenameJob> job) {
    return [job](AppState& st) {
        AiChat& c2 = Chat(st);
        const auto outcome = st.ops.UndoGroup(job->undo_group);
        if (outcome != ops::OpsManager::GroupUndo::Started) {
            AiBlockView& note = c2.Add(AiBlockKind::Note, Now());
            const bool pending = outcome == ops::OpsManager::GroupUndo::Pending;
            note.text = Text(pending ? I::AiAsstUndoBusy : I::AiAsstUndoGone);
            if (pending) c2.on_undo = MakeRenameUndo(job);
            Redraw(st);
            return;
        }
        AiChatSetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
        c2.peek_undo = false;
        c2.on_show = nullptr;
        AiChatHidePeek(st);
        if (job->one_folder && job->rows.size() <= 60) {
            if (ui::AiListOverlay* o = OverlayOf(job->folder)) {
                const uint64_t now = Now();
                o->dests.clear();
                o->morph.clear();
                const uint64_t step = (std::min<uint64_t>)(60, 1200 / (std::max<size_t>)(job->rows.size(), 1));
                for (size_t k = 0; k < job->rows.size(); ++k) {
                    const rn::Row& row = job->plan.rows[job->rows[k]];
                    o->morph[ui::AiListLower(job->sources[row.src].name)] = {row.new_name, now + 250 + step * k};
                }
                o->end_ms = now + 250 + step * job->rows.size() + 3000;
            }
        }
        if (app::Tab* tab = ActiveTab(st); tab && job->one_folder && SameDir(tab->current_path, job->folder)) {
            tab->pending_selected_names.clear();
            for (size_t k : job->rows) tab->pending_selected_names.push_back(job->sources[job->plan.rows[k].src].name);
            if (!tab->pending_selected_names.empty()) tab->pending_selected_name = tab->pending_selected_names.front();
        }
        auto started = std::make_shared<uint64_t>(Now());
        AiChatPush(st, 300, [job, started](AppState& s3) {
            size_t back = 0;
            for (const auto& p : job->old_paths)
                if (Exists(p)) ++back;
            if (back < job->old_paths.size() && !(Now() - *started > 3000 && !s3.ops.Status().active) &&
                Now() - *started < 600000)
                return 150;
            AiChat& c3 = Chat(s3);
            if (AiBlockView* rr = c3.Find(c3.result_id)) {
                rr->done = true;
                rr->title = Text(I::AiAsstUndone);
                rr->sub = Fill(Text(I::AiRnUndoneSub), L"{n}", Num(back));
            }
            AiBlockView& note = c3.Add(AiBlockKind::Note, Now());
            note.text = Text(I::AiRnUndoNote);
            AiChatSetChip(c3, ui::StatusAiState::Done, Text(I::AiAsstUndone));
            if (c3.agent)
                c3.agent->pending_note =
                    L"(The user clicked Undo in Pulse: the last rename_files was reversed and every file has its old name back.)";
            Redraw(s3);
            return -1;
        });
    };
}

// Opens 批量重命名 on `paths` with the job's rule; how many it renamed.
size_t OpenRenameDialog(AppState& s, const RenameJob& job, const std::vector<std::wstring>& paths) {
    rn::Preset preset;
    preset.steps = job.steps;
    for (rn::Step& st : preset.steps) {
        st.ai = true;
        // Back to the dialog's {n} (shown as its 序号 token, padded by digits).
        for (std::wstring* t : {&st.pattern, &st.text})
            for (size_t at = t->find(L"{number}"); at != std::wstring::npos; at = t->find(L"{number}", at + 3)) t->replace(at, 8, L"{n}");
    }
    preset.opts = job.opts;
    app::Tab* select_in = nullptr;
    if (app::Tab* tab = ActiveTab(s); tab && job.one_folder && SameDir(tab->current_path, job.folder)) select_in = tab;
    return ShowBatchRenameWith(s, paths, preset, select_in);
}

void RunRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    job->undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    ops::OpRequest r;
    r.type = ops::OpType::BatchRename;
    r.undo_group = job->undo_group;
    r.undo_label = Text(I::AiRnUndoLabel);
    for (size_t idx : job->plan.exec_order) {
        const rn::Row& row = job->plan.rows[idx];
        if (!row.Renames()) continue;
        r.sources.push_back(job->sources[row.src].path);
        r.new_names.push_back(row.new_name);
    }
    const size_t n = job->rows.size();
    // In the list: each name swaps in turn (renameRows(true, 170)), the tags go.
    uint64_t last_at = Now();
    if (job->one_folder && Shown(s, job->folder)) {
        ui::AiListOverlay& o = ListOverlay(job->folder);
        const uint64_t now = Now();
        o.dest_end = now;
        o.morph.clear();
        if (n <= 60) {
            const uint64_t step = (std::min<uint64_t>)(170, 1200 / (std::max<size_t>)(n, 1));
            for (size_t k = 0; k < n; ++k) {
                const rn::Row& row = job->plan.rows[job->rows[k]];
                last_at = now + 200 + step * k;
                o.morph[ui::AiListLower(row.new_name)] = {job->sources[row.src].name, last_at};
            }
        } else {
            for (size_t k : job->rows) o.flash[ui::AiListLower(job->plan.rows[k].new_name)] = now + 200;
            last_at = now + 200;
        }
        o.end_ms = 0;
    }
    if (app::Tab* tab = ActiveTab(s); tab && job->one_folder && SameDir(tab->current_path, job->folder) && !r.new_names.empty()) {
        tab->pending_selected_names = r.new_names;
        tab->pending_selected_name = r.new_names.front();
    }
    if (!r.sources.empty()) s.ops.Submit(std::move(r));
    // The progress step.
    a.steps_id = 0;
    ui::AiStepView& st = AddStep(s, a, ui::AiIcon::Rename, Text(I::AiRnStep), Fill(Text(I::AiRnMeta), L"{n}", Num(n)),
                                 L"0 / " + Num(n));
    st.progress = 0.0f;
    AiChatSetChip(c, ui::StatusAiState::Busy, Fill(Fill(Text(I::AiRnChipRun), L"{k}", L"0"), L"{n}", Num(n)), 0.0f);
    struct Watch {
        size_t on_disk = 0;
        uint64_t last_change = 0, started = 0, polled = 0;
    };
    auto w = std::make_shared<Watch>();
    w->started = w->last_change = Now();
    const uint64_t gen = a.generation;
    std::shared_ptr<const ui::AiListOverlay> ov = job->one_folder ? AiListOverlayFor(job->folder) : nullptr;
    AiChatPush(s, 0, [job, call_id, w, gen, ov, n, last_at](AppState& st2) {
        AiAgent& ag = Agent(st2);
        if (ag.generation != gen) return -1;
        const uint64_t now = Now();
        if (now - w->polled >= 150) {
            w->polled = now;
            size_t done = 0;
            for (const auto& p : job->new_paths)
                if (Exists(p)) ++done;
            if (done != w->on_disk) {
                w->on_disk = done;
                w->last_change = now;
            }
        }
        // What the list shows: the names that swapped so far.
        size_t shown = w->on_disk;
        if (ov && !ov->morph.empty()) {
            size_t swapped = 0;
            for (const auto& kv : ov->morph)
                if (now >= kv.second.at) ++swapped;
            shown = (std::min)(shown, swapped);
        }
        const float p = n ? static_cast<float>(shown) / static_cast<float>(n) : 1.0f;
        if (ui::AiStepView* step = LastStep(st2, ag)) {
            step->progress = p;
            step->sub = Num(shown) + L" / " + Num(n);
        }
        AiChatSetChip(Chat(st2), ui::StatusAiState::Busy,
                      Fill(Fill(Text(I::AiRnChipRun), L"{k}", Num(shown)), L"{n}", Num(n)), p);
        const bool idle = !st2.ops.Status().active;
        const bool all = w->on_disk >= n;
        if ((all && now >= last_at + 450) || (idle && now - w->last_change > 2500 && now >= last_at) ||
            now - w->started > 600000) {
            SettleRename(st2, job, call_id, w->on_disk);
            return -1;
        }
        return 50;
    });
}

void SettleRename(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id, size_t renamed) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const size_t n = job->rows.size();
    const bool all = renamed >= n;
    EndStep(s, a, renamed > 0,
            all ? Fill(Text(I::AiRnStepDone), L"{n}", Num(renamed))
                : Fill(Fill(Text(I::AiRnStepSome), L"{k}", Num(renamed)), L"{n}", Num(n)));
    if (AiBlockView* b = Block(s, a.steps_id)) {
        b->label = Fill(Text(I::AiRnSteps), L"{n}", Num(renamed));
        b->collapsed = true;
        b->open = false;
    }
    a.steps_id = 0;
    // The result card: 旅行 2026-10 · 10月3日 4 张 · 10月4日 3 张.
    AiBlockView& r = c.Add(AiBlockKind::Result, Now());
    r.title = Fill(Text(I::AiRnResultTitle), L"{n}", Num(renamed));
    r.sub = job->label;
    if (job->needs_taken) {
        std::vector<std::pair<std::pair<int, int>, size_t>> days;   // (month, day) -> files, in order
        for (size_t k : job->rows) {
            const rn::Source& src = job->sources[job->plan.rows[k].src];
            const uint64_t t = src.taken ? src.taken : src.modified;
            FILETIME f{static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
            SYSTEMTIME stime{};
            if (!FileTimeToSystemTime(&f, &stime)) continue;
            const std::pair<int, int> md{stime.wMonth, stime.wDay};
            auto it = std::find_if(days.begin(), days.end(), [&](const auto& d) { return d.first == md; });
            if (it == days.end()) days.push_back({md, 1});
            else ++it->second;
        }
        std::sort(days.begin(), days.end());
        for (size_t i = 0; i < days.size() && i < 4; ++i)
            r.sub += L" \u00B7 " + Fill(Fill(Fill(Text(I::AiRnDay), L"{m}", std::to_wstring(days[i].first.first)), L"{d}",
                                             std::to_wstring(days[i].first.second)),
                                        L"{n}", Num(days[i].second));
        if (days.size() > 4) r.sub += L" \u00B7 \u2026";
    }
    r.deny = Text(I::AiAsstUndo);
    r.accept = Text(I::AiRnOpenDialog);
    r.accept_icon = ui::AiIcon::Sliders;
    c.result_id = r.id;
    // 在批量重命名中打开: the renamed files with the rule, to go on from there.
    c.on_show = [job](AppState& st) {
        std::vector<std::wstring> paths;
        for (const auto& p : job->new_paths)
            if (Exists(p)) paths.push_back(p);
        if (paths.size() < 2) {
            NavigateTo(st, job->folder);
            return;
        }
        OpenRenameDialog(st, *job, paths);
    };
    c.on_undo = MakeRenameUndo(job);
    if (renamed && !job->rows.empty())
        c.peek_text = Fill(Fill(Text(I::AiPeekRenamed), L"{n}", Num(renamed)), L"{name}",
                           job->plan.rows[job->rows.front()].new_name);
    c.peek_undo = renamed > 0;
    if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->end_ms = Now() + 3000;
    // Back to the model.
    ToolOutcome out;
    out.data = json::Value::Object();
    out.data.set("ok", renamed > 0);
    out.data.set("renamed", static_cast<uint64_t>(renamed));
    out.data.set("total", static_cast<uint64_t>(job->sources.size()));
    json::Value examples = json::Value::Array();
    json::Value no_taken = json::Value::Array();
    for (size_t i = 0; i < job->rows.size(); ++i) {
        const rn::Row& row = job->plan.rows[job->rows[i]];
        if (examples.size() < 5) {
            json::Value e = json::Value::Object();
            e.set("from", Utf8(job->sources[row.src].name));
            e.set("to", Utf8(row.new_name));
            examples.push(std::move(e));
        }
        if (row.why == rn::Why::NoTakenDate && no_taken.size() < 5) {
            json::Value e = json::Value::Object();
            e.set("name", Utf8(job->sources[row.src].name));
            SYSTEMTIME stime{};
            const uint64_t t = job->sources[row.src].modified;
            FILETIME f{static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
            if (FileTimeToSystemTime(&f, &stime)) {
                char buf[32];
                sprintf_s(buf, "%04u-%02u-%02u %02u:%02u", stime.wYear, stime.wMonth, stime.wDay, stime.wHour, stime.wMinute);
                e.set("modified_used", buf);
            }
            no_taken.push(std::move(e));
        }
    }
    out.data.set("examples", std::move(examples));
    if (no_taken.size()) out.data.set("no_date_taken", std::move(no_taken));
    out.data.set("undo", "The user can undo the whole rename with the Undo button on the result card in Pulse.");
    if (!all) out.data.set("note", "Some files were not renamed (locked or changed meanwhile).");
    AddToolResult(a, call_id, out);
    ++a.call_index;
    AiChatSetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    AiChatPush(s, 350, [](AppState& st) { NextTool(st); return -1; });
}

// The 重命名方案 card, the tags in the list, then the confirmation.
void ShowRenamePlan(AppState& s, std::shared_ptr<RenameJob> job, const std::string& call_id) {
    AiAgent& a = Agent(s);
    AiChat& c = Chat(s);
    const rn::Plan& plan = job->plan;
    // Rows in the list's order when it shows the folder.
    std::vector<size_t> order(plan.rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (app::Tab* tab = ActiveTab(s); tab && job->one_folder && SameDir(tab->current_path, job->folder) &&
                                      tab->EntryCount() <= 20000) {
        std::unordered_map<std::wstring, size_t> at;
        for (size_t i = 0; i < tab->EntryCount(); ++i) at.emplace(ui::AiListLower(tab->EntryAt(i).name), i);
        const auto rank = [&](size_t i) {
            auto it = at.find(ui::AiListLower(job->sources[plan.rows[i].src].name));
            return it == at.end() ? SIZE_MAX : it->second;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t l, size_t r) { return rank(l) < rank(r); });
    }
    for (size_t i : order)
        if (plan.rows[i].Renames()) {
            job->rows.push_back(i);
            const rn::Row& row = plan.rows[i];
            job->old_paths.push_back(job->sources[row.src].path);
            job->new_paths.push_back(JoinPath(ParentDir(job->sources[row.src].path), row.new_name));
        }
    FoldSteps(s, a);
    AiBlockView& b = c.Add(AiBlockKind::Rename, Now());
    b.label = Text(I::AiRnTitle);
    b.meta = Fill(Text(I::AiRnMeta), L"{n}", Num(job->sources.size()));
    b.rule_label = Text(I::AiRnRule);
    b.rule = RuleOf(*job, b.hint);
    // Rows: four while folded (the first three and the first warned one).
    const size_t n = job->rows.size();
    size_t warned = SIZE_MAX;
    for (size_t k = 0; k < n && k < kRenameRows; ++k) {
        const rn::Row& row = plan.rows[job->rows[k]];
        ui::AiRenameRow v;
        v.old_name = job->sources[row.src].name;
        v.parts = NewNameParts(*job, row, v.ext);
        v.warn = row.why == rn::Why::NoTakenDate;
        if (v.warn && warned == SIZE_MAX) warned = k;
        b.rows.push_back(std::move(v));
    }
    const size_t listed = b.rows.size();
    const bool fold = listed > 5;
    const size_t fourth = warned != SIZE_MAX && warned >= 3 ? warned : 3;
    for (size_t k = 0; k < listed; ++k) b.rows[k].sample = !fold || k < 3 || k == fourth;
    size_t samples = 0;
    for (const auto& v : b.rows) samples += v.sample ? 1 : 0;
    if (fold) {
        b.more = Fill(Text(I::AiRnMore), L"{n}", Num(n - samples));
        b.less = Text(I::AiRnLess);
    }
    // Checks.
    size_t clashed = 0, no_taken = 0, bad = 0;
    std::wstring no_taken_name;
    for (const rn::Row& row : plan.rows) {
        if (row.status == rn::RowStatus::Conflict && row.added_n > 0) ++clashed;
        if (row.why == rn::Why::NoTakenDate) {
            if (!no_taken++) {
                const std::wstring& nm = job->sources[row.src].name;
                no_taken_name = nm.substr(0, nm.size() - ExtPart(nm).size());
            }
        }
        if (row.status == rn::RowStatus::Invalid || row.status == rn::RowStatus::Empty ||
            row.status == rn::RowStatus::Skipped || (row.status == rn::RowStatus::Conflict && !row.added_n))
            ++bad;
    }
    if (clashed) b.checks.push_back({Fill(Text(I::AiRnCheckClash), L"{n}", Num(clashed)), true});
    else b.checks.push_back({Text(I::AiRnCheckOk), false});
    if (job->needs_taken && job->opts.order == rn::Order::Taken && job->cams >= 2)
        b.checks.push_back({Fill(Text(I::AiRnCheckCams), L"{n}", Num(job->cams)), false});
    if (no_taken == 1)
        b.checks.push_back({Fill(Fill(Text(I::AiRnCheckNoTaken), L"{n}", L"1"), L"{name}", no_taken_name), true});
    else if (no_taken > 1)
        b.checks.push_back({Fill(Text(I::AiRnCheckNoTakenMany), L"{n}", Num(no_taken)), true});
    if (plan.same) b.checks.push_back({Fill(Text(I::AiRnCheckSame), L"{n}", Num(static_cast<size_t>(plan.same))), false});
    if (bad) b.checks.push_back({Fill(Text(I::AiRnCheckBad), L"{n}", Num(bad)), true});
    b.sub = Text(I::AiRnFoot);
    const uint32_t card_id = b.id;
    const uint64_t gen = a.generation;
    // The rows come in 140 ms apart (.rnr.in).
    auto ri = std::make_shared<size_t>(0);
    AiChatPush(s, 0, [card_id, ri, gen](AppState& st) {
        if (Agent(st).generation != gen) return -1;
        AiBlockView* cb = Chat(st).Find(card_id);
        if (!cb) return -1;
        while (*ri < cb->rows.size() && !cb->rows[*ri].sample && !cb->open) cb->rows[(*ri)++].in_ms = Now() - 1000;
        if (*ri >= cb->rows.size()) return -1;
        cb->rows[(*ri)++].in_ms = Now();
        return *ri < cb->rows.size() ? 140 : -1;
    });
    // The amber card; the turn waits here for the user.
    AiChatPush(s, 140 * 4 + 300, [job, card_id, call_id, gen](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return -1;
        AiChat& ch = Chat(st);
        // Rows the stagger skipped (folded) are simply there.
        if (AiBlockView* cb = ch.Find(card_id))
            for (auto& v : cb->rows)
                if (!v.in_ms) v.in_ms = Now() - 1000;
        AiChatSetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
        if (!st.aiAssistant.open) ch.unread = true;
        AiBlockView& cf = ch.Add(AiBlockKind::Confirm, Now());
        cf.title = Fill(Text(I::AiRnConfirmTitle), L"{n}", Num(job->rows.size()));
        cf.sub = Fill(Text(I::AiRnConfirmSub), L"{folder}", job->label);
        cf.deny = Text(I::AiAsstDeny);
        cf.accept = Text(I::AiRnAccept);
        cf.extra = Text(I::AiRnAdjust);
        cf.extra_tip = Text(I::AiRnAdjustTip);
        ch.confirm_id = cf.id;
        // Each file shows its new name in the list before you decide.
        if (job->one_folder && Shown(st, job->folder)) {
            cf.hint = Text(st.aiAssistant.floating ? I::AiRnHint : I::AiRnHintRight);
            ui::AiListOverlay& o = ListOverlay(job->folder);
            o.dests.clear();
            o.morph.clear();
            for (size_t k : job->rows) {
                const rn::Row& row = job->plan.rows[k];
                o.dests[ui::AiListLower(job->sources[row.src].name)] = {row.new_name, ui::AiListOverlay::kDestAccent};
            }
            o.dest_start = Now();
            o.dest_end = 0;
        }
        // Closes the confirmation with `title`; false when the turn moved on.
        const auto close = [card_id, gen](AppState& s2, std::wstring title) {
            AiChat& c2 = Chat(s2);
            if (AiBlockView* card = c2.Find(c2.confirm_id)) {
                const uint64_t now = Now();
                if (now > card->born_ms) c2.turn_start += now - card->born_ms;   // waiting is not model time
                card->done = true;
                card->title = std::move(title);
            }
            if (AiBlockView* pb = c2.Find(card_id)) pb->collapsed = true;
            c2.confirm_id = 0;
            c2.on_confirm = nullptr;
            c2.on_extra = nullptr;
            return Agent(s2).generation == gen;
        };
        ch.on_confirm = [job, call_id, close](AppState& s2, bool accept) {
            if (!close(s2, Fill(Text(accept ? I::AiRnConfirmed : I::AiRnDenied), L"{n}", Num(job->rows.size())))) return;
            AiAgent& a2 = Agent(s2);
            if (!accept) {
                if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) {
                    o->dest_end = Now();
                    o->end_ms = Now() + 500;
                }
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_denied",
                                                "The user declined this rename in Pulse; nothing changed. Do not propose "
                                                "the same rule again; ask what they would like instead."));
                ++a2.call_index;
                AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                NextTool(s2);
                Redraw(s2);
                return;
            }
            RunRename(s2, job, call_id);
            Redraw(s2);
        };
        // 在批量重命名中调整…: the dialog (modal) with the rule; the turn goes
        // on with what the user did there.
        ch.on_extra = [job, call_id, close, gen](AppState& s2) {
            AiChat& c2 = Chat(s2);
            const uint32_t confirm = c2.confirm_id;
            c2.on_confirm = nullptr;
            c2.on_extra = nullptr;
            if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->dest_end = Now();
            AiChatSetChip(c2, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
            Redraw(s2);
            std::vector<std::wstring> paths;
            for (const auto& src : job->sources) paths.push_back(src.path);
            const size_t done = OpenRenameDialog(s2, *job, paths);
            AiChat& c3 = Chat(s2);
            c3.confirm_id = confirm;
            if (!close(s2, done ? Fill(Text(I::AiRnAdjusted), L"{n}", Num(done)) : Text(I::AiRnAdjustClosed))) return;
            if (ui::AiListOverlay* o = job->one_folder ? OverlayOf(job->folder) : nullptr) o->end_ms = Now() + 500;
            AiAgent& a2 = Agent(s2);
            if (a2.generation != gen) return;
            if (done) {
                ToolOutcome out;
                out.data = json::Value::Object();
                out.data.set("ok", true);
                out.data.set("renamed", static_cast<uint64_t>(done));
                out.data.set("via", "batch_rename_dialog");
                out.data.set("note", "The user adjusted the rule in Pulse's Batch rename dialog and renamed the files "
                                     "there. They can undo it with Ctrl+Z in Pulse.");
                AddToolResult(a2, call_id, out);
            } else {
                AddToolResult(a2, call_id,
                              ToolOutcome::Fail("user_closed", "The user opened Batch rename with this rule and closed "
                                                               "it without renaming anything. Ask what they want."));
            }
            ++a2.call_index;
            AiChatSetChip(c3, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(s2);
            Redraw(s2);
        };
        return -1;
    });
}

void ToolRename(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const auto fail = [&](const char* code, std::string message) {
        AddToolResult(a, call.id, ToolOutcome::Fail(code, std::move(message)));
        ++a.call_index;
        NextTool(s);
    };
    auto job = std::make_shared<RenameJob>();
    rn::AiReply reply;
    if (!rn::ReadAiSteps(args, reply) || reply.steps.empty())
        return fail("invalid_args", reply.note.empty() ? "steps must list at least one valid step"
                                                       : "no usable step: " + Utf8(reply.note));
    job->steps = std::move(reply.steps);
    // {n} is the engine's legacy unpadded number; the model's "digits" is
    // meant for it, so read it as {number}.
    for (rn::Step& st : job->steps) {
        for (std::wstring* t : {&st.pattern, &st.text})
            for (size_t at = t->find(L"{n}"); at != std::wstring::npos; at = t->find(L"{n}", at + 8)) t->replace(at, 3, L"{number}");
    }
    job->opts.order = reply.has_order ? reply.order : rn::Order::List;
    job->opts.daily = reply.daily;
    job->opts.policy = rn::Policy::AutoNumber;
    // The files: the paths given, else the selection of the active tab.
    std::vector<std::wstring> paths;
    if (const json::Value* p = args.find("paths"); p && p->is_array())
        for (const json::Value& v : p->items()) paths.push_back(json::Wide(v.as_string()));
    if (paths.empty()) {
        app::Tab* tab = ActiveTab(s);
        if (tab && tab->snapshot && !fs::IsVirtualPath(tab->current_path)) paths = SelectedFullPaths(*tab);
        if (paths.empty())
            return fail("no_files", "Nothing is selected in Pulse. Pass paths, or ask the user to select the files first.");
    }
    std::unordered_set<std::wstring> seen;
    for (const std::wstring& p : paths) {
        if (p.size() < 3 || fs::IsVirtualPath(p)) return fail("invalid_path", "not an absolute path: " + Utf8(p));
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return fail("not_found", "no such file: " + Utf8(p));
        if (attr & FILE_ATTRIBUTE_DIRECTORY) return fail("not_a_file", "only files can be renamed here: " + Utf8(p));
        if (!seen.insert(rn::Lower(p)).second) continue;
        rn::Source src;
        src.path = p;
        src.name = Leaf(p);
        src.folder = Leaf(ParentDir(p));
        job->sources.push_back(std::move(src));
    }
    if (job->sources.size() > kMaxRename) return fail("too_many", "at most 2000 files at once");
    job->folder = ParentDir(job->sources.front().path);
    std::unordered_set<std::wstring> parents;
    for (const auto& src : job->sources) parents.insert(rn::Lower(ParentDir(src.path)));
    job->one_folder = parents.size() == 1;
    job->label = job->one_folder ? FolderLabel(job->folder) : Fill(Text(I::AiRnFolders), L"{n}", Num(parents.size()));
    job->needs_taken = rn::NeedsTakenDate(job->steps, job->opts);
    // 读取拍摄时间: a spark walks the photos while the properties are read.
    const size_t n = job->sources.size();
    std::shared_ptr<const ui::AiListOverlay> ov;
    if (job->needs_taken) {
        AddStep(s, a, ui::AiIcon::Camera, Text(I::AiRnStepRead), Fill(Text(I::AiRnMeta), L"{n}", Num(n)),
                Text(I::AiAsstStepReadWait));
        AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipReadingAny), 0.1f);
        if (job->one_folder && Shown(s, job->folder) && n <= 400) {
            ui::AiListOverlay& o = ListOverlay(job->folder);
            o.read_names.clear();
            for (const auto& src : job->sources) o.read_names.insert(ui::AiListLower(src.name));
            o.read_total = o.read_names.size();
            o.read_ms = (std::min<uint64_t>)(185, (std::max<uint64_t>)(12, 2400 / o.read_total)) * o.read_total;
            o.read_start = Now() + 300;
            o.watch_text = Text(I::AiListWatch);
            o.dests.clear();
            o.morph.clear();
            o.end_ms = 0;
            ov = g_list;
        }
    }
    const std::string id = call.id;
    const uint64_t gen = a.generation;
    RunJob(s, [job](const std::atomic<bool>& cancelled) { return ReadRenameSources(*job, cancelled); },
           [job, id, gen, ov](AppState& st, const ToolOutcome& out) {
               AiAgent& ag = Agent(st);
               if (ag.generation != gen) return;
               const auto finish = [job, id](AppState& s2, const ToolOutcome& res) {
                   AiAgent& a2 = Agent(s2);
                   if (!res.ok) {
                       if (job->needs_taken) EndStep(s2, a2, false, Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(res.error.message)));
                       AddToolResult(a2, id, res);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   if (job->needs_taken) {
                       const std::wstring read = Fill(Fill(Text(I::AiRnStepReadDone), L"{k}", Num(job->with_taken)), L"{m}",
                                                      Num(job->media - job->with_taken));
                       EndStep(s2, a2, true, read);
                       // Its own fold label (FoldSteps would repeat the context read's).
                       if (AiBlockView* b = Block(s2, a2.steps_id)) {
                           b->label = read;
                           b->collapsed = true;
                           b->open = false;
                       }
                       a2.steps_id = 0;
                   }
                   const rn::Plan& plan = job->plan;
                   if (plan.bad_regex || plan.blocked) {
                       AddToolResult(a2, id, ToolOutcome::Fail("bad_rule", plan.bad_regex ? "the regex is not valid"
                                                                                           : "the rule gives clashing names"));
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   if (plan.renames == 0) {
                       ToolOutcome none = ToolOutcome::Fail(
                           "no_change", plan.same ? "Every name stays the same with this rule; nothing to rename."
                                                  : "No file gets a usable new name with this rule (empty or invalid names).");
                       AddToolResult(a2, id, none);
                       ++a2.call_index;
                       AiChatPush(s2, 250, [](AppState& s3) { NextTool(s3); return -1; });
                       return;
                   }
                   ShowRenamePlan(s2, job, id);
                   Redraw(s2);
               };
               if (!ov) {
                   finish(st, out);
                   return;
               }
               // Let the walk reach the last photo first; the step and chip count along.
               AiChatPush(st, 0, [ov, gen, out, finish](AppState& s2) {
                   AiAgent& a2 = Agent(s2);
                   if (a2.generation != gen) return -1;
                   const uint64_t now = Now();
                   if (const size_t k = ui::AiListReadK(*ov, now)) {
                       const std::wstring kk = Num(k), nn = Num(ov->read_total);
                       if (ui::AiStepView* step = LastStep(s2, a2))
                           step->sub = Fill(Fill(Text(I::AiAsstStepReading), L"{k}", kk), L"{n}", nn);
                       AiChatSetChip(Chat(s2), ui::StatusAiState::Busy,
                                     Fill(Fill(Text(I::AiAsstChipReading), L"{k}", kk), L"{n}", nn),
                                     static_cast<float>(k) / static_cast<float>(ov->read_total));
                   }
                   if (now < ov->read_start + ov->read_ms + 300) return 50;
                   finish(s2, out);
                   return -1;
               });
           });
}
} // namespace ai_agent

} // namespace pulse
