#include "../drive_eject_ui.h"
// action_args_ext.cpp - The P4b runners of action_args.h: actions whose
// effect leaves Pulse (Git, shell menu items, other programs) or touches the
// system (cloud files, drives), with the words of their confirm cards, and
// the lock text of the actions only the user does.
//
// The gate (action_policy.h) asks before any of these runs: AskFor gives the
// card its words, the runner does what the menu row does once the user
// accepted. Nothing here asks or waits for the user itself.
#include "action_args.h"

#include "action_inventory.h"
#include "action_settings.h"
#include "../app_commands.h"
#include "../app_internal.h"
#include "../app_messages.h"
#include "../app_runtime.h"
#include "../cloud_roots.h"
#include "../drive_verbs.h"
#include "../git_status.h"
#include "../git_ui.h"
#include "../../common/localization.h"
#include "../../fs/bitlocker_volume.h"
#include "../../fs/cloud_state.h"
#include "../../fs/fs_enum.h"
#include "../../ui/fluent_menu.h"

#include <winnetwk.h>

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }
std::wstring W(const std::string& s) { return json::Wide(s); }
std::string U8(const std::wstring& s) { return json::Utf8(s); }
std::wstring Num(size_t n) { return std::to_wstring(n); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    for (size_t at = text.find(key); at != std::wstring::npos; at = text.find(key, at + value.size()))
        text.replace(at, key.size(), value);
    return text;
}

ArgRun Fail(std::string error, std::wstring why) {
    ArgRun r;
    r.error = std::move(error);
    r.why = std::move(why);
    return r;
}

void SetLine(ArgRun& r, const std::wstring& text, const std::wstring& bold) {
    const size_t at = bold.empty() ? std::wstring::npos : text.find(bold);
    if (at == std::wstring::npos) {
        r.line_before = text;
        return;
    }
    r.line_before = text.substr(0, at);
    r.line_bold = bold;
    r.line_after = text.substr(at + bold.size());
}

std::wstring Leaf(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.size() == 3 && path[1] == L':') return path.substr(0, 2);   // E:\ -> E:
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

ArgAsk NoCard(ArgAsk::State state) {
    ArgAsk a;
    a.state = state;
    return a;
}

// ---- The selection an action works on --------------------------------------------------

struct Target {
    std::vector<std::wstring> paths;
    std::wstring label;   // 发票.pdf / 发票.pdf 等 3 项
};

Target SelectionTarget(AppState& s) {
    Target t;
    const Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->SelectedCount() == 0) return t;
    t.paths = SelectedFullPaths(*tab);
    if (t.paths.empty()) return t;
    t.label = t.paths.size() == 1 ? Leaf(t.paths[0])
                                  : Fill(Fill(Text(I::AiActTargetMany), L"{name}", Leaf(t.paths[0])), L"{n}",
                                         Num(t.paths.size()));
    return t;
}

// ---- Git -------------------------------------------------------------------------------

std::wstring RepoRoot(AppState& s) {
    const Tab* tab = ActiveTab(s);
    return tab ? tab->git_root : std::wstring();
}

// The status now: git status run here (4 s at most: an explicit read the
// card depends on), else the last answer the folder view has.
std::shared_ptr<const GitRepoStatus> ReadStatus(const std::wstring& root) {
    const GitRunResult run =
        RunGit(root, L"--no-optional-locks status --porcelain=v2 --branch -z --untracked-files=normal", 4000, false);
    if (run.started && !run.timed_out && run.exit_code == 0) {
        auto status = std::make_shared<GitRepoStatus>();
        status->root = root;
        if (ParseGitStatusV2(run.out, *status)) {
            StoreGitStatus(status);
            return status;
        }
    }
    return GitStatusFor(root);
}

std::wstring BranchOf(const GitRepoStatus* st) {
    if (!st) return {};
    return st->branch.empty() ? st->head : st->branch;
}

// origin/feat/report -> origin; no upstream yet: origin (what a first push uses).
std::wstring RemoteOf(const GitRepoStatus* st) {
    if (!st || st->upstream.empty()) return L"origin";
    const size_t slash = st->upstream.find(L'/');
    return slash == std::wstring::npos ? st->upstream : st->upstream.substr(0, slash);
}

// 领先 origin 3 个提交 · 工作区干净
std::wstring StatusSub(const GitRepoStatus& st) {
    const std::wstring remote = RemoteOf(&st);
    std::wstring head;
    if (st.upstream.empty())
        head = Text(I::AiGitNoUpstream);
    else if (st.ahead > 0 && st.behind > 0)
        head = Fill(Fill(Fill(Text(I::AiGitAheadBehind), L"{remote}", remote), L"{a}", Num(static_cast<size_t>(st.ahead))),
                    L"{b}", Num(static_cast<size_t>(st.behind)));
    else if (st.ahead > 0)
        head = Fill(Fill(Text(I::AiGitAhead), L"{remote}", remote), L"{n}", Num(static_cast<size_t>(st.ahead)));
    else if (st.behind > 0)
        head = Fill(Fill(Text(I::AiGitBehind), L"{remote}", remote), L"{n}", Num(static_cast<size_t>(st.behind)));
    else
        head = Fill(Text(I::AiGitSynced), L"{remote}", remote);
    std::wstring tree;
    if (st.changed == 0) {
        tree = Text(I::AiGitClean);
    } else {
        wchar_t buf[128]{};
        swprintf_s(buf, l10n::Get(I::GitChanges).c_str(), st.changed);
        tree = buf;
    }
    return head + L" \u00B7 " + tree;
}

json::Value StatusData(const std::wstring& root, const GitRepoStatus* st) {
    json::Value d = json::Value::Object();
    d.set("repo", U8(Leaf(root)));
    if (!st) {
        d.set("status", "unknown: git status did not answer");
        return d;
    }
    d.set("branch", U8(BranchOf(st)));
    d.set("upstream", U8(st->upstream));
    d.set("ahead", static_cast<int64_t>(st->ahead));
    d.set("behind", static_cast<int64_t>(st->behind));
    d.set("changed", static_cast<int64_t>(st->changed));
    return d;
}

ArgRun NotRepo() {
    return Fail("not in a Git repository: open a folder inside one first", Text(I::AiGitNotRepo));
}

ArgRun GitRefresh(AppState& s, const json::Value&) {
    const std::wstring root = RepoRoot(s);
    if (root.empty()) return NotRepo();
    const auto st = ReadStatus(root);
    HandleGitCommand(s, kCmdGitRefresh, root);   // the marks in the list follow
    ArgRun r;
    r.ok = true;
    r.read_only = true;
    r.title = Text(I::AiActGitState);
    r.arg = BranchOf(st.get());
    r.sub = st ? StatusSub(*st) : std::wstring();
    r.data = StatusData(root, st.get());
    return r;
}

ArgRun GitRemote(AppState& s, bool push) {
    const std::wstring root = RepoRoot(s);
    if (root.empty()) return NotRepo();
    const auto st = GitStatusFor(root);
    const std::wstring branch = BranchOf(st.get());
    if (push && st && !st->upstream.empty() && st->ahead == 0) {
        ArgRun r;
        r.ok = true;
        r.skip = true;
        r.arg = branch;
        r.sub = Text(I::AiGitNothingToPush);
        r.data = StatusData(root, st.get());
        return r;
    }
    if (!push && st && st->upstream.empty())
        return Fail("this branch has no upstream to pull from", Text(I::AiGitPullNoUpstream));
    HandleGitCommand(s, push ? kCmdGitPush : kCmdGitPull, root);
    ArgRun r;
    r.ok = true;
    r.arg = branch;
    SetLine(r, Fill(Text(push ? I::AiGitPushStarted : I::AiGitPullStarted), L"{branch}", branch), branch);
    r.data = StatusData(root, st.get());
    r.data.set("started", true);
    r.data.set("note", "Git runs in the background; Pulse shows the outcome in a notification. "
                       "git.refresh reads the status again later. Pulse cannot undo it.");
    return r;
}

ArgRun GitPush(AppState& s, const json::Value&) { return GitRemote(s, true); }
ArgRun GitPull(AppState& s, const json::Value&) { return GitRemote(s, false); }

ArgAsk AskGit(AppState& s, bool push) {
    const std::wstring root = RepoRoot(s);
    if (root.empty()) return NoCard(ArgAsk::State::Fail);
    const auto st = ReadStatus(root);
    if (push && st && !st->upstream.empty() && st->ahead == 0) return NoCard(ArgAsk::State::Skip);
    if (!push && st && st->upstream.empty()) return NoCard(ArgAsk::State::Fail);
    const std::wstring branch = BranchOf(st.get()), remote = RemoteOf(st.get()), repo = Leaf(root);
    ArgAsk a;
    a.external = true;
    a.icon = push ? AskIcon::Push : AskIcon::Pull;
    if (push) {
        a.title = Text(I::AiGitPushT);
        const bool fresh = st && st->upstream.empty();
        const I sub = !st ? I::AiGitPushAnyS : fresh ? I::AiGitPushNewS : I::AiGitPushS;
        a.sub = Fill(Fill(Text(sub), L"{branch}", branch), L"{remote}", remote);
        if (st) a.sub = Fill(a.sub, L"{n}", Num(static_cast<size_t>(st->ahead)));
        a.file = repo + L" \u00B7 " + branch + L" \u2192 " + remote;
        if (fresh) a.file_note = Text(I::AiGitNewBranch);
        else if (st) a.file_note = Fill(Text(I::AiGitNCommits), L"{n}", Num(static_cast<size_t>(st->ahead)));
        a.deny = Text(I::AiGitPushDeny);
        a.accept = Text(I::GitPush);
    } else {
        a.title = Text(I::AiGitPullT);
        a.sub = Fill(Fill(Text(I::AiGitPullS), L"{branch}", branch), L"{remote}", remote);
        a.file = repo + L" \u00B7 " + remote + L" \u2192 " + branch;
        if (st && st->behind > 0)
            a.file_note = Fill(Text(I::AiGitNCommits), L"{n}", Num(static_cast<size_t>(st->behind)));
        else if (st && st->changed > 0)
            a.file_note = Fill(Text(I::AiGitDirtyNote), L"{n}", Num(static_cast<size_t>(st->changed)));
        a.deny = Text(I::AiGitPullDeny);
        a.accept = Text(I::GitPull);
    }
    return a;
}

// ---- Shell menu items (the selection's Explorer menu) -----------------------------------

// 解压到(&E)... -> 解压到 ; case is ignored by the comparison.
std::wstring VerbKey(std::wstring text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&') {
            if (i + 1 < text.size() && text[i + 1] == L'&') {
                out += L'&';
                ++i;
            }
            continue;
        }
        out += text[i];
    }
    // (&E) left as "(E)": drop a trailing access-key group.
    if (out.size() >= 3 && out.back() == L')' && out[out.size() - 3] == L'(') out.resize(out.size() - 3);
    while (!out.empty() && (out.back() == L'.' || out.back() == L'\u2026' || iswspace(out.back()))) out.pop_back();
    size_t lead = 0;
    while (lead < out.size() && iswspace(out[lead])) ++lead;
    return out.substr(lead);
}

bool SameVerb(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(VerbKey(a).c_str(), VerbKey(b).c_str()) == 0;
}

struct ShellRow {
    std::wstring label;   // 7-Zip ▸ 解压到当前文件夹
    std::wstring leaf;    // 解压到当前文件夹
    int command = 0;
    bool enabled = true;
    std::vector<std::wstring> children;   // a flyout header's rows
};

// The selection's shell rows as its menu would show them, without showing
// the menu: the same query, the same wait and the same rows as a right
// click. The snapshot (OpenMenu) is what a later click resolves against.
bool ShellRows(AppState& s, std::vector<ShellRow>& out) {
    Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->SelectedCount() == 0) return false;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return false;
    StartCtxQuery(s, std::move(paths), false, StaticVerbKey(*tab, tab->SelectedIndices()));
    WaitForShellMenuReady(s);
    s.context_menu.OpenMenu({});
    bool changed = false;
    const std::vector<ui::FluentMenuItem> rows = s.context_menu.BuildDisplay(s.ctxMenuPrefs, {}, changed);
    s.context_menu.CloseMenu();
    if (changed) s.ctxMenuPrefs.Save();
    for (const ui::FluentMenuItem& row : rows) {
        if (VerbKey(row.text).empty()) continue;
        ShellRow top;
        top.label = top.leaf = VerbKey(row.text);
        top.command = row.command;
        top.enabled = row.enabled;
        for (const ui::FluentMenuItem& child : row.children) {
            if (VerbKey(child.text).empty()) continue;
            top.children.push_back(VerbKey(child.text));
            ShellRow sub;
            sub.leaf = VerbKey(child.text);
            sub.label = top.label + L" \u25B8 " + sub.leaf;
            sub.command = child.command;
            sub.enabled = child.enabled;
            out.push_back(std::move(sub));
        }
        out.insert(out.end() - static_cast<std::ptrdiff_t>(top.children.size()), std::move(top));
    }
    return true;
}

// "7-Zip > 解压到当前文件夹", "7-Zip ▸ …" or just the row's own words.
const ShellRow* FindShellRow(const std::vector<ShellRow>& rows, const std::wstring& want) {
    std::wstring parent, leaf = want;
    for (const wchar_t* sep : {L"\u25B8", L">", L"\u203A"}) {
        const size_t at = want.find(sep);
        if (at == std::wstring::npos) continue;
        parent = VerbKey(want.substr(0, at));
        leaf = want.substr(at + wcslen(sep));
        break;
    }
    const ShellRow* hit = nullptr;
    for (const ShellRow& r : rows) {
        if (!SameVerb(r.leaf, leaf)) continue;
        if (!parent.empty()) {
            const size_t at = r.label.find(L" \u25B8 ");
            if (at == std::wstring::npos || !SameVerb(r.label.substr(0, at), parent)) continue;
        }
        if (!hit || (hit->command == 0 && r.command != 0)) hit = &r;
    }
    return hit;
}

json::Value RowNames(const std::vector<ShellRow>& rows, size_t cap) {
    json::Value names = json::Value::Array();
    for (const ShellRow& r : rows) {
        if (!r.children.empty() || !r.enabled) continue;
        if (names.size() >= cap) break;
        names.push(U8(r.label));
    }
    return names;
}

ArgRun ShellVerbs(AppState& s, const json::Value&) {
    const Target t = SelectionTarget(s);
    std::vector<ShellRow> rows;
    if (t.paths.empty() || !ShellRows(s, rows))
        return Fail("select the items first (select.items); the menu is the selection's", Text(I::ActWhySelect));
    ArgRun r;
    r.ok = true;
    r.read_only = true;
    r.arg = t.label;
    std::wstring names;
    size_t n = 0, shown = 0;
    for (const ShellRow& row : rows) {
        if (!row.children.empty() || !row.enabled) continue;
        ++n;
        if (shown < 4) names += (shown++ ? L"\u3001" : L"") + row.label;
    }
    if (n > shown) names += L"\u2026";
    r.sub = Fill(Fill(Text(I::AiShellVerbsSub), L"{n}", Num(n)), L"{names}", names);
    r.data = json::Value::Object();
    r.data.set("items", RowNames(rows, 60));
    r.data.set("note", "shell.run_verb {verb} runs one of these (Pulse asks the user first); a submenu row is "
                       "\"Parent ▸ Item\".");
    return r;
}

std::wstring VerbArg(const json::Value& args) {
    return W(args.get_string("verb", args.get_string("item", args.get_string("name"))));
}

ArgRun ShellRunVerb(AppState& s, const json::Value& args) {
    const std::wstring want = VerbArg(args);
    if (VerbKey(want).empty()) return Fail("say which menu item: verb (shell.verbs lists them)", Text(I::AiActWhyFailed));
    const Target t = SelectionTarget(s);
    std::vector<ShellRow> rows;
    if (t.paths.empty() || !ShellRows(s, rows))
        return Fail("select the items first (select.items); the menu is the selection's", Text(I::ActWhySelect));
    const ShellRow* row = FindShellRow(rows, want);
    if (!row || !row->enabled) {
        ArgRun r = Fail("the selection's menu has no such item (or it is greyed)",
                        Fill(Text(I::AiShellNoVerb), L"{verb}", VerbKey(want)));
        r.data = json::Value::Object();
        r.data.set("items", RowNames(rows, 60));
        return r;
    }
    if (!row->children.empty() || row->command == 0) {
        ArgRun r = Fail("that is a submenu: name one of its items as \"Parent ▸ Item\"",
                        Fill(Text(I::AiShellSubmenu), L"{verb}", row->label));
        json::Value kids = json::Value::Array();
        for (const std::wstring& c : row->children) kids.push(U8(row->label + L" \u25B8 " + c));
        r.data = json::Value::Object();
        r.data.set("items", std::move(kids));
        return r;
    }
    const std::wstring label = row->label;
    HandleShellMenuCommand(s, row->command);
    ArgRun r;
    r.ok = true;
    r.arg = t.label;
    SetLine(r, Fill(Text(I::AiShellRan), L"{verb}", label), label);
    r.data = json::Value::Object();
    r.data.set("ran", U8(label));
    r.data.set("note", "the item's program works outside Pulse; Pulse cannot tell what it changed or undo it");
    return r;
}

ArgAsk AskVerb(AppState& s, const json::Value& args) {
    const std::wstring want = VerbKey(VerbArg(args));
    const Target t = SelectionTarget(s);
    if (want.empty() || t.paths.empty()) return NoCard(ArgAsk::State::Fail);
    ArgAsk a;
    a.external = true;
    a.icon = AskIcon::Verb;
    a.title = Text(I::AiShellRunT);
    a.sub = Fill(Fill(Text(I::AiShellRunS), L"{target}", t.label), L"{verb}", want);
    a.file = t.label;
    a.file_note = want;
    a.deny = Text(I::AiActRunDeny);
    a.accept = Text(I::AiActRunAccept);
    return a;
}

// ---- Pinned toolbar apps -------------------------------------------------------------------

std::wstring ExeStem(const std::wstring& path) {
    std::wstring leaf = Leaf(path);
    const size_t dot = leaf.find_last_of(L'.');
    return dot == std::wstring::npos ? leaf : leaf.substr(0, dot);
}

bool Contains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (_wcsnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
    return false;
}

// The pin `want` names: its name, part of it, or its program's file name.
size_t FindToolbarApp(AppState& s, const std::wstring& want) {
    const size_t n = s.appPrefs.toolbar_apps.size();
    if (want.empty()) return n == 1 ? 0 : SIZE_MAX;
    for (size_t i = 0; i < n; ++i)
        if (_wcsicmp(ToolbarAppName(s, i).c_str(), want.c_str()) == 0) return i;
    for (size_t i = 0; i < n; ++i) {
        const std::wstring name = ToolbarAppName(s, i);
        if (Contains(name, want) || Contains(want, name) || Contains(ExeStem(s.appPrefs.toolbar_apps[i]), want))
            return i;
    }
    return SIZE_MAX;
}

json::Value ToolbarAppList(AppState& s) {
    json::Value apps = json::Value::Array();
    for (size_t i = 0; i < s.appPrefs.toolbar_apps.size(); ++i) apps.push(U8(ToolbarAppName(s, i)));
    return apps;
}

ArgRun OpenWith(AppState& s, const json::Value& args) {
    if (s.appPrefs.toolbar_apps.empty())
        return Fail("no programs are pinned to the toolbar", Text(I::AiOpenWithEmpty));
    const std::wstring want = W(args.get_string("app"));
    const size_t i = FindToolbarApp(s, want);
    if (i == SIZE_MAX) {
        ArgRun r = Fail("no pinned app by that name: app is one of apps",
                        Fill(Text(I::AiOpenWithNone), L"{app}", want));
        r.data = json::Value::Object();
        r.data.set("apps", ToolbarAppList(s));
        return r;
    }
    const Target t = SelectionTarget(s);
    if (t.paths.empty()) return Fail("select the items to open first", Text(I::ActWhySelect));
    const std::wstring name = ToolbarAppName(s, i);
    RunToolbarApp(s, i);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Fill(Fill(Text(I::AiOpenWithDone), L"{app}", name), L"{target}", t.label), name);
    r.data = json::Value::Object();
    r.data.set("app", U8(name));
    r.data.set("note", "the program runs outside Pulse; Pulse cannot undo what it does");
    return r;
}

ArgAsk AskOpenWith(AppState& s, const json::Value& args) {
    const size_t i = FindToolbarApp(s, W(args.get_string("app")));
    const Target t = SelectionTarget(s);
    if (i == SIZE_MAX || t.paths.empty()) return NoCard(ArgAsk::State::Fail);
    const std::wstring name = ToolbarAppName(s, i);
    ArgAsk a;
    a.external = true;
    a.icon = AskIcon::OpenWith;
    a.title = Fill(Text(I::AiOpenWithT), L"{app}", name);
    a.sub = Fill(Fill(Text(I::AiOpenWithS), L"{app}", name), L"{target}", t.label);
    a.file = t.label;
    a.file_note = Leaf(s.appPrefs.toolbar_apps[i]);
    a.deny = Text(I::AiOpenWithDeny);
    a.accept = Text(I::AiOpenWithAccept);
    return a;
}

// ---- Cloud files ---------------------------------------------------------------------------

ArgAsk AskCloudPin(AppState& s, bool keep) {
    const Tab* tab = ActiveTab(s);
    const Target t = SelectionTarget(s);
    if (!tab || t.paths.empty() || !IsUnderCloudFilesRoot(tab->current_path)) return NoCard(ArgAsk::State::Fail);
    const std::wstring provider = CloudRootLabelFor(t.paths[0]);
    ArgAsk a;
    a.icon = AskIcon::Cloud;
    a.title = Text(keep ? I::AiCloudKeepT : I::AiCloudFreeT);
    a.sub = Fill(Fill(Text(keep ? I::AiCloudKeepS : I::AiCloudFreeS), L"{target}", t.label), L"{provider}", provider);
    a.file = t.label;
    a.file_note = provider;
    a.deny = Text(I::AiAsstDeny);
    a.accept = Text(keep ? I::AiCloudKeepAccept : I::AiCloudFreeAccept);
    return a;
}

// The file 下载并预览 fetches: `path`, else the one selected file.
std::wstring CloudFileArg(AppState& s, const json::Value& args) {
    std::wstring path = W(args.get_string("path"));
    if (!path.empty()) return path;
    const Target t = SelectionTarget(s);
    return t.paths.size() == 1 ? t.paths[0] : std::wstring();
}

ArgRun CloudDownload(AppState& s, const json::Value& args) {
    const std::wstring path = CloudFileArg(s, args);
    if (path.empty()) return Fail("select one file (or give path)", Text(I::ActWhySelectOne));
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) ||
        (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return Fail("not a file", Text(I::ActWhySelectOne));
    const std::wstring name = Leaf(path);
    if (!fs::IsCloudPlaceholder(fa.dwFileAttributes)) {
        ArgRun r;
        r.ok = true;
        r.skip = true;
        r.arg = name;
        r.sub = Text(I::AiCloudDlLocal);
        return r;
    }
    StartCloudPreviewDownload(s, path, (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
    ArgRun r;
    r.ok = true;
    r.arg = name;
    SetLine(r, Fill(Text(I::AiCloudDlStarted), L"{name}", name), name);
    r.data = json::Value::Object();
    r.data.set("started", true);
    return r;
}

ArgAsk AskCloudDownload(AppState& s, const json::Value& args) {
    const std::wstring path = CloudFileArg(s, args);
    const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return NoCard(ArgAsk::State::Fail);
    if (!fs::IsCloudPlaceholder(attrs)) return NoCard(ArgAsk::State::Skip);
    const std::wstring name = Leaf(path), provider = CloudRootLabelFor(path);
    ArgAsk a;
    a.icon = AskIcon::Cloud;
    a.title = Text(I::AiCloudDlT);
    a.sub = Fill(Fill(Text(I::AiCloudDlS), L"{name}", name), L"{provider}", provider);
    a.file = name;
    a.file_note = provider;
    a.deny = Text(I::AiAsstDeny);
    a.accept = Text(I::AiCloudDlAccept);
    return a;
}

// ---- Drives ----------------------------------------------------------------------------------

struct DriveRef {
    std::wstring root;   // E:\ ; empty: none named or selected
    uint8_t type = 0;
};

// `drive` ("E", "E:", "E:\"), else the one drive selected in This PC.
DriveRef DriveArg(AppState& s, const json::Value& args) {
    DriveRef d;
    const std::wstring named = W(args.get_string("drive", args.get_string("path")));
    if (!named.empty()) {
        const wchar_t c = static_cast<wchar_t>(towupper(named[0]));
        if (c < L'A' || c > L'Z' || (named.size() > 1 && named[1] != L':')) return d;
        d.root = std::wstring(1, c) + L":\\";
        d.type = static_cast<uint8_t>(GetDriveTypeW(d.root.c_str()));
        return d;
    }
    const Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return d;
    const std::vector<int> sel = tab->SelectedIndices();
    if (sel.size() != 1 || sel[0] < 0 || static_cast<size_t>(sel[0]) >= tab->EntryCount()) return d;
    const fs::DirEntry e = tab->EntryAt(static_cast<size_t>(sel[0]));
    if (!e.drive_type) return d;
    d.root = fs::DriveRootOf(EntryFullPath(*tab, sel[0]));
    d.type = e.drive_type;
    return d;
}

std::wstring DriveLetter(const DriveRef& d) { return d.root.size() >= 2 ? d.root.substr(0, 2) : d.root; }

// 我的U盘 (E:)
std::wstring DriveName(const DriveRef& d) {
    wchar_t label[MAX_PATH + 1]{};
    const std::wstring letter = DriveLetter(d);
    if (GetVolumeInformationW(d.root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) && label[0])
        return std::wstring(label) + L" (" + letter + L")";
    return letter;
}

std::wstring ShareOf(const DriveRef& d) {
    wchar_t share[MAX_PATH + 1]{};
    DWORD len = MAX_PATH + 1;
    const std::wstring letter = DriveLetter(d);
    return WNetGetConnectionW(letter.c_str(), share, &len) == NO_ERROR ? std::wstring(share) : std::wstring();
}

ArgRun DriveVerb(AppState& s, const json::Value& args, bool eject) {
    const DriveRef d = DriveArg(s, args);
    if (d.root.empty()) return Fail("name a drive (drive: \"E:\") or select one in This PC", Text(I::AiDriveWhich));
    const std::wstring letter = DriveLetter(d);
    if (eject && !DriveEjectable(d.root, d.type))
        return Fail("that drive cannot be ejected", Fill(Text(I::AiDriveNotEjectable), L"{drive}", letter));
    if (!eject && d.type != DRIVE_REMOTE)
        return Fail("that is not a network drive", Fill(Text(I::AiDriveNotRemote), L"{drive}", letter));
    const bool started = eject ? StartPreparedDriveEject(s, d.root, d.type)
                               : StartDriveDisconnect(s.hwnd, WM_DRIVE_VERB, d.root, false);
    if (!started) return Fail("Windows did not take the request", Text(I::AiActWhyFailed));
    ArgRun r;
    r.ok = true;
    r.arg = letter;
    SetLine(r, Fill(Text(eject ? I::AiDriveEjecting : I::AiDriveDisconnecting), L"{drive}", letter), letter);
    r.data = json::Value::Object();
    r.data.set("note", "Windows finishes it in the background; Pulse reports a failure (a program using the drive)");
    return r;
}

ArgRun DriveEject(AppState& s, const json::Value& args) { return DriveVerb(s, args, true); }
ArgRun DriveDisconnect(AppState& s, const json::Value& args) { return DriveVerb(s, args, false); }

ArgAsk AskDrive(AppState& s, const json::Value& args, bool eject) {
    const DriveRef d = DriveArg(s, args);
    if (d.root.empty()) return NoCard(ArgAsk::State::Fail);
    if (eject ? !DriveEjectable(d.root, d.type) : d.type != DRIVE_REMOTE) return NoCard(ArgAsk::State::Fail);
    const std::wstring letter = DriveLetter(d), name = DriveName(d);
    ArgAsk a;
    a.icon = eject ? AskIcon::Eject : AskIcon::Disconnect;
    if (eject) {
        a.title = Fill(Text(I::AiDriveEjectT), L"{drive}", letter);
        a.sub = Fill(Text(I::AiDriveEjectS), L"{name}", name);
        a.accept = Text(I::Eject);
    } else {
        const std::wstring share = ShareOf(d);
        a.title = Fill(Text(I::AiDriveDiscT), L"{drive}", letter);
        a.sub = Fill(Fill(Text(I::AiDriveDiscS), L"{drive}", letter), L"{share}", share.empty() ? letter : share);
        a.file_note = share;
        a.accept = Text(I::AiDriveDiscAccept);
    }
    a.file = name;
    a.deny = Text(I::AiAsstDeny);
    return a;
}

// ---- The tables --------------------------------------------------------------------------------

using Runner = ArgRun (*)(AppState&, const json::Value&);

constexpr std::pair<std::string_view, Runner> kExtRunners[] = {
    {"git.refresh", GitRefresh},
    {"git.push", GitPush},
    {"git.pull", GitPull},
    {"shell.verbs", ShellVerbs},
    {"shell.run_verb", ShellRunVerb},
    {"toolbar.open_with", OpenWith},
    {"preview.cloud_download", CloudDownload},
    {"drive.eject", DriveEject},
    {"drive.disconnect", DriveDisconnect},
};

// Every action that asks each time has its words here (check_actions.py and
// the self test hold the list to the inventory's External rows).
constexpr std::string_view kAskIds[] = {
    "git.push", "git.pull", "shell.run_verb", "toolbar.open_with", "cloud.keep", "cloud.free",
    "preview.cloud_download", "drive.eject", "drive.disconnect",
};

constexpr std::string_view kLockIds[] = {"app.exit", "drive.unlock", "assistant.prefs", "assistant.organize_here"};

constexpr bool TablesKnown() {
    for (const auto& [id, _] : kExtRunners)
        if (!FindAction(id)) return false;
    for (std::string_view id : kAskIds)
        if (!FindAction(id)) return false;
    for (std::string_view id : kLockIds)
        if (!FindAction(id)) return false;
    return true;
}
static_assert(TablesKnown(), "action_args_ext: an id is missing from action_inventory.h");

} // namespace

bool HasExtRunner(std::string_view id) noexcept {
    for (const auto& [k, _] : kExtRunners)
        if (k == id) return true;
    return false;
}

ArgRun RunExt(AppState& s, std::string_view id, const json::Value& args) {
    for (const auto& [k, run] : kExtRunners)
        if (k == id) return run(s, args);
    return Fail("unknown action", Text(I::AiActWhyUnknown));
}

bool HasAskText(std::string_view id) noexcept {
    return std::find(std::begin(kAskIds), std::end(kAskIds), id) != std::end(kAskIds);
}

ArgAsk AskFor(AppState& s, std::string_view id, const json::Value& args) {
    if (id == "git.push") return AskGit(s, true);
    if (id == "git.pull") return AskGit(s, false);
    if (id == "shell.run_verb") return AskVerb(s, args);
    if (id == "toolbar.open_with") return AskOpenWith(s, args);
    if (id == "cloud.keep") return AskCloudPin(s, true);
    if (id == "cloud.free") return AskCloudPin(s, false);
    if (id == "preview.cloud_download") return AskCloudDownload(s, args);
    if (id == "drive.eject") return AskDrive(s, args, true);
    if (id == "drive.disconnect") return AskDrive(s, args, false);
    return {};   // the generic card
}

bool HasLockText(std::string_view id) noexcept {
    return std::find(std::begin(kLockIds), std::end(kLockIds), id) != std::end(kLockIds);
}

ArgLock LockFor(AppState& s, std::string_view id, const json::Value& args) {
    ArgLock k;
    k.title = Text(I::AiActWhyLocked);
    if (id == "drive.unlock") {
        const DriveRef d = DriveArg(s, args);
        k.sub = Text(I::AiLockUnlockS);
        k.where = d.root.empty() ? Text(I::AiLockUnlockWhereAny)
                                 : Fill(Text(I::AiLockUnlockWhere), L"{letter}", d.root.substr(0, 1));
    } else if (id == "app.exit") {
        k.sub = Text(I::AiLockExitS);
        k.where = Text(I::AiLockExitWhere);
    } else if (id == "assistant.prefs") {
        k.sub = Text(I::AiSetLockS);
        k.where = Fill(Text(I::AiLockWhereSettings), L"{page}", SettingsPageTitle(13));
    } else if (id == "assistant.organize_here") {
        // Task 46: it only fills the assistant's box for the user to send.
        k.sub = Text(I::AiLockOrgHereS);
        k.where = Text(I::AiLockOrgHereWhere);
    }
    return k;
}

} // namespace pulse::app::actions
