// dup_finder.cpp - 查找重复文件 in a tab (task 46, scenes ②-⑤). The scan is flow E's (ai_dup_scan.h) on a worker; everything
// else runs on the UI thread of the window that started it.
#include "dup_finder.h"

#include "ai_cleanup_scan.h"
#include "ai_dup_scan.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_model.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../fs/fs_enum.h"
#include "../ops/ops_manager.h"
#include "../ui/confirm_dialog.h"
#include "../ui/ui_view_model.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

namespace pulse::app::dup {
namespace {

using I = l10n::StringId;
namespace ai = app::ai;

std::wstring Text(I id) { return l10n::Get(id); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}

std::wstring Int(uint64_t n) {
    const std::wstring d = std::to_wstring(n);
    std::wstring out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += L',';
        out += d[i];
    }
    return out;
}

// 98.3 MB, 115 MB, 3.1 GB: three figures, like the 效果图.
std::wstring Size(uint64_t b) {
    static const wchar_t* const kUnits[] = {L"KB", L"MB", L"GB", L"TB"};
    if (b < 1024) return std::to_wstring(b) + L" B";
    double v = static_cast<double>(b) / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[32];
    swprintf_s(buf, v >= 100.0 ? L"%.0f %s" : L"%.1f %s", v, kUnits[u]);
    return buf;
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool Exists(const std::wstring& path) {
    const std::wstring p = path.size() >= MAX_PATH && path.size() > 2 && path[1] == L':' ? L"\\\\?\\" + path : path;
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring DateText(uint64_t ticks) {
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, lt{};
    SYSTEMTIME st{}, now{};
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return {};
    GetLocalTime(&now);
    const std::wstring t = Text(st.wYear == now.wYear ? I::AiDupDate : I::AiDupDateY);
    return Fill(Fill(Fill(t, L"{y}", std::to_wstring(st.wYear)), L"{m}", std::to_wstring(st.wMonth)), L"{d}",
                std::to_wstring(st.wDay));
}

FILETIME Ft(uint64_t ticks) { return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}; }

// ------------------------------------------------------------- sessions --

struct Work {
    ai::DupScan scan;
    ai::DupProgress progress;
    std::atomic<bool> cancel{false}, done{false};
    uint64_t min_bytes = kDefaultMinBytes;
};

// The pre-check before a recycle: each acting group's keeper and extras still
// the same bytes (the ops guard is all or nothing, so changed groups drop out
// here, as in flow E).
struct Check {
    std::vector<ai::DupGroup> groups;
    std::vector<char> ok;
    std::atomic<bool> cancel{false}, done{false};
};

struct Session {
    HWND owner = nullptr;
    uint32_t id = 0;
    std::wstring path;                      // pulse:ai-list:<id>
    std::vector<std::wstring> labels;       // per root: 下载, 图片, a folder's name
    std::wstring where;                     // 下载、图片
    std::shared_ptr<Work> work;
    bool show = true;
    bool finished = false;                  // the scan ended; work->scan is the UI's now
    bool newest = false;                    // rule ③: 最新的 (not saved)
    uint64_t started = 0, elapsed = 0, finished_at = 0, last_paint = 0, orphan_since = 0;
    // Recycle.
    bool busy = false, permanent = false;
    std::shared_ptr<Check> check;
    uint64_t task = 0;
    std::vector<std::wstring> sent;
    std::vector<uint64_t> sent_bytes;
    size_t skipped_groups = 0;
    std::wstring skipped_name;
    // Files gone since the scan (recycled, deleted, moved): back in their
    // group if they come back (撤销).
    std::vector<std::pair<size_t, ai::DupFile>> gone;
    bool emptied = false;                   // had groups, none left: 没有重复文件了
};

std::vector<std::shared_ptr<Session>> g_sessions;

Session* Find(const std::wstring& path) {
    for (const auto& x : g_sessions)
        if (x->path == path) return x.get();
    return nullptr;
}

Session* FindId(uint32_t id) {
    for (const auto& x : g_sessions)
        if (x->id == id) return x.get();
    return nullptr;
}

template <typename Fn>
void ForEachViewOf(AppState& s, const std::wstring& path, Fn&& fn) {
    for (const auto& layout : s.window_tabs.items)
        for (const auto& pane : layout->panes)
            if (pane && pane->view.current_path == path) fn(pane->view);
}

bool TabExists(AppState& s, const std::wstring& path) {
    bool found = false;
    ForEachViewOf(s, path, [&](app::Tab&) { found = true; });
    return found;
}

// ------------------------------------------------------------- the list --

std::wstring GroupName(const ai::DupGroup& g) {
    for (const ai::DupFile& f : g.files)
        if (f.clean) return Leaf(f.path);
    return g.files.empty() ? std::wstring() : Leaf(g.files[0].path);
}

size_t Keeper(const ai::DupGroup& g) {
    for (size_t i = 0; i < g.files.size(); ++i)
        if (g.files[i].keep) return i;
    return 0;
}

// "(1)" / 副本 / - Copy: what makes a name not clean (rule ②'s detail).
std::wstring NameMark(const std::wstring& path) {
    std::wstring stem = Leaf(path);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    if (!stem.empty() && stem.back() == L')') {
        const size_t open = stem.find_last_of(L'(');
        if (open != std::wstring::npos && open + 2 < stem.size()) return stem.substr(open);
    }
    std::wstring lower = stem;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    if (lower.find(L"\x526F\x672C") != std::wstring::npos) return L"\x526F\x672C";
    if (lower.find(L"copy") != std::wstring::npos) return L"Copy";
    return {};
}

// 工作\2026 Q3: the keeper's folder under its root (the root's name at the top).
std::wstring RelDir(const Session& x, const ai::DupFile& f) {
    const ai::DupScan& scan = x.work->scan;
    const std::wstring label = f.root < x.labels.size() ? x.labels[f.root] : std::wstring();
    if (f.root >= scan.roots.size()) return label;
    const std::wstring& root = scan.roots[f.root].path;
    const std::wstring parent = Parent(f.path);
    if (parent.size() <= root.size()) return label;
    std::wstring rest = parent.substr(root.size());
    while (!rest.empty() && rest.front() == L'\\') rest.erase(rest.begin());
    return rest.empty() ? label : rest;
}

std::wstring WhyText(const Session& x, const ai::DupGroup& g) {
    using W = ai::DupWhy;
    if (g.files.empty()) return {};
    if (g.both) return Fill(Text(I::DupFWhyBoth), L"{n}", Int(g.Kept()));
    const ai::DupFile& k = g.files[Keeper(g)];
    std::wstring out = Text(I::DupFWhyPre) + L" ";
    switch (g.why) {
    case W::Organized:
        return out + Text(I::DupFWhy1) + L" \u00B7 " + RelDir(x, k);
    case W::CleanName: {
        std::wstring mark;
        if (g.files.size() == 2) mark = NameMark(g.files[Keeper(g) == 0 ? 1 : 0].path);
        return out + Text(I::DupFWhy2) + L" \u00B7 " +
               (mark.empty() ? Text(I::DupFWhy2Many) : Fill(Text(I::DupFWhy2One), L"{mark}", mark));
    }
    case W::Picked:
        return out + Text(I::DupFWhyPicked);
    default: {
        bool same_name = true;
        for (const ai::DupFile& f : g.files)
            if (_wcsicmp(Leaf(f.path).c_str(), Leaf(k.path).c_str()) != 0) same_name = false;
        return out + Text(g.why == W::Newest ? I::DupFWhy3New : I::DupFWhy3) + L" \u00B7 " +
               (same_name ? DateText(k.mtime) : Text(I::DupFWhySameBytes));
    }
    }
}

struct Totals {
    size_t groups = 0, extra = 0;
    uint64_t bytes = 0;
};

Totals Count(const Session& x) {
    Totals t;
    if (!x.finished) return t;
    for (const ai::DupGroup& g : x.work->scan.groups) {
        if (g.files.size() < 2) continue;
        ++t.groups;
        t.extra += g.Extra();
        t.bytes += g.ExtraBytes();
    }
    return t;
}

// The rows of the tab: a group per header ("name\tmeta\twhy\twaste\tboth",
// laid out by ui_pane_list.cpp), 保留 / 多出 / 都留 in 状态, nothing selected.
void PutList(const Session& x) {
    std::vector<ai::AiListRow> rows;
    size_t shown = 0;
    if (x.finished) {
        for (const ai::DupGroup& g : x.work->scan.groups) {
            if (g.files.size() < 2) continue;
            if (shown >= 254) break;
            ++shown;
            const bool keep_all = g.Extra() == 0;
            std::wstring label = GroupName(g);
            label += L'\t';
            label += Fill(Fill(Text(I::DupFGroupMeta), L"{n}", Int(g.files.size())), L"{size}", Size(g.files[0].bytes));
            label += L'\t';
            label += WhyText(x, g);
            label += L'\t';
            label += keep_all ? Text(I::AiDupBoth) : Fill(Text(I::DupFWaste), L"{size}", Size(g.ExtraBytes()));
            label += L'\t';
            if (keep_all) label += L'1';
            // The copy (or copies) kept first, as the group's reason speaks of it.
            std::vector<const ai::DupFile*> order;
            for (const ai::DupFile& f : g.files) order.push_back(&f);
            std::stable_partition(order.begin(), order.end(), [](const ai::DupFile* f) { return f->keep; });
            for (const ai::DupFile* fp : order) {
                const ai::DupFile& f = *fp;
                ai::AiListRow r;
                r.path = f.path;
                r.reason = Text(!f.keep ? I::DupFExtra : g.both ? I::AiDupBoth : I::AiDupKeep);
                r.bytes = f.bytes;
                r.mtime = Ft(f.mtime);
                r.on = false;
                r.group = static_cast<uint8_t>(shown);
                r.group_label = label;
                rows.push_back(std::move(r));
            }
        }
    }
    std::wstring title = !x.finished ? Text(I::DupFTabRunning)
                         : shown ? Fill(Text(I::AiDupListTab), L"{g}", Int(shown))
                                 : Text(I::DupFTitle);
    ai::PutAiList(x.id, std::move(title), std::move(rows), Text(I::AiDupColumn));
}

// Reloads the tab where it is: scroll and selected rows stay.
void RefreshList(AppState& s, const Session& x) {
    struct Keep {
        app::Tab* tab;
        float scroll;
        std::unordered_set<std::wstring> picked;
    };
    std::vector<Keep> keep;
    ForEachViewOf(s, x.path, [&](app::Tab& tab) {
        Keep k{&tab, tab.scroll_y, {}};
        for (const int i : tab.SelectedIndices())
            if (i >= 0 && static_cast<size_t>(i) < tab.EntryCount()) k.picked.insert(tab.EntryAt(static_cast<size_t>(i)).full_path);
        keep.push_back(std::move(k));
    });
    if (keep.empty()) return;
    RefreshPath(s, x.path);
    for (Keep& k : keep) {
        k.tab->scroll_y = k.scroll;
        if (k.picked.empty()) continue;
        std::vector<int> again;
        for (size_t i = 0; i < k.tab->EntryCount(); ++i)
            if (k.picked.contains(k.tab->EntryAt(i).full_path)) again.push_back(static_cast<int>(i));
        if (!again.empty()) k.tab->SelectIndices(again);
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Files gone since the scan leave their group; ones back (撤销) return. A
// group that lost its keeper is marked again by the rules.
void Prune(Session& x) {
    std::vector<ai::DupGroup>& groups = x.work->scan.groups;
    const size_t before = Count(x).groups;
    std::vector<char> touched(groups.size(), 0);
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        auto& files = groups[gi].files;
        for (size_t i = files.size(); i-- > 0;) {
            if (Exists(files[i].path)) continue;
            x.gone.emplace_back(gi, files[i]);
            files.erase(files.begin() + static_cast<std::ptrdiff_t>(i));
            touched[gi] = 1;
        }
    }
    for (size_t i = x.gone.size(); i-- > 0;) {
        const size_t gi = x.gone[i].first;
        if (gi >= groups.size() || !Exists(x.gone[i].second.path)) continue;
        ai::DupFile f = std::move(x.gone[i].second);
        f.keep = false;
        groups[gi].files.push_back(std::move(f));
        x.gone.erase(x.gone.begin() + static_cast<std::ptrdiff_t>(i));
        touched[gi] = 1;
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (!touched[gi]) continue;
        ai::DupGroup& g = groups[gi];
        std::sort(g.files.begin(), g.files.end(),
                  [](const ai::DupFile& a, const ai::DupFile& b) { return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0; });
        if (g.picked && g.Kept() == 0) g.picked = false;
        ai::ApplyKeepRules(g, x.newest);
    }
    if (before > 0 && Count(x).groups == 0) x.emptied = true;
}

void Rebuild(AppState& s, Session& x, bool prune) {
    if (prune && x.finished) Prune(x);
    PutList(x);
    RefreshList(s, x);
}

// ----------------------------------------------------------- the pane --

void Decorate(const std::wstring& path, ui::PaneViewModel& out) {
    Session* found = Find(path);
    if (!found) return;
    Session& x = *found;
    std::wstring sub = Fill(Text(I::DupFHeaderSub), L"{where}", x.where);
    if (x.finished && x.elapsed) {
        wchar_t secs[24];
        swprintf_s(secs, x.elapsed < 100 ? L"%.2f" : L"%.1f", static_cast<double>((std::max<uint64_t>)(x.elapsed, 10)) / 1000.0);
        sub += L" \u00B7 " + Fill(Text(I::DupFSecs), L"{s}", secs);
    }
    out.header_text = Text(I::DupFTitle) + L"  \u00B7  " + sub;
    // 路径 names folders from the compared roots: 下载\工作\2026 Q3.
    if (x.work)
        for (size_t i = 0; i < x.work->scan.roots.size() && i < x.labels.size(); ++i)
            out.path_roots.emplace_back(x.work->scan.roots[i].path, x.labels[i]);
    out.banner_dismissible = false;
    out.banner_actions[0] = out.banner_actions[1] = ui::PaneBannerAction::None;
    out.banner_kind = 0;
    if (!x.finished) {
        const ai::DupProgress& p = x.work->progress;
        const int stage = (std::min)(p.stage.load(), 3);
        out.banner_title = Fill(Text(I::DupFRunTitle), L"{where}", x.where);
        out.banner_message = Fill(Fill(Text(I::DupFRunSub), L"{n}", Int(p.files.load())), L"{size}", Size(p.bytes.load()));
        out.banner_actions[0] = ui::PaneBannerAction::DupCancel;
        ui::DupFunnelView& f = out.dup_funnel;
        f.on = true;
        f.stage = stage;
        f.title[0] = Text(I::DupFS1);
        f.title[1] = Text(I::DupFS2);
        f.title[2] = Text(I::DupFS3);
        const uint64_t done = p.done.load(), todo = p.todo.load();
        const float frac = todo ? static_cast<float>(done) / static_cast<float>(todo) : 0.0f;
        for (int i = 0; i < 3; ++i) {
            f.progress[i] = -1.0f;
            f.value[i] = Text(I::DupFWait);
        }
        // scan.size_matches / head_matches are written before the stage moves on.
        if (stage == 0) f.value[0] = Fill(Text(I::DupFS1Run), L"{n}", Int(p.files.load()));
        if (stage >= 1) f.value[0] = Fill(Text(I::DupFS1Val), L"{n}", Int(x.work->scan.size_matches));
        if (stage == 1) {
            f.value[1] = Fill(Fill(Text(I::DupFProg), L"{k}", Int(done)), L"{n}", Int(todo));
            f.progress[1] = frac;
        }
        if (stage >= 2) {
            f.value[1] = Fill(Text(I::DupFS2Val), L"{n}", Int(x.work->scan.head_matches));
            f.value[2] = Fill(Fill(Text(I::DupFS3Val), L"{k}", Int(done)), L"{n}", Int(todo));
            f.progress[2] = frac;
        }
        return;
    }
    out.dup_chips = true;
    if (x.busy) {
        out.banner_title = Text(x.permanent ? I::DupFBusyPurge : I::DupFBusy);
        out.banner_message = Text(I::DupFBusySub);
        return;
    }
    const Totals t = Count(x);
    if (t.groups == 0) {
        out.banner_title.clear();
        out.banner_message.clear();
        out.dup_empty_title = Text(x.emptied ? I::DupFEmptyAfter : I::DupFEmpty);
        return;
    }
    const ai::DupScan& scan = x.work->scan;
    out.banner_title = t.extra ? Fill(Fill(Fill(Text(I::DupFSumTitle), L"{g}", Int(t.groups)), L"{n}", Int(t.extra)), L"{size}", Size(t.bytes))
                               : Fill(Text(I::DupFSumKeepAll), L"{g}", Int(t.groups));
    out.banner_message = Fill(Text(I::DupFSumSub), L"{n}", Int(scan.total_files));
    if (scan.online_skipped) out.banner_message += Fill(Text(I::DupFSumOnline), L"{n}", Int(scan.online_skipped));
    if (scan.truncated) out.banner_message += Text(I::DupFSumTruncated);
    int slot = 0;
    if (t.extra) {
        out.banner_actions[slot] = ui::PaneBannerAction::DupRecycle;
        out.banner_action_labels[slot++] = Fill(Text(I::DupFRecycleBtn), L"{n}", Int(t.extra));
    }
    out.banner_actions[slot] = ui::PaneBannerAction::DupRule;
    out.banner_action_labels[slot] = Text(x.newest ? I::DupFRuleNewest : I::DupFRuleOldest);
}

// ------------------------------------------------------------ recycle --

std::wstring RowName(const Session& x, const ai::DupGroup& g, const ai::DupFile& f, bool first) {
    const ai::DupFile& k = g.files[Keeper(g)];
    std::wstring row = Leaf(f.path);
    // Its folder when the keeper is elsewhere, or another copy has its name.
    bool where = _wcsicmp(Parent(f.path).c_str(), Parent(k.path).c_str()) != 0;
    for (const ai::DupFile& o : g.files)
        if (&o != &f && _wcsicmp(Leaf(o.path).c_str(), Leaf(f.path).c_str()) == 0) where = true;
    if (where) row += L" \u00B7 " + RelDir(x, f);
    if (first) {
        const bool same_name = _wcsicmp(Leaf(f.path).c_str(), Leaf(k.path).c_str()) == 0;
        row += L" \u00B7 " + Fill(Text(I::DupFAskKeep), L"{name}", same_name ? RelDir(x, k) : Leaf(k.path));
    }
    return row + L"    " + Size(f.bytes);
}

void AskRecycle(AppState& s, Session& x) {
    if (!x.finished || x.busy) return;
    const Totals t = Count(x);
    if (!t.extra) return;
    std::vector<ai::DupGroup> act;
    std::map<wchar_t, uint64_t> need;
    uint64_t largest = 0;
    ui::ConfirmDialogSpec spec;
    for (const ai::DupGroup& g : x.work->scan.groups) {
        if (g.files.size() < 2 || !g.Extra()) continue;
        act.push_back(g);
        bool first = true;
        for (const ai::DupFile& f : g.files) {
            if (f.keep) continue;
            if (spec.items.size() < 6) spec.items.push_back(RowName(x, g, f, first));
            first = false;
            const wchar_t drive = f.path.size() > 1 && f.path[1] == L':' ? static_cast<wchar_t>(towupper(f.path[0])) : 0;
            need[drive] += f.bytes;
            largest = (std::max)(largest, f.bytes);
        }
    }
    // Truthful about the bin: no bin there, or a copy larger than the bin, is
    // a permanent delete (red, asked twice); a bin with too little room left
    // drops its oldest items to take these.
    bool permanent = false, tight = false;
    wchar_t shown_drive = L'C';
    uint64_t shown_free = 0;
    for (const auto& [drive, bytes] : need) {
        const ai::BinState bin = drive ? ai::ReadBin(drive) : ai::BinState{};
        if (!bin.available || (bin.max_bytes && largest > bin.max_bytes)) {
            permanent = true;
            shown_drive = drive ? drive : L'?';
            break;
        }
        const uint64_t free = bin.max_bytes > bin.used_bytes ? bin.max_bytes - bin.used_bytes : 0;
        if (free < bytes) tight = true;
        if (shown_free == 0 || free < shown_free) {
            shown_free = free;
            shown_drive = drive;
        }
    }
    const std::wstring drive_text(1, shown_drive);
    spec.title = Fill(Text(I::DupFAskTitle), L"{n}", Int(t.extra));
    spec.message = Fill(Text(permanent ? I::DupFAskMsgPurge : I::DupFAskMsg), L"{size}", Size(t.bytes));
    spec.items_total = t.extra;
    spec.item_glyph = L"\xE8A5";
    spec.note = permanent ? Fill(Text(I::DupFAskFull), L"{size}", Size(t.bytes))
                : Fill(Fill(Text(tight ? I::DupFAskTight : I::DupFAskRoom), L"{drive}", drive_text), L"{free}", Size(shown_free));
    spec.confirm_text = Text(permanent ? I::DupFAskPurge : I::DupFAskOk);
    spec.cancel_text = Text(I::Cancel);
    spec.danger = permanent;
    spec.default_choice = permanent ? ui::ConfirmChoice::Cancel : ui::ConfirmChoice::Confirm;
    if (permanent) spec.confirm_again_text = Text(I::AiFoArm);
    const uint32_t id = x.id;
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return;
    Session* again = FindId(id);   // the dialog ran a message loop
    if (!again || again->busy) return;
    again->busy = true;
    again->permanent = permanent;
    auto check = std::make_shared<Check>();
    check->groups = std::move(act);
    check->ok.assign(check->groups.size(), 0);
    again->check = check;
    const HWND owner = s.hwnd;
    std::thread([check, owner] {
        for (size_t gi = 0; gi < check->groups.size(); ++gi) {
            if (check->cancel.load()) break;
            const ai::DupGroup& g = check->groups[gi];
            const ai::DupFile& keeper = g.files[Keeper(g)];
            bool same = true;
            for (const ai::DupFile& f : g.files)
                if (!f.keep && !ai::DupStillSame(keeper, f, check->cancel)) {
                    same = false;
                    break;
                }
            check->ok[gi] = same ? 1 : 0;
        }
        check->done.store(true);
        PostMessageW(owner, WM_UI_KICK, 0, 0);
    }).detach();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowOutcome(AppState& s, const Session& x, size_t moved, uint64_t bytes, bool failed) {
    if (!s.hwnd) return;
    std::vector<ui::SnackAction> actions;
    const bool undo = !x.permanent && moved && s.ops.CanUndo();
    if (undo) actions.push_back({Text(I::SnackUndo), WM_UNDO_DELETE});
    std::wstring text;
    if (failed && !moved) text = Text(I::DupFFailed);
    else if (x.skipped_groups == 1) text = Fill(Fill(Text(I::DupFPart), L"{n}", Int(moved)), L"{name}", x.skipped_name);
    else if (x.skipped_groups > 1) text = Fill(Fill(Text(I::DupFPartN), L"{n}", Int(moved)), L"{k}", Int(x.skipped_groups));
    else text = Fill(Fill(Text(x.permanent ? I::DupFPurged : I::DupFDone), L"{n}", Int(moved)), L"{size}", Size(bytes));
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, std::move(text), {}, std::move(actions),
                                   undo ? L"Ctrl+Z" : L"");
}

void SubmitRecycle(AppState& s, Session& x) {
    const std::shared_ptr<Check> check = std::move(x.check);
    x.sent.clear();
    x.sent_bytes.clear();
    x.skipped_groups = 0;
    x.skipped_name.clear();
    ops::OpRequest r;
    r.type = x.permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    r.duplicate_cleanup = !x.permanent;   // the guard locks each keeper and checks once more
    for (size_t gi = 0; gi < check->groups.size(); ++gi) {
        const ai::DupGroup& g = check->groups[gi];
        if (!check->ok[gi]) {
            if (++x.skipped_groups == 1) x.skipped_name = GroupName(g);
            continue;
        }
        const ai::DupFile& keeper = g.files[Keeper(g)];
        ops::DuplicateCleanupGroup cg;
        cg.keeper = {keeper.path, keeper.bytes, keeper.mtime};
        for (const ai::DupFile& f : g.files) {
            if (f.keep) continue;
            cg.extras.push_back({f.path, f.bytes, f.mtime});
            r.sources.push_back(f.path);
            x.sent.push_back(f.path);
            x.sent_bytes.push_back(f.bytes);
        }
        if (!x.permanent) r.duplicate_groups.push_back(std::move(cg));
    }
    if (r.sources.empty()) {
        x.busy = false;
        ShowOutcome(s, x, 0, 0, true);
        Rebuild(s, x, true);
        return;
    }
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    r.undo_group = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    r.undo_label = Text(I::AiDupUndoLabel);
    x.task = s.ops.Submit(std::move(r));
    if (!x.task) {
        x.busy = false;
        ShowOutcome(s, x, 0, 0, true);
    }
}

void Finish(AppState& s, Session& x, uint64_t now) {
    x.finished = true;
    x.finished_at = now;
    x.elapsed = now - x.started;
    if (x.newest)
        for (ai::DupGroup& g : x.work->scan.groups) ai::ApplyKeepRules(g, true);
    PutList(x);
    RefreshList(s, x);
}

void Drop(Session* x) {
    if (!x) return;
    x->work->cancel.store(true);
    if (x->check) x->check->cancel.store(true);
    for (size_t i = 0; i < g_sessions.size(); ++i)
        if (g_sessions[i].get() == x) {
            g_sessions.erase(g_sessions.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
}

Session* ActiveSession(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    Session* x = tab ? Find(tab->current_path) : nullptr;
    return x && x->owner == s.hwnd ? x : nullptr;
}

std::wstring RootLabel(const ai::DupRoot& r) {
    static const std::pair<const wchar_t*, I> kNames[] = {
        {L"downloads", I::AiDupRootDownloads}, {L"pictures", I::AiDupRootPictures}, {L"desktop", I::AiDupRootDesktop},
        {L"documents", I::AiDupRootDocuments}, {L"videos", I::AiDupRootVideos},     {L"music", I::AiDupRootMusic}};
    for (const auto& [key, id] : kNames)
        if (r.key == key) return Text(id);
    std::wstring label = app::TabTitle(r.path);
    return label.empty() ? Leaf(r.path) : label;
}

const char* WhyKey(const ai::DupGroup& g) {
    switch (g.why) {
    case ai::DupWhy::Organized: return "organized_folder";
    case ai::DupWhy::CleanName: return "clean_name";
    case ai::DupWhy::Newest: return "newest";
    case ai::DupWhy::Both: return "copies_in_organized_folders_all_kept";
    case ai::DupWhy::Picked: return "user_picked";
    default: return "earliest";
    }
}

std::string IsoDate(uint64_t ticks) {
    FILETIME ft = Ft(ticks), lt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return {};
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02u %02u:%02u", unsigned{st.wYear}, unsigned{st.wMonth}, unsigned{st.wDay},
              unsigned{st.wHour}, unsigned{st.wMinute});
    return buf;
}

}  // namespace

uint32_t Start(AppState& s, const std::vector<std::wstring>& roots, uint64_t min_bytes, bool show, std::string* error,
               std::wstring* why) {
    SetDupPaneDecorator(&Decorate);
    std::wstring current;
    if (const app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        current = tab->current_path;
    if (current.rfind(L"\\\\?\\", 0) == 0 && current.rfind(L"\\\\?\\UNC\\", 0) != 0) current = current.substr(4);
    auto x = std::make_shared<Session>();
    x->work = std::make_shared<Work>();
    x->work->min_bytes = min_bytes ? min_bytes : kDefaultMinBytes;
    std::string err;
    if (roots.empty() && current.empty()) err = "open a folder first, or name the folders to compare";
    if (!err.empty() || !ai::ResolveDupRoots(roots, current, x->work->scan.roots, err)) {
        if (error) *error = err;
        if (why) *why = Text(roots.empty() && current.empty() ? I::ActWhyOpenFolder : I::DupFNoFolder);
        return 0;
    }
    static uint32_t next = 0;
    x->id = 0x40000000u + (++next) * 8u;
    x->path = L"pulse:ai-list:" + std::to_wstring(x->id);
    x->owner = s.hwnd;
    x->show = show;
    x->started = GetTickCount64();
    for (const ai::DupRoot& r : x->work->scan.roots) {
        x->labels.push_back(RootLabel(r));
        x->where += (x->where.empty() ? L"" : Text(I::AiTkListSep)) + x->labels.back();
    }
    PutList(*x);
    g_sessions.push_back(x);
    const std::shared_ptr<Work> work = x->work;
    const HWND owner = s.hwnd;
    std::thread([work, owner] {
        ai::FindDuplicates(work->scan, work->min_bytes, work->progress, work->cancel);
        work->done.store(true);
        if (owner) PostMessageW(owner, WM_UI_KICK, 0, 0);
    }).detach();
    if (show) OpenTabAt(s, x->path);
    return x->id;
}

void FindInFolder(AppState& s) {
    std::wstring why;
    if (Start(s, {}, kDefaultMinBytes, true, nullptr, &why) || why.empty() || !s.hwnd) return;
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, std::move(why), {}, {});
}

bool Finished(uint32_t id) {
    const Session* x = FindId(id);
    return !x || x->finished || x->work->done.load();
}

bool ResultJson(uint32_t id, json::Value& out) {
    const Session* x = FindId(id);
    if (!x) return false;
    out = json::Value::Object();
    json::Value folders = json::Value::Array();
    for (const ai::DupRoot& r : x->work->scan.roots) folders.push(json::Utf8(r.path));
    out.set("folders", std::move(folders));
    if (x->work->cancel.load()) {
        out.set("cancelled", true);
        return true;
    }
    if (!x->finished && !x->work->done.load()) {
        out.set("running", true);
        return true;
    }
    const ai::DupScan& scan = x->work->scan;
    uint64_t extra_bytes = 0;
    size_t extra = 0, groups = 0;
    json::Value list = json::Value::Array();
    for (const ai::DupGroup& g : scan.groups) {
        if (g.files.size() < 2) continue;
        ++groups;
        extra += g.Extra();
        extra_bytes += g.ExtraBytes();
        if (list.size() >= 50) continue;
        json::Value row = json::Value::Object();
        row.set("bytes_each", g.files[0].bytes);
        row.set("keep_reason", WhyKey(g));
        json::Value files = json::Value::Array();
        for (const ai::DupFile& f : g.files) {
            json::Value file = json::Value::Object();
            file.set("path", json::Utf8(f.path));
            file.set("modified", IsoDate(f.mtime));
            file.set("keep", f.keep);
            files.push(std::move(file));
        }
        row.set("files", std::move(files));
        list.push(std::move(row));
    }
    out.set("compared_files", scan.total_files);
    out.set("groups_total", static_cast<uint64_t>(groups));
    out.set("extra_copies", static_cast<uint64_t>(extra));
    out.set("extra_bytes", extra_bytes);
    if (scan.online_skipped) out.set("skipped_cloud_only_files", scan.online_skipped);
    if (scan.truncated) out.set("truncated", true);
    out.set("groups", std::move(list));
    if (groups > 50) out.set("groups_listed", static_cast<uint64_t>(50));
    out.set("tab_opened", x->show);
    out.set("note", "Compared on this PC byte by byte; no file contents were sent anywhere. Only the user moves the "
                    "extra copies to the Recycle Bin, from the tab's button (it asks every time).");
    return true;
}

std::wstring Summary(uint32_t id) {
    const Session* x = FindId(id);
    if (!x) return {};
    if (!x->finished) return x->where;
    return x->where + L" \u00B7 " + Fill(Text(I::DupFGroupsN), L"{g}", Int(Count(*x).groups));
}

bool IsDupTab(const std::wstring& path) { return Find(path) != nullptr; }

bool Tick(AppState& s) {
    bool dirty = false;
    const uint64_t now = GetTickCount64();
    for (size_t i = 0; i < g_sessions.size();) {
        Session& x = *g_sessions[i];
        if (x.owner != s.hwnd) {
            ++i;
            continue;
        }
        const bool shown = TabExists(s, x.path);
        if (!x.finished) {
            if (x.work->done.load()) {
                Finish(s, x, now);
                dirty = true;
            } else if (shown && now - x.last_paint >= 100) {
                x.last_paint = now;
                dirty = true;
            }
        }
        if (x.check && x.check->done.load()) {
            SubmitRecycle(s, x);
            dirty = true;
        }
        // 取消或关掉标签页就停止，不留任何东西: a tab no pane shows any more.
        if (x.show && !x.busy) {
            if (shown) x.orphan_since = 0;
            else if (!x.orphan_since) x.orphan_since = now;
            else if (now - x.orphan_since > 2000) {
                Drop(&x);
                continue;
            }
        } else if (!x.show && x.finished && now - x.finished_at > 10 * 60 * 1000) {
            Drop(&x);
            continue;
        }
        ++i;
    }
    return dirty;
}

void BannerPress(AppState& s, ui::PaneBannerAction action) {
    Session* x = ActiveSession(s);
    if (!x) return;
    switch (action) {
    case ui::PaneBannerAction::DupCancel: {
        x->work->cancel.store(true);
        Drop(x);
        const app::LayoutTab* layout = s.window_tabs.Active();
        const app::Tab* tab = ActiveTab(s);
        if (layout && layout->panes.size() <= 1) CloseActiveTab(s);
        else if (tab && tab->CanGoBack()) GoBack(s);
        break;
    }
    case ui::PaneBannerAction::DupRule:
        if (!x->finished || x->busy) break;
        x->newest = !x->newest;
        for (ai::DupGroup& g : x->work->scan.groups) ai::ApplyKeepRules(g, x->newest);
        Rebuild(s, *x, false);
        break;
    case ui::PaneBannerAction::DupRecycle:
        AskRecycle(s, *x);
        break;
    default:
        break;
    }
}

bool CanKeepSelected(AppState& s) {
    const Session* x = ActiveSession(s);
    const app::Tab* tab = ActiveTab(s);
    return x && x->finished && !x->busy && tab && tab->SelectedCount() == 1;
}

void KeepSelected(AppState& s) {
    Session* x = ActiveSession(s);
    app::Tab* tab = ActiveTab(s);
    if (!CanKeepSelected(s) || !x || !tab) return;
    const std::vector<int> picked = tab->SelectedIndices();
    if (picked.size() != 1 || picked[0] < 0 || static_cast<size_t>(picked[0]) >= tab->EntryCount()) return;
    const std::wstring path = tab->EntryAt(static_cast<size_t>(picked[0])).full_path;
    for (ai::DupGroup& g : x->work->scan.groups)
        for (size_t i = 0; i < g.files.size(); ++i)
            if (_wcsicmp(g.files[i].path.c_str(), path.c_str()) == 0) {
                ai::PickKeeper(g, i);
                Rebuild(s, *x, false);
                return;
            }
}

void ReapplyRules(AppState& s) {
    Session* x = ActiveSession(s);
    app::Tab* tab = ActiveTab(s);
    if (!x || !tab || !x->finished || x->busy) return;
    std::unordered_set<std::wstring> paths;
    for (const int i : tab->SelectedIndices())
        if (i >= 0 && static_cast<size_t>(i) < tab->EntryCount()) paths.insert(tab->EntryAt(static_cast<size_t>(i)).full_path);
    for (ai::DupGroup& g : x->work->scan.groups) {
        bool in = paths.empty();
        for (const ai::DupFile& f : g.files)
            if (paths.contains(f.path)) in = true;
        if (!in) continue;
        g.picked = false;
        ai::ApplyKeepRules(g, x->newest);
    }
    Rebuild(s, *x, false);
}

bool OwnsTask(uint64_t task_id) {
    if (!task_id) return false;
    for (const auto& x : g_sessions)
        if (x->task == task_id) return true;
    return false;
}

void NoteFinished(AppState& s, const ops::FinishedTask& done) {
    const bool changes_files = done.is_undo || done.type == ops::OpType::RecycleDelete ||
                               done.type == ops::OpType::RealDelete || done.type == ops::OpType::Move ||
                               done.type == ops::OpType::Rename;
    for (size_t i = 0; i < g_sessions.size(); ++i) {
        Session& x = *g_sessions[i];
        if (x.owner != s.hwnd || !x.finished) continue;
        if (x.task && x.task == done.task_id) {
            x.task = 0;
            x.busy = false;
            size_t moved = 0;
            uint64_t bytes = 0;
            for (size_t k = 0; k < x.sent.size(); ++k)
                if (!Exists(x.sent[k])) {
                    ++moved;
                    bytes += x.sent_bytes[k];
                }
            ShowOutcome(s, x, moved, bytes, !done.success);
            Rebuild(s, x, true);
        } else if (changes_files && !x.busy && TabExists(s, x.path)) {
            Rebuild(s, x, true);
        }
    }
}

}  // namespace pulse::app::dup
